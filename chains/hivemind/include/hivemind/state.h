// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Truthcoin state machine: a literal port of truthcoin-dc v0.20.0 lib/state (mod.rs,
// block.rs, two_way_peg_data.rs, reputation.rs, decisions/, markets/, voting/, undo.rs)
// and lib/validation over our ordered KV store. Tables, key encodings and iteration order
// mirror their LMDB databases where the order is observable (bincode keys under memcmp
// order: u32 little-endian, addresses and ids as raw bytes). Every f64 step that is not
// plain IEEE arithmetic runs in their own compiled math (hivemind/math.h).
#pragma once

#include <hivemind/chain.h>
#include <hivemind/types.h>

#include <bt/kv.h>
#include <bt/mainchain.h>

#include <univalue.h>

#include <list>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace hivemind {

constexpr uint32_t WITHDRAWAL_BUNDLE_FAILURE_GAP = 4; // lib/state/mod.rs:53

/** A consensus failure. `panic` marks paths where their node panics instead of erroring;
 *  the network cannot connect such a block either, so we reject it too. The message is
 *  their Display text where it reaches an RPC reply. */
struct StateError : std::runtime_error {
    bool panic = false;
    explicit StateError(const std::string& m, bool p = false) : std::runtime_error(m), panic(p) {}
};
/** Error::InvalidTransaction { reason } */
StateError invalid_tx(const std::string& reason);

enum class BundleStatus : uint8_t { Confirmed = 0, Dropped = 1, Failed = 2, Pending = 3, Submitted = 4, SubmittedUnexpected = 5 };
const char* to_string(BundleStatus s);

template <class T>
struct HeightStamped {
    T value;
    uint32_t height;
};
/** lib/state/rollback.rs: NonEmpty<HeightStamped<T>> */
template <class T>
struct RollBack {
    std::vector<HeightStamped<T>> v; // never empty
    static RollBack make(T x, uint32_t h) { return RollBack{{{x, h}}}; }
    const HeightStamped<T>& latest() const { return v.back(); }
    bool push(T x, uint32_t h)
    {
        if (v.back().height > h) return false;
        v.push_back({x, h});
        return true;
    }
    std::pair<std::optional<RollBack>, HeightStamped<T>> pop() const
    {
        if (v.size() > 1) {
            RollBack r = *this;
            auto last = r.v.back();
            r.v.pop_back();
            return {r, last};
        }
        return {std::nullopt, v.front()};
    }
};

/** types/lib.rs WithdrawalBundle */
struct WithdrawalBundle {
    std::map<OutPoint, Output> spend_utxos; // BTreeMap order
    std::vector<bt::btc::TxOut> outputs;     // full tx outputs: [fee, commitment, payouts...]
    Bytes tx_bytes() const { return bt::btc::serialize_inputless_tx(2, outputs, 0); }
    Hash32 m6id() const { return bt::sha256d(tx_bytes()); }
};
std::optional<WithdrawalBundle> make_withdrawal_bundle(uint32_t block_height, uint64_t fee, std::map<OutPoint, Output> spend_utxos,
                                                       std::vector<bt::btc::TxOut> payouts);
bool predict_weight_ok(uint32_t n_outputs, uint32_t sum_txout_sizes);

struct WithdrawalBundleInfo {
    enum Kind : uint8_t { Known = 0, Unknown = 1, UnknownConfirmed = 2 } kind = Unknown;
    WithdrawalBundle bundle;                // Known
    std::map<OutPoint, Output> spend_utxos; // UnknownConfirmed
};

/** types/state.rs TwoWayPegEvent (RPC get_two_way_peg_events) */
struct TwoWayPegEvent {
    enum Kind : uint8_t { Deposit = 0, BundleSpend = 1, BundleReturn = 2 } kind = Deposit;
    OutPoint outpoint;
    Output output; // Deposit, BundleReturn
    Hash32 m6id;   // BundleSpend, BundleReturn
};

