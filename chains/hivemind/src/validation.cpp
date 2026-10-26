// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// lib/validation/{mod,decision,market,vote}.rs. Error texts are their Display forms
// (state::Error::InvalidTransaction = "invalid transaction: {reason}", etc.).
#include "state_internal.h"

#include <hivemind/fmt.h>
#include <hivemind/math.h>

#include <cmath>

namespace hivemind {

using bt::kv::Txn;

static std::string did_debug(const DecisionId& d)
{
    return "DecisionId([" + std::to_string(d.b[0]) + ", " + std::to_string(d.b[1]) + ", " + std::to_string(d.b[2]) + "])";
}
static std::string mid_debug(const MarketId& m)
{
    std::string s = "MarketId([";
    for (int i = 0; i < 6; i++) s += (i ? ", " : "") + std::to_string(m.b[i]);
    return s + "])";
}
static std::string type_debug(const DecisionType& t)
{
    switch (t.kind) {
    case DecisionType::Binary: return "Binary";
    case DecisionType::Scaled:
        return "Scaled { min: " + fmt::debug(t.min) + ", max: " + fmt::debug(t.max) + ", increment: " + fmt::debug(t.increment) + " }";
    case DecisionType::Category: {
        std::string s = "Category { options: [";
        for (size_t i = 0; i < t.options.size(); i++) s += (i ? ", \"" : "\"") + t.options[i] + "\"";
        return s + "] }";
    }
    }
    return "?";
}
static std::string state_debug(MarketState s) { return to_string(s); }

uint64_t tx_fee(const FilledTransaction& ft)
{
    auto in = ft.value_in();
    if (!in) throw StateError("amount overflow");
    uint64_t out = 0;
    for (auto& o : ft.tx->outputs) {
        uint64_t v = o.get_value();
        if (out + v < out) throw StateError("amount overflow");
        out += v;
    }
    if (*in < out) throw StateError("value in is less than value out");
    return *in - out;
}

uint64_t miner_fee(const FilledTransaction& ft)
{
    if (ft.get<TxTrade>() || ft.get<TxAmplifyBeta>()) return math::TRADE_MINER_FEE_SATS;
    return tx_fee(ft);
}

// market.rs validate_maker_auth
static Address validate_maker_auth(const FilledTransaction& ft)
{
    if (ft.tx->inputs.empty()) throw invalid_tx("Transaction must have at least one input");
    if (ft.spent_utxos.empty()) throw invalid_tx("No spent UTXOs found");
    return ft.spent_utxos[0].address;
}

/** decision.rs validate_claim_payload. `via_dbs` = called through `state.decisions()`
 *  (the Dbs impl of DecisionValidationInterface answers None for the height and both
 *  timestamps: PR #47's bug, current consensus). */
static uint64_t validate_claim_payload(const State& s, const Txn& t, const ClaimDecisionPayload& p, const Address& maker,
                                       std::optional<uint32_t> override_height, bool via_dbs)
{
    auto& dt = p.decision_type;
    if (dt.kind == DecisionType::Binary || dt.kind == DecisionType::Scaled) {
        if (p.decisions.size() != 1)
            throw invalid_tx(type_debug(dt) + " claim must have exactly 1 decision, got " + std::to_string(p.decisions.size()));
        if (p.decisions[0].option_labels) throw invalid_tx("option_labels must not be set for binary/scaled decisions");
    } else {
        if (p.decisions.size() != 1) throw invalid_tx("Category claim must have exactly 1 decision entry, got " + std::to_string(p.decisions.size()));
        if (dt.options.size() < 2) throw invalid_tx("Category claim requires at least 2 options");
        auto& e = p.decisions[0];
        if (!e.option_labels) throw invalid_tx("Category claim entry must have option_labels");
        if (e.option_labels->size() < 2) throw invalid_tx("Category claim entry must have at least 2 option_labels");
        if (*e.option_labels != dt.options)
            throw invalid_tx("option_labels (" + std::to_string(e.option_labels->size()) + ") must match decision_type options (" +
                             std::to_string(dt.options.size()) + ")");
    }
    uint64_t current_ts = via_dbs ? 0 : s.mainchain_timestamp(t).value_or(0);
    std::optional<uint32_t> current_height = override_height ? override_height : (via_dbs ? std::nullopt : s.height(t));
    uint64_t genesis_ts = via_dbs ? 0 : s.genesis_timestamp(t).value_or(0);
    for (auto& e : p.decisions) {
        DecisionId id;
        id.b = e.decision_id_bytes;
        std::string err;
        auto d = Decision::make(maker.b, dt, e.header, e.description, e.option_0_label, e.option_1_label, e.tags.value_or(std::vector<std::string>{}),
                                &err);
        if (!d) throw StateError(err);
        s.validate_decision_claim(t, id, current_ts, current_height, genesis_ts);
    }
    uint64_t total = 0;
    for (auto& e : p.decisions) {
        DecisionId id;
        id.b = e.decision_id_bytes;
        if (!id.is_standard()) continue;
        uint64_t fee = s.fee_for_decision_id(t, id);
        if (total + fee < total) throw invalid_tx("Listing fee overflow");
        total += fee;
    }
    return total;
}

// decision.rs validate_complete_decision_claim (with `state` as the interface)
static void validate_complete_decision_claim(const State& s, const Txn& t, const FilledTransaction& ft, std::optional<uint32_t> override_height)
{
    auto* p = ft.get<ClaimDecisionPayload>();
    Address maker = validate_maker_auth(ft);
    uint64_t total = validate_claim_payload(s, t, *p, maker, override_height, false);
    if (total > 0) {
        uint64_t fee = tx_fee(ft);
        if (fee < total)
            throw invalid_tx("Insufficient listing fee: tx fee is " + std::to_string(fee) + " sats but listing fee requires " +
                             std::to_string(total) + " sats");
    }
}

// market.rs:35-276
static void validate_market_creation(const State& s, const Txn& t, const FilledTransaction& ft, std::optional<uint32_t> override_height)
{
    auto* v = ft.get<TxCreateMarket>();
    Address maker = validate_maker_auth(ft);
    std::map<DecisionId, const DecisionClaimEntry*> pending_claims;
    std::map<DecisionId, DecisionType> pending_types;
    uint64_t total_listing_fee = 0;
    for (auto& payload : v->new_claims) {
        uint64_t fee = validate_claim_payload(s, t, payload, maker, override_height, true);
        if (total_listing_fee + fee < total_listing_fee) throw invalid_tx("Listing fee overflow");
        total_listing_fee += fee;
        for (auto& e : payload.decisions) {
            DecisionId id;
            id.b = e.decision_id_bytes;
            if (pending_claims.count(id))
                throw invalid_tx("Duplicate new claim for decision " + id.to_hex());
            auto existing = s.decision_entry(t, id);
            if (existing && existing->decision)
                throw invalid_tx("New claim for decision " + id.to_hex() + " collides with already-claimed decision");
            pending_claims[id] = &e;
            pending_types[id] = payload.decision_type;
        }
    }
    if (v->dimension_specs.empty()) throw invalid_tx("Market must have at least one dimension");
    std::set<DecisionId> seen;
    for (auto& spec : v->dimension_specs) {
        if (!seen.insert(spec.id).second) throw invalid_tx("Duplicate dimension for decision " + did_debug(spec.id));
        auto pt = pending_types.find(spec.id);
        if (pt != pending_types.end()) {
            if (spec.kind == DimensionSpec::Categorical) {
                if (pt->second.kind == DecisionType::Category) {
                    if (pt->second.options.size() < 2) throw invalid_tx("Categorical dimensions require at least 2 options");
                } else {
                    throw invalid_tx("Categorical dimension references new decision " + did_debug(spec.id) + " which is not a Category type");
                }
            }
            continue;
        }
        auto entry = s.decision_entry(t, spec.id);
        if (!entry) throw invalid_tx("Referenced decision " + did_debug(spec.id) + " does not exist");
        if (!entry->decision) throw invalid_tx("Referenced decision " + did_debug(spec.id) + " was never claimed");
        if (spec.kind == DimensionSpec::Categorical) {
            if (!entry->decision->is_categorical())
                throw invalid_tx("Categorical dimension references decision " + did_debug(spec.id) + " which is not a Category type");
            if (entry->decision->option_count().value_or(0) < 2) throw invalid_tx("Categorical dimensions require at least 2 options");
        }
    }
    if (v->trading_fee && !(*v->trading_fee >= 0.0 && *v->trading_fee <= 1.0))
        throw invalid_tx("Trading fee must be between 0.0 and 1.0, got " + fmt::display(*v->trading_fee));
    if (v->tx_pow_difficulty && *v->tx_pow_difficulty > 0) {
        if (!math::txpow_validate(v->tx_pow_hash_selector.value_or(0), v->tx_pow_ordering.value_or(0), *v->tx_pow_difficulty))
            throw invalid_tx("TX-PoW config invalid: difficulty must be 0..=32 and hash_selector must be non-zero when difficulty > 0");
    }
    MarketId expected = compute_market_id(v->title, v->description, maker, v->dimension_specs);
    Address treasury = market_treasury_address(expected);
    std::optional<uint64_t> treasury_sats;
    for (auto& o : ft.tx->outputs)
        if (o.content.kind == Content::MarketFunds && !o.content.is_fee && o.content.market_id == expected && o.address == treasury) {
            treasury_sats = o.content.value;
            break;
        }
    if (!treasury_sats)
        throw invalid_tx("CreateMarket tx must have MarketFunds (treasury) output with market_id " + expected.to_hex() + " to " +
                         treasury.to_base58());
    if (*treasury_sats == 0) throw invalid_tx("CreateMarket treasury output must be positive");
    if (total_listing_fee > 0) {
        uint64_t fee = tx_fee(ft);
        if (fee < total_listing_fee)
            throw invalid_tx("Insufficient listing fee for new claims: tx fee is " + std::to_string(fee) + " sats but listing fees require " +
                             std::to_string(total_listing_fee) + " sats");
    }
}

// market.rs:278-475
void validate_trade(const State& s, const Archive& a, const Txn& t, const FilledTransaction& ft)
{
    auto* tr = ft.get<TxTrade>();
    auto market = s.market(t, tr->market_id);
    if (!market) throw invalid_tx("Market " + mid_debug(tr->market_id) + " does not exist");
    if (market->market_state != MarketState::Trading)
        throw invalid_tx("Market not in trading state (current: " + state_debug(market->market_state) + ")");
    auto tip = s.tip(t);
    if (!tip) throw invalid_tx("No chain tip when validating trade");
    auto tip_height = s.height(t);
    if (!tip_height) throw invalid_tx("No chain height when validating trade");
    auto prev_height = a.height(t, tr->prev_block_hash);
    if (!prev_height) throw invalid_tx("Trade prev_block_hash " + bt::hex_encode(tr->prev_block_hash) + " does not reference a known block");
    if (tr->prev_block_hash != *tip && !a.is_descendant(t, tr->prev_block_hash, *tip))
        throw invalid_tx("Trade prev_block_hash " + bt::hex_encode(tr->prev_block_hash) + " is not on the active chain");
    uint32_t depth = *tip_height > *prev_height ? *tip_height - *prev_height : 0;
    if (depth > 10) throw invalid_tx("Trade prev_block_hash is " + std::to_string(depth) + " blocks behind tip, exceeds window of 10");
    if (market->txpow_enabled()) {
        if (!tr->tx_pow_nonce) throw invalid_tx("Trade requires TX-PoW nonce for this market");
        Bytes data = serialize_trade_for_pow(market->id, tr->outcome_index, tr->shares, tr->trader, tr->limit_sats, tr->prev_block_hash);
        if (!math::txpow_verify(market->tx_pow_hash_selector, market->tx_pow_ordering, market->tx_pow_difficulty, data, *tr->tx_pow_nonce))
            throw invalid_tx("TX-PoW verification failed: insufficient proof-of-work");
    }
    if (size_t(tr->outcome_index) >= market->shares.size())
        throw invalid_tx("Outcome index " + std::to_string(tr->outcome_index) + " out of range (market has " +
                         std::to_string(market->shares.size()) + " tradeable outcomes)");
    if (tr->shares == 0) throw invalid_tx("Shares to trade must be non-zero");
    if (!ft.tx->outputs.empty()) throw invalid_tx("Trade transactions must have no outputs");
    validate_maker_auth(ft);
    auto in = ft.value_in();
    if (!in) throw invalid_tx("Failed to compute input value");
    if (tr->is_buy()) {
        if (tr->limit_sats == 0) throw invalid_tx("Buy limit (max_cost) must be positive");
        if (*in < tr->limit_sats)
            throw invalid_tx("Input value " + std::to_string(*in) + " sats insufficient for limit_sats " + std::to_string(tr->limit_sats) + " sats");
    } else {
        bool has_input = false;
        for (auto& u : ft.spent_utxos)
            if (u.address == tr->trader) has_input = true;
        bool has_proof = ft.actor_address && *ft.actor_address == tr->trader;
        if (!has_input && !has_proof) throw invalid_tx("Sell transaction has no proof of trader " + tr->trader.to_base58() + " identity");
        int64_t owned = 0;
        if (auto acc = s.share_account(t, tr->trader)) {
            auto it = acc->positions.find({tr->market_id, tr->outcome_index});
            if (it != acc->positions.end()) owned = it->second;
        }
        if (owned < 0 || uint64_t(owned) < tr->shares_abs())
            throw invalid_tx("Insufficient shares: trying to sell " + std::to_string(tr->shares_abs()) + " but only own " + std::to_string(owned) +
                             " for outcome " + std::to_string(tr->outcome_index));
        if (*in < math::TRADE_MINER_FEE_SATS)
            throw invalid_tx("Input value " + std::to_string(*in) + " sats insufficient for miner fee 1000 sats");
    }
}

// market.rs:477-570
static void validate_amplify_beta(const State& s, const Txn& t, const FilledTransaction& ft)
{
    auto* am = ft.get<TxAmplifyBeta>();
    if (am->amount == 0) throw invalid_tx("AmplifyBeta amount must be positive");
    auto market = s.market(t, am->market_id);
    if (!market) throw invalid_tx("Market " + mid_debug(am->market_id) + " does not exist");
    if (market->market_state != MarketState::Trading)
        throw invalid_tx("Market not in trading state (current: " + state_debug(market->market_state) + ")");
    if (market->creator_address != am->market_author)
        throw invalid_tx("AmplifyBeta author " + am->market_author.to_base58() + " does not match market creator " +
                         market->creator_address.to_base58());
    bool has_input = false;
    for (auto& u : ft.spent_utxos)
        if (u.address == am->market_author) has_input = true;
    bool has_proof = ft.actor_address && *ft.actor_address == am->market_author;
    if (!has_input && !has_proof) throw invalid_tx("AmplifyBeta has no proof of market author " + am->market_author.to_base58() + " identity");
    if (!ft.tx->outputs.empty()) throw invalid_tx("AmplifyBeta transactions must have no outputs");
    auto in = ft.value_in();
    if (!in) throw invalid_tx("Failed to compute input value");
    uint64_t required = am->amount + math::TRADE_MINER_FEE_SATS;
    if (required < am->amount) throw invalid_tx("AmplifyBeta amount overflow");
    if (*in < required)
        throw invalid_tx("Input value " + std::to_string(*in) + " sats insufficient for amplify amount " + std::to_string(am->amount) +
                         " + miner fee 1000 sats");
}

// ------------------------------------------------------------------ vote.rs
static void validate_voter_eligibility(const State& s, const Txn& t, const Address& voter)
{
    if (s.reputation(t, voter) <= 0.0) throw invalid_tx("Voter has no reputation (voting rights)");
}

/** Decision of a vote: InvalidDecisionId { reason } when missing. */
static Decision validate_decision_entry(const State& s, const Txn& t, const DecisionId& id, const std::string& prefix = "")
{
    auto e = s.decision_entry(t, id);
    if (!e) throw StateError("invalid decision ID: " + prefix + "Decision " + did_debug(id) + " does not exist");
    if (!e->decision) throw StateError("invalid decision ID: " + prefix + "Decision " + did_debug(id) + " has no decision");
    return *e->decision;
}

static void validate_vote_value(const Decision& d, double v, const std::string& prefix = "")
{
    if (std::isnan(v)) return;
    if (d.is_categorical()) {
        double max_idx = double(d.option_count().value_or(0));
        // `vote_value as usize`: saturating, NaN -> 0
        double idx = v <= 0 ? 0 : v >= 18446744073709551615.0 ? 18446744073709551615.0 : std::trunc(v);
        bool ok = v >= 0.0 && v == idx && v <= max_idx;
        if (!ok)
            throw invalid_tx(prefix + "Categorical vote value " + fmt::display(v) + " must be an integer index in 0..=" + fmt::display(max_idx) +
                             " (where " + fmt::display(max_idx) + " = Inconclusive)");
        return;
    }
    if (!(v >= 0.0 && v <= 1.0)) throw invalid_tx(prefix + "Vote value " + fmt::display(v) + " outside valid normalized range [0.0, 1.0]");
}

static void validate_voting_period(const State& s, const Txn& t, const DecisionId& id, const std::string& prefix = "")
{
    if (s.decision_state(t, id) != DecisionState::Voting) throw invalid_tx(prefix + "Decision " + did_debug(id) + " is not in voting period");
}

static void validate_no_duplicate_vote(const State& s, const Txn& t, const Address& voter, uint32_t period, const DecisionId& id,
                                       const std::string& prefix = "")
{
    if (s.vote(t, period, voter, id)) throw invalid_tx(prefix + "Duplicate vote: voter already voted on this decision in this period");
}

static bool proves_actor(const FilledTransaction& ft, const Address& a)
{
    for (auto& u : ft.spent_utxos)
        if (u.address == a) return true;
    return ft.actor_address && *ft.actor_address == a;
}

static void validate_vote_submission(const State& s, const Txn& t, const FilledTransaction& ft)
{
    auto* v = ft.get<TxSubmitVote>();
    if (!proves_actor(ft, v->voter)) throw invalid_tx("No proof of voter " + v->voter.to_base58() + " identity");
    validate_voter_eligibility(s, t, v->voter);
    DecisionId id;
    id.b = v->decision_id_bytes;
    if (v->voting_period != id.voting_period())
        throw invalid_tx("Vote period mismatch: decision " + id.to_hex() + " must be voted on in period " + std::to_string(id.voting_period()) +
                         ", but transaction specifies period " + std::to_string(v->voting_period));
    Decision d = validate_decision_entry(s, t, id);
    validate_vote_value(d, v->vote_value);
    validate_voting_period(s, t, id);
    validate_no_duplicate_vote(s, t, v->voter, id.voting_period(), id);
}

static void validate_reputation_transfer(const State& s, const Txn& t, const FilledTransaction& ft)
{
    auto* tr = ft.get<TxTransferReputation>();
    bool has_input = false;
    for (auto& u : ft.spent_utxos)
        if (u.address == tr->sender) has_input = true;
    if (!has_input)
        throw invalid_tx("Reputation transfer from " + tr->sender.to_base58() +
                         " must spend a Bitcoin UTXO owned by the sender (replay protection)");
    if (tr->amount <= 0.0) throw invalid_tx("Transfer amount must be positive");
    if (!std::isfinite(tr->amount)) throw invalid_tx("Transfer amount must be a finite number");
    if (tr->amount != math::round_reputation(tr->amount))
        throw invalid_tx("Transfer amount " + fmt::display(tr->amount) + " exceeds reputation precision (max 10 decimal digits)");
    if (tr->sender == tr->dest) throw invalid_tx("Cannot transfer reputation to self");
    double rep = s.reputation(t, tr->sender);
    if (rep < tr->amount)
        throw invalid_tx("Insufficient reputation: have " + fmt::display(rep) + ", need " + fmt::display(tr->amount));
}

static void validate_ballot(const State& s, const Txn& t, const FilledTransaction& ft)
{
    auto* b = ft.get<TxSubmitBallot>();
    if (!proves_actor(ft, b->voter)) throw invalid_tx("No proof of voter " + b->voter.to_base58() + " identity");
    validate_voter_eligibility(s, t, b->voter);
    std::set<std::pair<uint32_t, DecisionId>> seen;
    for (size_t idx = 0; idx < b->votes.size(); idx++) {
        auto& item = b->votes[idx];
        DecisionId id;
        id.b = item.decision_id_bytes;
        std::string pre = "Ballot item " + std::to_string(idx) + ": ";
        uint32_t vp = id.voting_period();
        if (vp != b->voting_period)
            throw invalid_tx(pre + "period mismatch: decision " + id.to_hex() + " must be voted on in period " + std::to_string(vp) +
                             ", but transaction specifies period " + std::to_string(b->voting_period));
        if (!seen.insert({vp, id}).second)
            throw invalid_tx(pre + "duplicate vote for decision " + did_debug(id) + " in period VotingPeriodId(" + std::to_string(vp) + ")");
        Decision d = validate_decision_entry(s, t, id, pre);
        validate_vote_value(d, item.vote_value, pre);
        validate_voting_period(s, t, id, pre);
        validate_no_duplicate_vote(s, t, b->voter, vp, id, pre);
    }
}

// mod.rs:91-153
std::optional<uint64_t> validate_market_transaction(const State& s, const Txn& t, const FilledTransaction& ft, const Archive& a,
                                                    std::optional<uint32_t> override_height)
{
    if (ft.get<ClaimDecisionPayload>()) validate_complete_decision_claim(s, t, ft, override_height);
    if (ft.get<TxCreateMarket>()) validate_market_creation(s, t, ft, override_height);
    if (ft.get<TxTrade>()) {
        validate_trade(s, a, t, ft);
        return math::TRADE_MINER_FEE_SATS;
    }
    if (ft.get<TxAmplifyBeta>()) {
        validate_amplify_beta(s, t, ft);
        return math::TRADE_MINER_FEE_SATS;
    }
    if (ft.get<TxSubmitVote>()) validate_vote_submission(s, t, ft);
    if (ft.get<TxSubmitBallot>()) validate_ballot(s, t, ft);
    if (ft.get<TxTransferReputation>()) validate_reputation_transfer(s, t, ft);
    return std::nullopt;
}

void check_duplicate_decision_claims(const std::vector<Transaction>& txs)
{
    std::set<std::array<uint8_t, 3>> seen;
    for (auto& tx : txs) {
        if (!tx.data) continue;
        std::vector<const ClaimDecisionPayload*> payloads;
        if (auto* p = std::get_if<ClaimDecisionPayload>(&*tx.data)) payloads.push_back(p);
        else if (auto* m = std::get_if<TxCreateMarket>(&*tx.data))
            for (auto& c : m->new_claims) payloads.push_back(&c);
        for (auto* p : payloads)
            for (auto& e : p->decisions)
                if (!seen.insert(e.decision_id_bytes).second)
                    throw StateError("duplicate decision claim in block: [" + std::to_string(e.decision_id_bytes[0]) + ", " +
                                     std::to_string(e.decision_id_bytes[1]) + ", " + std::to_string(e.decision_id_bytes[2]) + "]");
    }
}

void validate_coinbase_outputs(const std::vector<Output>& outs)
{
    for (auto& o : outs)
        if (o.content.kind == Content::MarketFunds) throw StateError("bad coinbase output content");
}

void validate_fees(uint64_t coinbase_value, const std::vector<FilledTransaction>& txs, const std::set<size_t>& skipped)
{
    uint64_t total = 0;
    for (size_t i = 0; i < txs.size(); i++) {
        if (skipped.count(i)) continue;
        uint64_t f = miner_fee(txs[i]);
        if (total + f < total) throw StateError("amount overflow");
        total += f;
    }
    if (coinbase_value > total) throw StateError("total fees less than coinbase value");
}

} // namespace hivemind
