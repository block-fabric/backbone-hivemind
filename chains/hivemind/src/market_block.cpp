// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// lib/state/markets/block.rs: the market effects of connecting and disconnecting a block
// (decision claims, market creation, trades with LMSR pricing and slippage skips, votes,
// reputation transfers, AmplifyBeta, treasury/fee UTXO consolidation, period
// transitions, consensus, settlement) and their undo.
#include "state_internal.h"

#include <hivemind/fmt.h>
#include <hivemind/market_block.h>
#include <hivemind/math.h>

#include <algorithm>

namespace hivemind {

using bt::kv::Txn;

static Bytes u32le(uint32_t v)
{
    Writer w;
    w.u32(v);
    return w.buf;
}
static const Bytes UNIT{};
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

// ------------------------------------------------------------------ StateUpdate
// block.rs:19-108
struct MarketStateUpdate {
    MarketId market_id;
    std::optional<std::pair<size_t, int64_t>> share_delta;
    std::optional<Hash32> txid;
    std::optional<uint64_t> volume_sats, fee_sats;
};
struct PendingSellPayout {
    MarketId market_id;
    Address seller;
    uint64_t payout_sats = 0, fee_sats = 0;
    uint32_t outcome_index = 0;
    Hash32 txid;
};
struct PendingBuySettlement {
    MarketId market_id;
    Address trader;
    uint64_t input_value_sats = 0, lmsr_cost_sats = 0, market_fee_sats = 0;
    Hash32 txid;
    bool is_amplify = false;
};
struct VoteSubmission {
    VoteKey key;
    VoteEntry entry;
};

struct StateUpdate {
    std::vector<MarketStateUpdate> market_updates;
    std::vector<Market> market_creations;
    std::map<std::pair<Address, MarketId>, std::map<uint32_t, int64_t>> share_account_changes;
    std::vector<VoteSubmission> votes;
    std::vector<PendingSellPayout> sell_payouts;
    std::vector<PendingBuySettlement> buy_settlements;
    std::vector<std::tuple<Address, uint64_t, Hash32>> sell_input_changes;

    void add_share_change(const Address& a, const MarketId& m, uint32_t outcome, int64_t delta)
    {
        auto& v = share_account_changes[{a, m}][outcome];
        v = int64_t(uint64_t(v) + uint64_t(delta));
    }

    // block.rs:83-108
    void verify_internal_consistency() const
    {
        std::set<MarketId> created;
        for (auto& c : market_creations)
            if (!created.insert(c.id).second) throw invalid_tx("Duplicate market creation for ID: " + mid_debug(c.id));
        for (auto& u : market_updates)
            if (created.count(u.market_id))
                throw invalid_tx("Market " + mid_debug(u.market_id) + " cannot be both created and updated in same block");
    }