/** Insertion-ordered set of +/- leaf changes (types/lib.rs AccumulatorDiff, LinkedHashMap). */
class AccumulatorDiff {
public:
    void insert(const bt::utreexo::NodeHash& h);
    void remove(const bt::utreexo::NodeHash& h);
    void split(std::vector<bt::utreexo::NodeHash>& ins, std::vector<bt::utreexo::NodeHash>& dels) const;
    bool empty() const { return order.empty(); }

private:
    std::list<std::pair<bt::utreexo::NodeHash, bool>> order;
    std::unordered_map<bt::utreexo::NodeHash, std::list<std::pair<bt::utreexo::NodeHash, bool>>::iterator, bt::utreexo::NodeHashHasher> idx;
};

// ------------------------------------------------------------------ decisions (lib/state/decisions)
enum class DecisionState : uint8_t { Created = 0, Claimed = 1, Voting = 2, Resolved = 3, Invalid = 4 };
const char* to_string(DecisionState s);

struct Decision {
    bt::Hash20 market_maker_pubkey_hash;
    DecisionType decision_type;
    std::string header, description;
    std::optional<std::string> option_0_label, option_1_label;
    std::vector<std::string> tags;
    /** Decision::new: nullopt and the error (kind InvalidDecisionId / InvalidScaled) in *err. */
    static std::optional<Decision> make(const bt::Hash20& maker, DecisionType t, std::string header, std::string description,
                                        std::optional<std::string> l0, std::optional<std::string> l1, std::vector<std::string> tags,
                                        std::string* err);
    bool is_scaled() const { return decision_type.kind == DecisionType::Scaled; }
    bool is_categorical() const { return decision_type.kind == DecisionType::Category; }
    std::optional<size_t> option_count() const;
    std::optional<uint16_t> inconclusive_index() const;
    std::pair<std::string, std::string> binary_labels() const;
    /** normalize_value / validate_and_normalize (types.rs). */
    double normalize_value(double v) const;
    double denormalize_value(double v) const;
};
int compare(const Decision& a, const Decision& b); // derived Ord

struct DecisionEntry {
    DecisionId decision_id;
    std::optional<Decision> decision;
    Hash32 claiming_txid;
};
bool operator<(const DecisionEntry& a, const DecisionEntry& b); // derived Ord

struct DecisionStateHistory {
    struct Item {
        DecisionState state;
        uint32_t height;
        std::optional<double> outcome;
    };
    std::vector<Item> state_history;
    DecisionState current_state() const { return state_history.empty() ? DecisionState::Created : state_history.back().state; }
    std::optional<double> consensus_outcome() const;
    std::optional<uint32_t> state_height(DecisionState s) const;
};

struct PeriodPricing {
    uint64_t p_period = 0, p_floor = 0, mints = 0, claimed = 0;
    uint32_t last_reprice_block = 0;
    uint64_t window_open_claimed = 0, window_open_mints = 0;
    static PeriodPricing seeded(uint64_t p_seed, uint32_t h);
    uint64_t period_capacity() const { return mints * 5 * 5; }
};

struct DecisionConfig {
    bool is_blocks = false;
    uint32_t quantity = 86400; // DecisionConfig::default (decisions/types.rs:407-414): one day
};
constexpr uint32_t FUTURE_PERIODS = 20;
constexpr uint64_t INITIAL_DECISIONS_PER_PERIOD = 500;
constexpr uint64_t GENESIS_P_PERIOD_SATS = 10'000;
uint32_t get_current_period(uint64_t ts, std::optional<uint32_t> height, uint64_t genesis_ts, const DecisionConfig& c);
std::pair<uint64_t, uint64_t> period_boundaries(uint32_t period, const DecisionConfig& c, uint64_t genesis_ts);
std::string period_to_name(uint32_t period);
uint32_t reprice_interval(const DecisionConfig& c);
/** math::decisions fee_for_index; nullopt with ListingFeeError Display in *err. */
std::optional<uint64_t> fee_for_index(uint64_t p_period, uint64_t mints, uint32_t decision_index, std::string* err);
bool slot_unlocked(uint32_t decision_index, uint64_t mints);
uint64_t slot_price(uint64_t p_period, size_t tier);

// ------------------------------------------------------------------ markets (lib/state/markets)
enum class MarketState : uint8_t { Trading = 0, Cancelled = 1, Invalid = 2, Settled = 3 }; // serde variant index
const char* to_string(MarketState s);
bool can_transition(MarketState from, MarketState to);

