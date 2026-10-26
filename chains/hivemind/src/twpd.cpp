// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// lib/state/two_way_peg_data.rs: deposits, withdrawal bundle events and the pending
// bundle. Truthcoin's copy is thunder-rust's plus the per-height TwoWayPegEvent record
// (RPC get_two_way_peg_events) and s13_ deposit addresses; ported from our Thunder
// drop-in, which reproduces thunder-rust v0.18.2 line by line.
#include "state_internal.h"

#include <bt/crypto.h>

#include <algorithm>

namespace hivemind {

using bt::kv::Txn;
using bt::utreexo::NodeHash;
using bt::mainchain::TwoWayPegData;

static NodeHash nh(const Hash32& h) { return NodeHash::some(h); }
static Bytes u32le(uint32_t v)
{
    Writer w;
    w.u32(v);
    return w.buf;
}
static const Bytes UNIT{};
static void enc_output(Writer& w, const Output& o) { bincode(w, o); }
static Bytes m6id_key(const Hash32& m)
{
    Writer w;
    w.bincode_bytes(m.span());
    return w.buf;
}
template <class T, class F>
static void enc_rollback(Writer& w, const RollBack<T>& rb, F encv)
{
    w.u64(rb.v.size());
    for (auto& hs : rb.v) {
        encv(w, hs.value);
        w.u32(hs.height);
    }
}
static void put_latest_failed(Txn& t, const RollBack<Hash32>& rb)
{
    Writer w;
    enc_rollback(w, rb, [](Writer& ww, const Hash32& h) { ww.raw(h); });
    t.put(State::T_LATEST_FAILED, UNIT, w.buf);
}
static Bytes enc_block_ref(const Hash32& h, uint32_t height)
{
    Writer w;
    w.bincode_bytes(h.span());
    w.u32(height);
    return w.buf;
}
static std::optional<std::pair<uint32_t, std::pair<Hash32, uint32_t>>> seq_last(const Txn& t, uint8_t table)
{
    auto l = t.last(table); // memcmp-largest key, exactly as LMDB's last()
    if (!l) return std::nullopt;
    Reader kr(l->first), vr(l->second);
    uint32_t seq = kr.u32();
    Bytes hb = vr.bincode_bytes();
    uint32_t h = vr.u32();
    return std::make_pair(seq, std::make_pair(Hash32::from_span(hb), h));
}
static uint32_t varint_size(uint64_t n) { return uint32_t(bt::btc::compact_size_len(n)); }
static std::optional<uint32_t> txout_size(uint32_t spk_size)
{
    uint64_t s = 8ull + varint_size(spk_size) + spk_size;
    if (s > UINT32_MAX) return std::nullopt;
    return uint32_t(s);
}

// ------------------------------------------------------------------ two-way peg
// lib/types/proto.rs
Output State::deposit_output(const bt::mainchain::Deposit& d)
{
    Output o;
    o.content.kind = Content::Value;
    o.content.value = d.value_sats;
    std::string s(d.address.begin(), d.address.end());
    o.address = Address::ALL_ZEROS;
    // proto.rs:470-497: the s13_ deposit form, else a bare address (deposits made before
    // the prefixed form), else the all-zeros burn address.
    if (Reader::utf8_valid(s)) {
        if (auto a = Address::from_deposit_address(s)) o.address = *a;
        else if (auto b = Address::from_base58(s)) o.address = *b;
    }
    return o;
}

// two_way_peg_data.rs:23-114
std::optional<WithdrawalBundle> State::collect_withdrawal_bundle(const Txn& t, uint32_t block_height) const
{
    struct Agg {
        bt::btc::Address addr;
        uint64_t value = 0, main_fee = 0;
        std::map<OutPoint, Output> utxos;
    };
    std::vector<Agg> aggs; // HashMap keyed by Address (Eq includes network/hrp)
    for (auto& [op, o] : utxos(t)) {
        if (o.content.kind != Content::Withdrawal) continue;
        auto it = std::find_if(aggs.begin(), aggs.end(), [&](const Agg& a) { return a.addr == o.content.main_address; });
        if (it == aggs.end()) {
            aggs.push_back(Agg{o.content.main_address, 0, 0, {}});
            it = std::prev(aggs.end());
        }
        if (it->value + o.content.value < it->value) throw StateError("AmountOverflow");
        it->value += o.content.value;
        if (it->main_fee + o.content.main_fee < it->main_fee) throw StateError("AmountOverflow");
        it->main_fee += o.content.main_fee;
        it->utxos[op] = o;
    }
    if (aggs.empty()) return std::nullopt;
    // sort by Reverse((main_fee, value, main_address))
    std::sort(aggs.begin(), aggs.end(), [](const Agg& a, const Agg& b) {
        if (a.main_fee != b.main_fee) return a.main_fee > b.main_fee;
        if (a.value != b.value) return a.value > b.value;
        return b.addr < a.addr;
    });
    uint64_t fee = 0;
    std::map<OutPoint, Output> spend;
    std::vector<bt::btc::TxOut> outs;
    uint32_t txouts_size = 0;
    for (auto& a : aggs) {
        Bytes spk = a.addr.script_pubkey();
        uint64_t n_outputs = outs.size() + 1;
        if (n_outputs > UINT32_MAX) break;
        if (spk.size() > UINT32_MAX) continue;
        auto ts = txout_size(uint32_t(spk.size()));
        if (!ts) continue;
        uint64_t sum = uint64_t(txouts_size) + *ts;
        if (sum > UINT32_MAX) break;
        txouts_size = uint32_t(sum);
        if (!predict_weight_ok(uint32_t(n_outputs), txouts_size)) break; // `break`, not skip (bug §5)
        for (auto& [op, o] : a.utxos) spend[op] = o;
        outs.push_back({a.value, spk});
        if (fee + a.main_fee < fee) throw StateError("Amount addition overflow", true); // `+=` panics
        fee += a.main_fee;
    }
    auto b = make_withdrawal_bundle(block_height, fee, std::move(spend), std::move(outs));
    if (!b) throw StateError("BundleTooHeavy");
    return b;
}

// two_way_peg_data.rs:116-280
void State::connect_bundle_submitted(Txn& t, uint32_t block_height, AccumulatorDiff& diff, std::vector<TwoWayPegEvent>& ev, const Hash32& m6id) const
{
    auto pending = pending_bundle_m6id(t);
    if (pending && *pending == m6id) {
        auto b = bundle(t, m6id);
        if (!b) throw StateError("PendingWithdrawalBundleUnknown");
        if (b->first.kind != WithdrawalBundleInfo::Known) throw StateError("PendingWithdrawalBundleUnknown");
        for (auto& [op, o] : b->first.bundle.spend_utxos) {
            diff.remove(nh(pointed_output_hash(op, o)));
            auto k = op.key();
            if (!t.del(T_UTXOS, ByteSpan(k.data(), k.size()))) throw StateError("NoUtxo " + op.to_string());
            Writer w;
            enc_spent(w, SpentOutput{o, InPoint{InPoint::Withdrawal, m6id, 0}});
            t.put(T_STXOS, ByteSpan(k.data(), k.size()), w.buf);
            ev.push_back(TwoWayPegEvent{TwoWayPegEvent::BundleSpend, op, {}, m6id});
        }
        if (b->second.latest().value != BundleStatus::Pending) throw StateError("assert Pending", true);
        if (!b->second.push(BundleStatus::Submitted, block_height)) throw StateError("push submitted", true);
        put_bundle(t, m6id, b->first, b->second);
        t.del(T_PENDING_BUNDLE, UNIT);
        return;
    }
    if (auto b = bundle(t, m6id)) {
        auto st = b->second.latest().value;
        auto kind = b->first.kind;
        if (st == BundleStatus::Confirmed) throw StateError("ConfirmedResubmitted");
        if (st == BundleStatus::Submitted || st == BundleStatus::SubmittedUnexpected) throw StateError("Resubmitted");
        if (st == BundleStatus::Dropped) {
            if (kind != WithdrawalBundleInfo::Known) throw StateError("UnknownDropped");
            // warn: dropped bundle submitted
        } else if (st == BundleStatus::Pending) {
            if (kind == WithdrawalBundleInfo::Known) throw StateError("DroppedPending");
            throw StateError("UnknownPending");
        } else if (st == BundleStatus::Failed) {
            if (kind == WithdrawalBundleInfo::UnknownConfirmed) throw StateError("UnknownConfirmedFailed");
            // warn: failed bundle resubmitted
        }
        if (!b->second.push(BundleStatus::SubmittedUnexpected, block_height)) throw StateError("push submitted unexpected", true);
        put_bundle(t, m6id, b->first, b->second);
        return;
    }
    // Unknown withdrawal bundle submitted
    WithdrawalBundleInfo info;
    info.kind = WithdrawalBundleInfo::Unknown;
    put_bundle(t, m6id, info, RollBack<BundleStatus>::make(BundleStatus::Submitted, block_height));
}

// two_way_peg_data.rs:282-402
void State::connect_bundle_confirmed(Txn& t, uint32_t block_height, AccumulatorDiff& diff, std::vector<TwoWayPegEvent>& ev, const Hash32& m6id) const
{
    auto b = bundle(t, m6id);
    if (!b) throw StateError("UnknownWithdrawalBundle");
    auto& [info, st] = *b;
    if (st.latest().value == BundleStatus::Confirmed) return; // already applied
    if (st.latest().value != BundleStatus::Submitted && st.latest().value != BundleStatus::SubmittedUnexpected)
        throw StateError("assert Submitted", true);
    if (info.kind == WithdrawalBundleInfo::UnknownConfirmed) throw StateError("UnknownWithdrawalBundleReconfirmed");
    if (info.kind == WithdrawalBundleInfo::Unknown) {
        if (block_height != 0) throw StateError("UnknownWithdrawalBundleConfirmed"); // halts the chain (bug §5.11)
        std::map<OutPoint, Output> all;
        for (auto& [op, o] : utxos(t)) all[op] = o;
        for (auto& [op, o] : all) {
            auto k = op.key();
            Writer w;
            enc_spent(w, SpentOutput{o, InPoint{InPoint::Withdrawal, m6id, 0}});
            t.put(T_STXOS, ByteSpan(k.data(), k.size()), w.buf);
            diff.remove(nh(pointed_output_hash(op, o)));
            ev.push_back(TwoWayPegEvent{TwoWayPegEvent::BundleSpend, op, {}, m6id});
        }
        t.clear(T_UTXOS);
        info.kind = WithdrawalBundleInfo::UnknownConfirmed;
        info.spend_utxos = std::move(all);
    } else if (info.kind == WithdrawalBundleInfo::Known) {
        if (st.latest().value == BundleStatus::SubmittedUnexpected) {
            for (auto& [op, o] : info.bundle.spend_utxos) {
                auto k = op.key();
                if (!t.del(T_UTXOS, ByteSpan(k.data(), k.size()))) throw StateError("UnexpectedWithdrawalBundleInsolvency");
                Writer w;
                enc_spent(w, SpentOutput{o, InPoint{InPoint::Withdrawal, m6id, 0}});
                t.put(T_STXOS, ByteSpan(k.data(), k.size()), w.buf);
                diff.remove(nh(pointed_output_hash(op, o)));
                ev.push_back(TwoWayPegEvent{TwoWayPegEvent::BundleSpend, op, {}, m6id});
            }
        }
    }
    if (!st.push(BundleStatus::Confirmed, block_height)) throw StateError("push confirmed", true);
    put_bundle(t, m6id, info, st);
}

// two_way_peg_data.rs:404-478
void State::connect_bundle_failed(Txn& t, uint32_t block_height, AccumulatorDiff& diff, std::vector<TwoWayPegEvent>& ev, const Hash32& m6id) const
{
    auto b = bundle(t, m6id);
    if (!b) throw StateError("UnknownWithdrawalBundle");
    auto& [info, st] = *b;
    if (st.latest().value == BundleStatus::Failed) return; // already applied
    if (st.latest().value != BundleStatus::Submitted && st.latest().value != BundleStatus::SubmittedUnexpected)
        throw StateError("assert Submitted", true);
    if (info.kind == WithdrawalBundleInfo::Known && st.latest().value != BundleStatus::SubmittedUnexpected) {
        for (auto& [op, o] : info.bundle.spend_utxos) {
            auto k = op.key();
            t.del(T_STXOS, ByteSpan(k.data(), k.size()));
            Writer w;
            enc_output(w, o);
            t.put(T_UTXOS, ByteSpan(k.data(), k.size()), w.buf);
            diff.insert(nh(pointed_output_hash(op, o)));
            ev.push_back(TwoWayPegEvent{TwoWayPegEvent::BundleReturn, op, o, m6id});
        }
        auto lf = latest_failed_rollback(t);
        RollBack<Hash32> nl;
        if (lf) {
            nl = *lf;
            if (!nl.push(m6id, block_height)) throw StateError("push latest failed", true);
        } else {
            nl = RollBack<Hash32>::make(m6id, block_height);
        }
        put_latest_failed(t, nl);
    }
    if (!st.push(BundleStatus::Failed, block_height)) throw StateError("push failed", true);
    put_bundle(t, m6id, info, st);
}

// two_way_peg_data.rs:560-667
void State::connect_2wpd(Txn& t, const TwoWayPegData& d) const
{
    auto ht = height(t);
    if (!ht) throw StateError("NoTip");
    uint32_t block_height = *ht;
    AccumulatorDiff diff;
    std::vector<TwoWayPegEvent> events;
    std::optional<Hash32> latest_deposit_block, latest_bundle_event_block;
    for (auto& [event_block, bi] : d.block_info) {
        for (auto& ev : bi.events) {
            if (auto dep = std::get_if<bt::mainchain::Deposit>(&ev)) {
                OutPoint op;
                op.kind = OutPoint::Deposit;
                op.txid = dep->txid;
                op.vout = dep->vout;
                Output o = deposit_output(*dep);
                auto k = op.key();
                Writer w;
                enc_output(w, o);
                t.put(T_UTXOS, ByteSpan(k.data(), k.size()), w.buf);
                diff.insert(nh(pointed_output_hash(op, o)));
                events.push_back(TwoWayPegEvent{TwoWayPegEvent::Deposit, op, o, {}});
                latest_deposit_block = event_block;
            } else {
                auto& we = std::get<bt::mainchain::WithdrawalBundleEvent>(ev);
                switch (we.status) {
                case bt::mainchain::BundleStatus::Submitted: connect_bundle_submitted(t, block_height, diff, events, we.m6id); break;
                case bt::mainchain::BundleStatus::Confirmed: connect_bundle_confirmed(t, block_height, diff, events, we.m6id); break;
                case bt::mainchain::BundleStatus::Failed: connect_bundle_failed(t, block_height, diff, events, we.m6id); break;
                }
                latest_bundle_event_block = event_block;
            }
        }
    }
    if (!events.empty()) put_twpd_events(t, block_height, events);
    if (latest_deposit_block) {
        auto l = seq_last(t, T_DEPOSIT_BLOCKS);
        uint32_t seq = l ? l->first + 1 : 0;
        t.put(T_DEPOSIT_BLOCKS, u32le(seq), enc_block_ref(*latest_deposit_block, block_height));
    }
    if (latest_bundle_event_block) {
        auto l = seq_last(t, T_BUNDLE_EVENT_BLOCKS);
        uint32_t seq = l ? l->first + 1 : 0;
        t.put(T_BUNDLE_EVENT_BLOCKS, u32le(seq), enc_block_ref(*latest_bundle_event_block, block_height));
    }
    auto lf = latest_failed_bundle(t);
    uint32_t last_failure_height = lf ? lf->first : 0;
    if (uint32_t(block_height - last_failure_height) >= WITHDRAWAL_BUNDLE_FAILURE_GAP && !pending_bundle_m6id(t)) {
        if (auto bundle_ = collect_withdrawal_bundle(t, block_height)) {
            Hash32 m6id = bundle_->m6id();
            t.put(T_PENDING_BUNDLE, UNIT, m6id.span());
            RollBack<BundleStatus> st;
            if (auto existing = bundle(t, m6id)) {
                st = existing->second;
                if (!st.push(BundleStatus::Pending, block_height)) throw StateError("push pending", true);
            } else {
                st = RollBack<BundleStatus>::make(BundleStatus::Pending, block_height);
            }
            WithdrawalBundleInfo info;
            info.kind = WithdrawalBundleInfo::Known;
            info.bundle = std::move(*bundle_);
            put_bundle(t, m6id, info, st);
        }
    }
    apply_diff(t, diff);
}

// two_way_peg_data.rs:669-737
void State::disconnect_bundle_submitted(Txn& t, uint32_t block_height, AccumulatorDiff& diff, const Hash32& m6id) const
{
    auto b = bundle(t, m6id);
    if (!b) {
        auto p = pending_bundle_m6id(t);
        if (p && *p == m6id) return; // already applied
        throw StateError("UnknownWithdrawalBundle");
    }
    auto [rest, latest] = b->second.pop();
    if (latest.value != BundleStatus::Submitted && latest.value != BundleStatus::SubmittedUnexpected) throw StateError("assert", true);
    if (latest.height != block_height) throw StateError("assert height", true);
    if (b->first.kind == WithdrawalBundleInfo::Known) {
        if (rest && rest->latest().value == BundleStatus::Pending) {
            auto& su = b->first.bundle.spend_utxos;
            for (auto it = su.rbegin(); it != su.rend(); ++it) {
                auto k = it->first.key();
                if (!t.del(T_STXOS, ByteSpan(k.data(), k.size()))) throw StateError("NoStxo");
                Writer w;
                enc_output(w, it->second);
                t.put(T_UTXOS, ByteSpan(k.data(), k.size()), w.buf);
                diff.insert(nh(pointed_output_hash(it->first, it->second)));
            }
            t.put(T_PENDING_BUNDLE, UNIT, m6id.span());
        }
    }
    if (rest) put_bundle(t, m6id, b->first, *rest);
    else t.del(T_BUNDLES, m6id_key(m6id));
}

// two_way_peg_data.rs:739-822
void State::disconnect_bundle_confirmed(Txn& t, uint32_t block_height, AccumulatorDiff& diff, const Hash32& m6id) const
{
    auto b = bundle(t, m6id);
    if (!b) throw StateError("UnknownWithdrawalBundle");
    auto info = b->first;
    auto [prev, latest] = b->second.pop();
    if (latest.value == BundleStatus::Submitted || latest.value == BundleStatus::SubmittedUnexpected) return; // already applied
    if (latest.value != BundleStatus::Confirmed) throw StateError("assert Confirmed", true);
    if (latest.height != block_height) throw StateError("assert height", true);
    if (!prev) throw StateError("Pop confirmed bundle status should be valid", true);
    auto pst = prev->latest().value;
    if (pst != BundleStatus::Submitted && pst != BundleStatus::SubmittedUnexpected) throw StateError("assert", true);
    if (info.kind == WithdrawalBundleInfo::Known) {
        if (pst == BundleStatus::SubmittedUnexpected) {
            for (auto& [op, o] : info.bundle.spend_utxos) {
                auto k = op.key();
                Writer w;
                enc_output(w, o);
                t.put(T_UTXOS, ByteSpan(k.data(), k.size()), w.buf);
                if (!t.del(T_STXOS, ByteSpan(k.data(), k.size()))) throw StateError("NoStxo");
                diff.insert(nh(pointed_output_hash(op, o)));
            }
        }
    } else if (info.kind == WithdrawalBundleInfo::UnknownConfirmed) {
        for (auto& [op, o] : info.spend_utxos) {
            auto k = op.key();
            Writer w;
            enc_output(w, o);
            t.put(T_UTXOS, ByteSpan(k.data(), k.size()), w.buf);
            if (!t.del(T_STXOS, ByteSpan(k.data(), k.size()))) throw StateError("NoStxo");
            diff.insert(nh(pointed_output_hash(op, o)));
        }
        info.kind = WithdrawalBundleInfo::Unknown;
        info.spend_utxos.clear();
    }
    put_bundle(t, m6id, info, *prev);
}

// two_way_peg_data.rs:824-910
void State::disconnect_bundle_failed(Txn& t, uint32_t block_height, AccumulatorDiff& diff, const Hash32& m6id) const
{
    auto b = bundle(t, m6id);
    if (!b) throw StateError("UnknownWithdrawalBundle");
    auto [prev, latest] = b->second.pop();
    if (latest.value == BundleStatus::Submitted) return; // already applied
    if (latest.value != BundleStatus::Failed) throw StateError("assert Failed", true);
    if (latest.height != block_height) throw StateError("assert height", true);
    if (!prev) throw StateError("Pop failed bundle status should be valid", true);
    auto pst = prev->latest().value;
    if (pst != BundleStatus::Submitted && pst != BundleStatus::SubmittedUnexpected) throw StateError("assert", true);
    if (b->first.kind == WithdrawalBundleInfo::Known && pst != BundleStatus::SubmittedUnexpected) {
        auto& su = b->first.bundle.spend_utxos;
        for (auto it = su.rbegin(); it != su.rend(); ++it) {
            auto k = it->first.key();
            Writer w;
            enc_spent(w, SpentOutput{it->second, InPoint{InPoint::Withdrawal, m6id, 0}});
            t.put(T_STXOS, ByteSpan(k.data(), k.size()), w.buf);
            if (!t.del(T_UTXOS, ByteSpan(k.data(), k.size()))) throw StateError("NoUtxo");
            diff.remove(nh(pointed_output_hash(it->first, it->second)));
        }
        auto lf = latest_failed_rollback(t);
        if (!lf) throw StateError("latest failed withdrawal bundle should exist", true);
        auto [prest, platest] = lf->pop();
        if (platest.value != m6id || platest.height != block_height) throw StateError("assert latest failed", true);
        if (prest) put_latest_failed(t, *prest);
        else t.del(T_LATEST_FAILED, UNIT);
    }
    put_bundle(t, m6id, b->first, *prev);
}

// two_way_peg_data.rs:1010-1117
void State::disconnect_2wpd(Txn& t, const TwoWayPegData& d) const
{
    auto ht = height(t);
    if (!ht) throw StateError("Height should not be None", true);
    uint32_t block_height = *ht;
    AccumulatorDiff diff;
    std::optional<Hash32> latest_deposit_block, latest_bundle_event_block;
    t.del(T_TWPD_EVENTS, u32le(block_height));
    for (auto bit = d.block_info.rbegin(); bit != d.block_info.rend(); ++bit) {
        auto& [event_block, bi] = *bit;
        for (auto eit = bi.events.rbegin(); eit != bi.events.rend(); ++eit) {
            if (auto dep = std::get_if<bt::mainchain::Deposit>(&*eit)) {
                OutPoint op;
                op.kind = OutPoint::Deposit;
                op.txid = dep->txid;
                op.vout = dep->vout;
                Output o = deposit_output(*dep);
                auto k = op.key();
                if (!t.del(T_UTXOS, ByteSpan(k.data(), k.size()))) throw StateError("NoUtxo " + op.to_string());
                diff.remove(nh(pointed_output_hash(op, o)));
                if (!latest_deposit_block) latest_deposit_block = event_block;
            } else {
                auto& we = std::get<bt::mainchain::WithdrawalBundleEvent>(*eit);
                switch (we.status) {
                case bt::mainchain::BundleStatus::Submitted: disconnect_bundle_submitted(t, block_height, diff, we.m6id); break;
                case bt::mainchain::BundleStatus::Confirmed: disconnect_bundle_confirmed(t, block_height, diff, we.m6id); break;
                case bt::mainchain::BundleStatus::Failed: disconnect_bundle_failed(t, block_height, diff, we.m6id); break;
                }
                if (!latest_bundle_event_block) latest_bundle_event_block = event_block;
            }
        }
    }
    if (latest_bundle_event_block) {
        auto l = seq_last(t, T_BUNDLE_EVENT_BLOCKS);
        if (!l) throw StateError("NoWithdrawalBundleEventBlock");
        if (l->second.first != *latest_bundle_event_block || l->second.second != block_height) throw StateError("assert bundle event block", true);
        if (!t.del(T_BUNDLE_EVENT_BLOCKS, u32le(l->first))) throw StateError("NoWithdrawalBundleEventBlock");
    }
    auto lf = latest_failed_bundle(t);
    uint32_t last_failure_height = lf ? lf->first : 0;
    if (uint32_t(block_height - last_failure_height) >= WITHDRAWAL_BUNDLE_FAILURE_GAP) {
        if (auto p = pending_bundle_m6id(t)) {
            auto b = bundle(t, *p);
            if (!b) throw StateError("PendingWithdrawalBundleUnknown");
            if (b->second.latest().height == block_height) {
                t.del(T_PENDING_BUNDLE, UNIT);
                auto [rest, _] = b->second.pop();
                if (rest) put_bundle(t, *p, b->first, *rest);
                else t.del(T_BUNDLES, m6id_key(*p));
            }
        }
    }
    if (latest_deposit_block) {
        auto l = seq_last(t, T_DEPOSIT_BLOCKS);
        if (!l) throw StateError("NoDepositBlock");
        if (l->second.first != *latest_deposit_block || l->second.second != block_height) throw StateError("assert deposit block", true);
        if (!t.del(T_DEPOSIT_BLOCKS, u32le(l->first))) throw StateError("NoDepositBlock");
    }
    apply_diff(t, diff);
}

} // namespace hivemind
