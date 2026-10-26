// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// lib/state/block.rs (prevalidate, connect_prevalidated, disconnect_tip) and the
// transaction checks of lib/state/mod.rs (fill_transaction, validate_filled_transaction,
// validate_transaction). Branch order follows theirs, so every accept/reject decision and
// every accumulator insertion order is the same.
#include "state_internal.h"

#include <hivemind/math.h>

#include <algorithm>

namespace hivemind {

using bt::kv::Txn;
using bt::utreexo::NodeHash;

static NodeHash nh(const Hash32& h) { return NodeHash::some(h); }

static Bytes u32le(uint32_t v)
{
    Writer w;
    w.u32(v);
    return w.buf;
}
static const Bytes UNIT{};

/** txid(); borsh refuses NaN f64 there (the block is invalid). */
static Hash32 txid_or_panic(const Transaction& tx)
{
    try {
        return tx.txid();
    } catch (const bt::DecodeError& e) {
        throw StateError(std::string("failed to serialize with borsh to compute a hash: ") + e.what(), true);
    }
}

FilledTransaction State::fill(const Txn& t, const std::map<OutPoint, Output>& unconfirmed, const Transaction& tx) const
{
    FilledTransaction ft{&tx, {}, std::nullopt};
    for (auto& [op, _] : tx.inputs) {
        if (auto u = utxo(t, op)) {
            ft.spent_utxos.push_back(*u);
            continue;
        }
        auto it = unconfirmed.find(op);
        if (it == unconfirmed.end()) throw StateError("utxo " + op.to_string() + " doesn't exist");
        ft.spent_utxos.push_back(it->second);
    }
    return ft;
}

// mod.rs:620-662
uint64_t State::validate_filled(const Txn& t, const FilledTransaction& ft, const Archive& archive, std::optional<uint32_t> override_height) const
{
    // validate_utxo_hashes
    for (size_t i = 0; i < ft.tx->inputs.size(); i++) {
        auto& [op, h] = ft.tx->inputs[i];
        Hash32 computed = pointed_output_hash(op, ft.spent_utxos[i]);
        if (computed != h)
            throw StateError("Computed Utxo hash (" + bt::hex_encode(computed) + ") for input (" + op.to_string() + ") does not match input hash (" +
                             bt::hex_encode(h) + ")");
    }
    uint64_t in = 0, out = 0;
    for (size_t i = 0; i < ft.tx->inputs.size(); i++) {
        auto& u = ft.spent_utxos[i];
        if (u.content.kind == Content::Withdrawal)
            throw StateError("withdrawal output " + ft.tx->inputs[i].first.to_string() + " cannot be spent by a transaction");
        uint64_t v = u.get_value();
        if (in + v < in) throw StateError("amount overflow");
        in += v;
    }
    if (auto fee = validate_market_transaction(*this, t, ft, archive, override_height)) return *fee;
    for (auto& o : ft.tx->outputs) {
        uint64_t v = o.get_value();
        if (out + v < out) throw StateError("amount overflow");
        out += v;
    }
    if (out > in) throw StateError("value in is less than value out");
    return in - out;
}

// mod.rs:664-705
uint64_t State::validate_transaction(const Txn& t, const std::map<OutPoint, Output>& unconfirmed, const AuthorizedTransaction& tx,
                                     const Archive& archive) const
{
    if (tx.transaction.has_nan()) throw StateError("failed to serialize with borsh to compute a hash", true);
    FilledTransaction ft = fill(t, unconfirmed, tx.transaction);
    if (tx.actor_proof) ft.actor_address = address_from_vk(tx.actor_proof->verifying_key);
    for (size_t i = 0; i < tx.authorizations.size() && i < ft.spent_utxos.size(); i++)
        if (address_from_vk(tx.authorizations[i].verifying_key) != ft.spent_utxos[i].address)
            throw StateError("wrong public key for address");
    if (!verify_transaction(tx)) throw StateError("failed to verify authorization");
    return validate_filled(t, ft, archive, std::nullopt);
}

// block.rs:17-202
PrevalidatedBlock State::prevalidate(const Txn& t, const Header& h, const Body& body, const Archive& archive) const
{
    auto tip_hash = tip(t);
    if (h.prev_side_hash != tip_hash) throw StateError("invalid header: prev side hash");
    auto ht = height(t);
    uint32_t next_height = ht ? *ht + 1 : 0;
    if (body.authorizations.size() > body_sigops_limit(next_height)) throw StateError("too many sigops");
    size_t body_size;
    try {
        body_size = borsh_bytes(body).size();
    } catch (const bt::DecodeError& e) {
        throw StateError(std::string("borsh serialization error: ") + e.what());
    }
    if (body_size > body_size_limit(next_height)) throw StateError("body too large");

    auto acc = accumulator(t);
    std::vector<std::array<uint8_t, 37>> all_input_keys;
    std::vector<std::pair<bool, NodeHash>> diff_txs;
    PrevalidatedBlock pre;
    pre.next_height = next_height;
    uint64_t total_fees = 0;
    std::map<OutPoint, Output> body_outputs; // outputs of earlier txs of this body
    for (size_t tx_idx = 0; tx_idx < body.transactions.size(); tx_idx++) {
        auto& tx = body.transactions[tx_idx];
        Hash32 txid = txid_or_panic(tx);
        FilledTransaction ft{&tx, {}, std::nullopt};
        std::vector<NodeHash> spent_hashes;
        for (auto& [op, uh] : tx.inputs) {
            if (auto u = utxo(t, op)) {
                ft.spent_utxos.push_back(*u);
                spent_hashes.push_back(nh(uh));
            } else {
                auto it = body_outputs.find(op);
                if (it == body_outputs.end()) throw StateError("utxo " + op.to_string() + " doesn't exist");
                ft.spent_utxos.push_back(it->second);
            }
            all_input_keys.push_back(op.key());
            diff_txs.emplace_back(false, nh(uh));
        }
        for (uint32_t vout = 0; vout < tx.outputs.size(); vout++)
            diff_txs.emplace_back(true, nh(pointed_output_hash(OutPoint::regular(txid, vout), tx.outputs[vout])));
        for (uint32_t vout = 0; vout < tx.outputs.size(); vout++) body_outputs[OutPoint::regular(txid, vout)] = tx.outputs[vout];
        auto ok = acc.verify(tx.proof, spent_hashes);
        if (!ok) throw StateError("utreexo error");
        if (!*ok) throw StateError("Utreexo proof verification failed for tx " + bt::hex_encode(txid));
        if (tx_idx < body.actor_proofs.size() && body.actor_proofs[tx_idx])
            ft.actor_address = address_from_vk(body.actor_proofs[tx_idx]->verifying_key);
        uint64_t fee = validate_filled(t, ft, archive, next_height);
        if (total_fees + fee < total_fees) throw StateError("amount overflow");
        total_fees += fee;
        pre.filled.push_back(std::move(ft));
    }
    check_duplicate_decision_claims(body.transactions);
    auto mr = compute_merkle_root(body.coinbase, pre.filled);
    if (!mr) throw StateError("failed to compute merkle root");
    if (*mr != h.merkle_root)
        throw StateError("invalid body: expected merkle root " + bt::hex_encode(h.merkle_root) + ", but computed " + bt::hex_encode(*mr));
    std::sort(all_input_keys.begin(), all_input_keys.end());
    for (size_t i = 1; i < all_input_keys.size(); i++)
        if (all_input_keys[i] == all_input_keys[i - 1]) throw StateError("utxo double spent");
    Hash32 cb_txid = h.coinbase_txid();
    uint64_t cb_value = 0;
    for (uint32_t vout = 0; vout < body.coinbase.outputs.size(); vout++) {
        auto& o = body.coinbase.outputs[vout];
        if (cb_value + o.get_value() < cb_value) throw StateError("amount overflow");
        cb_value += o.get_value();
        pre.diff.insert(nh(pointed_output_hash(OutPoint::coinbase(cb_txid, vout), o)));
    }
    for (auto& [ins, x] : diff_txs) {
        if (ins) pre.diff.insert(x);
        else pre.diff.remove(x);
    }
    if (cb_value > total_fees) throw StateError("total fees less than coinbase value");
    size_t k = 0;
    for (auto& ft : pre.filled)
        for (auto& u : ft.spent_utxos) {
            if (k >= body.authorizations.size()) break; // zip stops at the shorter
            if (address_from_vk(body.authorizations[k].verifying_key) != u.address) throw StateError("wrong public key for address");
            k++;
        }
    if (!verify_body(body)) throw StateError("failed to verify authorization");
    std::vector<NodeHash> ins, dels;
    pre.diff.split(ins, dels);
    if (auto e = acc.modify(ins, dels)) throw StateError("utreexo error: " + *e);
    if (acc.get_roots() != h.roots) throw StateError("Computed Utreexo roots do not match the header roots");
    pre.merkle_root = *mr;
    pre.total_fees = total_fees;
    pre.coinbase_value = cb_value;
    return pre;
}

// block.rs:204-335
void State::connect_prevalidated(Txn& t, const Header& h, const Body& body, PrevalidatedBlock pre, uint64_t mainchain_timestamp) const
{
    if (tip(t) != h.prev_side_hash) throw StateError("invalid header: prev side hash");
    if (pre.merkle_root != h.merkle_root) throw StateError("invalid body");
    ConnectContext ctxt = connect_begin(*this, t, body, pre.next_height, mainchain_timestamp);
    Hash32 cb_txid = h.coinbase_txid();
    for (uint32_t vout = 0; vout < body.coinbase.outputs.size(); vout++) {
        auto k = OutPoint::coinbase(cb_txid, vout).key();
        Writer w;
        bincode(w, body.coinbase.outputs[vout]);
        t.put(T_UTXOS, ByteSpan(k.data(), k.size()), w.buf);
    }
    ConnectTxs mtx = connect_txs(*this, t, ctxt, pre.filled, pre.coinbase_value);
    AccumulatorDiff diff = std::move(pre.diff);
    for (size_t idx = 0; idx < pre.filled.size(); idx++) {
        auto& ft = pre.filled[idx];
        if (mtx.skipped.count(idx)) {
            unapply_skipped_tx_diff(diff, *ft.tx);
            continue;
        }
        Hash32 txid = ft.tx->txid();
        for (uint32_t vin = 0; vin < ft.tx->inputs.size(); vin++) {
            auto& op = ft.tx->inputs[vin].first;
            auto u = utxo(t, op);
            if (!u) throw StateError("utxo " + op.to_string() + " doesn't exist");
            auto k = op.key();
            t.del(T_UTXOS, ByteSpan(k.data(), k.size()));
            Writer w;
            enc_spent(w, SpentOutput{*u, InPoint{InPoint::Regular, txid, vin}});
            t.put(T_STXOS, ByteSpan(k.data(), k.size()), w.buf);
        }
        for (uint32_t vout = 0; vout < ft.tx->outputs.size(); vout++) {
            auto k = OutPoint::regular(txid, vout).key();
            Writer w;
            bincode(w, ft.tx->outputs[vout]);
            t.put(T_UTXOS, ByteSpan(k.data(), k.size()), w.buf);
        }
    }
    connect_end(*this, t, diff, ctxt, std::move(mtx));
    Hash32 bh = h.hash();
    t.put(T_TIP, UNIT, bh.span());
    t.put(T_HEIGHT, UNIT, u32le(pre.next_height));
    apply_diff(t, diff);
}

// block.rs:638-785
void State::disconnect_tip(Txn& t, const Header& h, const Body& body) const
{
    auto tip_hash = tip(t);
    if (!tip_hash) throw StateError("no tip");
    if (*tip_hash != h.hash()) throw StateError("invalid header: block hash");
    AccumulatorDiff diff;
    auto ht = height(t);
    if (!ht) throw StateError("Height should not be None", true);
    uint32_t height_ = *ht;
    auto skipped = disconnect_begin(*this, t, diff, height_);
    std::vector<TradeShareDelta> deltas;
    for (size_t i = body.transactions.size(); i-- > 0;) {
        if (skipped.count(uint32_t(i))) continue;
        auto& tx = body.transactions[i];
        disconnect_tx(*this, t, diff, tx, deltas);
        Hash32 txid = tx.txid();
        for (size_t vout = tx.outputs.size(); vout-- > 0;) {
            OutPoint op = OutPoint::regular(txid, uint32_t(vout));
            diff.remove(nh(pointed_output_hash(op, tx.outputs[vout])));
            auto k = op.key();
            if (!t.del(T_UTXOS, ByteSpan(k.data(), k.size()))) throw StateError("utxo " + op.to_string() + " doesn't exist");
        }
        for (auto in = tx.inputs.rbegin(); in != tx.inputs.rend(); ++in) {
            auto k = in->first.key();
            auto s = stxo(t, in->first);
            if (!s) throw StateError("stxo " + in->first.to_string() + " doesn't exist");
            diff.insert(nh(in->second));
            t.del(T_STXOS, ByteSpan(k.data(), k.size()));
            Writer w;
            bincode(w, s->output);
            t.put(T_UTXOS, ByteSpan(k.data(), k.size()), w.buf);
        }
    }
    Hash32 cb_txid = h.coinbase_txid();
    for (size_t vout = body.coinbase.outputs.size(); vout-- > 0;) {
        OutPoint op = OutPoint::coinbase(cb_txid, uint32_t(vout));
        diff.remove(nh(pointed_output_hash(op, body.coinbase.outputs[vout])));
        auto k = op.key();
        if (!t.del(T_UTXOS, ByteSpan(k.data(), k.size()))) throw StateError("utxo " + op.to_string() + " doesn't exist");
    }
    disconnect_end(*this, t, height_, skipped, deltas);
    if (!h.prev_side_hash && height_ == 0) {
        t.del(T_TIP, UNIT);
        t.del(T_HEIGHT, UNIT);
    } else if (!h.prev_side_hash || height_ == 0) {
        throw StateError("no tip");
    } else {
        t.put(T_TIP, UNIT, h.prev_side_hash->span());
        t.put(T_HEIGHT, UNIT, u32le(height_ - 1));
    }
    // Their accumulator update here is overwritten by the archived snapshot of the new tip
    // (net_task.rs disconnect_tip_); the caller restores it, we still apply it for parity.
    apply_diff(t, diff);
}

} // namespace hivemind