struct Market {
    MarketId id;
    std::string title, description;
    std::vector<std::string> tags;
    Address creator_address;
    std::vector<DimensionSpec> dimension_specs;
    std::vector<DecisionId> decision_ids;
    std::vector<std::vector<uint64_t>> state_combos; // usize
    uint32_t created_at_height = 0;
    std::optional<uint32_t> expires_at_height;
    uint8_t tau_from_now = 5;
    uint64_t storage_fee_sats = 0;
    MarketState market_state = MarketState::Trading;
    double trading_fee = 0.005;
    uint64_t liquidity_base_sats = 0;
    std::vector<int64_t> shares;
    std::vector<double> final_prices;
    uint64_t version = 0;
    uint32_t last_updated_height = 0;
    uint64_t total_volume_sats = 0;
    std::vector<uint64_t> outcome_volumes_sats;
    uint8_t tx_pow_hash_selector = 0, tx_pow_ordering = 0, tx_pow_difficulty = 0;

    /** (full combo index, combo) of the tradeable combos (abstain states excluded). */
    std::vector<std::pair<size_t, const std::vector<uint64_t>*>> valid_state_combos() const;
    /** Market::calculate_final_prices (market.rs:505-596). */
    std::vector<double> calculate_final_prices(const std::map<DecisionId, double>& outcomes, const std::map<DecisionId, Decision>& decisions) const;
    bool txpow_enabled() const { return tx_pow_difficulty > 0; }
};
MarketId compute_market_id(const std::string& title, const std::string& description, const Address& creator, const std::vector<DimensionSpec>& dims);
Address market_treasury_address(const MarketId& m);
Address market_author_fee_address(const MarketId& m);
OutPoint share_payout_outpoint(const MarketId& m, const Address& a, uint32_t height, uint32_t seq);
OutPoint sell_payout_outpoint(const MarketId& m, const Address& a, const Hash32& txid);
OutPoint buy_change_outpoint(const MarketId& m, const Address& a, const Hash32& txid);
OutPoint sell_input_change_outpoint(const Address& a, const Hash32& txid);
/** MarketBuilder::build: nullopt and the MarketError Display in *err. */
std::optional<Market> build_market(const std::string& title, const std::string& description, const std::vector<std::string>& tags,
                                   const Address& creator, const std::vector<DimensionSpec>& dims, double trading_fee, uint8_t sel,
                                   uint8_t ord, uint8_t diff, uint32_t created_at, const std::map<DecisionId, Decision>& decisions,
                                   std::string* err);
std::optional<std::vector<std::vector<uint64_t>>> generate_state_combos(const std::vector<DimensionSpec>& dims,
                                                                        const std::map<DecisionId, Decision>& decisions, std::string* err);

struct ShareAccount {
    std::map<std::pair<MarketId, uint32_t>, int64_t> positions;
    uint32_t last_updated_height = 0;
};

struct SharePayoutRecord {
    MarketId market_id;
    Address address;
    uint32_t outcome_index = 0;
    int64_t shares_redeemed = 0;
    double final_price = 0;
    uint64_t payout_sats = 0;
};
enum class FeeRole : uint8_t { CorrectVoter = 0, DecisionAuthor = 1, MarketAuthor = 2 };
struct FeePayoutRecord {
    Address address;
    uint64_t amount_sats = 0;
    FeeRole fee_role = FeeRole::CorrectVoter;
};
struct MarketPayoutSummary {
    MarketId market_id;
    uint64_t treasury_distributed = 0, total_fees_distributed = 0;
    uint32_t shareholder_count = 0;
    std::vector<SharePayoutRecord> payouts;
    std::vector<FeePayoutRecord> fee_payouts;
    std::optional<std::pair<Address, uint64_t>> creator_refund;
    uint32_t block_height = 0;
};

