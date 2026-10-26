// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Internal interfaces between the state translation units (not installed).
#pragma once

#include <hivemind/chain.h>
#include <hivemind/state.h>

#include <set>

namespace hivemind {

// state.cpp
void put_outcome(bt::kv::Txn& t, const DecisionOutcome& o);
void put_stats(bt::kv::Txn& t, const VotingPeriodStats& s);
void put_twpd_events(bt::kv::Txn& t, uint32_t height, const std::vector<TwoWayPegEvent>& ev);
Bytes pricing_bytes(const PeriodPricing& p);
PeriodPricing pricing_from(ByteSpan b);

// validation.cpp (lib/validation)
/** Value in minus value out (validation::tx_fee). */
uint64_t tx_fee(const FilledTransaction& ft);
/** validation::miner_fee: TRADE_MINER_FEE_SATS for trades and AmplifyBeta, else tx_fee. */
uint64_t miner_fee(const FilledTransaction& ft);
/** validate_market_transaction: the miner fee of a trade/AmplifyBeta, nullopt otherwise. */
std::optional<uint64_t> validate_market_transaction(const State& s, const bt::kv::Txn& t, const FilledTransaction& ft, const Archive& a,
                                                    std::optional<uint32_t> override_height);
void check_duplicate_decision_claims(const std::vector<Transaction>& txs);
void validate_coinbase_outputs(const std::vector<Output>& outs);
void validate_fees(uint64_t coinbase_value, const std::vector<FilledTransaction>& txs, const std::set<size_t>& skipped);
/** MarketValidator::validate_trade (used by the trade simulation too). */
void validate_trade(const State& s, const Archive& a, const bt::kv::Txn& t, const FilledTransaction& ft);

// decisions.cpp (decisions/database.rs block-time parts)
void snapshot_period_pricing(const State& s, bt::kv::Txn& t, uint32_t h);
void restore_period_pricing_undo(const State& s, bt::kv::Txn& t, uint32_t h);
void mint_genesis(const State& s, bt::kv::Txn& t, uint64_t ts, uint32_t h, uint64_t genesis_ts);
void process_block_pricing(const State& s, bt::kv::Txn& t, uint32_t h, uint64_t ts, uint64_t genesis_ts);
void delete_periods_above(const State& s, bt::kv::Txn& t, uint32_t max_period);
void rollback_decision_states_to_height(const State& s, bt::kv::Txn& t, uint32_t h);
void transition_decision_to_voting(const State& s, bt::kv::Txn& t, const DecisionId& id, uint32_t h);
void transition_decision_to_resolved(const State& s, bt::kv::Txn& t, const DecisionId& id, uint32_t h, double outcome);
std::vector<DecisionId> claimed_decisions_needing_voting(const State& s, const bt::kv::Txn& t, uint32_t current_period);
std::vector<DecisionId> voting_decisions_needing_resolution(const State& s, const bt::kv::Txn& t, uint32_t current_period);

// voting.cpp (voting/mod.rs)
/** calculate_and_store_consensus: the undo entry when consensus was computed and committed. */
std::optional<ConsensusUndoEntry> calculate_and_store_consensus(const State& s, bt::kv::Txn& t, uint32_t voting_period,
                                                                const std::vector<DecisionId>& decision_ids, uint64_t ts, uint32_t h);
void revert_consensus(const State& s, bt::kv::Txn& t, const std::vector<ConsensusUndoEntry>& undo);

// settlement.cpp (markets/database.rs settlement, payouts.rs)
std::vector<SettlementUndoEntry> transition_and_payout_resolved_markets(const State& s, bt::kv::Txn& t, AccumulatorDiff& diff, uint32_t h);
void revert_settlement(const State& s, bt::kv::Txn& t, AccumulatorDiff& diff, const std::vector<SettlementUndoEntry>& undo, uint32_t h);

// market_block.cpp (markets/block.rs)
struct ConnectContext {
    uint32_t height = 0;
    uint64_t mainchain_timestamp = 0;
    uint64_t genesis_ts = 0;
    uint32_t current_period = 0;
};
struct StateUpdate; // opaque
struct ConnectTxs {
    std::shared_ptr<StateUpdate> update;
    std::set<size_t> skipped;
};
ConnectContext connect_begin(const State& s, bt::kv::Txn& t, const Body& b, uint32_t h, uint64_t mainchain_timestamp);
ConnectTxs connect_txs(const State& s, bt::kv::Txn& t, const ConnectContext& c, const std::vector<FilledTransaction>& txs, uint64_t coinbase_value);
void unapply_skipped_tx_diff(AccumulatorDiff& d, const Transaction& tx);
void connect_end(const State& s, bt::kv::Txn& t, AccumulatorDiff& d, const ConnectContext& c, ConnectTxs txs);
std::set<uint32_t> disconnect_begin(const State& s, bt::kv::Txn& t, AccumulatorDiff& d, uint32_t h);
struct TradeShareDelta {
    Address address;
    MarketId market_id;
    uint32_t outcome_index = 0;
    int64_t share_delta = 0;
};
void disconnect_tx(const State& s, bt::kv::Txn& t, AccumulatorDiff& d, const Transaction& tx, std::vector<TradeShareDelta>& deltas);
void disconnect_end(const State& s, bt::kv::Txn& t, uint32_t h, const std::set<uint32_t>& skipped, const std::vector<TradeShareDelta>& deltas);

// undo encodings (undo.cpp)
Bytes enc_settlement_undo(const std::vector<SettlementUndoEntry>& v);
std::vector<SettlementUndoEntry> dec_settlement_undo(ByteSpan b);
Bytes enc_consensus_undo(const std::vector<ConsensusUndoEntry>& v);
std::vector<ConsensusUndoEntry> dec_consensus_undo(ByteSpan b);
Bytes enc_consolidation_undo(const ConsolidationUndoData& d);
ConsolidationUndoData dec_consolidation_undo(ByteSpan b);
Bytes enc_rep_undo(const std::vector<ReputationTransferUndoEntry>& v);
std::vector<ReputationTransferUndoEntry> dec_rep_undo(ByteSpan b);
Bytes enc_u32s(const std::vector<uint32_t>& v);
std::vector<uint32_t> dec_u32s(ByteSpan b);
Bytes u32_key(uint32_t v);

} // namespace hivemind
