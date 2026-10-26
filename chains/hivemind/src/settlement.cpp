// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// lib/state/markets/{market.rs, payouts.rs} and the settlement half of database.rs:
// market construction and ids, final prices, fixed-redemption share payouts, the 2:1:1
// fee split, and their undo. Floats: products and comparisons are plain IEEE (no
// contraction); ndarray `sum()` and the proportional allocation run in their code.
#include "state_internal.h"

#include <hivemind/fmt.h>
#include <hivemind/math.h>

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace hivemind {

using bt::kv::Txn;

static std::string did_debug(const DecisionId& d)
{
    return "DecisionId([" + std::to_string(d.b[0]) + ", " + std::to_string(d.b[1]) + ", " + std::to_string(d.b[2]) + "])";
}

/** Rust `x as u64` (saturating; NaN -> 0). */
static uint64_t f64_as_u64(double x)
{
    if (std::isnan(x) || x <= 0) return 0;
    if (x >= 18446744073709551616.0) return UINT64_MAX;
    return uint64_t(x);
}

// ------------------------------------------------------------------ market.rs
MarketId compute_market_id(const std::string& title, const std::string& description, const Address& creator, const std::vector<DimensionSpec>& dims)
{
    Writer w;
    w.borsh_bytes(ByteSpan((const uint8_t*)title.data(), title.size()));
    w.borsh_bytes(ByteSpan((const uint8_t*)description.data(), description.size()));
    w.raw(creator.b);
    w.borsh_len(dims.size());
    for (auto& d : dims) borsh(w, d);
    Hash32 h = bt::blake3(w.buf);
    MarketId m;
    std::memcpy(m.b.data(), h.data(), 6);
    return m;
}

static Address market_address(const MarketId& m, const char* domain)
{
    bt::Blake3 h;
    h.update(ByteSpan((const uint8_t*)domain, std::strlen(domain)));
    h.update(ByteSpan(m.b.data(), 6));
    Hash32 d = h.finalize();
    Address a;
    std::memcpy(a.b.data(), d.data(), 20);
    return a;
}
Address market_treasury_address(const MarketId& m) { return market_address(m, "MARKET_TREASURY_ADDRESS"); }
Address market_author_fee_address(const MarketId& m) { return market_address(m, "MARKET_AUTHOR_FEE_ADDRESS"); }

// payouts.rs generate_share_payout_outpoint
OutPoint share_payout_outpoint(const MarketId& m, const Address& a, uint32_t height, uint32_t seq)
{
    bt::Blake3 h;
    h.update(ByteSpan((const uint8_t*)"SHARE_PAYOUT", 12));
    h.update(ByteSpan(m.b.data(), 6));
    h.update(a.b.span());
    Writer w;
    w.u32(height);
    w.u32(seq);
    h.update(w.buf);
    return OutPoint::payout(h.finalize(), seq);
}

// market.rs:206-266
std::optional<std::vector<std::vector<uint64_t>>> generate_state_combos(const std::vector<DimensionSpec>& dims,
                                                                        const std::map<DecisionId, Decision>& decisions, std::string* err)
{
    auto fail = [&](const std::string& m) -> std::optional<std::vector<std::vector<uint64_t>>> {
        if (err) *err = m;
        return std::nullopt;
    };
    if (dims.empty()) return fail("Invalid market dimensions");
    std::set<DecisionId> seen;
    for (auto& d : dims)
        if (!seen.insert(d.id).second) return fail("Duplicate decision in market dimensions: " + did_debug(d.id));
    std::vector<uint64_t> ranges, tradeable;
    for (auto& d : dims) {
        auto it = decisions.find(d.id);
        if (it == decisions.end()) return fail("Decision not found: " + did_debug(d.id));
        if (d.kind == DimensionSpec::Single) {
            ranges.push_back(3);
            tradeable.push_back(2);
        } else {
            auto n = it->second.option_count();
            if (!n) return fail("Invalid market dimensions");
            if (*n < 2) return fail("Invalid market dimensions");
            ranges.push_back(*n);
            tradeable.push_back(*n);
        }
    }
    uint64_t outcomes = 1;
    for (auto r : tradeable) {
        unsigned __int128 p = (unsigned __int128)outcomes * r;
        if (p > UINT64_MAX) return fail("Too many market states: " + std::to_string(UINT64_MAX) + " (max 256)");
        outcomes = uint64_t(p);
    }
    if (outcomes > math::MAX_OUTCOMES) return fail("Too many market states: " + std::to_string(outcomes) + " (max 256)");
    // markets.rs cartesian_product
    std::vector<std::vector<uint64_t>> res{{}};
    for (auto dim : ranges) {
        std::vector<std::vector<uint64_t>> nr;
        nr.reserve(res.size() * dim);
        for (auto& c : res)
            for (uint64_t v = 0; v < dim; v++) {
                auto nc = c;
                nc.push_back(v);
                nr.push_back(std::move(nc));
            }
        res = std::move(nr);
    }
    return res;
}

std::vector<std::pair<size_t, const std::vector<uint64_t>*>> Market::valid_state_combos() const
{
    std::vector<std::pair<size_t, const std::vector<uint64_t>*>> r;
    for (size_t i = 0; i < state_combos.size(); i++) {
        auto& c = state_combos[i];
        bool ok = true;
        for (size_t d = 0; d < c.size() && d < dimension_specs.size(); d++)
            if (dimension_specs[d].kind == DimensionSpec::Single && c[d] >= 2) ok = false;
        if (ok) r.emplace_back(i, &c);
    }
    return r;
}

std::optional<Market> build_market(const std::string& title, const std::string& description, const std::vector<std::string>& tags,
                                   const Address& creator, const std::vector<DimensionSpec>& dims, double trading_fee, uint8_t sel,
                                   uint8_t ord, uint8_t diff, uint32_t created_at, const std::map<DecisionId, Decision>& decisions,
                                   std::string* err)
{
    auto combos = generate_state_combos(dims, decisions, err);
    if (!combos) return std::nullopt;
    if (dims.empty() || combos->empty()) {
        if (err) *err = "Invalid market dimensions";
        return std::nullopt;
    }
    Market m;
    m.title = title;
    m.description = description;
    m.tags = tags;
    m.creator_address = creator;
    m.dimension_specs = dims;
    for (auto& d : dims) m.decision_ids.push_back(d.id);
    m.state_combos = std::move(*combos);
    m.created_at_height = created_at;
    m.expires_at_height = std::nullopt;
    m.tau_from_now = 5; // calculate_max_tau: 5 for every decision present, else 5
    m.market_state = MarketState::Trading;
    m.trading_fee = trading_fee;
    m.liquidity_base_sats = 0;
    m.version = 0;
    m.last_updated_height = created_at;
    m.total_volume_sats = 0;
    m.tx_pow_hash_selector = sel;
    m.tx_pow_ordering = ord;
    m.tx_pow_difficulty = diff;
    size_t n = m.valid_state_combos().size();
    m.shares.assign(n, 0);
    m.final_prices.assign(n, 0.0);
    m.outcome_volumes_sats.assign(n, 0);
    m.storage_fee_sats = 1000 + uint64_t(n) * uint64_t(n);
    m.id = compute_market_id(title, description, creator, dims);
    return m;
}

// market.rs:505-596
std::vector<double> Market::calculate_final_prices(const std::map<DecisionId, double>& outcomes, const std::map<DecisionId, Decision>& decisions) const
{
    std::vector<std::vector<double>> axes;
    for (auto& spec : dimension_specs) {
        auto oit = outcomes.find(spec.id);
        auto dit = decisions.find(spec.id);
        if (spec.kind == DimensionSpec::Single) {
            double o = oit != outcomes.end() ? oit->second : 0.5;
            bool scaled = dit != decisions.end() && dit->second.is_scaled();
            if (scaled) axes.push_back({1.0 - o, o});
            else if (o > 0.7) axes.push_back({0.0, 1.0});
            else if (o < 0.3) axes.push_back({1.0, 0.0});
            else axes.push_back({0.5, 0.5});
        } else {
            size_t n = 2;
            if (dit != decisions.end())
                if (auto c = dit->second.option_count()) n = *c;
            std::optional<size_t> winner;
            if (oit != outcomes.end()) {
                double o = oit->second;
                if (!(o == 0.5 || !std::isfinite(o))) {
                    double r = std::round(o);
                    if (!(std::fabs(o - r) > DBL_EPSILON)) {
                        // `rounded as i64` (finite, so saturating cast)
                        int64_t idx = r >= 9223372036854775808.0 ? INT64_MAX : r <= -9223372036854775808.0 ? INT64_MIN : int64_t(r);
                        if (idx >= 0 && uint64_t(idx) < n) winner = size_t(idx);
                    }
                }
            }
            std::vector<double> axis;
            if (winner) {
                for (size_t k = 0; k < n; k++) axis.push_back(k == *winner ? 1.0 : 0.0);
            } else {
                double eq = 1.0 / double(n);
                for (size_t k = 0; k < n; k++) axis.push_back(eq);
            }
            axes.push_back(std::move(axis));
        }
    }
    auto valid = valid_state_combos();
    std::vector<double> prices(valid.size(), 0.0);
    for (size_t i = 0; i < valid.size(); i++) {
        double joint = 1.0;
        auto& combo = *valid[i].second;
        for (size_t dim = 0; dim < combo.size(); dim++)
            if (dim < axes.size() && combo[dim] < axes[dim].size()) joint *= axes[dim][combo[dim]];
        prices[i] = joint;
    }
    math::ndarray_normalize(prices);
    return prices;
}

// ------------------------------------------------------------------ payouts.rs
// payouts.rs:11-38
static bool is_vote_correct(const VoteValue& v, double consensus, const Decision& d)
{
    switch (v.kind) {
    case VoteValue::Abstain: return false;
    case VoteValue::Binary: return (v.b ? 1.0 : 0.0) == consensus;
    case VoteValue::Scalar:
        if (d.is_scaled()) return math::round_to_precision(std::fabs(v.s - consensus), 6) <= 0.05;
        return v.s == consensus;
    case VoteValue::Categorical:
        if (double(v.c) == consensus) return true;
        return consensus == 0.5 && d.inconclusive_index() == std::optional<uint16_t>(v.c);
    }
    return false;
}

// payouts.rs:40-95
static std::map<DecisionId, std::vector<std::pair<Address, double>>> find_correct_voters(const State& s, const Txn& t, const std::vector<DecisionId>& ids,
                                                                                         const std::map<DecisionId, double>& outcomes,
                                                                                         const std::map<DecisionId, Decision>& decisions)
{
    std::map<DecisionId, std::vector<std::pair<Address, double>>> res;
    for (auto& id : ids) {
        auto oit = outcomes.find(id);
        auto dit = decisions.find(id);
        if (oit == outcomes.end() || dit == decisions.end()) {
            res[id] = {};
            continue;
        }
        std::vector<std::pair<Address, double>> cv;
        for (auto& [k, e] : s.votes_for_period(t, id.voting_period())) {
            if (k.decision_id != id) continue;
            if (!is_vote_correct(e.value, oit->second, dit->second)) continue;
            double rep = s.reputation(t, k.voter);
            if (rep > 0.0) cv.emplace_back(k.voter, rep);
        }
        std::stable_sort(cv.begin(), cv.end(), [](auto& a, auto& b) { return a.first < b.first; });
        res[id] = cv;
    }
    return res;
}

static std::vector<std::pair<size_t, uint64_t>> allocate(const std::vector<double>& w, uint64_t pool, const char* what)
{
    std::string err;
    auto r = math::allocate_u64(w, pool, &err);
    if (!r) throw invalid_tx(std::string(what) + err);
    return *r;
}

// payouts.rs:97-230
static std::vector<FeePayoutRecord> fee_distribution(const State& s, const Txn& t, const Market& m, uint64_t fee_sats,
                                                     const std::map<DecisionId, double>& outcomes, const std::map<DecisionId, Decision>& decisions)
{
    if (fee_sats == 0) return {};
    uint64_t voter_pool = 0, author_pool = 0, market_pool = 0;
    for (auto& [k, amt] : allocate({2.0, 1.0, 1.0}, fee_sats, "Fee pool split failed: ")) {
        if (k == 0) voter_pool = amt;
        else if (k == 1) author_pool = amt;
        else market_pool = amt;
    }
    std::vector<FeePayoutRecord> out;
    auto correct = find_correct_voters(s, t, m.decision_ids, outcomes, decisions);
    if (voter_pool > 0) {
        std::vector<double> w(m.decision_ids.size(), 1.0);
        for (auto& [k, sub] : allocate(w, voter_pool, "Voter decision pool split failed: ")) {
            auto& voters = correct[m.decision_ids[k]];
            if (voters.empty()) {
                market_pool += sub;
                continue;
            }
            std::vector<double> vw;
            for (auto& [_, rep] : voters) vw.push_back(rep);
            for (auto& [vk, amt] : allocate(vw, sub, "Voter payout allocation failed: ")) out.push_back({voters[vk].first, amt, FeeRole::CorrectVoter});
        }
    }
    if (author_pool > 0) {
        std::set<Address> uniq;
        for (auto& id : m.decision_ids)
            if (auto it = decisions.find(id); it != decisions.end()) {
                Address a;
                a.b = it->second.market_maker_pubkey_hash;
                uniq.insert(a);
            }
        std::vector<Address> authors(uniq.begin(), uniq.end());
        std::vector<double> w(authors.size(), 1.0);
        for (auto& [k, amt] : allocate(w, author_pool, "Decision author allocation failed: ")) out.push_back({authors[k], amt, FeeRole::DecisionAuthor});
    }
    if (market_pool > 0) out.push_back({m.creator_address, market_pool, FeeRole::MarketAuthor});
    std::stable_sort(out.begin(), out.end(), [](const FeePayoutRecord& a, const FeePayoutRecord& b) {
        if (a.fee_role != b.fee_role) return uint8_t(a.fee_role) < uint8_t(b.fee_role);
        return a.address < b.address;
    });
    return out;
}

// database.rs:1205-1232
static void reclaim_payout_roundups(std::vector<SharePayoutRecord>& p, uint64_t excess)
{
    auto value = [](const SharePayoutRecord& r) { return std::fmax(double(r.shares_redeemed) * r.final_price, 0.0); };
    auto frac = [&](const SharePayoutRecord& r) {
        double v = value(r);
        return v - std::floor(v);
    };
    std::vector<size_t> order(p.size());
    for (size_t i = 0; i < p.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        double fa = frac(p[a]), fb = frac(p[b]);
        // frac(b).partial_cmp(frac(a)).unwrap_or(Equal): descending
        if (std::isnan(fa) || std::isnan(fb)) return false;
        return fb < fa;
    });
    for (size_t i : order) {
        if (excess == 0) break;
        uint64_t fl = f64_as_u64(std::floor(value(p[i])));
        if (p[i].payout_sats > fl) {
            p[i].payout_sats -= 1;
            excess -= 1;
        }
    }
}