// ------------------------------------------------------------------ voting (lib/state/voting)
struct VoteValue {
    enum Kind : uint8_t { Binary = 0, Scalar = 1, Categorical = 2, Abstain = 3 } kind = Abstain;
    bool b = false;
    double s = 0;
    uint16_t c = 0;
    std::optional<double> to_float() const;
};
/** VoteValidator::convert_vote_value_with_decision */
VoteValue convert_vote_value(double v, const Decision* d);
struct VoteKey {
    uint32_t period_id = 0;
    Address voter;
    DecisionId decision_id;
};
struct VoteEntry {
    VoteValue value;
    uint64_t timestamp = 0;
    uint32_t block_height = 0;
};
struct DecisionOutcome {
    DecisionId decision_id;
    uint32_t period_id = 0;
    double outcome_value = 0, min = 0, max = 1, confidence = 0;
    uint64_t total_votes = 0;
    double total_reputation_weight = 0;
    uint64_t finalized_at = 0;
    uint32_t block_height = 0;
    bool is_consensus = true;
    // DecisionResolution
    uint8_t res_status = 0; // Pending
    uint64_t res_changed_at = 0;
    uint32_t res_changed_height = 0, res_vote_count = 0, res_min_votes = 0;
    uint64_t res_deadline = 0;
    bool res_outcome_ready = true;
    std::optional<std::optional<uint16_t>> categorical_winner;
};
struct VotingPeriodStats {
    uint32_t period_id = 0;
    uint64_t total_voters = 0, total_votes = 0, total_decisions = 0;
    double avg_participation_rate = 0, total_reputation_weight = 0;
    uint64_t consensus_decisions = 0, calculated_at = 0;
    std::optional<std::vector<double>> first_loading;
    std::optional<double> certainty;
    std::optional<std::map<Address, std::pair<double, double>>> reputation_changes;
};

// ------------------------------------------------------------------ undo (lib/state/undo.rs)
struct SettlementUndoEntry {
    Market pre_settlement_market;
    MarketPayoutSummary payout_summary;
    std::optional<std::pair<OutPoint, Output>> treasury_utxo, fee_utxo;
};
struct ConsensusUndoEntry {
    uint32_t period_id = 0;
    std::vector<DecisionId> decision_outcome_ids;
    bool had_period_stats = false;
    std::vector<DecisionId> resolved_decision_ids;
    std::map<Address, double> pre_consensus_reputation;
};
struct ReputationTransferUndoEntry {
    Address sender;
    double sender_pre = 0;
    Address receiver;
    double receiver_pre = 0;
};
struct ConsolidationUndoEntry {
    MarketId market_id;
    std::vector<std::pair<OutPoint, Output>> old_treasury_utxos, old_fee_utxos;
    std::optional<OutPoint> old_treasury_pointer, old_fee_pointer, new_treasury_utxo, new_fee_utxo;
    std::vector<OutPoint> sell_payout_utxos, buy_change_utxos;
};
struct ConsolidationUndoData {
    std::vector<ConsolidationUndoEntry> entries;
    std::vector<OutPoint> sell_input_change_utxos;
};

struct PrevalidatedBlock {
    std::vector<FilledTransaction> filled;
    Hash32 merkle_root;
    uint64_t total_fees = 0;
    uint64_t coinbase_value = 0;
    uint32_t next_height = 0;
    AccumulatorDiff diff;
};



class State {
public:
    explicit State(DecisionConfig cfg = {}) : cfg_(cfg) {}

    // table ids in the KV store (archive uses 40+)
    enum Table : uint8_t {
        T_TIP = 1, T_HEIGHT, T_UTXOS, T_STXOS, T_PENDING_BUNDLE, T_LATEST_FAILED, T_BUNDLES, T_DEPOSIT_BLOCKS, T_BUNDLE_EVENT_BLOCKS,
        T_ACCUMULATOR, T_TWPD_EVENTS, T_MAIN_TS, T_GENESIS_TS, T_REPUTATION,
        T_PERIOD_DECISIONS, T_DECISION_HISTORIES, T_PERIOD_PRICING, T_PERIOD_PRICING_UNDO,
        T_MARKETS, T_MARKETS_BY_STATE, T_MARKETS_BY_EXPIRY, T_MARKETS_BY_DECISION, T_SHARE_ACCOUNTS, T_MARKET_FUNDS, T_MEMPOOL_SHARES,
        T_VOTES, T_DECISION_OUTCOMES, T_PERIOD_STATS,
        T_SETTLEMENT_UNDO, T_CONSENSUS_UNDO, T_CONSOLIDATION_UNDO, T_MINTING_UNDO, T_REP_TRANSFER_UNDO, T_SKIPPED_UNDO
    };

