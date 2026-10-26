// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <hivemind/mempool.h>

#include <deque>

namespace hivemind {

std::vector<std::array<uint8_t, 3>> claimed_decision_ids(const Transaction& tx)
{
    std::vector<std::array<uint8_t, 3>> r;
    if (!tx.data) return r;
    if (auto* p = std::get_if<ClaimDecisionPayload>(&*tx.data))
        for (auto& e : p->decisions) r.push_back(e.decision_id_bytes);
    if (auto* m = std::get_if<TxCreateMarket>(&*tx.data))
        for (auto& c : m->new_claims)
            for (auto& e : c.decisions) r.push_back(e.decision_id_bytes);
    return r;
}

static std::optional<Hash32> parent_txid(const OutPoint& op)
{
    if (op.kind == OutPoint::Regular) return op.txid;
    return std::nullopt;
}

static bool is_trade(const AuthorizedTransaction& tx) { return tx.transaction.data && std::holds_alternative<TxTrade>(*tx.transaction.data); }

std::set<Hash32> Mempool::ancestors(const Transaction& tx) const
{
    std::set<Hash32> found;
    std::deque<Hash32> pending;
    for (auto& [op, _] : tx.inputs)
        if (auto p = parent_txid(op)) pending.push_back(*p);
    while (!pending.empty()) {
        Hash32 id = pending.front();
        pending.pop_front();
        if (found.count(id)) continue;
        auto it = txs_.find(id);
        if (it == txs_.end()) continue;
        found.insert(id);
        for (auto& [op, _] : it->second.transaction.inputs)
            if (auto p = parent_txid(op)) pending.push_back(*p);
    }
    return found;
}

void Mempool::put(const AuthorizedTransaction& tx)
{
    auto a = ancestors(tx.transaction);
    if (a.size() >= MAX_UNCONFIRMED_ANCESTORS)
        throw MempoolError(MempoolError::TooManyAncestors, "can't add transaction, it has " + std::to_string(a.size()) +
                                                               " unconfirmed ancestors and the limit is 25");
    insert(tx);
}

void Mempool::put_disconnected(const AuthorizedTransaction& tx) { insert(tx); }

void Mempool::insert(const AuthorizedTransaction& tx)
{
    Hash32 txid = tx.transaction.txid();
    auto claims = claimed_decision_ids(tx.transaction);
    for (auto& d : claims) {
        auto it = claims_.find(d);
        if (it != claims_.end() && it->second != txid)
            throw MempoolError(MempoolError::DecisionClaimed,
                               "can't add transaction, decision " + bt::hex_encode(ByteSpan(d.data(), 3)) + " already claimed in mempool");
    }
    // Their insert runs in one LMDB write transaction that is dropped on any error, so a
    // refused transaction leaves nothing behind. Check everything first, then write.
    for (auto& [op, _] : tx.transaction.inputs)
        if (spent_.count(op)) throw MempoolError(MempoolError::DoubleSpent, "can't add transaction, utxo double spent");
    {
        std::set<OutPoint> seen; // a transaction spending one outpoint twice
        for (auto& [op, _] : tx.transaction.inputs)
            if (!seen.insert(op).second) throw MempoolError(MempoolError::DoubleSpent, "can't add transaction, utxo double spent");
    }
    for (auto& d : claims) claims_[d] = txid;
    for (auto& [op, _] : tx.transaction.inputs) spent_[op] = txid;
    txs_[txid] = tx;
    if (is_trade(tx)) {
        uint64_t next = ++trade_counter_;
        std::array<uint8_t, 8> k{};
        for (int i = 0; i < 8; i++) k[i] = uint8_t(next >> (8 * i));
        trade_order_[k] = txid;
    }
}

static void drop_claims(std::map<std::array<uint8_t, 3>, Hash32>& claims, const Transaction& tx)
{
    // delete_decision_claims deletes by decision id; only this tx can hold them (insert
    // refuses a second claimant), so erase the ones it holds
    Hash32 id = tx.txid();
    for (auto& d : claimed_decision_ids(tx))
        if (auto it = claims.find(d); it != claims.end() && it->second == id) claims.erase(it);
}

void Mempool::remove(const Hash32& txid)
{
    std::deque<Hash32> q{txid};
    while (!q.empty()) {
        Hash32 id = q.front();
        q.pop_front();
        auto it = txs_.find(id);
        if (it == txs_.end()) continue;
        auto tx = it->second;
        for (auto& [op, _] : tx.transaction.inputs) spent_.erase(op);
        drop_claims(claims_, tx.transaction);
        if (is_trade(tx))
            for (auto t = trade_order_.begin(); t != trade_order_.end(); ++t)
                if (t->second == id) {
                    trade_order_.erase(t);
                    break;
                }
        txs_.erase(it);
        for (uint32_t v = 0; v < tx.transaction.outputs.size(); v++) {
            auto s = spent_.find(OutPoint::regular(id, v));
            if (s != spent_.end()) q.push_back(s->second);
        }
    }
}

void Mempool::remove_confirmed(const Hash32& txid)
{
    auto it = txs_.find(txid);
    if (it == txs_.end()) return;
    auto tx = it->second;
    for (auto& [op, _] : tx.transaction.inputs) spent_.erase(op);
    drop_claims(claims_, tx.transaction);
    if (is_trade(tx))
        for (auto t = trade_order_.begin(); t != trade_order_.end(); ++t)
            if (t->second == txid) {
                trade_order_.erase(t);
                break;
            }
    txs_.erase(it);
}

std::vector<Hash32> Mempool::evict_decision_claim_conflicts(const Transaction& tx)
{
    Hash32 confirmed = tx.txid();
    std::vector<Hash32> ev;
    for (auto& d : claimed_decision_ids(tx)) {
        auto it = claims_.find(d);
        if (it != claims_.end() && it->second != confirmed) {
            Hash32 z = it->second;
            if (!txs_.count(z)) { // a claim without its transaction: drop it
                claims_.erase(it);
                continue;
            }
            remove(z);
            ev.push_back(z);
        }
    }
    return ev;
}

std::optional<Hash32> Mempool::spender(const OutPoint& op) const
{
    auto it = spent_.find(op);
    if (it == spent_.end()) return std::nullopt;
    return it->second;
}

std::map<OutPoint, Output> Mempool::unconfirmed_outputs(const Transaction& tx) const
{
    std::map<OutPoint, Output> r;
    for (auto& [op, _] : tx.inputs) {
        if (op.kind != OutPoint::Regular) continue;
        auto it = txs_.find(op.txid);
        if (it == txs_.end()) continue;
        if (op.vout >= it->second.transaction.outputs.size()) continue;
        r[op] = it->second.transaction.outputs[op.vout];
    }
    return r;
}

std::vector<AuthorizedTransaction> Mempool::topological(std::optional<size_t> limit) const
{
    std::vector<AuthorizedTransaction> order;
    std::set<Hash32> placed;
    for (auto& [txid, _] : txs_) {
        if (limit && order.size() >= *limit) break;
        std::vector<Hash32> stack{txid};
        while (!stack.empty()) {
            if (limit && order.size() >= *limit) break;
            Hash32 id = stack.back();
            if (placed.count(id)) {
                stack.pop_back();
                continue;
            }
            auto it = txs_.find(id);
            if (it == txs_.end()) {
                stack.pop_back();
                continue;
            }
            std::optional<Hash32> parent;
            for (auto& [op, __] : it->second.transaction.inputs)
                if (auto p = parent_txid(op); p && txs_.count(*p) && !placed.count(*p)) {
                    parent = p;
                    break;
                }
            if (parent) {
                stack.push_back(*parent);
            } else {
                placed.insert(id);
                order.push_back(it->second);
                stack.pop_back();
            }
        }
    }
    return order;
}

std::vector<AuthorizedTransaction> Mempool::trades_ordered() const
{
    std::vector<AuthorizedTransaction> r;
    for (auto& [_, id] : trade_order_)
        if (auto it = txs_.find(id); it != txs_.end()) r.push_back(it->second);
    return r;
}

std::map<OutPoint, Output> Mempool::own_unconfirmed_utxos(const std::set<Address>& addresses, const std::set<OutPoint>& confirmed) const
{
    std::map<OutPoint, Output> res;
    std::set<OutPoint> trusted;
    for (auto& tx : topological(std::nullopt)) {
        Hash32 txid = tx.transaction.txid();
        bool ok = true;
        for (auto& [op, _] : tx.transaction.inputs)
            if (!confirmed.count(op) && !trusted.count(op)) ok = false;
        if (!ok) continue;
        for (uint32_t v = 0; v < tx.transaction.outputs.size(); v++) {
            auto& o = tx.transaction.outputs[v];
            if (!addresses.count(o.address)) continue;
            OutPoint op = OutPoint::regular(txid, v);
            trusted.insert(op);
            if (!spent_.count(op)) res[op] = o;
        }
    }
    return res;
}

std::map<OutPoint, Output> Mempool::unconfirmed_utxos(const Address& a) const
{
    std::map<OutPoint, Output> r;
    for (auto& [txid, tx] : txs_) {
        bool relevant = false;
        for (auto& o : tx.transaction.outputs)
            if (o.address == a) relevant = true;
        for (auto& au : tx.authorizations)
            if (address_from_vk(au.verifying_key) == a) relevant = true;
        if (tx.actor_proof && address_from_vk(tx.actor_proof->verifying_key) == a) relevant = true;
        if (!relevant) continue;
        for (uint32_t v = 0; v < tx.transaction.outputs.size(); v++)
            if (tx.transaction.outputs[v].address == a) r[OutPoint::regular(txid, v)] = tx.transaction.outputs[v];
    }
    return r;
}

std::set<std::array<uint8_t, 3>> Mempool::pending_decision_claim_ids() const
{
    std::set<std::array<uint8_t, 3>> r;
    for (auto& [d, _] : claims_) r.insert(d);
    return r;
}

std::optional<AuthorizedTransaction> Mempool::get(const Hash32& txid) const
{
    auto it = txs_.find(txid);
    if (it == txs_.end()) return std::nullopt;
    return it->second;
}

void Mempool::regenerate_proofs(const bt::utreexo::MemForest& acc)
{
    std::vector<Hash32> ids;
    for (auto& [id, _] : txs_) ids.push_back(id);
    for (auto& id : ids) {
        auto it = txs_.find(id);
        if (it == txs_.end()) continue;
        auto unconf = unconfirmed_outputs(it->second.transaction);
        std::vector<bt::utreexo::NodeHash> targets;
        for (auto& [op, uh] : it->second.transaction.inputs)
            if (!unconf.count(op)) targets.push_back(bt::utreexo::NodeHash::some(uh));
        auto p = acc.prove(targets);
        if (p) it->second.transaction.proof = *p;
        else remove(id);
    }
}

} // namespace hivemind