// database.rs:881-969
static MarketPayoutSummary calculate_share_payouts(const State& s, const Txn& t, const Market& m, uint32_t h, const std::map<DecisionId, double>& outcomes,
                                                   const std::map<DecisionId, Decision>& decisions)
{
    uint64_t treasury = s.market_funds_sats(t, m.id, false);
    std::vector<SharePayoutRecord> payouts;
    for (auto& [a, acc] : s.all_share_accounts(t))
        for (auto& [k, shares] : acc.positions) {
            if (k.first != m.id) continue;
            if (k.second >= m.final_prices.size()) throw StateError("index out of bounds", true);
            double price = m.final_prices[k.second];
            SharePayoutRecord r;
            r.market_id = m.id;
            r.address = a;
            r.outcome_index = k.second;
            r.shares_redeemed = shares;
            r.final_price = price;
            r.payout_sats = f64_as_u64(std::fmax(std::round(double(shares) * price), 0.0));
            payouts.push_back(r);
        }
    uint64_t total = 0;
    for (auto& p : payouts) total += p.payout_sats; // Iterator::sum: wraps in release
    if (total > treasury) {
        reclaim_payout_roundups(payouts, total - treasury);
        total = 0;
        for (auto& p : payouts) total += p.payout_sats;
    }
    if (total > treasury)
        throw invalid_tx("Market payout obligation " + std::to_string(total) + " exceeds treasury " + std::to_string(treasury) + " for market " + m.id.to_hex());
    uint64_t refund = treasury - total;
    MarketPayoutSummary sum;
    sum.market_id = m.id;
    sum.treasury_distributed = total;
    if (refund > 0) sum.creator_refund = std::make_pair(m.creator_address, refund);
    for (auto& p : payouts)
        if (p.payout_sats > 0) sum.shareholder_count++;
    uint64_t fee_sats = s.market_funds_sats(t, m.id, true);
    sum.fee_payouts = fee_distribution(s, t, m, fee_sats, outcomes, decisions);
    for (auto& f : sum.fee_payouts) sum.total_fees_distributed += f.amount_sats;
    sum.payouts = std::move(payouts);
    sum.block_height = h;
    return sum;
}