    const DecisionConfig& config() const { return cfg_; }

    // ---- core getters
    std::optional<Hash32> tip(const bt::kv::Txn& t) const;
    std::optional<uint32_t> height(const bt::kv::Txn& t) const;
    std::optional<Output> utxo(const bt::kv::Txn& t, const OutPoint& op) const;
    std::optional<SpentOutput> stxo(const bt::kv::Txn& t, const OutPoint& op) const;
    std::vector<std::pair<OutPoint, Output>> utxos(const bt::kv::Txn& t) const;
    std::vector<std::pair<OutPoint, SpentOutput>> stxos(const bt::kv::Txn& t) const;
    bt::utreexo::MemForest accumulator(const bt::kv::Txn& t) const;
    void put_accumulator(bt::kv::Txn& t, const bt::utreexo::MemForest& f) const;
    std::optional<Hash32> pending_bundle_m6id(const bt::kv::Txn& t) const;
    std::optional<std::pair<WithdrawalBundleInfo, RollBack<BundleStatus>>> bundle(const bt::kv::Txn& t, const Hash32& m6id) const;
    std::vector<std::pair<Hash32, std::pair<WithdrawalBundleInfo, RollBack<BundleStatus>>>> bundles(const bt::kv::Txn& t) const;
    std::optional<std::pair<WithdrawalBundle, uint32_t>> pending_bundle(const bt::kv::Txn& t) const;
    std::optional<std::pair<uint32_t, Hash32>> latest_failed_bundle(const bt::kv::Txn& t) const;
    std::optional<RollBack<Hash32>> latest_failed_rollback(const bt::kv::Txn& t) const;
    std::vector<std::pair<uint32_t, std::pair<Hash32, uint32_t>>> deposit_blocks(const bt::kv::Txn& t) const;
    std::vector<std::pair<uint32_t, std::pair<Hash32, uint32_t>>> bundle_event_blocks(const bt::kv::Txn& t) const;
    std::vector<TwoWayPegEvent> two_way_peg_events(const bt::kv::Txn& t, uint32_t height) const;
    uint64_t sidechain_wealth(const bt::kv::Txn& t) const;
    std::optional<uint64_t> mainchain_timestamp(const bt::kv::Txn& t) const;
    std::optional<uint64_t> genesis_timestamp(const bt::kv::Txn& t) const;
    /** (current ts or 0, height, genesis ts or 0) — State::period_context */
    std::tuple<uint64_t, std::optional<uint32_t>, uint64_t> period_context(const bt::kv::Txn& t) const;

    // ---- reputation (reputation.rs)
    double reputation(const bt::kv::Txn& t, const Address& a) const;
    std::map<Address, double> all_reputations(const bt::kv::Txn& t) const; // rep > 0
    void set_reputation(bt::kv::Txn& t, const Address& a, double rep) const;
    void clear_and_restore_reputation(bt::kv::Txn& t, const std::map<Address, double>& snap) const;

    // ---- decisions (decisions/database.rs)
    std::optional<DecisionEntry> decision_entry(const bt::kv::Txn& t, const DecisionId& id) const;
    std::vector<DecisionEntry> period_decisions(const bt::kv::Txn& t, uint32_t period) const;
    std::vector<std::pair<uint32_t, std::vector<DecisionEntry>>> all_period_decisions(const bt::kv::Txn& t) const;
    std::optional<DecisionStateHistory> decision_history(const bt::kv::Txn& t, const DecisionId& id) const;
    std::vector<std::pair<DecisionId, DecisionStateHistory>> all_decision_histories(const bt::kv::Txn& t) const;
    DecisionState decision_state(const bt::kv::Txn& t, const DecisionId& id) const;
    std::optional<PeriodPricing> period_pricing(const bt::kv::Txn& t, uint32_t period) const;
    std::optional<uint32_t> highest_minted_period(const bt::kv::Txn& t) const;
    uint64_t fee_for_decision_id(const bt::kv::Txn& t, const DecisionId& id) const;
    /** validate_decision_claim (database.rs:575-665). */
    void validate_decision_claim(const bt::kv::Txn& t, const DecisionId& id, uint64_t ts, std::optional<uint32_t> h, uint64_t genesis_ts) const;
    uint64_t standard_claimed_count(const bt::kv::Txn& t, uint32_t period) const;
    std::set<DecisionId> claimed_ids_for_period(const bt::kv::Txn& t, uint32_t period) const;
    std::vector<DecisionId> available_decisions_in_period(const bt::kv::Txn& t, uint32_t period) const;
    uint64_t total_for(uint32_t period, uint64_t ts, std::optional<uint32_t> h, uint64_t genesis_ts) const;

