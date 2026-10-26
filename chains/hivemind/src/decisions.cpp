// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Block-time parts of lib/state/decisions/database.rs and pricing.rs: period minting,
// listing-fee repricing, the per-block pricing snapshot used for undo, decision state
// transitions and their rollback.
#include "state_internal.h"

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

// database.rs:173-190: the whole table, in key order, as Vec<(period, Some(pricing))>
void snapshot_period_pricing(const State& s, Txn& t, uint32_t h)
{
    Writer w;
    std::vector<std::pair<uint32_t, Bytes>> rows;
    t.scan(State::T_PERIOD_PRICING, [&](ByteSpan k, ByteSpan v) {
        Reader r(k);
        rows.emplace_back(r.u32(), Bytes(v.begin(), v.end()));
        return true;
    });
    w.u64(rows.size());
    for (auto& [p, v] : rows) {
        w.u32(p);
        w.u8(1);
        w.raw(v);
    }
    t.put(State::T_PERIOD_PRICING_UNDO, u32le(h), w.buf);
}

// database.rs:192-226
void restore_period_pricing_undo(const State& s, Txn& t, uint32_t h)
{
    auto v = t.get(State::T_PERIOD_PRICING_UNDO, u32le(h));
    if (!v) return;
    Reader r(*v);
    uint64_t n = r.u64();
    std::vector<std::pair<uint32_t, std::optional<PeriodPricing>>> snaps;
    std::set<uint32_t> keys;
    for (uint64_t i = 0; i < n; i++) {
        uint32_t p = r.u32();
        std::optional<PeriodPricing> pp;
        if (r.u8()) pp = pricing_from(r.raw(8 * 4 + 4 + 8 * 2));
        snaps.emplace_back(p, pp);
        keys.insert(p);
    }
    std::vector<uint32_t> current;
    t.scan(State::T_PERIOD_PRICING, [&](ByteSpan k, ByteSpan) {
        Reader kr(k);
        current.push_back(kr.u32());
        return true;
    });
    for (auto k : current)
        if (!keys.count(k)) t.del(State::T_PERIOD_PRICING, u32le(k));
    for (auto& [p, pp] : snaps)
        if (pp) s.put_period_pricing(t, p, *pp);
    t.del(State::T_PERIOD_PRICING_UNDO, u32le(h));
}

// database.rs:350-371: fill empty sets from the highest existing period + 1 up to target
static void mint_periods_up_to(const State& s, Txn& t, uint32_t target)
{
    uint32_t highest = 0;
    t.scan(State::T_PERIOD_DECISIONS, [&](ByteSpan k, ByteSpan) {
        Reader r(k);
        highest = std::max(highest, r.u32());
        return true;
    });
    for (uint64_t p = uint64_t(highest) + 1; p <= target; p++) s.put_period_decisions(t, uint32_t(p), {});
}

// database.rs:142-171
void mint_genesis(const State& s, Txn& t, uint64_t ts, uint32_t h, uint64_t genesis_ts)
{
    uint32_t current = get_current_period(ts, h, genesis_ts, s.config());
    uint32_t target = current + FUTURE_PERIODS - 1;
    mint_periods_up_to(s, t, target);
    for (uint32_t p = current; p <= target; p++) {
        if (s.period_pricing(t, p)) continue;
        uint32_t offset = p - current;
        uint64_t mints = uint64_t(FUTURE_PERIODS) > offset ? uint64_t(FUTURE_PERIODS) - offset : 0;
        PeriodPricing pp;
        pp.p_period = GENESIS_P_PERIOD_SATS;
        pp.p_floor = GENESIS_P_PERIOD_SATS / 16;
        pp.mints = mints;
        pp.claimed = 0;
        pp.last_reprice_block = h;
        pp.window_open_claimed = 0;
        pp.window_open_mints = mints;
        s.put_period_pricing(t, p, pp);
    }
}