static Output value_output(const Address& a, uint64_t v)
{
    Output o;
    o.address = a;
    o.content.kind = Content::Value;
    o.content.value = v;
    return o;
}

// database.rs:971-1080
static void apply_payouts(const State& s, Txn& t, AccumulatorDiff& diff, const MarketPayoutSummary& sum, uint32_t h)
{
    uint32_t seq = 0;
    for (auto& p : sum.payouts) {
        if (p.payout_sats > 0) s.insert_utxo(t, share_payout_outpoint(p.market_id, p.address, h, seq), value_output(p.address, p.payout_sats), diff);
        s.remove_shares(t, p.address, p.market_id, p.outcome_index, p.shares_redeemed, h);
        seq += 1;
    }
    for (auto& f : sum.fee_payouts) {
        s.insert_utxo(t, share_payout_outpoint(sum.market_id, f.address, h, seq), value_output(f.address, f.amount_sats), diff);
        seq += 1;
    }
    if (sum.creator_refund)
        s.insert_utxo(t, share_payout_outpoint(sum.market_id, sum.creator_refund->first, h, seq),
                      value_output(sum.creator_refund->first, sum.creator_refund->second), diff);
    if (auto op = s.market_funds_utxo(t, sum.market_id, false)) {
        s.delete_utxo(t, *op, diff);
        s.clear_market_funds_utxo(t, sum.market_id, false);
    }
    if (auto op = s.market_funds_utxo(t, sum.market_id, true)) {
        s.delete_utxo(t, *op, diff);
        s.clear_market_funds_utxo(t, sum.market_id, true);
    }
}