    // block.rs:110-152
    void validate_all_changes(const State& s, const Txn& t) const
    {
        verify_internal_consistency();
        for (auto& u : market_updates)
            if (!s.market(t, u.market_id)) throw invalid_tx("Market " + mid_debug(u.market_id) + " does not exist");
        for (auto& c : market_creations) {
            if (s.market(t, c.id)) throw invalid_tx("Market " + mid_debug(c.id) + " already exists");
            for (size_t i = 0; i < c.shares.size(); i++)
                if (c.shares[i] < 0)
                    throw invalid_tx("Share quantity at index " + std::to_string(i) + " must be non-negative, got " + std::to_string(c.shares[i]));
            if (c.shares.size() < 2) throw invalid_tx("Market must have at least 2 outcomes, got " + std::to_string(c.shares.size()));
        }
    }
};

OutPoint sell_payout_outpoint(const MarketId& m, const Address& a, const Hash32& txid)
{
    bt::Blake3 h;
    h.update(ByteSpan((const uint8_t*)"SELL_PAYOUT", 11));
    h.update(ByteSpan(m.b.data(), 6));
    h.update(a.b.span());
    h.update(txid.span());
    return OutPoint::payout(h.finalize(), 0);
}

OutPoint buy_change_outpoint(const MarketId& m, const Address& a, const Hash32& txid)
{
    bt::Blake3 h;
    h.update(ByteSpan((const uint8_t*)"BUY_CHANGE", 10));
    h.update(ByteSpan(m.b.data(), 6));
    h.update(a.b.span());
    h.update(txid.span());
    return OutPoint::payout(h.finalize(), 0);
}

OutPoint sell_input_change_outpoint(const Address& a, const Hash32& txid)
{
    bt::Blake3 h;
    h.update(ByteSpan((const uint8_t*)"SELL_INPUT_CHANGE", 17));
    h.update(a.b.span());
    h.update(txid.span());
    return OutPoint::payout(h.finalize(), 0);
}

// block.rs:374-683 consolidate_market_utxos
static std::optional<ConsolidationUndoData> consolidate(const State& s, Txn& t, AccumulatorDiff& diff, uint32_t height, const StateUpdate& u)
{
    std::set<MarketId> markets;
    for (auto& p : u.sell_payouts) markets.insert(p.market_id);
    for (auto& b : u.buy_settlements) markets.insert(b.market_id);
    if (markets.empty() && u.sell_input_changes.empty()) return std::nullopt;
    ConsolidationUndoData undo;
    for (auto& mid : markets) {
        uint64_t treasury_total = 0, fee_total = 0;
        std::vector<OutPoint> treasury_consume, fee_consume;
        auto old_treasury_ptr = s.market_funds_utxo(t, mid, false);
        auto old_fee_ptr = s.market_funds_utxo(t, mid, true);
        ConsolidationUndoEntry e;
        e.market_id = mid;
        e.old_treasury_pointer = old_treasury_ptr;
        e.old_fee_pointer = old_fee_ptr;
        if (old_treasury_ptr)
            if (auto o = s.utxo(t, *old_treasury_ptr)) {
                treasury_total += o->get_value();
                e.old_treasury_utxos.emplace_back(*old_treasury_ptr, *o);
                treasury_consume.push_back(*old_treasury_ptr);
            }
        if (old_fee_ptr)
            if (auto o = s.utxo(t, *old_fee_ptr)) {
                fee_total += o->get_value();
                e.old_fee_utxos.emplace_back(*old_fee_ptr, *o);
                fee_consume.push_back(*old_fee_ptr);
            }
        std::vector<const PendingBuySettlement*> buys;
        for (auto& b : u.buy_settlements)
            if (b.market_id == mid) buys.push_back(&b);
        for (auto* b : buys) {
            treasury_total += b->lmsr_cost_sats;
            fee_total += b->market_fee_sats;
        }
        std::vector<const PendingSellPayout*> sells;
        for (auto& p : u.sell_payouts)
            if (p.market_id == mid) sells.push_back(&p);
        uint64_t total_sell_payouts = 0, total_sell_fees = 0;
        for (auto* p : sells) total_sell_payouts += p->payout_sats;
        for (auto* p : sells) total_sell_fees += p->fee_sats;
        fee_total += total_sell_fees;
        uint64_t total_sell_gross = total_sell_payouts + total_sell_fees;
        if (total_sell_gross > treasury_total)
            throw invalid_tx("Treasury underflow: sell gross " + std::to_string(total_sell_gross) + " (payouts " + std::to_string(total_sell_payouts) +
                             " + fees " + std::to_string(total_sell_fees) + ") exceed treasury " + std::to_string(treasury_total) +
                             " for market " + mid_debug(mid));
        bool has_treasury_work = !treasury_consume.empty() || !sells.empty() || !buys.empty();
        if (has_treasury_work && (treasury_total > 0 || !sells.empty())) {
            for (auto& op : treasury_consume) s.delete_utxo(t, op, diff);
            s.clear_market_funds_utxo(t, mid, false);
            for (auto* p : sells) {
                OutPoint op = sell_payout_outpoint(mid, p->seller, p->txid);
                Output o;
                o.address = p->seller;
                o.content.kind = Content::Value;
                o.content.value = p->payout_sats;
                s.insert_utxo(t, op, o, diff);
                e.sell_payout_utxos.push_back(op);
            }
            for (auto* b : buys) {
                uint64_t a = b->input_value_sats;
                bool ok = a >= math::TRADE_MINER_FEE_SATS;
                if (ok) a -= math::TRADE_MINER_FEE_SATS;
                ok = ok && a >= b->lmsr_cost_sats;
                if (ok) a -= b->lmsr_cost_sats;
                ok = ok && a >= b->market_fee_sats;
                if (ok) a -= b->market_fee_sats;
                if (!ok)
                    throw invalid_tx("Buy change underflow: input " + std::to_string(b->input_value_sats) + " < fees 1000 + cost " +
                                     std::to_string(b->lmsr_cost_sats) + " + market_fee " + std::to_string(b->market_fee_sats));
                if (a > 0) {
                    OutPoint op = buy_change_outpoint(mid, b->trader, b->txid);
                    Output o;
                    o.address = b->trader;
                    o.content.kind = Content::Value;
                    o.content.value = a;
                    s.insert_utxo(t, op, o, diff);
                    e.buy_change_utxos.push_back(op);
                }
            }
            if (treasury_total < total_sell_payouts || treasury_total - total_sell_payouts < total_sell_fees)
                throw invalid_tx("Treasury remainder underflow: treasury " + std::to_string(treasury_total) + " < payouts " +
                                 std::to_string(total_sell_payouts) + " + fees " + std::to_string(total_sell_fees));
            uint64_t remaining = treasury_total - total_sell_payouts - total_sell_fees;
            if (remaining > 0) {
                OutPoint op = OutPoint::market_funds(mid, height, false);
                Output o;
                o.address = market_treasury_address(mid);
                o.content.kind = Content::MarketFunds;
                o.content.market_id = mid;
                o.content.value = remaining;
                o.content.is_fee = false;
                s.insert_utxo(t, op, o, diff);
                s.set_market_funds_utxo(t, mid, false, op);
                e.new_treasury_utxo = op;
            }
        }
        bool has_fee_work = !fee_consume.empty() || total_sell_fees > 0 || !buys.empty();
        if (has_fee_work && fee_total > 0) {
            for (auto& op : fee_consume) s.delete_utxo(t, op, diff);
            s.clear_market_funds_utxo(t, mid, true);
            OutPoint op = OutPoint::market_funds(mid, height, true);
            Output o;
            o.address = market_author_fee_address(mid);
            o.content.kind = Content::MarketFunds;
            o.content.market_id = mid;
            o.content.value = fee_total;
            o.content.is_fee = true;
            s.insert_utxo(t, op, o, diff);
            s.set_market_funds_utxo(t, mid, true, op);
            e.new_fee_utxo = op;
        }
        undo.entries.push_back(std::move(e));
    }
    for (auto& [a, change, txid] : u.sell_input_changes)
        if (change > 0) {
            OutPoint op = sell_input_change_outpoint(a, txid);
            Output o;
            o.address = a;
            o.content.kind = Content::Value;
            o.content.value = change;
            s.insert_utxo(t, op, o, diff);
            undo.sell_input_change_utxos.push_back(op);
        }
    return undo;
}

// block.rs:154-308 apply_all_changes
static std::optional<ConsolidationUndoData> apply_all_changes(const State& s, Txn& t, AccumulatorDiff& diff, uint32_t height, const StateUpdate& u)
{
    for (auto& m : u.market_creations) s.add_market(t, m);
    // aggregated_deltas: per market, the deltas in block order (HashMap over markets: each
    // market is independent)
    std::map<MarketId, std::vector<const MarketStateUpdate*>> agg;
    for (auto& up : u.market_updates)
        if (up.share_delta) agg[up.market_id].push_back(&up);
    for (auto& [mid, deltas] : agg) {
        auto m = s.market(t, mid);
        if (!m) throw invalid_tx("Market " + mid_debug(mid) + " not found");
        auto new_shares = m->shares;
        for (auto* d : deltas) {
            auto [idx, delta] = *d->share_delta;
            new_shares[idx] = int64_t(uint64_t(new_shares[idx]) + uint64_t(delta));
            if (d->volume_sats) {
                if (idx >= m->outcome_volumes_sats.size()) throw invalid_tx("Failed to update volume: InvalidOutcomeIndex(" + std::to_string(idx) + ")");
                uint64_t& ov = m->outcome_volumes_sats[idx];
                ov = ov + *d->volume_sats < ov ? UINT64_MAX : ov + *d->volume_sats;
                m->total_volume_sats = m->total_volume_sats + *d->volume_sats < m->total_volume_sats ? UINT64_MAX : m->total_volume_sats + *d->volume_sats;
            }
        }
        if (new_shares.size() != m->shares.size()) throw invalid_tx("Failed to update market state: InvalidDimensions");
        m->shares = new_shares;
        m->version += 1;
        m->last_updated_height = height;
        s.update_market(t, *m);
        s.clear_mempool_shares(t, mid);
    }
    std::map<MarketId, uint64_t> amplify;
    for (auto& b : u.buy_settlements)
        if (b.is_amplify) amplify[b.market_id] += b.lmsr_cost_sats;
    for (auto& [mid, amount] : amplify) {
        auto m = s.market(t, mid);
        if (!m) throw invalid_tx("Market " + mid_debug(mid) + " not found");
        if (m->liquidity_base_sats + amount < m->liquidity_base_sats) throw invalid_tx("Liquidity base overflow for market " + mid_debug(mid));
        m->liquidity_base_sats += amount;
        s.update_market(t, *m);
    }
    for (auto& [key, changes] : u.share_account_changes)
        for (auto& [outcome, delta] : changes) {
            if (delta == 0) continue;
            if (delta > 0) s.add_shares(t, key.first, key.second, outcome, delta, height);
            else s.remove_shares(t, key.first, key.second, outcome, int64_t(uint64_t(0) - uint64_t(delta)), height);
        }
    {
        std::set<MarketId> traded;
        for (auto& [key, _] : u.share_account_changes) traded.insert(key.second);
        for (auto& mid : traded) s.verify_share_invariant(t, mid);
    }
    for (auto& v : u.votes) s.put_vote(t, v.key, v.entry);
    return consolidate(s, t, diff, height, u);
}

// ------------------------------------------------------------------ per-transaction effects
static double market_beta(const Market& m) { return math::derive_beta_from_liquidity(m.liquidity_base_sats, m.shares.size()); }

// block.rs:1547-1575
static std::pair<std::vector<int64_t>, double> running_market_state(const MarketId& mid, const Market& m, const StateUpdate& u)
{
    auto eff = m.shares;
    for (auto& up : u.market_updates)
        if (up.market_id == mid && up.share_delta) {
            auto [idx, d] = *up.share_delta;
            eff[idx] = int64_t(uint64_t(eff[idx]) + uint64_t(d));
        }
    uint64_t pending = 0;
    for (auto& b : u.buy_settlements)
        if (b.market_id == mid && b.is_amplify) pending += b.lmsr_cost_sats; // Iterator::sum: wraps in release
    uint64_t base = m.liquidity_base_sats + pending < m.liquidity_base_sats ? UINT64_MAX : m.liquidity_base_sats + pending;
    return {eff, math::derive_beta_from_liquidity(base, eff.size())};
}

enum class TradeResult { Applied, Skipped };

// block.rs:1810-2008
static TradeResult apply_trade(const State& s, const Txn& t, const FilledTransaction& ft, StateUpdate& u, std::string* skip_reason)
{
    auto* tr = ft.get<TxTrade>();
    bool is_buy = tr->is_buy();
    uint64_t shares_abs = tr->shares_abs();
    size_t outcome = tr->outcome_index;
    auto m = s.market(t, tr->market_id);
    if (!m) throw invalid_tx("Market " + mid_debug(tr->market_id) + " does not exist");
    if (m->market_state != MarketState::Trading)
        throw invalid_tx(std::string("Cannot trade: market is in ") + to_string(m->market_state) + " state");
    auto [eff, beta] = running_market_state(tr->market_id, *m, u);
    auto nw = eff;
    if (outcome >= nw.size()) throw StateError("index out of bounds", true); // ndarray panics
    nw[outcome] = int64_t(uint64_t(nw[outcome]) + uint64_t(tr->shares));
    if (nw[outcome] < 0)
        throw invalid_tx("Trade would result in negative market shares: " + std::to_string(nw[outcome]) + " for outcome " + std::to_string(outcome));
    auto in = ft.value_in();
    if (!in) throw invalid_tx("Failed to compute input value");
    Hash32 txid = ft.tx->txid();
    uint64_t volume = 0, fee = 0;
    if (is_buy) {
        std::string err;
        auto base = math::update_cost(eff, nw, beta, &err);
        if (!base) throw invalid_tx("Failed to calculate trade cost: " + err);
        auto bc = math::buy_cost(*base, m->trading_fee, &err);
        if (!bc) throw invalid_tx("Buy cost calculation failed: " + err);
        if (bc->exceeds_limit(tr->limit_sats)) {
            if (skip_reason)
                *skip_reason = "Buy cost " + std::to_string(bc->total_cost_sats) + " sats + miner fee 1000 sats exceeds max cost " +
                               std::to_string(tr->limit_sats) + " sats";
            return TradeResult::Skipped;
        }
        u.buy_settlements.push_back({tr->market_id, tr->trader, *in, bc->base_cost_sats, bc->trading_fee_sats, txid, false});
        volume = bc->total_cost_sats;
        fee = bc->trading_fee_sats;
    } else {
        int64_t owned = 0;
        if (auto acc = s.share_account(t, tr->trader)) {
            auto it = acc->positions.find({tr->market_id, tr->outcome_index});
            if (it != acc->positions.end()) owned = it->second;
        }
        int64_t pending = 0;
        auto it = u.share_account_changes.find({tr->trader, tr->market_id});
        if (it != u.share_account_changes.end()) {
            auto jt = it->second.find(tr->outcome_index);
            if (jt != it->second.end()) pending = jt->second;
        }
        int64_t effective = int64_t(uint64_t(owned) + uint64_t(pending));
        if (effective < int64_t(shares_abs))
            throw invalid_tx("Insufficient shares: trying to sell " + std::to_string(shares_abs) + " but only own " + std::to_string(owned) +
                             " (effective: " + std::to_string(effective) + ") for outcome " + std::to_string(tr->outcome_index));
        std::string err;
        auto proceeds = math::update_cost(nw, eff, beta, &err);
        if (!proceeds) throw invalid_tx("Failed to calculate sell proceeds: " + err);
        auto sp = math::sell_proceeds(*proceeds, m->trading_fee, &err);
        if (!sp) throw invalid_tx("Sell proceeds calculation failed: " + err);
        if (sp->net_proceeds_sats < tr->limit_sats) {
            if (skip_reason)
                *skip_reason = "Sell proceeds " + std::to_string(sp->net_proceeds_sats) + " sats below minimum " + std::to_string(tr->limit_sats) + " sats";
            return TradeResult::Skipped;
        }
        u.sell_payouts.push_back({tr->market_id, tr->trader, sp->net_proceeds_sats, sp->trading_fee_sats, tr->outcome_index, txid});
        uint64_t change = *in > math::TRADE_MINER_FEE_SATS ? *in - math::TRADE_MINER_FEE_SATS : 0;
        if (change > 0) u.sell_input_changes.emplace_back(tr->trader, change, txid);
        volume = sp->net_proceeds_sats;
        fee = sp->trading_fee_sats;
    }
    u.market_updates.push_back({tr->market_id, std::make_pair(outcome, tr->shares), txid, volume, fee});
    u.add_share_change(tr->trader, tr->market_id, tr->outcome_index, tr->shares);
    return TradeResult::Applied;
}

static Address extract_creator(const FilledTransaction& ft)
{
    if (ft.spent_utxos.empty()) throw invalid_tx("No spent UTXOs found");
    return ft.spent_utxos[0].address;
}

static Decision make_decision(const Address& maker, const DecisionType& t, const DecisionClaimEntry& e)
{
    std::string err;
    auto d = Decision::make(maker.b, t, e.header, e.description, e.option_0_label, e.option_1_label, e.tags.value_or(std::vector<std::string>{}), &err);
    if (!d) throw StateError(err);
    return *d;
}

// block.rs:1421-1477
static void apply_decision_claim(const State& s, Txn& t, const FilledTransaction& ft, uint32_t height)
{
    auto* c = ft.get<ClaimDecisionPayload>();
    Address maker = extract_creator(ft);
    Hash32 txid = ft.tx->txid();
    for (auto& e : c->decisions) {
        DecisionId id;
        id.b = e.decision_id_bytes;
        s.claim_decision(t, id, make_decision(maker, c->decision_type, e), txid, height);
    }
}

// block.rs:2010-2149
static void apply_market_creation(const State& s, Txn& t, const FilledTransaction& ft, StateUpdate& u, uint32_t height)
{
    auto* v = ft.get<TxCreateMarket>();
    Address creator = extract_creator(ft);
    Hash32 txid = ft.tx->txid();
    for (auto& p : v->new_claims)
        for (auto& e : p.decisions) {
            DecisionId id;
            id.b = e.decision_id_bytes;
            s.claim_decision(t, id, make_decision(creator, p.decision_type, e), txid, height);
        }
    std::map<DecisionId, Decision> decisions;
    for (auto& spec : v->dimension_specs) {
        auto e = s.decision_entry(t, spec.id);
        if (!e) throw StateError("invalid decision ID: Decision " + did_debug(spec.id) + " does not exist");
        if (!e->decision) throw StateError("invalid decision ID: Decision " + did_debug(spec.id) + " has no decision");
        decisions[spec.id] = *e->decision;
    }
    // compute_market_tags: BTreeSet of every tag of the dimension decisions
    std::set<std::string> tagset;
    for (auto& [_, d] : decisions)
        for (auto& tg : d.tags) tagset.insert(tg);
    std::vector<std::string> tags(tagset.begin(), tagset.end());
    std::string err;
    auto m = build_market(v->title, v->description, tags, creator, v->dimension_specs, v->trading_fee.value_or(math::DEFAULT_TRADING_FEE),
                          v->tx_pow_hash_selector.value_or(0), v->tx_pow_ordering.value_or(0), v->tx_pow_difficulty.value_or(0), height,
                          decisions, &err);
    if (!m) throw invalid_tx("Market creation failed: " + err);
    Address treasury = market_treasury_address(m->id);
    for (uint32_t vout = 0; vout < ft.tx->outputs.size(); vout++) {
        auto& o = ft.tx->outputs[vout];
        if (o.content.kind == Content::MarketFunds && !o.content.is_fee && o.content.market_id == m->id && o.address == treasury) {
            s.set_market_funds_utxo(t, m->id, false, OutPoint::regular(txid, vout));
            m->liquidity_base_sats = o.content.value;
            break;
        }
    }
    u.market_creations.push_back(*m);
}

// block.rs:2151-2180
static void apply_amplify_beta(const FilledTransaction& ft, StateUpdate& u)
{
    auto* a = ft.get<TxAmplifyBeta>();
    auto in = ft.value_in();
    if (!in) throw invalid_tx("Failed to compute input value");
    u.buy_settlements.push_back({a->market_id, a->market_author, *in, a->amount, 0, ft.tx->txid(), true});
}

static VoteSubmission make_vote(const State& s, const Txn& t, const Address& voter, const DecisionId& id, double value, uint64_t ts, uint32_t h,
                                const Hash32& txid)
{
    std::optional<Decision> d;
    if (auto e = s.decision_entry(t, id)) d = e->decision;
    VoteSubmission v;
    v.key.period_id = id.voting_period();
    v.key.voter = voter;
    v.key.decision_id = id;
    v.entry.value = convert_vote_value(value, d ? &*d : nullptr);
    v.entry.timestamp = ts;
    v.entry.block_height = h;
    (void)txid;
    return v;
}

// block.rs:2212-2282
static void apply_submit_vote(const State& s, Txn& t, const FilledTransaction& ft, StateUpdate& u, uint32_t height)
{
    auto* v = ft.get<TxSubmitVote>();
    DecisionId id;
    id.b = v->decision_id_bytes;
    uint32_t vp = id.voting_period();
    if (v->voting_period != vp)
        throw invalid_tx("Vote period mismatch: decision " + id.to_hex() + " was claimed in period " + std::to_string(id.period_index()) +
                         " and must be voted on in period " + std::to_string(vp) + ", but transaction specifies period " +
                         std::to_string(v->voting_period));
    auto ts = s.mainchain_timestamp(t);
    if (!ts) throw invalid_tx("No mainchain timestamp available");
    u.votes.push_back(make_vote(s, t, v->voter, id, v->vote_value, *ts, height, ft.tx->txid()));
}

// block.rs:2315-2401
static void apply_submit_ballot(const State& s, Txn& t, const FilledTransaction& ft, StateUpdate& u, uint32_t height)
{
    auto* b = ft.get<TxSubmitBallot>();
    auto ts = s.mainchain_timestamp(t);
    if (!ts) throw invalid_tx("No mainchain timestamp available");
    std::optional<uint32_t> expected;
    for (auto& item : b->votes) {
        DecisionId id;
        id.b = item.decision_id_bytes;
        uint32_t vp = id.voting_period();
        if (expected) {
            if (vp != *expected)
                throw invalid_tx("Ballot period mismatch: decision " + id.to_hex() + " requires period " + std::to_string(vp) +
                                 " but ballot expects period " + std::to_string(*expected));
        } else {
            expected = vp;
            if (b->voting_period != vp)
                throw invalid_tx("Ballot period mismatch: decisions require period " + std::to_string(vp) + " but transaction specifies period " +
                                 std::to_string(b->voting_period));
        }
        u.votes.push_back(make_vote(s, t, b->voter, id, item.vote_value, *ts, height, ft.tx->txid()));
    }
}

// block.rs:2436-2494
static void apply_transfer_reputation(const State& s, Txn& t, const FilledTransaction& ft, uint32_t height)
{
    auto* tr = ft.get<TxTransferReputation>();
    double sender_rep = s.reputation(t, tr->sender);
    if (sender_rep < tr->amount)
        throw invalid_tx("Insufficient reputation at apply: have " + fmt::display(sender_rep) + ", need " + fmt::display(tr->amount));
    double receiver_rep = s.reputation(t, tr->dest);
    std::vector<ReputationTransferUndoEntry> undo;
    if (auto v = t.get(State::T_REP_TRANSFER_UNDO, u32le(height))) undo = dec_rep_undo(*v);
    undo.push_back({tr->sender, sender_rep, tr->dest, receiver_rep});
    t.put(State::T_REP_TRANSFER_UNDO, u32le(height), enc_rep_undo(undo));
    s.set_reputation(t, tr->sender, sender_rep - tr->amount);
    s.set_reputation(t, tr->dest, receiver_rep + tr->amount);
}

// ------------------------------------------------------------------ connect
// block.rs:737-796
ConnectContext connect_begin(const State& s, Txn& t, const Body& body, uint32_t height, uint64_t mainchain_timestamp)
{
    if (height == 0) {
        Writer w;
        w.u64(mainchain_timestamp);
        t.put(State::T_GENESIS_TS, UNIT, w.buf);
        if (!body.coinbase.outputs.empty()) s.set_reputation(t, body.coinbase.outputs[0].address, 1.0);
    }
    uint64_t genesis_ts = s.genesis_timestamp(t).value_or(0);
    uint32_t prev_highest = s.highest_minted_period(t).value_or(0);
    snapshot_period_pricing(s, t, height);
    t.put(State::T_MINTING_UNDO, u32le(height), u32le(prev_highest));
    uint32_t current = get_current_period(mainchain_timestamp, height, genesis_ts, s.config());
    for (auto& id : claimed_decisions_needing_voting(s, t, current)) transition_decision_to_voting(s, t, id, height);
    validate_coinbase_outputs(body.coinbase.outputs);
    return ConnectContext{height, mainchain_timestamp, genesis_ts, current};
}

// block.rs:807-881
ConnectTxs connect_txs(const State& s, Txn& t, const ConnectContext& c, const std::vector<FilledTransaction>& txs, uint64_t coinbase_value)
{
    auto u = std::make_shared<StateUpdate>();
    std::set<size_t> skipped;
    for (size_t idx = 0; idx < txs.size(); idx++) {
        auto& ft = txs[idx];
        if (!ft.tx->data) continue;
        switch (ft.tx->data->index()) {
        case TRADE:
            if (apply_trade(s, t, ft, *u, nullptr) == TradeResult::Skipped) skipped.insert(idx);
            break;
        case CREATE_MARKET: apply_market_creation(s, t, ft, *u, c.height); break;
        case CLAIM_DECISION: apply_decision_claim(s, t, ft, c.height); break;
        case SUBMIT_VOTE: apply_submit_vote(s, t, ft, *u, c.height); break;
        case SUBMIT_BALLOT: apply_submit_ballot(s, t, ft, *u, c.height); break;
        case TRANSFER_REPUTATION: apply_transfer_reputation(s, t, ft, c.height); break;
        case AMPLIFY_BETA: apply_amplify_beta(ft, *u); break;
        }
    }
    u->validate_all_changes(s, t);
    validate_fees(coinbase_value, txs, skipped);
    return ConnectTxs{u, skipped};
}

// block.rs:884-900
void unapply_skipped_tx_diff(AccumulatorDiff& d, const Transaction& tx)
{
    Hash32 txid = tx.txid();
    for (uint32_t vout = 0; vout < tx.outputs.size(); vout++)
        d.remove(bt::utreexo::NodeHash::some(pointed_output_hash(OutPoint::regular(txid, vout), tx.outputs[vout])));
    for (auto& [_, h] : tx.inputs) d.insert(bt::utreexo::NodeHash::some(h));
}

// block.rs:903-1042
void connect_end(const State& s, Txn& t, AccumulatorDiff& diff, const ConnectContext& c, ConnectTxs txs)
{
    if (auto undo = apply_all_changes(s, t, diff, c.height, *txs.update)) t.put(State::T_CONSOLIDATION_UNDO, u32le(c.height), enc_consolidation_undo(*undo));
    if (c.height == 0) mint_genesis(s, t, c.mainchain_timestamp, c.height, c.genesis_ts);
    else process_block_pricing(s, t, c.height, c.mainchain_timestamp, c.genesis_ts);
    std::map<uint32_t, std::vector<DecisionId>> periods;
    for (auto& id : voting_decisions_needing_resolution(s, t, c.current_period)) periods[id.voting_period()].push_back(id);
    std::vector<ConsensusUndoEntry> cundo;
    for (auto& [vp, ids] : periods)
        if (auto e = calculate_and_store_consensus(s, t, vp, ids, c.mainchain_timestamp, c.height)) cundo.push_back(std::move(*e));
    if (!cundo.empty()) t.put(State::T_CONSENSUS_UNDO, u32le(c.height), enc_consensus_undo(cundo));
    auto sundo = transition_and_payout_resolved_markets(s, t, diff, c.height);
    if (!sundo.empty()) t.put(State::T_SETTLEMENT_UNDO, u32le(c.height), enc_settlement_undo(sundo));
    if (!txs.skipped.empty()) {
        std::vector<uint32_t> idx;
        for (auto i : txs.skipped) idx.push_back(uint32_t(i));
        std::sort(idx.begin(), idx.end());
        t.put(State::T_SKIPPED_UNDO, u32le(c.height), enc_u32s(idx));
    }
    Writer w;
    w.u64(c.mainchain_timestamp);
    t.put(State::T_MAIN_TS, UNIT, w.buf);
}

// ------------------------------------------------------------------ disconnect
// block.rs:1233-1315
static void revert_consolidation(const State& s, Txn& t, AccumulatorDiff& diff, const ConsolidationUndoData& undo)
{
    for (auto it = undo.entries.rbegin(); it != undo.entries.rend(); ++it) {
        auto& e = *it;
        if (e.new_treasury_utxo) s.delete_utxo(t, *e.new_treasury_utxo, diff);
        if (e.new_fee_utxo) s.delete_utxo(t, *e.new_fee_utxo, diff);
        for (auto& op : e.sell_payout_utxos) s.delete_utxo(t, op, diff);
        for (auto& op : e.buy_change_utxos) s.delete_utxo(t, op, diff);
        for (auto& [op, o] : e.old_treasury_utxos) s.insert_utxo(t, op, o, diff);
        for (auto& [op, o] : e.old_fee_utxos) s.insert_utxo(t, op, o, diff);
        if (e.old_treasury_pointer) s.set_market_funds_utxo(t, e.market_id, false, *e.old_treasury_pointer);
        else s.clear_market_funds_utxo(t, e.market_id, false);
        if (e.old_fee_pointer) s.set_market_funds_utxo(t, e.market_id, true, *e.old_fee_pointer);
        else s.clear_market_funds_utxo(t, e.market_id, true);
    }
    for (auto& op : undo.sell_input_change_utxos) s.delete_utxo(t, op, diff);
}

// block.rs:1046-1089
std::set<uint32_t> disconnect_begin(const State& s, Txn& t, AccumulatorDiff& diff, uint32_t height)
{
    if (auto v = t.get(State::T_SETTLEMENT_UNDO, u32le(height))) {
        revert_settlement(s, t, diff, dec_settlement_undo(*v), height);
        t.del(State::T_SETTLEMENT_UNDO, u32le(height));
    }
    if (auto v = t.get(State::T_CONSENSUS_UNDO, u32le(height))) {
        revert_consensus(s, t, dec_consensus_undo(*v));
        t.del(State::T_CONSENSUS_UNDO, u32le(height));
    }
    if (auto v = t.get(State::T_CONSOLIDATION_UNDO, u32le(height))) {
        revert_consolidation(s, t, diff, dec_consolidation_undo(*v));
        t.del(State::T_CONSOLIDATION_UNDO, u32le(height));
    }
    std::set<uint32_t> skipped;
    if (auto v = t.get(State::T_SKIPPED_UNDO, u32le(height)))
        for (auto i : dec_u32s(*v)) skipped.insert(i);
    return skipped;
}

/** fill_transaction_from_stxos (mod.rs:526-554), in reverse input order with the
 *  inpoint assertion (theirs panics on a mismatch). */
static FilledTransaction fill_from_stxos(const State& s, const Txn& t, const Transaction& tx)
{
    FilledTransaction ft{&tx, {}, std::nullopt};
    Hash32 txid = tx.txid();
    std::vector<Output> rev;
    for (size_t vin = tx.inputs.size(); vin-- > 0;) {
        auto& op = tx.inputs[vin].first;
        auto st = s.stxo(t, op);
        if (!st) throw StateError("stxo " + op.to_string() + " doesn't exist");
        if (!(st->inpoint.kind == InPoint::Regular && st->inpoint.id == txid && st->inpoint.vin == uint32_t(vin)))
            throw StateError("assertion failed: stxo inpoint", true);
        rev.push_back(st->output);
    }
    ft.spent_utxos.assign(rev.rbegin(), rev.rend());
    return ft;
}

// block.rs:1635-1779
static TradeShareDelta revert_trade_market_state(const State& s, Txn& t, AccumulatorDiff& diff, const FilledTransaction& ft)
{
    auto* tr = ft.get<TxTrade>();
    bool is_buy = tr->is_buy();
    uint32_t height = s.height(t).value_or(0);
    auto m = s.market(t, tr->market_id);
    if (!m) throw invalid_tx("Market not found during trade revert");
    int64_t delta = int64_t(tr->shares_abs());
    size_t outcome = tr->outcome_index;
    double beta = market_beta(*m);
    std::string err;
    if (outcome >= m->shares.size()) throw StateError("index out of bounds", true);
    auto pre = m->shares;
    if (is_buy) {
        pre[outcome] = int64_t(uint64_t(pre[outcome]) - uint64_t(delta));
        auto base = math::update_cost(pre, m->shares, beta, &err);
        if (!base) throw invalid_tx("LMSR calc failed during buy trade revert: " + err);
        auto bc = math::buy_cost(*base, m->trading_fee, &err);
        if (!bc) throw invalid_tx("Buy cost calc failed during trade revert: " + err);
        if (outcome >= m->outcome_volumes_sats.size()) throw invalid_tx("Volume revert failed during buy trade revert: InvalidOutcomeIndex");
        auto& ov = m->outcome_volumes_sats[outcome];
        ov = ov > bc->total_cost_sats ? ov - bc->total_cost_sats : 0;
        m->total_volume_sats = m->total_volume_sats > bc->total_cost_sats ? m->total_volume_sats - bc->total_cost_sats : 0;
    } else {
        pre[outcome] = int64_t(uint64_t(pre[outcome]) + uint64_t(delta));
        auto base = math::update_cost(m->shares, pre, beta, &err);
        if (!base) throw invalid_tx("LMSR calc failed during sell trade revert: " + err);
        auto sp = math::sell_proceeds(*base, m->trading_fee, &err);
        if (!sp) throw invalid_tx("Sell proceeds calc failed during trade revert: " + err);
        if (outcome >= m->outcome_volumes_sats.size()) throw invalid_tx("Volume revert failed during sell trade revert: InvalidOutcomeIndex");
        auto& ov = m->outcome_volumes_sats[outcome];
        ov = ov > sp->gross_proceeds_sats ? ov - sp->gross_proceeds_sats : 0;
        m->total_volume_sats = m->total_volume_sats > sp->gross_proceeds_sats ? m->total_volume_sats - sp->gross_proceeds_sats : 0;
    }
    m->shares = pre;
    m->version += 1;
    m->last_updated_height = height;
    s.update_market(t, *m);
    Hash32 txid = ft.tx->txid();
    if (is_buy) {
        s.delete_utxo(t, buy_change_outpoint(tr->market_id, tr->trader, txid), diff);
    } else {
        s.delete_utxo(t, sell_payout_outpoint(tr->market_id, tr->trader, txid), diff);
        s.delete_utxo(t, sell_input_change_outpoint(tr->trader, txid), diff);
    }
    return TradeShareDelta{tr->trader, tr->market_id, tr->outcome_index, is_buy ? -delta : delta};
}

// block.rs:1092-1131
void disconnect_tx(const State& s, Txn& t, AccumulatorDiff& diff, const Transaction& tx, std::vector<TradeShareDelta>& deltas)
{
    if (!tx.data) return;
    FilledTransaction ft = fill_from_stxos(s, t, tx);
    switch (tx.data->index()) {
    case CLAIM_DECISION:
        for (auto& e : std::get<ClaimDecisionPayload>(*tx.data).decisions) {
            DecisionId id;
            id.b = e.decision_id_bytes;
            s.revert_decision_claim(t, id);
        }
        break;
    case CREATE_MARKET: {
        auto& v = std::get<TxCreateMarket>(*tx.data);
        Address creator = extract_creator(ft);
        s.delete_market(t, compute_market_id(v.title, v.description, creator, v.dimension_specs));
        for (auto& p : v.new_claims)
            for (auto& e : p.decisions) {
                DecisionId id;
                id.b = e.decision_id_bytes;
                s.revert_decision_claim(t, id);
            }
        break;
    }
    case TRADE: deltas.push_back(revert_trade_market_state(s, t, diff, ft)); break;
    case SUBMIT_VOTE: {
        auto& v = std::get<TxSubmitVote>(*tx.data);
        DecisionId id;
        id.b = v.decision_id_bytes;
        s.delete_vote(t, id.voting_period(), v.voter, id);
        break;
    }
    case SUBMIT_BALLOT: {
        auto& b = std::get<TxSubmitBallot>(*tx.data);
        for (auto& item : b.votes) {
            DecisionId id;
            id.b = item.decision_id_bytes;
            s.delete_vote(t, id.voting_period(), b.voter, id);
        }
        break;
    }
    case TRANSFER_REPUTATION: break;
    case AMPLIFY_BETA: {
        auto& a = std::get<TxAmplifyBeta>(*tx.data);
        auto m = s.market(t, a.market_id);
        if (!m) throw invalid_tx("Market not found during amplify_beta revert");
        if (m->liquidity_base_sats < a.amount) throw invalid_tx("Liquidity base underflow during amplify_beta revert");
        m->liquidity_base_sats -= a.amount;
        s.update_market(t, *m);
        break;
    }
    }
}

// block.rs:1134-1214
void disconnect_end(const State& s, Txn& t, uint32_t height, const std::set<uint32_t>& skipped, const std::vector<TradeShareDelta>& deltas)
{
    if (!skipped.empty()) t.del(State::T_SKIPPED_UNDO, u32le(height));
    std::map<std::pair<Address, MarketId>, std::map<uint32_t, int64_t>> batched;
    for (auto& d : deltas) {
        auto& v = batched[{d.address, d.market_id}][d.outcome_index];
        v = int64_t(uint64_t(v) + uint64_t(d.share_delta));
    }
    for (auto& [key, changes] : batched)
        for (auto& [outcome, net] : changes) {
            if (net > 0) s.add_shares(t, key.first, key.second, outcome, net, height);
            else if (net < 0) s.remove_shares(t, key.first, key.second, outcome, int64_t(uint64_t(0) - uint64_t(net)), height);
        }
    if (auto v = t.get(State::T_REP_TRANSFER_UNDO, u32le(height))) {
        auto entries = dec_rep_undo(*v);
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            s.set_reputation(t, it->sender, it->sender_pre);
            s.set_reputation(t, it->receiver, it->receiver_pre);
        }
        t.del(State::T_REP_TRANSFER_UNDO, u32le(height));
    }
    if (height > 0) rollback_decision_states_to_height(s, t, height - 1);
    if (auto v = t.get(State::T_MINTING_UNDO, u32le(height))) {
        Reader r(*v);
        delete_periods_above(s, t, r.u32());
        t.del(State::T_MINTING_UNDO, u32le(height));
    }
    restore_period_pricing_undo(s, t, height);
}

// ------------------------------------------------------------------ trade simulation (block.rs:1781-1808)
struct TradeSimulation::Impl {
    StateUpdate u;
};
TradeSimulation::TradeSimulation() : impl_(std::make_unique<Impl>()) {}
TradeSimulation::~TradeSimulation() = default;

bool TradeSimulation::apply(const State& s, const Archive& a, const Txn& t, const FilledTransaction& ft, std::string* skip_reason)
{
    if (!ft.tx->data) return true;
    if (ft.get<TxTrade>()) {
        validate_trade(s, a, t, ft);
        return apply_trade(s, t, ft, impl_->u, skip_reason) == TradeResult::Applied;
    }
    if (ft.get<TxAmplifyBeta>()) {
        apply_amplify_beta(ft, impl_->u);
        return true;
    }
    return true;
}

} // namespace hivemind
