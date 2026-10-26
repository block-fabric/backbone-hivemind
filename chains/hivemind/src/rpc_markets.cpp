// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// JSON-RPC: decisions, markets, trades, votes, reputation and openapi_schema
// (truthcoin-dc v0.20.0 rpc-api/{lib,markets}.rs, app/rpc_server.rs). Response objects
// carry their struct fields in declaration order; f64 as serde_json (ryu) writes them;
// errors as their custom_err (code -1, the error chain text). Every price, cost and fee is
// computed by THEIR math (hivemind::math over lib/math) and State getters.
#include <hivemind/app.h>
#include <hivemind/fmt.h>
#include <hivemind/json.h>
#include <hivemind/math.h>

#include "state_internal.h"

#include <bt/crypto.h>
#include <bt/log.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

namespace hivemind {

using bt::jsonrpc::app_error;
using bt::jsonrpc::Params;
using bt::kv::Txn;

namespace {

#include "openapi_docs.inc"

template <class F>
bt::jsonrpc::Handler P(F f)
{
    return [f](const Params& p) -> UniValue {
        try {
            return f(p);
        } catch (const json::ParseError& e) {
            throw bt::jsonrpc::invalid_params(e.what());
        } catch (const bt::jsonrpc::RpcError&) {
            throw;
        } catch (const std::exception& e) {
            throw app_error(e.what());
        }
    };
}

UniValue obj() { return UniValue(UniValue::VOBJ); }
UniValue arr() { return UniValue(UniValue::VARR); }
UniValue u64v(uint64_t v) { return UniValue(v); }
UniValue opt_str(const std::optional<std::string>& s) { return s ? UniValue(*s) : UniValue(); }
UniValue strs(const std::vector<std::string>& v)
{
    UniValue a = arr();
    for (auto& s : v) a.push_back(s);
    return a;
}

// ---- request parsing (serde Deserialize of the request structs; unknown fields ignored)
[[noreturn]] void pfail(const std::string& m) { throw json::ParseError(m); }
const UniValue& req_field(const UniValue& v, const char* k)
{
    if (!v.isObject()) pfail("invalid type: expected a struct");
    if (!v.exists(k)) pfail(std::string("missing field `") + k + "`");
    return v.find_value(k);
}
const UniValue& opt_field(const UniValue& v, const char* k)
{
    if (!v.isObject()) pfail("invalid type: expected a struct");
    return v.find_value(k); // null when absent
}
std::optional<std::string> parse_opt_str(const UniValue& v) { return v.isNull() ? std::nullopt : std::optional<std::string>(json::parse_str(v)); }
std::optional<std::vector<std::string>> parse_opt_strs(const UniValue& v)
{
    if (v.isNull()) return std::nullopt;
    if (!v.isArray()) pfail("invalid type: expected a sequence");
    std::vector<std::string> r;
    for (auto& x : v.getValues()) r.push_back(json::parse_str(x));
    return r;
}
std::optional<uint64_t> parse_opt_u64(const UniValue& v) { return v.isNull() ? std::nullopt : std::optional<uint64_t>(json::parse_u64(v)); }
std::optional<uint32_t> parse_opt_u32(const UniValue& v) { return v.isNull() ? std::nullopt : std::optional<uint32_t>(json::parse_u32(v)); }
std::optional<double> parse_opt_f64(const UniValue& v) { return v.isNull() ? std::nullopt : std::optional<double>(json::parse_f64(v)); }
std::optional<bool> parse_opt_bool(const UniValue& v) { return v.isNull() ? std::nullopt : std::optional<bool>(json::parse_bool(v)); }
std::optional<uint8_t> parse_opt_u8(const UniValue& v)
{
    if (v.isNull()) return std::nullopt;
    uint64_t x = json::parse_u64(v);
    if (x > 255) pfail("invalid value: integer `" + std::to_string(x) + "`, expected u8");
    return uint8_t(x);
}
size_t parse_usize(const UniValue& v) { return size_t(json::parse_u64(v)); }

std::optional<DecisionState> parse_decision_state(const UniValue& v)
{
    if (v.isNull()) return std::nullopt;
    std::string s = json::parse_str(v);
    static const char* names[] = {"Created", "Claimed", "Voting", "Resolved", "Invalid"};
    for (int i = 0; i < 5; i++)
        if (s == names[i]) return DecisionState(i);
    pfail("unknown variant `" + s + "`, expected one of `Created`, `Claimed`, `Voting`, `Resolved`, `Invalid`");
}

// rpc_server.rs parse_market_id
MarketId parse_market_id_str(const std::string& s)
{
    auto b = bt::hex_decode(s);
    if (!b) throw app_error("Invalid market ID hex format");
    if (b->size() != 6) throw app_error("Market ID must be exactly 6 bytes");
    MarketId m;
    std::memcpy(m.b.data(), b->data(), 6);
    return m;
}

DecisionId parse_did(const std::string& s)
{
    std::string err;
    auto d = DecisionId::from_hex(s, &err);
    if (!d) throw app_error("invalid decision ID: " + err);
    return *d;
}

// decisions/types.rs Decision::id(): blake3(borsh(maker, type, header, description, l0, l1, tags))
Hash32 decision_hash(const Decision& d)
{
    Writer w;
    w.raw(d.market_maker_pubkey_hash);
    borsh(w, d.decision_type);
    auto bstr = [&](const std::string& s) { w.borsh_bytes(ByteSpan((const uint8_t*)s.data(), s.size())); };
    auto bopt = [&](const std::optional<std::string>& s) {
        w.u8(s ? 1 : 0);
        if (s) bstr(*s);
    };
    bstr(d.header);
    bstr(d.description);
    bopt(d.option_0_label);
    bopt(d.option_1_label);
    w.u32(uint32_t(d.tags.size()));
    for (auto& t : d.tags) bstr(t);
    return bt::blake3(w.buf);
}

UniValue decision_info(const DecisionId& id, const Decision& d)
{
    UniValue o = obj();
    o.pushKV("id", bt::hex_encode(decision_hash(d)));
    o.pushKV("market_maker_pubkey_hash", bt::hex_encode(d.market_maker_pubkey_hash));
    o.pushKV("is_standard", id.is_standard());
    o.pushKV("decision_type", json::decision_type(d.decision_type));
    o.pushKV("header", d.header);
    o.pushKV("description", d.description);
    o.pushKV("tags", strs(d.tags));
    return o;
}

const char* decision_state_name(DecisionState s)
{
    static const char* names[] = {"Created", "Claimed", "Voting", "Resolved", "Invalid"};
    return names[size_t(s)];
}

const char* market_status_name(MarketState s)
{
    switch (s) {
    case MarketState::Trading: return "trading";
    case MarketState::Cancelled: return "cancelled";
    case MarketState::Invalid: return "invalid";
    case MarketState::Settled: return "settled";
    }
    return "trading";
}

std::string usize_list_debug(const std::vector<uint64_t>& v)
{
    std::string s = "[";
    for (size_t i = 0; i < v.size(); i++) s += (i ? ", " : "") + std::to_string(v[i]);
    return s + "]";
}

std::string did_debug(const DecisionId& d)
{
    return "DecisionId([" + std::to_string(d.b[0]) + ", " + std::to_string(d.b[1]) + ", " + std::to_string(d.b[2]) + "])";
}

// market.rs describe_outcome
std::optional<std::string> describe_outcome(const Market& m, const std::vector<uint64_t>& positions, const std::map<DecisionId, Decision>& decisions)
{
    if (positions.size() != m.decision_ids.size()) return std::nullopt;
    std::string out;
    for (size_t i = 0; i < m.decision_ids.size(); i++) {
        auto it = decisions.find(m.decision_ids[i]);
        if (it == decisions.end()) return std::nullopt;
        const Decision& d = it->second;
        std::string desc;
        if (d.is_categorical()) {
            auto& labels = d.decision_type.options;
            desc = d.header + ": " + (positions[i] < labels.size() ? labels[positions[i]] : std::string("Unknown"));
        } else if (d.is_scaled()) {
            if (positions[i] == 0) desc = d.header + ": " + fmt::display(d.decision_type.min) + " (Min)";
            else if (positions[i] == 1) desc = d.header + ": " + fmt::display(d.decision_type.max) + " (Max)";
            else desc = d.header + ": Abstain";
        } else {
            auto [l0, l1] = d.binary_labels();
            if (positions[i] == 0) desc = d.header + ": " + l0;
            else if (positions[i] == 1) desc = d.header + ": " + l1;
            else desc = d.header + ": Abstain";
        }
        if (i) out += ", ";
        out += desc;
    }
    return out;
}
std::string outcome_label(const Market& m, size_t state_index, const std::map<DecisionId, Decision>& decisions)
{
    if (state_index < m.state_combos.size())
        if (auto d = describe_outcome(m, m.state_combos[state_index], decisions)) return *d;
    return "Outcome " + std::to_string(state_index);
}

std::map<DecisionId, Decision> market_decisions(const State& st, const Txn& t, const Market& m)
{
    std::map<DecisionId, Decision> r;
    for (auto& id : m.decision_ids)
        if (auto e = st.decision_entry(t, id); e && e->decision) r[id] = *e->decision;
    return r;
}

double market_beta(const Market& m) { return math::derive_beta_from_liquidity(m.liquidity_base_sats, m.shares.size()); }

// trading::calculate_prices, or uniform on error (rpc_server.rs view_market / current_prices)
std::vector<double> prices_or_uniform(const std::vector<int64_t>& shares, double beta)
{
    if (auto p = math::prices(shares, beta, nullptr)) return *p;
    size_t n = shares.size();
    return n ? std::vector<double>(n, 1.0 / double(n)) : std::vector<double>();
}

UniValue market_dimensions(const Market& m, const std::map<DecisionId, Decision>& decisions)
{
    UniValue a = arr();
    for (size_t i = 0; i < m.dimension_specs.size(); i++) {
        const DecisionId& id = m.dimension_specs[i].id;
        auto it = decisions.find(id);
        if (it == decisions.end()) throw app_error("Decision not found: " + did_debug(id));
        const Decision& d = it->second;
        UniValue kind = obj();
        if (d.decision_type.kind == DecisionType::Binary) {
            auto [l0, l1] = d.binary_labels();
            kind.pushKV("type", "binary");
            kind.pushKV("lower_label", l0);
            kind.pushKV("upper_label", l1);
        } else if (d.decision_type.kind == DecisionType::Scaled) {
            kind.pushKV("type", "scaled");
            kind.pushKV("min", json::f64(d.decision_type.min));
            kind.pushKV("max", json::f64(d.decision_type.max));
            kind.pushKV("increment", json::f64(d.decision_type.increment));
            kind.pushKV("unit", UniValue());
            kind.pushKV("lower_label", d.option_0_label.value_or("Lower basis"));
            kind.pushKV("upper_label", d.option_1_label.value_or("Upper basis"));
        } else {
            kind.pushKV("type", "category");
            kind.pushKV("options", strs(d.decision_type.options));
        }
        UniValue o = obj();
        o.pushKV("dimension_index", u64v(i));
        o.pushKV("decision_id", id.to_hex());
        o.pushKV("name", d.header);
        o.pushKV("kind", kind);
        a.push_back(o);
    }
    return a;
}

UniValue market_outcomes(const Market& m, const std::map<DecisionId, Decision>& decisions, const std::vector<double>& prices)
{
    UniValue a = arr();
    auto valid = m.valid_state_combos();
    for (size_t i = 0; i < valid.size(); i++) {
        auto [full, combo] = valid[i];
        UniValue o = obj();
        o.pushKV("outcome_index", u64v(i));
        o.pushKV("label", outcome_label(m, full, decisions));
        o.pushKV("price", json::f64(i < prices.size() ? prices[i] : 0.0));
        o.pushKV("volume_sats", u64v(i < m.outcome_volumes_sats.size() ? m.outcome_volumes_sats[i] : 0));
        o.pushKV("full_state_index", u64v(full));
        UniValue c = arr();
        for (auto x : *combo) c.push_back(u64v(x));
        o.pushKV("coordinates", c);
        a.push_back(o);
    }
    return a;
}

std::string pct1(double x)
{
    char b[64];
    std::snprintf(b, sizeof b, "%.1f", x);
    return b;
}

// rpc_server.rs view_market
UniValue view_market(Node& n, const std::string& market_id)
{
    MarketId id = parse_market_id_str(market_id);
    return n.with_state([&](const State& st, const Archive&, const Txn& t) -> UniValue {
        auto m = st.market(t, id);
        if (!m) return UniValue();
        auto decisions = market_decisions(st, t, *m);
        auto mshares = st.mempool_shares(t, id);
        double beta = market_beta(*m);
        auto prices = prices_or_uniform(mshares ? *mshares : m->shares, beta);
        UniValue dims = market_dimensions(*m, decisions);
        UniValue outs = market_outcomes(*m, decisions, prices);
        UniValue resolution;
        if (m->market_state == MarketState::Settled) {
            auto valid = m->valid_state_combos();
            UniValue wins = arr();
            std::vector<std::pair<std::string, double>> wl;
            for (size_t i = 0; i < valid.size(); i++) {
                double fp = i < m->final_prices.size() ? m->final_prices[i] : 0.0;
                if (fp > 0.0) {
                    std::string label = outcome_label(*m, valid[i].first, decisions);
                    UniValue w = obj();
                    w.pushKV("outcome_index", u64v(i));
                    w.pushKV("label", label);
                    w.pushKV("price", json::f64(fp));
                    wins.push_back(w);
                    wl.emplace_back(label, fp);
                }
            }
            std::string summary;
            if (wl.size() == 1) summary = "Resolved: " + wl[0].first;
            else if (wl.empty()) summary = "No winning outcome";
            else {
                summary = "Resolved: ";
                for (size_t i = 0; i < wl.size(); i++) summary += (i ? ", " : "") + wl[i].first + " (" + pct1(wl[i].second * 100.0) + "%)";
            }
            resolution = obj();
            resolution.pushKV("winning_outcomes", wins);
            resolution.pushKV("summary", summary);
        }
        UniValue o = obj();
        o.pushKV("market_id", market_id);
        o.pushKV("title", m->title);
        o.pushKV("description", m->description);
        o.pushKV("tags", strs(m->tags));
        o.pushKV("creator_address", m->creator_address.to_base58());
        o.pushKV("created_at_height", u64v(m->created_at_height));
        o.pushKV("expires_at_height", m->expires_at_height ? u64v(*m->expires_at_height) : UniValue());
        o.pushKV("dimensions", dims);
        o.pushKV("outcomes", outs);
        o.pushKV("state", market_status_name(m->market_state));
        o.pushKV("beta", json::f64(beta));
        o.pushKV("liquidity_base_sats", u64v(m->liquidity_base_sats));
        o.pushKV("treasury_sats", u64v(st.market_funds_sats(t, id, false)));
        o.pushKV("total_volume_sats", u64v(m->total_volume_sats));
        o.pushKV("trading_fee_rate", json::f64(m->trading_fee));
        o.pushKV("resolution", resolution);
        o.pushKV("tx_pow_hash_selector", u64v(m->tx_pow_hash_selector));
        o.pushKV("tx_pow_ordering", u64v(m->tx_pow_ordering));
        o.pushKV("tx_pow_difficulty", u64v(m->tx_pow_difficulty));
        return o;
    });
}

UniValue share_position(const std::string& market_id, uint32_t outcome, int64_t shares, const Market& m, const std::vector<double>& prices, double& value_out)
{
    double price = outcome < prices.size() ? prices[outcome] : 0.0;
    double value = double(shares) * price;
    value_out = value;
    std::string name = outcome < m.state_combos.size() ? "Outcome " + std::to_string(outcome) + ": " + usize_list_debug(m.state_combos[outcome])
                                                       : "Outcome " + std::to_string(outcome);
    UniValue o = obj();
    o.pushKV("market_id", market_id);
    o.pushKV("outcome_index", u64v(outcome));
    o.pushKV("outcome_name", name);
    o.pushKV("shares", UniValue(shares));
    o.pushKV("avg_purchase_price", json::f64(price));
    o.pushKV("current_price", json::f64(price));
    o.pushKV("current_value", json::f64(value));
    o.pushKV("unrealized_pnl", json::f64(0.0));
    o.pushKV("cost_basis", json::f64(value));
    return o;
}

// rpc_server.rs user_positions / market_user_positions + market_positions
UniValue market_positions(Node& n, const Address& addr, const std::optional<std::string>& mid)
{
    if (mid) {
        MarketId id = parse_market_id_str(*mid);
        return n.with_state([&](const State& st, const Archive&, const Txn& t) {
            UniValue positions = arr();
            double total_value = -0.0, total_cost = -0.0; // Iterator::sum starts at -0.0
            auto acc = st.share_account(t, addr);
            if (auto m = st.market(t, id); m && acc) {
                auto prices = prices_or_uniform(m->shares, market_beta(*m));
                for (auto& [k, sh] : acc->positions) {
                    if (k.first != id) continue;
                    double v;
                    positions.push_back(share_position(*mid, k.second, sh, *m, prices, v));
                    total_value += v;
                    total_cost += v;
                }
            }
            UniValue o = obj();
            o.pushKV("address", addr.to_base58());
            o.pushKV("positions", positions);
            o.pushKV("total_value", json::f64(total_value));
            o.pushKV("total_cost_basis", json::f64(total_cost));
            o.pushKV("total_unrealized_pnl", json::f64(total_value - total_cost));
            o.pushKV("active_markets", u64v(1));
            o.pushKV("last_updated_height", u64v(0));
            return o;
        });
    }
    return n.with_state([&](const State& st, const Archive&, const Txn& t) {
        UniValue positions = arr();
        double total_value = 0.0, total_cost = 0.0;
        std::set<MarketId> active;
        uint32_t last = 0;
        std::map<MarketId, std::pair<Market, std::vector<double>>> cache;
        if (auto acc = st.share_account(t, addr)) {
            for (auto& [k, sh] : acc->positions) {
                auto it = cache.find(k.first);
                if (it == cache.end()) {
                    auto m = st.market(t, k.first);
                    if (!m) continue;
                    it = cache.emplace(k.first, std::make_pair(*m, prices_or_uniform(m->shares, market_beta(*m)))).first;
                }
                double v;
                positions.push_back(share_position(k.first.to_hex(), k.second, sh, it->second.first, it->second.second, v));
                total_value += v;
                total_cost += v;
                active.insert(k.first);
                last = std::max(last, it->second.first.last_updated_height);
            }
        }
        UniValue o = obj();
        o.pushKV("address", addr.to_base58());
        o.pushKV("positions", positions);
        o.pushKV("total_value", json::f64(total_value));
        o.pushKV("total_cost_basis", json::f64(total_cost));
        o.pushKV("total_unrealized_pnl", json::f64(total_value - total_cost));
        o.pushKV("active_markets", u64v(active.size()));
        o.pushKV("last_updated_height", u64v(last));
        return o;
    });
}

// ---- voting periods (voting/period_calculator.rs, voting/mod.rs)
enum class PeriodStatus { Pending, Active, Closed, Resolved };
const char* period_status_name(PeriodStatus s)
{
    switch (s) {
    case PeriodStatus::Pending: return "Pending";
    case PeriodStatus::Active: return "Active";
    case PeriodStatus::Closed: return "Closed";
    case PeriodStatus::Resolved: return "Resolved";
    }
    return "Pending";
}
struct VotingPeriod {
    uint32_t id = 0;
    uint64_t start = 0, end = 0;
    PeriodStatus status = PeriodStatus::Pending;
    std::vector<DecisionId> decision_ids;
};

std::vector<DecisionEntry> all_claimed(const State& st, const Txn& t)
{
    std::vector<DecisionEntry> r;
    for (auto& [p, es] : st.all_period_decisions(t))
        for (auto& e : es)
            if (e.decision) r.push_back(e);
    return r;
}

PeriodStatus period_status(uint64_t start, uint64_t end, uint64_t cur, bool has_outcomes)
{
    if (has_outcomes) return PeriodStatus::Resolved;
    if (cur >= end) return PeriodStatus::Closed;
    if (cur >= start) return PeriodStatus::Active;
    return PeriodStatus::Pending;
}

// calculate_voting_period; nullopt = their Err (voting period 0)
std::optional<VotingPeriod> calculate_voting_period(const State& st, const Txn& t, uint32_t period, uint32_t height, uint64_t ts, uint64_t genesis)
{
    if (period == 0) return std::nullopt;
    auto [s, e] = period_boundaries(period, st.config(), genesis);
    VotingPeriod vp;
    vp.id = period;
    vp.start = s;
    vp.end = e;
    for (auto& d : all_claimed(st, t))
        if (d.decision_id.period_index() == period - 1) vp.decision_ids.push_back(d.decision_id);
    uint64_t cur = st.config().is_blocks ? uint64_t(height) : ts;
    vp.status = period_status(s, e, cur, st.period_stats(t, period).has_value());
    return vp;
}

// get_active_period (get_all_active_periods, first Active)
std::optional<VotingPeriod> active_period(const State& st, const Txn& t, uint64_t ts, uint32_t height, uint64_t genesis)
{
    std::map<uint32_t, std::vector<DecisionId>> by_period;
    for (auto& d : all_claimed(st, t)) by_period[d.decision_id.voting_period()].push_back(d.decision_id);
    uint64_t cur = st.config().is_blocks ? uint64_t(height) : ts;
    for (auto& [p, ids] : by_period) {
        auto [s, e] = period_boundaries(p, st.config(), genesis);
        auto status = period_status(s, e, cur, st.period_stats(t, p).has_value());
        if (status == PeriodStatus::Active) return VotingPeriod{p, s, e, status, ids};
    }
    return std::nullopt;
}

std::vector<std::pair<VoteKey, VoteEntry>> votes_where(const State& st, const Txn& t, const std::function<bool(const VoteKey&)>& keep)
{
    std::vector<std::pair<VoteKey, VoteEntry>> r;
    for (auto& kv : st.all_votes(t))
        if (keep(kv.first)) r.push_back(kv);
    return r;
}

// ---- decision slots (rpc_server.rs allocate_decision_slots)
struct SlotRequest {
    uint32_t period_index = 0;
    DecisionType decision_type;
    std::string header, description;
    std::optional<std::string> option_0_label, option_1_label;
    std::optional<std::vector<std::string>> option_labels, tags;
};
struct AllocatedSlot {
    DecisionId decision_id;
    DecisionType decision_type;
    DecisionClaimEntry entry;
    uint64_t listing_fee_sats = 0;
};

std::vector<AllocatedSlot> allocate_decision_slots(Node& n, const std::vector<SlotRequest>& reqs)
{
    auto pending = n.pending_decision_claim_ids();
    std::map<uint32_t, std::set<uint32_t>> picked;
    std::vector<AllocatedSlot> out;
    for (size_t i = 0; i < reqs.size(); i++) {
        auto& r = reqs[i];
        if (r.header.size() > 100)
            throw app_error("Header for entry " + std::to_string(i) + " (period " + std::to_string(r.period_index) + ") must be 100 bytes or less");
        if (r.description.size() > 2000)
            throw app_error("Description for entry " + std::to_string(i) + " (period " + std::to_string(r.period_index) +
                            ") must be 2000 bytes or less");
        auto [pricing, avail] = n.with_state([&](const State& st, const Archive&, const Txn& t) {
            return std::make_pair(st.period_pricing(t, r.period_index), st.available_decisions_in_period(t, r.period_index));
        });
        if (!pricing) throw app_error("no pricing record for period " + std::to_string(r.period_index));
        auto& set = picked[r.period_index];
        std::optional<DecisionId> pick;
        for (auto& id : avail)
            if (slot_unlocked(id.decision_index(), pricing->mints) && !pending.count(id.b) && !set.count(id.decision_index())) {
                pick = id;
                break;
            }
        if (!pick) throw app_error("No available standard slot in period " + std::to_string(r.period_index));
        set.insert(pick->decision_index());
        std::string err;
        auto fee = fee_for_index(pricing->p_period, pricing->mints, pick->decision_index(), &err);
        if (!fee) throw app_error(err);
        AllocatedSlot s;
        s.decision_id = *pick;
        s.decision_type = r.decision_type;
        s.entry.decision_id_bytes = pick->b;
        s.entry.header = r.header;
        s.entry.description = r.description;
        s.entry.option_0_label = r.option_0_label;
        s.entry.option_1_label = r.option_1_label;
        s.entry.option_labels = r.option_labels;
        s.entry.tags = r.tags;
        s.listing_fee_sats = *fee;
        out.push_back(std::move(s));
    }
    return out;
}

uint64_t sum_fees(const std::vector<AllocatedSlot>& v)
{
    uint64_t s = 0;
    for (auto& a : v) {
        if (s + a.listing_fee_sats < s) throw app_error("listing fee overflow");
        s += a.listing_fee_sats;
    }
    return s;
}

std::optional<std::array<uint8_t, 3>> pow_config(const Market& m)
{
    return std::array<uint8_t, 3>{m.tx_pow_hash_selector, m.tx_pow_ordering, m.tx_pow_difficulty};
}

Address first_wallet_address(Wallet& w)
{
    auto as = w.addresses();
    if (as.empty()) throw app_error("Wallet has no addresses");
    return *as.begin();
}

Hash32 tip_or_throw(Node& n)
{
    auto t = n.tip();
    if (!t) throw app_error("Chain has no tip");
    return *t;
}

Market market_or_throw(Node& n, const MarketId& id)
{
    auto m = n.with_state([&](const State& st, const Archive&, const Txn& t) { return st.market(t, id); });
    if (!m) throw app_error("Market not found");
    return *m;
}

std::vector<int64_t> mempool_or_market_shares(Node& n, const Market& m)
{
    auto ms = n.with_state([&](const State& st, const Archive&, const Txn& t) { return st.mempool_shares(t, m.id); });
    return ms ? *ms : m.shares;
}

DecisionType parse_claim_type(const std::string& type, std::optional<double> min, std::optional<double> max, std::optional<double> inc)
{
    DecisionType t;
    if (type == "binary") t.kind = DecisionType::Binary;
    else if (type == "scaled") {
        if (!min) throw app_error("min is required for scaled decisions");
        if (!max) throw app_error("max is required for scaled decisions");
        t.kind = DecisionType::Scaled;
        t.min = *min;
        t.max = *max;
        t.increment = inc.value_or(1.0);
    } else
        throw app_error("Unknown decision_type: '" + type + "'. Expected 'binary', 'scaled', or 'category'");
    return t;
}

} // namespace

void register_rpc_markets(bt::jsonrpc::Server& s, App& app, bool public_api, bool private_api)
{
    Node& n = app.node;
    Wallet& w = app.wallet;
    // open_api: the public server's document, the private-only server's, or both
    const char* doc = public_api && private_api ? OPENAPI_ALL : public_api ? OPENAPI_PUBLIC : OPENAPI_PRIVATE;
    s.add("openapi_schema", {}, 0, [doc](const Params&) {
        UniValue v;
        v.read(doc);
        return v;
    });
    if (public_api) {
        s.add("calculate_initial_liquidity", {"request"}, 1, P([&n](const Params& p) {
                  const UniValue& rq = p[0];
                  double beta = json::parse_f64(req_field(rq, "beta"));
                  auto dims = parse_opt_str(opt_field(rq, "dimensions"));
                  std::optional<size_t> num;
                  if (!opt_field(rq, "num_outcomes").isNull()) num = parse_usize(opt_field(rq, "num_outcomes"));
                  if (beta <= 0.0) throw app_error("Beta parameter must be positive, got: " + fmt::display(beta));
                  size_t count;
                  std::string config, breakdown;
                  if (num) {
                      if (*num < 2) throw app_error("Number of outcomes must be at least 2");
                      count = *num;
                      config = "Preview: " + std::to_string(count) + " outcomes";
                      breakdown = std::to_string(count) + " outcomes specified";
                  } else if (dims) {
                      auto specs = parse_dimensions(*dims);
                      if (!specs) throw app_error("Failed to parse dimensions: Invalid market dimensions");
                      count = 1;
                      std::string desc;
                      for (size_t i = 0; i < specs->size(); i++) {
                          auto& sp = (*specs)[i];
                          if (i) desc += " \xC3\x97 ";
                          if (sp.kind == DimensionSpec::Single) {
                              count *= 2;
                              desc += "binary(2)";
                          } else {
                              size_t k = n.with_state([&](const State& st, const Archive&, const Txn& t) -> size_t {
                                  auto e = st.decision_entry(t, sp.id);
                                  if (e && e->decision && e->decision->option_count()) return *e->decision->option_count();
                                  return 2;
                              });
                              count *= k;
                              desc += "categorical(" + std::to_string(k) + ")";
                          }
                      }
                      config = std::to_string(specs->size()) + " dimensions";
                      breakdown = desc + " = " + std::to_string(count) + " outcomes";
                  } else {
                      throw app_error("Either dimensions or num_outcomes must be provided");
                  }
                  std::string err;
                  auto sats = math::to_sats(math::lmsr_liquidity(beta, count), math::Rounding::Up, &err);
                  if (!sats) throw app_error("Liquidity calculation failed: " + err);
                  UniValue o = obj();
                  o.pushKV("beta", json::f64(beta));
                  o.pushKV("num_outcomes", u64v(count));
                  o.pushKV("initial_liquidity_sats", u64v(*sats));
                  o.pushKV("min_treasury_sats", u64v(*sats));
                  o.pushKV("market_config", config);
                  o.pushKV("outcome_breakdown", breakdown);
                  return o;
              }));
        s.add("decision_fee_for_id", {"decision_id_hex"}, 1, P([&n](const Params& p) {
                  std::string err;
                  auto id = DecisionId::from_hex(json::parse_str(p[0]), &err);
                  if (!id) throw app_error("invalid decision id: invalid decision ID: " + err);
                  return u64v(n.with_state([&](const State& st, const Archive&, const Txn& t) { return st.fee_for_decision_id(t, *id); }));
              }));
        auto decision_get = [&n](const Params& p) -> UniValue {
            DecisionId id = parse_did(json::parse_str(p[0]));
            return n.with_state([&](const State& st, const Archive&, const Txn& t) -> UniValue {
                auto e = st.decision_entry(t, id);
                if (!e) return UniValue();
                UniValue content;
                if (!e->decision) content = UniValue("Empty");
                else {
                    content = obj();
                    content.pushKV("Decision", decision_info(id, *e->decision));
                }
                UniValue o = obj();
                o.pushKV("decision_id_hex", id.to_hex());
                o.pushKV("period_index", u64v(id.period_index()));
                o.pushKV("decision_index", u64v(id.decision_index()));
                o.pushKV("content", content);
                return o;
            });
        };
        s.add("decision_get", {"decision_id"}, 1, P(decision_get));
        s.add("decision_list", {"filter"}, 0, P([&n](const Params& p) {
                  std::optional<uint32_t> fperiod;
                  std::optional<DecisionState> fstatus;
                  bool has_filter = !p[0].isNull();
                  if (has_filter) {
                      fperiod = parse_opt_u32(opt_field(p[0], "period"));
                      fstatus = parse_decision_state(opt_field(p[0], "status"));
                  }
                  return n.with_state([&](const State& st, const Archive&, const Txn& t) {
                      auto [ts, h, g] = st.period_context(t);
                      uint32_t current = get_current_period(ts, h, g, st.config());
                      std::vector<uint32_t> periods;
                      if (fperiod) periods.push_back(*fperiod);
                      else
                          for (uint32_t pp = current; pp <= current + FUTURE_PERIODS - 1; pp++)
                              if (st.total_for(pp, ts, h, g) > 0) periods.push_back(pp);
                      UniValue res = arr();
                      for (uint32_t period : periods) {
                          for (auto& id : st.available_decisions_in_period(t, period)) {
                              if (fstatus && *fstatus != DecisionState::Created) continue;
                              UniValue o = obj();
                              o.pushKV("decision_id_hex", id.to_hex());
                              o.pushKV("period_index", u64v(id.period_index()));
                              o.pushKV("decision_index", u64v(id.decision_index()));
                              o.pushKV("state", "Created");
                              o.pushKV("decision", UniValue());
                              res.push_back(o);
                          }
                          for (auto& e : st.period_decisions(t, period)) {
                              if (!e.decision) continue;
                              uint32_t vp = e.decision_id.voting_period();
                              uint32_t cm1 = current == 0 ? 0 : current - 1;
                              DecisionState state = vp < cm1 ? DecisionState::Resolved
                                                    : (vp == current || vp == cm1) ? DecisionState::Voting
                                                                                    : DecisionState::Claimed;
                              if (fstatus && *fstatus != state) continue;
                              UniValue o = obj();
                              o.pushKV("decision_id_hex", e.decision_id.to_hex());
                              o.pushKV("period_index", u64v(e.decision_id.period_index()));
                              o.pushKV("decision_index", u64v(e.decision_id.decision_index()));
                              o.pushKV("state", decision_state_name(state));
                              o.pushKV("decision", decision_info(e.decision_id, *e.decision));
                              res.push_back(o);
                          }
                      }
                      return res;
                  });
              }));
        s.add("decision_listing_fee", {"period"}, 1, P([&n](const Params& p) {
                  uint32_t period = json::parse_u32(p[0]);
                  auto pr = n.with_state([&](const State& st, const Archive&, const Txn& t) { return st.period_pricing(t, period); });
                  if (!pr) throw app_error("no pricing record for period " + std::to_string(period));
                  UniValue tiers = arr();
                  for (size_t i = 0; i < 5; i++) tiers.push_back(u64v(slot_price(pr->p_period, i)));
                  UniValue o = obj();
                  o.pushKV("p_period", u64v(pr->p_period));
                  o.pushKV("p_floor", u64v(pr->p_floor));
                  o.pushKV("mints", u64v(pr->mints));
                  o.pushKV("tier_prices", tiers);
                  o.pushKV("last_reprice_block", u64v(pr->last_reprice_block));
                  o.pushKV("period_capacity", u64v(pr->period_capacity()));
                  o.pushKV("claimed", u64v(pr->claimed));
                  return o;
              }));
        s.add("decision_status", {}, 0, P([&n](const Params&) {
                  return n.with_state([&](const State& st, const Archive&, const Txn& t) {
                      auto [ts, h, g] = st.period_context(t);
                      uint32_t cur = get_current_period(ts, h, g, st.config());
                      UniValue o = obj();
                      o.pushKV("is_testing_mode", st.config().is_blocks);
                      o.pushKV("blocks_per_period", u64v(st.config().is_blocks ? st.config().quantity : 0));
                      o.pushKV("current_period", u64v(cur));
                      o.pushKV("current_period_name", period_to_name(cur));
                      return o;
                  });
              }));
        s.add("list_open_periods_with_pricing", {}, 0, P([&n](const Params&) {
                  return n.with_state([&](const State& st, const Archive&, const Txn& t) {
                      auto [ts, h, g] = st.period_context(t);
                      uint32_t cur = get_current_period(ts, h, g, st.config());
                      UniValue res = arr();
                      uint32_t last = cur + 19 < cur ? UINT32_MAX : cur + 19;
                      for (uint64_t period = cur; period <= last; period++) {
                          auto pr = st.period_pricing(t, uint32_t(period));
                          if (!pr) continue;
                          uint32_t slots[5] = {0, 0, 0, 0, 0};
                          std::optional<uint8_t> ctier;
                          std::optional<uint64_t> cfee;
                          for (auto& id : st.available_decisions_in_period(t, uint32_t(period))) {
                              if (!slot_unlocked(id.decision_index(), pr->mints)) continue;
                              size_t tier = id.decision_index() / 100;
                              if (tier >= 5) continue;
                              slots[tier] = slots[tier] == UINT32_MAX ? UINT32_MAX : slots[tier] + 1;
                              if (!ctier || uint8_t(tier) < *ctier) {
                                  ctier = uint8_t(tier);
                                  cfee = fee_for_index(pr->p_period, pr->mints, id.decision_index(), nullptr);
                              }
                          }
                          if (ctier && cfee) {
                              UniValue o = obj();
                              o.pushKV("period_index", u64v(period));
                              o.pushKV("cheapest_available_slot_sats", u64v(*cfee));
                              o.pushKV("cheapest_available_tier", u64v(*ctier));
                              UniValue sl = arr();
                              for (auto x : slots) sl.push_back(u64v(x));
                              o.pushKV("slots_available_by_tier", sl);
                              res.push_back(o);
                          }
                      }
                      return res;
                  });
              }));
        s.add("market_get", {"market_id"}, 1, P([&n](const Params& p) { return view_market(n, json::parse_str(p[0])); }));
        s.add("market_list", {}, 0, P([&n](const Params&) {
                  return n.with_state([&](const State& st, const Archive&, const Txn& t) {
                      UniValue res = arr();
                      for (auto& m : st.all_markets(t)) {
                          std::string desc = m.description;
                          // chars().count() > 100 -> first 97 chars + "..."
                          size_t chars = 0, cut = std::string::npos;
                          for (size_t i = 0; i < desc.size(); i++)
                              if ((uint8_t(desc[i]) & 0xC0) != 0x80) {
                                  if (chars == 97) cut = i;
                                  chars++;
                              }
                          if (chars > 100) desc = desc.substr(0, cut) + "...";
                          UniValue o = obj();
                          o.pushKV("market_id", m.id.to_hex());
                          o.pushKV("title", m.title);
                          o.pushKV("description", desc);
                          o.pushKV("outcome_count", u64v(m.valid_state_combos().size()));
                          o.pushKV("state", market_status_name(m.market_state));
                          o.pushKV("volume_sats", u64v(m.total_volume_sats));
                          o.pushKV("created_at_height", u64v(m.created_at_height));
                          res.push_back(o);
                      }
                      return res;
                  });
              }));
        s.add("market_price_history", {"market_id"}, 1, P([&n](const Params& p) {
                  MarketId id = parse_market_id_str(json::parse_str(p[0]));
                  // markets/price_history.rs: walk the active chain from the tip down to the
                  // creation block, reverting each block's trades / AmplifyBeta deposits
                  return n.with_state([&](const State& st, const Archive& a, const Txn& t) {
                      auto m = st.market(t, id);
                      if (!m) throw app_error("Market not found");
                      auto tip = st.tip(t);
                      if (!tip) throw app_error("market " + id.to_hex() + " has no creation block in the active chain");
                      std::vector<int64_t> shares = m->shares;
                      uint64_t liq = m->liquidity_base_sats;
                      std::vector<UniValue> points;
                      std::optional<Hash32> cur = *tip;
                      while (cur) {
                          auto ht = a.height(t, *cur);
                          auto body = a.body(t, *cur);
                          if (!ht || !body) throw app_error("archive error");
                          std::vector<uint32_t> skipped;
                          if (auto v = t.get(State::T_SKIPPED_UNDO, u32_key(*ht))) skipped = dec_u32s(*v);
                          struct Change {
                              bool trade;
                              uint32_t outcome;
                              int64_t shares;
                              uint64_t amount;
                          };
                          std::vector<Change> changes;
                          for (uint32_t i = 0; i < body->transactions.size(); i++) {
                              if (std::find(skipped.begin(), skipped.end(), i) != skipped.end()) continue;
                              auto& tx = body->transactions[i];
                              if (!tx.data) continue;
                              if (auto* tr = std::get_if<TxTrade>(&*tx.data); tr && tr->market_id == id) changes.push_back({true, tr->outcome_index, tr->shares, 0});
                              if (auto* am = std::get_if<TxAmplifyBeta>(&*tx.data); am && am->market_id == id) changes.push_back({false, 0, 0, am->amount});
                          }
                          bool creation = *ht == m->created_at_height;
                          if (creation || !changes.empty()) {
                              auto bv = a.best_main_verification(t, *cur);
                              uint64_t ts = 0;
                              if (bv)
                                  if (auto mh = a.main_header(t, *bv)) ts = mh->timestamp;
                              std::string err;
                              auto pr = math::prices(shares, math::derive_beta_from_liquidity(liq, shares.size()), &err);
                              if (!pr) throw app_error("failed to calculate market prices");
                              UniValue pt = obj();
                              pt.pushKV("height", u64v(*ht));
                              pt.pushKV("block_hash", json::hash(*cur));
                              pt.pushKV("timestamp", u64v(ts));
                              UniValue pa = arr();
                              for (double x : *pr) pa.push_back(json::f64(x));
                              pt.pushKV("prices", pa);
                              points.push_back(pt);
                          }
                          if (creation) {
                              UniValue res = arr();
                              for (auto it = points.rbegin(); it != points.rend(); ++it) res.push_back(*it);
                              return res;
                          }
                          for (auto& c : changes) {
                              if (c.trade) {
                                  if (c.outcome >= shares.size()) throw app_error("block trades an outcome the market does not have");
                                  shares[c.outcome] -= c.shares;
                              } else {
                                  if (liq < c.amount) throw app_error("liquidity base underflow");
                                  liq -= c.amount;
                              }
                          }
                          auto hd = a.header(t, *cur);
                          cur = hd ? hd->prev_side_hash : std::nullopt;
                      }
                      throw app_error("market " + id.to_hex() + " has no creation block in the active chain");
                  });
              }));
        s.add("market_positions", {"address", "market_id"}, 1, P([&n](const Params& p) {
                  return market_positions(n, json::parse_address(p[0]), parse_opt_str(p[1]));
              }));
        s.add("vote_list", {"filter"}, 1, P([&n](const Params& p) {
                  const UniValue& f = p[0];
                  if (!f.isObject()) pfail("invalid type: expected struct VoteFilter");
                  std::optional<Address> voter;
                  if (!f.find_value("voter").isNull()) voter = json::parse_address(f.find_value("voter"));
                  auto did = parse_opt_str(f.find_value("decision_id"));
                  auto pid = parse_opt_u32(f.find_value("period_id"));
                  return n.with_state([&](const State& st, const Archive&, const Txn& t) {
                      std::vector<std::pair<VoteKey, VoteEntry>> votes;
                      if (voter) {
                          votes = votes_where(st, t, [&](const VoteKey& k) {
                              return k.voter == *voter && (!pid || k.period_id == *pid) && (!did || k.decision_id.to_hex() == *did);
                          });
                      } else if (did) {
                          std::string err;
                          auto id = DecisionId::from_hex(*did, &err);
                          if (!id) throw app_error("Invalid decision ID: invalid decision ID: " + err);
                          votes = votes_where(st, t, [&](const VoteKey& k) { return k.decision_id == *id && (!pid || k.period_id == *pid); });
                      } else if (pid) {
                          votes = st.votes_for_period(t, *pid);
                      }
                      UniValue res = arr();
                      for (auto& [k, e] : votes) {
                          auto v = e.value.to_float();
                          if (!v) continue;
                          double display = *v;
                          if (auto de = st.decision_entry(t, k.decision_id); de && de->decision) display = de->decision->denormalize_value(*v);
                          UniValue o = obj();
                          o.pushKV("voter_address", k.voter.to_base58());
                          o.pushKV("decision_id", k.decision_id.to_hex());
                          o.pushKV("vote_value", json::f64(display));
                          o.pushKV("period_id", u64v(k.period_id));
                          o.pushKV("block_height", u64v(e.block_height));
                          o.pushKV("txid", "");
                          o.pushKV("is_batch_vote", false);
                          res.push_back(o);
                      }
                      return res;
                  });
              }));
        s.add("vote_period", {"period_id"}, 0, P([&n](const Params& p) {
                  auto pid_in = parse_opt_u32(p[0]);
                  return n.with_state([&](const State& st, const Archive&, const Txn& t) -> UniValue {
                      auto [ts, h, g] = st.period_context(t);
                      uint32_t height = h.value_or(0);
                      uint32_t pid;
                      if (pid_in) pid = *pid_in;
                      else {
                          auto ap = active_period(st, t, ts, height, g);
                          if (!ap) return UniValue();
                          pid = ap->id;
                      }
                      auto vp = calculate_voting_period(st, t, pid, height, ts, g);
                      if (!vp) return UniValue();
                      UniValue decisions = arr();
                      for (auto& id : vp->decision_ids) {
                          auto e = st.decision_entry(t, id);
                          UniValue o = obj();
                          o.pushKV("decision_id_hex", id.to_hex());
                          if (e && e->decision) {
                              o.pushKV("header", e->decision->header);
                              o.pushKV("is_standard", id.is_standard());
                              o.pushKV("decision_type", json::decision_type(e->decision->decision_type));
                          } else {
                              o.pushKV("header", "");
                              o.pushKV("is_standard", id.is_standard());
                              o.pushKV("decision_type", json::decision_type(DecisionType{}));
                          }
                          decisions.push_back(o);
                      }
                      auto votes = st.votes_for_period(t, pid);
                      std::set<Address> voters;
                      for (auto& [k, _] : votes) voters.insert(k.voter);
                      uint64_t total_voters = voters.size(), total_votes = votes.size();
                      UniValue stats = obj();
                      stats.pushKV("total_voters", u64v(total_voters));
                      stats.pushKV("active_voters", u64v(voters.size()));
                      stats.pushKV("total_votes", u64v(total_votes));
                      stats.pushKV("participation_rate", json::f64(total_voters > 0 ? double(voters.size()) / double(total_voters) : 0.0));
                      UniValue consensus;
                      if (vp->status == PeriodStatus::Resolved) {
                          std::map<std::string, double> outcomes;
                          for (auto& o : st.outcomes_for_period(t, pid)) {
                              double v = o.outcome_value;
                              if (auto e = st.decision_entry(t, o.decision_id); e && e->decision) v = e->decision->denormalize_value(v);
                              outcomes[o.decision_id.to_hex()] = v;
                          }
                          UniValue oc = obj();
                          for (auto& [k, v] : outcomes) oc.pushKV(k, json::f64(v));
                          UniValue fl = arr(), sc = obj();
                          double certainty = 0.0;
                          if (auto ps = st.period_stats(t, pid)) {
                              if (ps->reputation_changes)
                                  for (auto& [a, ch] : *ps->reputation_changes) {
                                      UniValue c = obj();
                                      c.pushKV("old_score", json::f64(ch.first));
                                      c.pushKV("new_score", json::f64(ch.second));
                                      sc.pushKV(a.to_base58(), c);
                                  }
                              if (ps->first_loading)
                                  for (double x : *ps->first_loading) fl.push_back(json::f64(x));
                              certainty = ps->certainty.value_or(0.0);
                          }
                          consensus = obj();
                          consensus.pushKV("outcomes", oc);
                          consensus.pushKV("first_loading", fl);
                          consensus.pushKV("certainty", json::f64(certainty));
                          consensus.pushKV("score_changes", sc);
                          consensus.pushKV("outliers", arr());
                          UniValue dimsz = arr();
                          dimsz.push_back(u64v(voters.size()));
                          dimsz.push_back(u64v(vp->decision_ids.size()));
                          consensus.pushKV("vote_matrix_dimensions", dimsz);
                          consensus.pushKV("algorithm_version", "SVD-PCA-v1.0");
                      }
                      UniValue o = obj();
                      o.pushKV("period_id", u64v(pid));
                      o.pushKV("status", period_status_name(vp->status));
                      o.pushKV("start_height", u64v(0));
                      o.pushKV("end_height", u64v(0));
                      o.pushKV("start_time", u64v(vp->start));
                      o.pushKV("end_time", u64v(vp->end));
                      o.pushKV("decisions", decisions);
                      o.pushKV("stats", stats);
                      o.pushKV("consensus", consensus);
                      return o;
                  });
              }));
        s.add("vote_voter", {"address"}, 1, P([&n](const Params& p) {
                  Address a = json::parse_address(p[0]);
                  return n.with_state([&](const State& st, const Archive&, const Txn& t) {
                      auto [ts, h, g] = st.period_context(t);
                      auto votes = votes_where(st, t, [&](const VoteKey& k) { return k.voter == a; });
                      std::set<uint32_t> periods;
                      for (auto& [k, _] : votes) periods.insert(k.period_id);
                      UniValue part;
                      if (auto ap = active_period(st, t, ts, h.value_or(0), g)) {
                          uint32_t cast = 0;
                          for (auto& [k, _] : votes)
                              if (k.period_id == ap->id) cast++;
                          uint32_t avail = uint32_t(ap->decision_ids.size());
                          part = obj();
                          part.pushKV("period_id", u64v(ap->id));
                          part.pushKV("votes_cast", u64v(cast));
                          part.pushKV("decisions_available", u64v(avail));
                          part.pushKV("participation_rate", json::f64(avail > 0 ? double(cast) / double(avail) : 0.0));
                      }
                      UniValue o = obj();
                      o.pushKV("address", a.to_base58());
                      o.pushKV("votecoin_balance", json::f64(st.reputation(t, a)));
                      o.pushKV("total_votes", u64v(votes.size()));
                      o.pushKV("periods_active", u64v(periods.size()));
                      o.pushKV("is_active", !votes.empty());
                      o.pushKV("current_period_participation", part);
                      return o;
                  });
              }));
        s.add("vote_voters", {}, 0, P([&n](const Params&) {
                  return n.with_state([&](const State& st, const Archive&, const Txn& t) {
                      UniValue res = arr();
                      auto all = st.all_votes(t);
                      for (auto& [a, rep] : st.all_reputations(t)) {
                          uint64_t cnt = 0;
                          for (auto& [k, _] : all)
                              if (k.voter == a) cnt++;
                          UniValue o = obj();
                          o.pushKV("address", a.to_base58());
                          o.pushKV("votecoin_balance", json::f64(rep));
                          o.pushKV("total_votes", u64v(cnt));
                          o.pushKV("is_active", cnt > 0);
                          res.push_back(o);
                      }
                      return res;
                  });
              }));
    }
    if (private_api) {
        s.add("decision_claim", {"request"}, 1, P([&app, &w](const Params& p) {
                  const UniValue& rq = p[0];
                  std::string type = json::parse_str(req_field(rq, "decision_type"));
                  const UniValue& items = req_field(rq, "decisions");
                  if (!items.isArray()) pfail("invalid type: expected a sequence");
                  auto mn = parse_opt_f64(opt_field(rq, "min")), mx = parse_opt_f64(opt_field(rq, "max")), inc = parse_opt_f64(opt_field(rq, "increment"));
                  uint64_t tx_fee = json::parse_u64(req_field(rq, "tx_fee_sats"));
                  auto cap = parse_opt_u64(opt_field(rq, "max_listing_fee_sats"));
                  std::vector<SlotRequest> reqs;
                  for (auto& it : items.getValues()) {
                      SlotRequest r;
                      r.period_index = json::parse_u32(req_field(it, "period_index"));
                      r.header = json::parse_str(req_field(it, "header"));
                      r.description = parse_opt_str(opt_field(it, "description")).value_or("");
                      r.option_0_label = parse_opt_str(opt_field(it, "option_0_label"));
                      r.option_1_label = parse_opt_str(opt_field(it, "option_1_label"));
                      r.option_labels = parse_opt_strs(opt_field(it, "option_labels"));
                      r.tags = parse_opt_strs(opt_field(it, "tags"));
                      reqs.push_back(std::move(r));
                  }
                  DecisionType dt;
                  if (type == "category") {
                      if (reqs.size() != 1) throw app_error("Category claim must have exactly 1 decision entry");
                      auto labels = reqs[0].option_labels.value_or(std::vector<std::string>{});
                      if (labels.size() < 2) throw app_error("Category claim requires at least 2 option labels");
                      dt.kind = DecisionType::Category;
                      dt.options = labels;
                  } else {
                      dt = parse_claim_type(type, mn, mx, inc);
                  }
                  for (auto& r : reqs) r.decision_type = dt;
                  auto alloc = allocate_decision_slots(app.node, reqs);
                  uint64_t fees = sum_fees(alloc);
                  if (cap && fees > *cap) throw app_error("Listing fee " + std::to_string(fees) + " exceeds max_listing_fee_sats " + std::to_string(*cap));
                  ClaimDecisionPayload pl;
                  pl.decision_type = dt;
                  UniValue ids = arr();
                  for (auto& a : alloc) {
                      pl.decisions.push_back(a.entry);
                      ids.push_back(a.decision_id.to_hex());
                  }
                  if (fees + tx_fee < fees) throw app_error("total fee overflow");
                  Hash32 txid = app.sign_and_send(w.claim_decision(pl, fees + tx_fee));
                  UniValue o = obj();
                  o.pushKV("txid", json::hash(txid));
                  o.pushKV("decision_ids", ids);
                  o.pushKV("listing_fee_paid_sats", u64v(fees));
                  return o;
              }));
        s.add("market_create", {"request"}, 1, P([&app, &w](const Params& p) {
                  const UniValue& rq = p[0];
                  std::string title = json::parse_str(req_field(rq, "title"));
                  std::string description = json::parse_str(req_field(rq, "description"));
                  const UniValue& dims = req_field(rq, "dimensions");
                  if (!dims.isArray()) pfail("invalid type: expected a sequence");
                  auto beta = parse_opt_f64(opt_field(rq, "beta"));
                  auto fee = parse_opt_f64(opt_field(rq, "trading_fee"));
                  auto liq = parse_opt_u64(opt_field(rq, "initial_liquidity"));
                  auto sel = parse_opt_u8(opt_field(rq, "tx_pow_hash_selector"));
                  auto ord = parse_opt_u8(opt_field(rq, "tx_pow_ordering"));
                  auto diff = parse_opt_u8(opt_field(rq, "tx_pow_difficulty"));
                  uint64_t tx_fee = json::parse_u64(req_field(rq, "tx_fee_sats"));
                  auto cap = parse_opt_u64(opt_field(rq, "max_listing_fee_sats"));
                  if (dims.empty()) throw app_error("market_create requires at least one dimension");
                  // DimensionInput: internally tagged by "type" (existing | new)
                  struct Dim {
                      bool existing;
                      std::optional<DecisionId> id;
                      uint32_t period_index = 0;
                  };
                  std::vector<Dim> parsed;
                  std::vector<SlotRequest> reqs;
                  for (size_t i = 0; i < dims.size(); i++) {
                      const UniValue& d = dims[i];
                      std::string tag = json::parse_str(req_field(d, "type"));
                      if (tag == "existing") {
                          std::string ids = json::parse_str(req_field(d, "id"));
                          std::string err;
                          auto id = DecisionId::from_hex(ids, &err);
                          if (!id) throw app_error("Dimension " + std::to_string(i) + ": invalid decision id " + ids + ": invalid decision ID: " + err);
                          parsed.push_back({true, id, 0});
                      } else if (tag == "new") {
                          SlotRequest r;
                          r.period_index = json::parse_u32(req_field(d, "period_index"));
                          std::string dtype = json::parse_str(req_field(d, "decision_type"));
                          r.header = json::parse_str(req_field(d, "header"));
                          r.description = parse_opt_str(opt_field(d, "description")).value_or("");
                          r.option_0_label = parse_opt_str(opt_field(d, "option_0_label"));
                          r.option_1_label = parse_opt_str(opt_field(d, "option_1_label"));
                          r.option_labels = parse_opt_strs(opt_field(d, "option_labels"));
                          r.tags = parse_opt_strs(opt_field(d, "tags"));
                          auto mn = parse_opt_f64(opt_field(d, "min")), mx = parse_opt_f64(opt_field(d, "max")), inc = parse_opt_f64(opt_field(d, "increment"));
                          std::string pre = "Dimension " + std::to_string(i) + ": ";
                          if (dtype == "binary") r.decision_type.kind = DecisionType::Binary;
                          else if (dtype == "scaled") {
                              if (!mn) throw app_error(pre + "scaled decision requires min");
                              if (!mx) throw app_error(pre + "scaled decision requires max");
                              r.decision_type.kind = DecisionType::Scaled;
                              r.decision_type.min = *mn;
                              r.decision_type.max = *mx;
                              r.decision_type.increment = inc.value_or(1.0);
                          } else if (dtype == "category") {
                              if (!r.option_labels) throw app_error(pre + "category decision requires option_labels");
                              if (r.option_labels->size() < 2) throw app_error(pre + "category decision requires at least 2 option_labels");
                              r.decision_type.kind = DecisionType::Category;
                              r.decision_type.options = *r.option_labels;
                          } else
                              throw app_error(pre + "unknown decision_type '" + dtype + "'");
                          parsed.push_back({false, std::nullopt, r.period_index});
                          reqs.push_back(std::move(r));
                      } else {
                          pfail("unknown variant `" + tag + "`, expected `existing` or `new`");
                      }
                  }
                  auto alloc = allocate_decision_slots(app.node, reqs);
                  uint64_t fees = sum_fees(alloc);
                  if (cap && fees > *cap) throw app_error("Listing fee " + std::to_string(fees) + " exceeds max_listing_fee_sats " + std::to_string(*cap));
                  std::vector<std::pair<DecisionId, DecisionType>> resolved;
                  UniValue claimed = arr();
                  size_t ai = 0;
                  for (size_t i = 0; i < parsed.size(); i++) {
                      if (parsed[i].existing) {
                          DecisionId id = *parsed[i].id;
                          auto e = app.node.with_state([&](const State& st, const Archive&, const Txn& t) { return st.decision_entry(t, id); });
                          if (!e) throw app_error("Dimension " + std::to_string(i) + ": decision " + id.to_hex() + " does not exist");
                          if (!e->decision) throw app_error("Dimension " + std::to_string(i) + ": decision " + id.to_hex() + " was never claimed");
                          resolved.emplace_back(id, e->decision->decision_type);
                      } else {
                          auto& a = alloc[ai++];
                          resolved.emplace_back(a.decision_id, a.decision_type);
                          UniValue c = obj();
                          c.pushKV("id", a.decision_id.to_hex());
                          c.pushKV("period_index", u64v(parsed[i].period_index));
                          c.pushKV("listing_fee_paid_sats", u64v(a.listing_fee_sats));
                          claimed.push_back(c);
                      }
                  }
                  std::string dstr = "[";
                  std::vector<size_t> cats;
                  for (size_t i = 0; i < resolved.size(); i++) {
                      if (i) dstr += ",";
                      if (resolved[i].second.kind == DecisionType::Category) {
                          dstr += "[" + resolved[i].first.to_hex() + "]";
                          cats.push_back(resolved[i].second.options.size());
                      } else
                          dstr += resolved[i].first.to_hex();
                  }
                  dstr += "]";
                  CreateMarketInput in;
                  in.title = title;
                  in.description = description;
                  in.dimensions = dstr;
                  in.beta = beta;
                  in.trading_fee = fee;
                  in.initial_liquidity = liq;
                  if (!cats.empty()) in.category_option_counts = cats;
                  in.tx_pow_hash_selector = sel;
                  in.tx_pow_ordering = ord;
                  in.tx_pow_difficulty = diff;
                  for (auto& a : alloc) {
                      ClaimDecisionPayload pl;
                      pl.decision_type = a.decision_type;
                      pl.decisions.push_back(a.entry);
                      in.new_claims.push_back(pl);
                  }
                  if (fees + tx_fee < fees) throw app_error("total fee overflow");
                  auto [tx, mid] = w.create_market(in, fees + tx_fee);
                  Hash32 txid = app.sign_and_send(tx);
                  UniValue o = obj();
                  o.pushKV("txid", json::hash(txid));
                  o.pushKV("market_id", mid.to_hex());
                  o.pushKV("claimed_decisions", claimed);
                  return o;
              }));
        s.add("market_buy", {"request"}, 1, P([&app, &w](const Params& p) {
                  const UniValue& rq = p[0];
                  std::string mids = json::parse_str(req_field(rq, "market_id"));
                  size_t outcome = parse_usize(req_field(rq, "outcome_index"));
                  int64_t amount = json::parse_i64(req_field(rq, "shares_amount"));
                  auto max_cost = parse_opt_u64(opt_field(rq, "max_cost"));
                  bool dry = parse_opt_bool(opt_field(rq, "dry_run")).value_or(false);
                  MarketId id = parse_market_id_str(mids);
                  Market m = market_or_throw(app.node, id);
                  auto cur = mempool_or_market_shares(app.node, m);
                  double beta = market_beta(m);
                  if (outcome >= cur.size()) throw app_error("Outcome index " + std::to_string(outcome) + " out of range"); // theirs panics
                  auto next = cur;
                  next[outcome] += amount;
                  std::string err;
                  auto cost = math::update_cost(cur, next, beta, &err);
                  if (!cost) throw app_error("LMSR cost calculation failed: " + err);
                  auto bc = math::buy_cost(*cost, m.trading_fee, &err);
                  if (!bc) throw app_error("Cost calculation failed: " + err);
                  double new_price = 0.0;
                  if (auto pr = math::prices(next, beta, nullptr); pr && outcome < pr->size()) new_price = (*pr)[outcome];
                  UniValue o = obj();
                  if (dry) {
                      o.pushKV("txid", UniValue());
                  } else {
                      if (!max_cost) throw app_error("max_cost is required when dry_run is false");
                      if (bc->exceeds_limit(*max_cost))
                          throw app_error("Share cost " + std::to_string(bc->total_cost_sats) + " sats + miner fee " + std::to_string(math::TRADE_MINER_FEE_SATS) +
                                          " sats exceeds maximum cost " + std::to_string(*max_cost) + " sats (slippage protection)");
                      Address trader = first_wallet_address(w);
                      Hash32 prev = tip_or_throw(app.node);
                      Hash32 txid = app.sign_and_send(w.trade(id, uint32_t(outcome), amount, trader, *max_cost, pow_config(m), prev));
                      o.pushKV("txid", bt::hex_encode(txid));
                  }
                  o.pushKV("cost_sats", u64v(bc->total_cost_sats));
                  o.pushKV("trading_fee_sats", u64v(bc->trading_fee_sats));
                  o.pushKV("new_price", json::f64(new_price));
                  return o;
              }));
        s.add("market_sell", {"request"}, 1, P([&app, &w](const Params& p) {
                  const UniValue& rq = p[0];
                  std::string mids = json::parse_str(req_field(rq, "market_id"));
                  size_t outcome = parse_usize(req_field(rq, "outcome_index"));
                  int64_t amount = json::parse_i64(req_field(rq, "shares_amount"));
                  Address seller = json::parse_address(req_field(rq, "seller_address"));
                  auto min_proceeds = parse_opt_u64(opt_field(rq, "min_proceeds"));
                  bool dry = parse_opt_bool(opt_field(rq, "dry_run")).value_or(false);
                  MarketId id = parse_market_id_str(mids);
                  Market m = market_or_throw(app.node, id);
                  int64_t owned = app.node.with_state([&](const State& st, const Archive&, const Txn& t) -> int64_t {
                      if (auto acc = st.share_account(t, seller)) {
                          auto it = acc->positions.find({id, uint32_t(outcome)});
                          if (it != acc->positions.end()) return it->second;
                      }
                      return 0;
                  });
                  if (owned < amount)
                      throw app_error("Insufficient shares: address " + seller.to_base58() + " owns " + std::to_string(owned) + " but trying to sell " +
                                      std::to_string(amount));
                  auto cur = mempool_or_market_shares(app.node, m);
                  double beta = market_beta(m);
                  if (outcome >= cur.size()) throw app_error("Outcome index " + std::to_string(outcome) + " out of range"); // theirs panics
                  auto next = cur;
                  next[outcome] -= amount;
                  std::string err;
                  auto oldc = math::treasury(cur, beta, &err);
                  if (!oldc) throw app_error("Cost calculation error: " + err);
                  auto newc = math::treasury(next, beta, &err);
                  if (!newc) throw app_error("Cost calculation error: " + err);
                  double gross = *oldc - *newc;
                  auto sp = math::sell_proceeds(gross, m.trading_fee, &err);
                  if (!sp) throw app_error("Proceeds calculation failed: " + err);
                  double new_price = 0.0;
                  if (auto pr = math::prices(next, beta, nullptr); pr && outcome < pr->size()) new_price = (*pr)[outcome];
                  UniValue o = obj();
                  if (dry) {
                      o.pushKV("txid", UniValue());
                  } else {
                      uint64_t minp = min_proceeds.value_or(0);
                      if (sp->net_proceeds_sats < minp)
                          throw app_error("Net proceeds " + std::to_string(sp->net_proceeds_sats) + " below minimum " + std::to_string(minp) +
                                          " (slippage protection)");
                      Hash32 prev = tip_or_throw(app.node);
                      Hash32 txid = app.sign_and_send(w.trade(id, uint32_t(outcome), -amount, seller, minp, pow_config(m), prev));
                      o.pushKV("txid", bt::hex_encode(txid));
                  }
                  o.pushKV("proceeds_sats", u64v(sp->gross_proceeds_sats));
                  o.pushKV("trading_fee_sats", u64v(sp->trading_fee_sats));
                  o.pushKV("net_proceeds_sats", u64v(sp->net_proceeds_sats));
                  o.pushKV("new_price", json::f64(new_price));
                  return o;
              }));
        s.add("market_amplify_beta", {"request"}, 1, P([&app, &w](const Params& p) {
                  const UniValue& rq = p[0];
                  MarketId id = parse_market_id_str(json::parse_str(req_field(rq, "market_id")));
                  uint64_t amount = json::parse_u64(req_field(rq, "amount_sats"));
                  Market m = market_or_throw(app.node, id);
                  return UniValue(bt::hex_encode(app.sign_and_send(w.amplify_beta(id, amount, m.creator_address))));
              }));
        s.add("vote_submit", {"votes", "fee_sats"}, 2, P([&app, &w](const Params& p) {
                  if (!p[0].isArray()) pfail("invalid type: expected a sequence");
                  struct In {
                      std::string id;
                      double v;
                  };
                  std::vector<In> in;
                  for (auto& x : p[0].getValues()) in.push_back({json::parse_str(req_field(x, "decision_id")), json::parse_f64(req_field(x, "vote_value"))});
                  uint64_t fee = json::parse_u64(p[1]);
                  if (in.empty()) throw app_error("Ballot cannot be empty");
                  std::vector<BallotItem> items;
                  std::optional<uint32_t> period;
                  for (auto& v : in) {
                      std::string err;
                      auto id = DecisionId::from_hex(v.id, &err);
                      if (!id) throw app_error("Invalid decision ID: invalid decision ID: " + err);
                      uint32_t vp = id->voting_period();
                      if (!period) period = vp;
                      else if (*period != vp)
                          throw app_error("All votes in ballot must be for same period. Expected " + std::to_string(*period) + ", got " + std::to_string(vp) +
                                          " for decision " + v.id);
                      auto e = app.node.with_state([&](const State& st, const Archive&, const Txn& t) { return st.decision_entry(t, *id); });
                      if (!e) throw app_error("Decision " + v.id + " does not exist");
                      if (!e->decision) throw app_error("Decision " + v.id + " has no decision claimed");
                      const Decision& d = *e->decision;
                      // decisions/types.rs validate_and_normalize
                      double x = v.v;
                      if (!std::isnan(x)) {
                          if (d.decision_type.kind == DecisionType::Scaled) {
                              double mn = d.decision_type.min, mx = d.decision_type.max, inc = d.decision_type.increment;
                              if (x < mn || x > mx)
                                  throw app_error("invalid vote value: Value " + fmt::display(x) + " is outside allowed range [" + fmt::display(mn) + ", " +
                                                  fmt::display(mx) + "]");
                              double delta = x - mn, k = std::round(delta / inc), tol = std::fmax(std::fabs(inc) * 1e-9, 1e-12);
                              if (!(std::fabs(delta - k * inc) <= tol))
                                  throw app_error("invalid vote value: Value " + fmt::display(x) + " is not a multiple of increment " + fmt::display(inc) +
                                                  " from min " + fmt::display(mn));
                          } else if (d.decision_type.kind == DecisionType::Binary) {
                              if (!(x >= 0.0 && x <= 1.0))
                                  throw app_error("invalid vote value: Binary vote value " + fmt::display(x) + " must be between 0.0 and 1.0");
                          } else {
                              size_t n_opt = d.decision_type.options.size();
                              // `user_value as usize` saturates
                              uint64_t idx = x <= 0.0 ? 0 : x >= 18446744073709551615.0 ? UINT64_MAX : uint64_t(x);
                              if (x < 0.0 || x != double(idx) || idx > n_opt)
                                  throw app_error("invalid vote value: Categorical vote value " + fmt::display(x) + " must be a valid option index (0..=" +
                                                  std::to_string(n_opt) + ", where " + std::to_string(n_opt) + " = Inconclusive)");
                          }
                          x = d.normalize_value(x);
                      }
                      BallotItem b;
                      b.decision_id_bytes = id->b;
                      b.vote_value = x;
                      items.push_back(b);
                  }
                  LOG_INFO("vote_submit: Voter attempting to submit %zu votes for period %u", items.size(), *period);
                  return UniValue(bt::hex_encode(app.sign_and_send(w.submit_ballot(items, *period, fee))));
              }));
        s.add("create_trade", {"request"}, 1, P([&app, &w](const Params& p) {
                  const UniValue& rq = p[0];
                  std::string mids = json::parse_str(req_field(rq, "market_id"));
                  size_t outcome = parse_usize(req_field(rq, "outcome_index"));
                  int64_t amount = json::parse_i64(req_field(rq, "shares_amount"));
                  uint64_t limit = json::parse_u64(req_field(rq, "limit_sats"));
                  std::optional<Address> trader;
                  if (!opt_field(rq, "trader_address").isNull()) trader = json::parse_address(opt_field(rq, "trader_address"));
                  std::string prevs = json::parse_str(req_field(rq, "prev_block_hash"));
                  MarketId id = parse_market_id_str(mids);
                  Market m = market_or_throw(app.node, id);
                  auto pb = bt::hex_decode(prevs);
                  if (!pb) throw app_error("invalid prev_block_hash hex: Invalid character");
                  if (pb->size() != 32) throw app_error("prev_block_hash must be 32 bytes (64 hex chars)");
                  Hash32 prev = Hash32::from_span(*pb);
                  Address tr = trader ? *trader : first_wallet_address(w);
                  auto at = w.authorize(w.trade(id, uint32_t(outcome), amount, tr, limit, pow_config(m), prev));
                  Writer wr;
                  bincode(wr, at);
                  UniValue o = obj();
                  o.pushKV("signed_tx_hex", bt::hex_encode(wr.buf));
                  o.pushKV("txid", bt::hex_encode(at.transaction.txid()));
                  return o;
              }));
    }
}

} // namespace hivemind