// pricing.rs:53-73
static void apply_reprice_with_floor(PeriodPricing& p, uint32_t h)
{
    if (p.last_reprice_block == h) return;
    uint64_t sales = p.claimed > p.window_open_claimed ? p.claimed - p.window_open_claimed : 0;
    unsigned __int128 cap = (unsigned __int128)p.window_open_mints * 25;
    uint64_t capsat = cap > UINT64_MAX ? UINT64_MAX : uint64_t(cap); // saturating_mul
    uint64_t available = capsat > p.window_open_claimed ? capsat - p.window_open_claimed : 0;
    uint64_t np = std::max(math::reprice(p.p_period, sales, available), p.p_floor);
    p.p_period = np;
    p.last_reprice_block = h;
    p.window_open_claimed = p.claimed;
    p.window_open_mints = p.mints;
}

// database.rs:322-340
static uint64_t compute_p_average(const State& s, const Txn& t, uint32_t current)
{
    unsigned __int128 sum = 0, count = 0;
    for (uint32_t off = 1; off < FUTURE_PERIODS; off++)
        if (auto p = s.period_pricing(t, current + off)) {
            sum += p->p_period;
            count += 1;
        }
    if (count == 0) return GENESIS_P_PERIOD_SATS;
    return uint64_t(sum / count);
}

static std::optional<uint32_t> highest_minted(const Txn& t)
{
    std::optional<uint32_t> hi;
    t.scan(State::T_PERIOD_DECISIONS, [&](ByteSpan k, ByteSpan) {
        Reader r(k);
        uint32_t p = r.u32();
        hi = hi ? std::max(*hi, p) : p;
        return true;
    });
    return hi;
}

// database.rs:232-302
void process_block_pricing(const State& s, Txn& t, uint32_t h, uint64_t ts, uint64_t genesis_ts)
{
    uint32_t new_current = get_current_period(ts, h, genesis_ts, s.config());
    uint32_t target = new_current + FUTURE_PERIODS - 1;
    uint32_t prev_highest = highest_minted(t).value_or(0);
    uint32_t prev_current = prev_highest + 1 > FUTURE_PERIODS ? prev_highest + 1 - FUTURE_PERIODS : 0;
    mint_periods_up_to(s, t, target);
    bool did_transition = new_current > prev_current;
    if (did_transition) {
        for (uint64_t np = uint64_t(prev_highest) + 1; np <= target; np++) {
            uint64_t seed = compute_p_average(s, t, new_current);
            s.put_period_pricing(t, uint32_t(np), PeriodPricing::seeded(seed, h));
        }
        uint32_t surviving_end = std::min(prev_highest, target);
        if (new_current <= surviving_end)
            for (uint64_t p = new_current; p <= surviving_end; p++) {
                auto pp = s.period_pricing(t, uint32_t(p));
                if (!pp) continue;
                uint64_t transitions = new_current - prev_current;
                pp->mints = pp->mints + transitions < pp->mints ? UINT64_MAX : pp->mints + transitions;
                apply_reprice_with_floor(*pp, h);
                s.put_period_pricing(t, uint32_t(p), *pp);
            }
    } else {
        uint32_t interval = reprice_interval(s.config());
        uint32_t active_end = new_current + FUTURE_PERIODS - 1;
        bool any_due = false;
        for (uint64_t p = new_current; p <= active_end && !any_due; p++)
            if (auto pp = s.period_pricing(t, uint32_t(p)))
                if ((h > pp->last_reprice_block ? h - pp->last_reprice_block : 0) >= interval) any_due = true;
        if (any_due)
            for (uint64_t p = new_current; p <= active_end; p++) {
                auto pp = s.period_pricing(t, uint32_t(p));
                if (!pp) continue;
                apply_reprice_with_floor(*pp, h);
                s.put_period_pricing(t, uint32_t(p), *pp);
            }
    }
}

