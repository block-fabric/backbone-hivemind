// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// lib/mempool.rs (truthcoin-dc v0.20.0): transactions keyed by txid (iterated in txid
// byte order, as their LMDB table), one spender per outpoint, unconfirmed parents (a chain
// of at most 25 ancestors), pending decision claims (one claimant per decision id), and
// trades in insertion order (counter keys little-endian under memcmp order, as theirs).
#pragma once

#include <hivemind/types.h>

#include <map>
#include <set>
#include <stdexcept>

namespace hivemind {

struct MempoolError : std::runtime_error {
    enum Kind { DoubleSpent, DecisionClaimed, TooManyAncestors } kind;
    MempoolError(Kind k, const std::string& m) : std::runtime_error(m), kind(k) {}
};

class Mempool {
public:
    static constexpr size_t MAX_UNCONFIRMED_ANCESTORS = 25; // mempool.rs:24

    /** put: refuses a chain with too many unconfirmed ancestors, then insert. */
    void put(const AuthorizedTransaction& tx);
    /** put_disconnected: a transaction of a disconnected block (no ancestor limit). */
    void put_disconnected(const AuthorizedTransaction& tx);
    /** delete with descendants. */
    void remove(const Hash32& txid);
    /** delete_confirmed: keep the children. */
    void remove_confirmed(const Hash32& txid);
    /** evict_decision_claim_conflicts: the mempool claimants of decisions `tx` claims. */
    std::vector<Hash32> evict_decision_claim_conflicts(const Transaction& tx);
    std::optional<Hash32> spender(const OutPoint& op) const;
    std::set<Hash32> ancestors(const Transaction& tx) const;
    /** outputs of mempool parents that `tx` spends */
    std::map<OutPoint, Output> unconfirmed_outputs(const Transaction& tx) const;
    /** parents before children; `limit` caps the walk (closed under parents) */
    std::vector<AuthorizedTransaction> topological(std::optional<size_t> limit) const;
    std::vector<AuthorizedTransaction> trades_ordered() const;
    /** own_unconfirmed_utxos (mempool.rs:598-651) */
    std::map<OutPoint, Output> own_unconfirmed_utxos(const std::set<Address>& addresses, const std::set<OutPoint>& confirmed) const;
    /** outputs to `a` of mempool txs relevant to `a` (get_unconfirmed_utxos) */
    std::map<OutPoint, Output> unconfirmed_utxos(const Address& a) const;
    std::set<std::array<uint8_t, 3>> pending_decision_claim_ids() const;
    const std::map<Hash32, AuthorizedTransaction>& all() const { return txs_; }
    std::optional<AuthorizedTransaction> get(const Hash32& txid) const;
    /** regenerate_proofs: evict what the accumulator cannot prove. */
    void regenerate_proofs(const bt::utreexo::MemForest& acc);
    size_t size() const { return txs_.size(); }

private:
    void insert(const AuthorizedTransaction& tx);
    std::map<Hash32, AuthorizedTransaction> txs_;
    std::map<OutPoint, Hash32> spent_;
    std::map<std::array<uint8_t, 3>, Hash32> claims_;
    std::map<std::array<uint8_t, 8>, Hash32> trade_order_; // u64 little-endian keys
    uint64_t trade_counter_ = 0;
};

/** The decision ids a transaction claims (ClaimDecision and CreateMarket new claims). */
std::vector<std::array<uint8_t, 3>> claimed_decision_ids(const Transaction& tx);

} // namespace hivemind