    // ---- markets (markets/database.rs)
    std::optional<Market> market(const bt::kv::Txn& t, const MarketId& id) const;
    std::vector<Market> all_markets(const bt::kv::Txn& t) const;
    std::vector<Market> markets_by_state(const bt::kv::Txn& t, MarketState s) const;
    std::optional<ShareAccount> share_account(const bt::kv::Txn& t, const Address& a) const;
    std::vector<std::pair<Address, ShareAccount>> all_share_accounts(const bt::kv::Txn& t) const;
    std::optional<OutPoint> market_funds_utxo(const bt::kv::Txn& t, const MarketId& id, bool is_fee) const;
    uint64_t market_funds_sats(const bt::kv::Txn& t, const MarketId& id, bool is_fee) const;
    std::optional<std::vector<int64_t>> mempool_shares(const bt::kv::Txn& t, const MarketId& id) const;
    void put_mempool_shares(bt::kv::Txn& t, const MarketId& id, const std::vector<int64_t>& s) const;
    void clear_mempool_shares(bt::kv::Txn& t, const MarketId& id) const;
    std::vector<MarketId> markets_by_decision(const bt::kv::Txn& t, const DecisionId& d) const;

    // ---- voting (voting/database.rs)
    std::optional<VoteEntry> vote(const bt::kv::Txn& t, uint32_t period, const Address& voter, const DecisionId& d) const;
    std::vector<std::pair<VoteKey, VoteEntry>> votes_for_period(const bt::kv::Txn& t, uint32_t period) const;
    std::vector<std::pair<VoteKey, VoteEntry>> all_votes(const bt::kv::Txn& t) const;
    std::optional<DecisionOutcome> decision_outcome(const bt::kv::Txn& t, const DecisionId& d) const;
    std::vector<DecisionOutcome> outcomes_for_period(const bt::kv::Txn& t, uint32_t period) const;
    std::optional<VotingPeriodStats> period_stats(const bt::kv::Txn& t, uint32_t period) const;

    // ---- validation / connection (block.rs)
    PrevalidatedBlock prevalidate(const bt::kv::Txn& t, const Header& h, const Body& b, const Archive& archive) const;
    void connect_prevalidated(bt::kv::Txn& t, const Header& h, const Body& b, PrevalidatedBlock pre, uint64_t mainchain_timestamp) const;
    void disconnect_tip(bt::kv::Txn& t, const Header& h, const Body& b) const;
    /** State::validate_transaction (mempool): fill from utxos + unconfirmed, keys, signatures,
     *  then validate_filled_transaction. Returns the fee. */
    uint64_t validate_transaction(const bt::kv::Txn& t, const std::map<OutPoint, Output>& unconfirmed, const AuthorizedTransaction& tx,
                                  const Archive& archive) const;
    /** validate_filled_transaction (mod.rs:620-662). */
    uint64_t validate_filled(const bt::kv::Txn& t, const FilledTransaction& ft, const Archive& archive, std::optional<uint32_t> override_height) const;
    /** Fill from the utxo table, then `unconfirmed`; throws NoUtxo. */
    FilledTransaction fill(const bt::kv::Txn& t, const std::map<OutPoint, Output>& unconfirmed, const Transaction& tx) const;

    // ---- two-way peg (two_way_peg_data.rs)
    void connect_2wpd(bt::kv::Txn& t, const bt::mainchain::TwoWayPegData& d) const;
    void disconnect_2wpd(bt::kv::Txn& t, const bt::mainchain::TwoWayPegData& d) const;
    std::optional<WithdrawalBundle> collect_withdrawal_bundle(const bt::kv::Txn& t, uint32_t block_height) const;
    /** proto.rs: deposit address = s13_ form or bare base58 of the OP_RETURN bytes, else ALL_ZEROS. */
    static Output deposit_output(const bt::mainchain::Deposit& d);