// database.rs:392-410
void delete_periods_above(const State& s, Txn& t, uint32_t max_period)
{
    std::vector<uint32_t> del;
    t.scan(State::T_PERIOD_DECISIONS, [&](ByteSpan k, ByteSpan) {
        Reader r(k);
        uint32_t p = r.u32();
        if (p > max_period) del.push_back(p);
        return true;
    });
    for (auto p : del) t.del(State::T_PERIOD_DECISIONS, u32le(p));
}

// database.rs:1045-1094
void rollback_decision_states_to_height(const State& s, Txn& t, uint32_t h)
{
    std::vector<std::pair<DecisionId, DecisionStateHistory>> upd;
    std::vector<DecisionId> unclaim;
    for (auto& [id, hist] : s.all_decision_histories(t)) {
        bool was_claimed = hist.current_state() != DecisionState::Created;
        auto& v = hist.state_history;
        v.erase(std::remove_if(v.begin(), v.end(), [&](const DecisionStateHistory::Item& i) { return i.height > h; }), v.end());
        bool now_created = hist.current_state() == DecisionState::Created;
        if (was_claimed && now_created) unclaim.push_back(id);
        upd.emplace_back(id, hist);
    }
    for (auto& [id, hist] : upd) s.put_decision_history(t, id, hist);
    for (auto& id : unclaim) {
        uint32_t p = id.period_index();
        if (!t.get(State::T_PERIOD_DECISIONS, u32le(p))) continue;
        auto set = s.period_decisions(t, p);
        set.erase(std::remove_if(set.begin(), set.end(), [&](const DecisionEntry& e) { return e.decision_id == id; }), set.end());
        if (set.empty()) t.del(State::T_PERIOD_DECISIONS, u32le(p));
        else s.put_period_decisions(t, p, set);
    }
}

static std::string did_debug(const DecisionId& d)
{
    return "DecisionId([" + std::to_string(d.b[0]) + ", " + std::to_string(d.b[1]) + ", " + std::to_string(d.b[2]) + "])";
}

// database.rs:1004-1043, types.rs:305-346
void transition_decision_to_voting(const State& s, Txn& t, const DecisionId& id, uint32_t h)
{
    auto hist = s.decision_history(t, id);
    if (!hist) throw StateError("invalid decision ID: Decision " + did_debug(id) + " has no state history");
    auto cur = hist->current_state();
    if (cur != DecisionState::Claimed) // can_transition_to(Voting): Claimed -> Voting only
        throw StateError(std::string("invalid decision state: Cannot transition to Voting from ") + to_string(cur));
    hist->state_history.push_back({DecisionState::Voting, h, std::nullopt});
    s.put_decision_history(t, id, *hist);
}

void transition_decision_to_resolved(const State& s, Txn& t, const DecisionId& id, uint32_t h, double outcome)
{
    auto hist = s.decision_history(t, id);
    if (!hist) throw StateError("invalid decision ID: Decision " + did_debug(id) + " has no state history");
    auto cur = hist->current_state();
    if (cur != DecisionState::Voting)
        throw StateError(std::string("invalid decision state: Cannot transition to Resolved from ") + to_string(cur));
    if (outcome < -1.0) throw StateError("invalid decision state: Consensus outcome is invalid (< -1.0)");
    hist->state_history.push_back({DecisionState::Resolved, h, outcome});
    s.put_decision_history(t, id, *hist);
}

// database.rs:506-549 (in decision_state_histories key order)
std::vector<DecisionId> claimed_decisions_needing_voting(const State& s, const Txn& t, uint32_t current_period)
{
    std::vector<DecisionId> r;
    for (auto& [id, hist] : s.all_decision_histories(t))
        if (hist.current_state() == DecisionState::Claimed && current_period > id.period_index()) r.push_back(id);
    return r;
}

std::vector<DecisionId> voting_decisions_needing_resolution(const State& s, const Txn& t, uint32_t current_period)
{
    std::vector<DecisionId> r;
    for (auto& [id, hist] : s.all_decision_histories(t))
        if (hist.current_state() == DecisionState::Voting && current_period > id.voting_period()) r.push_back(id);
    return r;
}

} // namespace hivemind