// database.rs:1082-1146
static void revert_payouts(const State& s, Txn& t, AccumulatorDiff& diff, const MarketPayoutSummary& sum, uint32_t h)
{
    uint32_t seq = 0;
    for (auto& p : sum.payouts) {
        if (p.payout_sats > 0) s.delete_utxo(t, share_payout_outpoint(p.market_id, p.address, h, seq), diff);
        s.add_shares(t, p.address, p.market_id, p.outcome_index, p.shares_redeemed, h);
        seq += 1;
    }
    for (auto& f : sum.fee_payouts) {
        s.delete_utxo(t, share_payout_outpoint(sum.market_id, f.address, h, seq), diff);
        seq += 1;
    }
    if (sum.creator_refund) s.delete_utxo(t, share_payout_outpoint(sum.market_id, sum.creator_refund->first, h, seq), diff);
}

// database.rs:356-516
std::vector<SettlementUndoEntry> transition_and_payout_resolved_markets(const State& s, Txn& t, AccumulatorDiff& diff, uint32_t h)
{
    std::vector<SettlementUndoEntry> undo;
    auto markets = s.markets_by_state(t, MarketState::Trading);
    std::stable_sort(markets.begin(), markets.end(), [](const Market& a, const Market& b) { return a.id < b.id; });
    for (auto& m : markets) {
        if (m.decision_ids.empty()) continue;
        bool all = true;
        std::map<DecisionId, double> outcomes;
        std::map<DecisionId, Decision> decisions;
        for (auto& id : m.decision_ids) {
            if (s.decision_state(t, id) != DecisionState::Resolved) {
                all = false;
                break;
            }
            if (auto o = s.decision_outcome(t, id)) outcomes[id] = o->outcome_value;
            if (auto e = s.decision_entry(t, id); e && e->decision) decisions[id] = *e->decision;
        }
        if (!all) continue;
        SettlementUndoEntry e;
        e.pre_settlement_market = m;
        if (auto op = s.market_funds_utxo(t, m.id, false))
            if (auto o = s.utxo(t, *op)) e.treasury_utxo = std::make_pair(*op, *o);
        if (auto op = s.market_funds_utxo(t, m.id, true))
            if (auto o = s.utxo(t, *op)) e.fee_utxo = std::make_pair(*op, *o);
        Market mk = m;
        mk.final_prices = mk.calculate_final_prices(outcomes, decisions);
        mk.version += 1;
        mk.last_updated_height = h;
        auto summary = calculate_share_payouts(s, t, mk, h, outcomes, decisions);
        apply_payouts(s, t, diff, summary, h);
        if (!can_transition(mk.market_state, MarketState::Settled)) throw StateError("Database consistency error: Failed to settle market");
        mk.market_state = MarketState::Settled;
        mk.version += 1;
        mk.last_updated_height = h;
        s.update_market(t, mk);
        e.payout_summary = std::move(summary);
        undo.push_back(std::move(e));
    }
    return undo;
}

// block.rs:1318-1378
void revert_settlement(const State& s, Txn& t, AccumulatorDiff& diff, const std::vector<SettlementUndoEntry>& undo, uint32_t h)
{
    for (auto it = undo.rbegin(); it != undo.rend(); ++it) {
        revert_payouts(s, t, diff, it->payout_summary, h);
        if (it->treasury_utxo) {
            s.insert_utxo(t, it->treasury_utxo->first, it->treasury_utxo->second, diff);
            s.set_market_funds_utxo(t, it->pre_settlement_market.id, false, it->treasury_utxo->first);
        }
        if (it->fee_utxo) {
            s.insert_utxo(t, it->fee_utxo->first, it->fee_utxo->second, diff);
            s.set_market_funds_utxo(t, it->pre_settlement_market.id, true, it->fee_utxo->first);
        }
        s.restore_market(t, it->pre_settlement_market);
    }
}

} // namespace hivemind
