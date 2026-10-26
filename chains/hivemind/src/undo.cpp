// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Encodings of the per-height undo records (lib/state/undo.rs). Internal to this node.
#include "state_internal.h"

namespace hivemind {

Bytes u32_key(uint32_t v)
{
    Writer w;
    w.u32(v);
    return w.buf;
}

static void enc_opt_op(Writer& w, const std::optional<OutPoint>& o)
{
    if (!o) return w.u8(0);
    w.u8(1);
    bincode(w, *o);
}
static std::optional<OutPoint> dec_opt_op(Reader& r)
{
    if (!r.u8()) return std::nullopt;
    return de_outpoint(r);
}
static void enc_ops(Writer& w, const std::vector<OutPoint>& v)
{
    w.u64(v.size());
    for (auto& o : v) bincode(w, o);
}
static std::vector<OutPoint> dec_ops(Reader& r)
{
    std::vector<OutPoint> v;
    uint64_t n = r.u64();
    for (uint64_t i = 0; i < n; i++) v.push_back(de_outpoint(r));
    return v;
}
static void enc_pairs(Writer& w, const std::vector<std::pair<OutPoint, Output>>& v)
{
    w.u64(v.size());
    for (auto& [op, o] : v) {
        bincode(w, op);
        bincode(w, o);
    }
}
static std::vector<std::pair<OutPoint, Output>> dec_pairs(Reader& r)
{
    std::vector<std::pair<OutPoint, Output>> v;
    uint64_t n = r.u64();
    for (uint64_t i = 0; i < n; i++) {
        OutPoint op = de_outpoint(r);
        v.emplace_back(op, de_output(r));
    }
    return v;
}
static void enc_opt_pair(Writer& w, const std::optional<std::pair<OutPoint, Output>>& p)
{
    if (!p) return w.u8(0);
    w.u8(1);
    bincode(w, p->first);
    bincode(w, p->second);
}
static std::optional<std::pair<OutPoint, Output>> dec_opt_pair(Reader& r)
{
    if (!r.u8()) return std::nullopt;
    OutPoint op = de_outpoint(r);
    return std::make_pair(op, de_output(r));
}
static void enc_rep_map(Writer& w, const std::map<Address, double>& m)
{
    w.u64(m.size());
    for (auto& [a, v] : m) {
        w.raw(a.b);
        bincode_f64(w, v);
    }
}
static std::map<Address, double> dec_rep_map(Reader& r)
{
    std::map<Address, double> m;
    uint64_t n = r.u64();
    for (uint64_t i = 0; i < n; i++) {
        Address a;
        a.b = r.array<20>();
        m[a] = de_f64(r);
    }
    return m;
}
static void enc_dids(Writer& w, const std::vector<DecisionId>& v)
{
    w.u64(v.size());
    for (auto& d : v) w.raw(ByteSpan(d.b.data(), 3));
}
static std::vector<DecisionId> dec_dids(Reader& r)
{
    std::vector<DecisionId> v;
    uint64_t n = r.u64();
    for (uint64_t i = 0; i < n; i++) {
        DecisionId d;
        auto a = r.array<3>();
        std::memcpy(d.b.data(), a.data(), 3);
        v.push_back(d);
    }
    return v;
}

static void enc_summary(Writer& w, const MarketPayoutSummary& s)
{
    w.raw(ByteSpan(s.market_id.b.data(), 6));
    w.u64(s.treasury_distributed);
    w.u64(s.total_fees_distributed);
    w.u32(s.shareholder_count);
    w.u64(s.payouts.size());
    for (auto& p : s.payouts) {
        w.raw(ByteSpan(p.market_id.b.data(), 6));
        w.raw(p.address.b);
        w.u32(p.outcome_index);
        w.u64(uint64_t(p.shares_redeemed));
        bincode_f64(w, p.final_price);
        w.u64(p.payout_sats);
    }
    w.u64(s.fee_payouts.size());
    for (auto& f : s.fee_payouts) {
        w.raw(f.address.b);
        w.u64(f.amount_sats);
        w.u32(uint32_t(f.fee_role));
    }
    if (s.creator_refund) {
        w.u8(1);
        w.raw(s.creator_refund->first.b);
        w.u64(s.creator_refund->second);
    } else {
        w.u8(0);
    }
    w.u32(s.block_height);
}
static MarketPayoutSummary dec_summary(Reader& r)
{
    MarketPayoutSummary s;
    auto m = r.array<6>();
    std::memcpy(s.market_id.b.data(), m.data(), 6);
    s.treasury_distributed = r.u64();
    s.total_fees_distributed = r.u64();
    s.shareholder_count = r.u32();
    uint64_t n = r.u64();
    for (uint64_t i = 0; i < n; i++) {
        SharePayoutRecord p;
        auto pm = r.array<6>();
        std::memcpy(p.market_id.b.data(), pm.data(), 6);
        p.address.b = r.array<20>();
        p.outcome_index = r.u32();
        p.shares_redeemed = int64_t(r.u64());
        p.final_price = de_f64(r);
        p.payout_sats = r.u64();
        s.payouts.push_back(p);
    }
    n = r.u64();
    for (uint64_t i = 0; i < n; i++) {
        FeePayoutRecord f;
        f.address.b = r.array<20>();
        f.amount_sats = r.u64();
        f.fee_role = FeeRole(r.u32());
        s.fee_payouts.push_back(f);
    }
    if (r.u8()) {
        Address a;
        a.b = r.array<20>();
        s.creator_refund = std::make_pair(a, r.u64());
    }
    s.block_height = r.u32();
    return s;
}

Bytes enc_settlement_undo(const std::vector<SettlementUndoEntry>& v)
{
    Writer w;
    w.u64(v.size());
    for (auto& e : v) {
        Writer mw;
        enc_market(mw, e.pre_settlement_market);
        w.bincode_bytes(mw.buf);
        enc_summary(w, e.payout_summary);
        enc_opt_pair(w, e.treasury_utxo);
        enc_opt_pair(w, e.fee_utxo);
    }
    return w.buf;
}
std::vector<SettlementUndoEntry> dec_settlement_undo(ByteSpan b)
{
    Reader r(b);
    std::vector<SettlementUndoEntry> v;
    uint64_t n = r.u64();
    for (uint64_t i = 0; i < n; i++) {
        SettlementUndoEntry e;
        Bytes mb = r.bincode_bytes();
        Reader mr(mb);
        e.pre_settlement_market = dec_market(mr);
        e.payout_summary = dec_summary(r);
        e.treasury_utxo = dec_opt_pair(r);
        e.fee_utxo = dec_opt_pair(r);
        v.push_back(std::move(e));
    }
    return v;
}

Bytes enc_consensus_undo(const std::vector<ConsensusUndoEntry>& v)
{
    Writer w;
    w.u64(v.size());
    for (auto& e : v) {
        w.u32(e.period_id);
        enc_dids(w, e.decision_outcome_ids);
        w.u8(e.had_period_stats);
        enc_dids(w, e.resolved_decision_ids);
        enc_rep_map(w, e.pre_consensus_reputation);
    }
    return w.buf;
}
std::vector<ConsensusUndoEntry> dec_consensus_undo(ByteSpan b)
{
    Reader r(b);
    std::vector<ConsensusUndoEntry> v;
    uint64_t n = r.u64();
    for (uint64_t i = 0; i < n; i++) {
        ConsensusUndoEntry e;
        e.period_id = r.u32();
        e.decision_outcome_ids = dec_dids(r);
        e.had_period_stats = r.u8();
        e.resolved_decision_ids = dec_dids(r);
        e.pre_consensus_reputation = dec_rep_map(r);
        v.push_back(std::move(e));
    }
    return v;
}

Bytes enc_consolidation_undo(const ConsolidationUndoData& d)
{
    Writer w;
    w.u64(d.entries.size());
    for (auto& e : d.entries) {
        w.raw(ByteSpan(e.market_id.b.data(), 6));
        enc_pairs(w, e.old_treasury_utxos);
        enc_pairs(w, e.old_fee_utxos);
        enc_opt_op(w, e.old_treasury_pointer);
        enc_opt_op(w, e.old_fee_pointer);
        enc_opt_op(w, e.new_treasury_utxo);
        enc_opt_op(w, e.new_fee_utxo);
        enc_ops(w, e.sell_payout_utxos);
        enc_ops(w, e.buy_change_utxos);
    }
    enc_ops(w, d.sell_input_change_utxos);
    return w.buf;
}
ConsolidationUndoData dec_consolidation_undo(ByteSpan b)
{
    Reader r(b);
    ConsolidationUndoData d;
    uint64_t n = r.u64();
    for (uint64_t i = 0; i < n; i++) {
        ConsolidationUndoEntry e;
        auto m = r.array<6>();
        std::memcpy(e.market_id.b.data(), m.data(), 6);
        e.old_treasury_utxos = dec_pairs(r);
        e.old_fee_utxos = dec_pairs(r);
        e.old_treasury_pointer = dec_opt_op(r);
        e.old_fee_pointer = dec_opt_op(r);
        e.new_treasury_utxo = dec_opt_op(r);
        e.new_fee_utxo = dec_opt_op(r);
        e.sell_payout_utxos = dec_ops(r);
        e.buy_change_utxos = dec_ops(r);
        d.entries.push_back(std::move(e));
    }
    d.sell_input_change_utxos = dec_ops(r);
    return d;
}

Bytes enc_rep_undo(const std::vector<ReputationTransferUndoEntry>& v)
{
    Writer w;
    w.u64(v.size());
    for (auto& e : v) {
        w.raw(e.sender.b);
        bincode_f64(w, e.sender_pre);
        w.raw(e.receiver.b);
        bincode_f64(w, e.receiver_pre);
    }
    return w.buf;
}
std::vector<ReputationTransferUndoEntry> dec_rep_undo(ByteSpan b)
{
    Reader r(b);
    std::vector<ReputationTransferUndoEntry> v;
    uint64_t n = r.u64();
    for (uint64_t i = 0; i < n; i++) {
        ReputationTransferUndoEntry e;
        e.sender.b = r.array<20>();
        e.sender_pre = de_f64(r);
        e.receiver.b = r.array<20>();
        e.receiver_pre = de_f64(r);
        v.push_back(e);
    }
    return v;
}

Bytes enc_u32s(const std::vector<uint32_t>& v)
{
    Writer w;
    w.u64(v.size());
    for (auto x : v) w.u32(x);
    return w.buf;
}
std::vector<uint32_t> dec_u32s(ByteSpan b)
{
    Reader r(b);
    std::vector<uint32_t> v;
    uint64_t n = r.u64();
    for (uint64_t i = 0; i < n; i++) v.push_back(r.u32());
    return v;
}

} // namespace hivemind