    /** Replay summary, the same JSON as tests/oracle's `hm_oracle replay` digest. */
    UniValue digest(const bt::kv::Txn& t) const;

    // ---- utxo writes that record their leaf (markets/utxos.rs)
    void insert_utxo(bt::kv::Txn& t, const OutPoint& op, const Output& o, AccumulatorDiff& d) const;
    bool delete_utxo(bt::kv::Txn& t, const OutPoint& op, AccumulatorDiff& d) const;

    // internals shared by the translation units
    void put_bundle(bt::kv::Txn& t, const Hash32& m6id, const WithdrawalBundleInfo& info, const RollBack<BundleStatus>& st) const;
    void apply_diff(bt::kv::Txn& t, const AccumulatorDiff& d) const;
    void put_market(bt::kv::Txn& t, const Market& m) const;
    void add_market(bt::kv::Txn& t, const Market& m) const;
    void delete_market(bt::kv::Txn& t, const MarketId& id) const;
    void update_market(bt::kv::Txn& t, const Market& m) const;  // validates the state transition
    void restore_market(bt::kv::Txn& t, const Market& m) const; // no transition check
    void set_market_funds_utxo(bt::kv::Txn& t, const MarketId& id, bool is_fee, const OutPoint& op) const;
    void clear_market_funds_utxo(bt::kv::Txn& t, const MarketId& id, bool is_fee) const;
    void add_shares(bt::kv::Txn& t, const Address& a, const MarketId& m, uint32_t outcome, int64_t shares, uint32_t h) const;
    void remove_shares(bt::kv::Txn& t, const Address& a, const MarketId& m, uint32_t outcome, int64_t shares, uint32_t h) const;
    void verify_share_invariant(const bt::kv::Txn& t, const MarketId& m) const;
    void put_vote(bt::kv::Txn& t, const VoteKey& k, const VoteEntry& e) const;
    bool delete_vote(bt::kv::Txn& t, uint32_t period, const Address& voter, const DecisionId& d) const;
    void claim_decision(bt::kv::Txn& t, const DecisionId& id, const Decision& d, const Hash32& txid, std::optional<uint32_t> h) const;
    void revert_decision_claim(bt::kv::Txn& t, const DecisionId& id) const;
    void put_decision_history(bt::kv::Txn& t, const DecisionId& id, const DecisionStateHistory& h) const;
    void put_period_pricing(bt::kv::Txn& t, uint32_t period, const PeriodPricing& p) const;
    void put_period_decisions(bt::kv::Txn& t, uint32_t period, const std::vector<DecisionEntry>& set) const;

private:
    DecisionConfig cfg_;
    // two_way_peg_data.rs helpers
    void connect_bundle_submitted(bt::kv::Txn& t, uint32_t h, AccumulatorDiff& diff, std::vector<TwoWayPegEvent>& ev, const Hash32& m6id) const;
    void connect_bundle_confirmed(bt::kv::Txn& t, uint32_t h, AccumulatorDiff& diff, std::vector<TwoWayPegEvent>& ev, const Hash32& m6id) const;
    void connect_bundle_failed(bt::kv::Txn& t, uint32_t h, AccumulatorDiff& diff, std::vector<TwoWayPegEvent>& ev, const Hash32& m6id) const;
    void disconnect_bundle_submitted(bt::kv::Txn& t, uint32_t h, AccumulatorDiff& diff, const Hash32& m6id) const;
    void disconnect_bundle_confirmed(bt::kv::Txn& t, uint32_t h, AccumulatorDiff& diff, const Hash32& m6id) const;
    void disconnect_bundle_failed(bt::kv::Txn& t, uint32_t h, AccumulatorDiff& diff, const Hash32& m6id) const;
};

// value encodings (also used by the archive and the RPC)
void enc_bundle_info(Writer& w, const WithdrawalBundleInfo& i);
WithdrawalBundleInfo dec_bundle_info(Reader& r);
void enc_market(Writer& w, const Market& m);
Market dec_market(Reader& r);
void enc_decision(Writer& w, const Decision& d);
Decision dec_decision(Reader& r);
void enc_spent(Writer& w, const SpentOutput& s);
SpentOutput dec_spent(Reader& r);

} // namespace hivemind
