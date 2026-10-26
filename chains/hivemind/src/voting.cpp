// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// lib/state/voting/mod.rs: closing a voting period. The matrix math runs in their code
// (math::run_consensus); the bookkeeping around it (reputation commit with the
// conservation checks and the re-anchoring to 1.0) is plain IEEE arithmetic in their
// order: sequential sums in address order starting from -0.0 (Rust's f64 `Sum`).
#include "state_internal.h"

#include <hivemind/fmt.h>
#include <hivemind/math.h>

#include <cmath>

namespace hivemind {

using bt::kv::Txn;

static double sum_values(const std::map<Address, double>& m)
{
    double s = -0.0;
    for (auto& [_, v] : m) s += v;
    return s;
}

// mod.rs:21-34
static std::map<Address, double> compute_post(const std::map<Address, double>& pre, const std::map<Address, double>& updates)
{
    auto post = pre;
    for (auto& [a, rep] : updates) {
        if (rep > 0.0) post[a] = rep;
        else post.erase(a);
    }
    return post;
}

// mod.rs:36-54
static void reanchor(std::map<Address, double>& post)
{
    double sum = sum_values(post);
    double residual = 1.0 - sum;
    if (residual == 0.0) return;
    if (post.empty()) return;
    // max_by(partial_cmp, then address): the last maximum in iteration order wins
    auto best = post.begin();
    for (auto it = post.begin(); it != post.end(); ++it) {
        auto& [a, v] = *it;
        auto& [ba, bv] = *best;
        int c;
        if (std::isnan(v) || std::isnan(bv)) c = 0;
        else c = v < bv ? -1 : v > bv ? 1 : 0;
        if (c == 0) c = a < ba ? -1 : ba < a ? 1 : 0;
        if (c >= 0) best = it; // max_by keeps the later element on ties
    }
    best->second += residual;
}

// mod.rs:56-86
static void verify_conservation(const std::map<Address, double>& pre, const std::map<Address, double>& post)
{
    double pre_sum = sum_values(pre), post_sum = sum_values(post);
    double drift = std::fabs(post_sum - pre_sum);
    if (drift > 1e-9)
        throw StateError("consensus violated per-block reputation conservation: pre_sum=" + fmt::display(pre_sum) +
                         ", post_sum=" + fmt::display(post_sum) + ", drift=" + fmt::display(drift));
    double anchor = std::fabs(post_sum - 1.0);
    if (anchor > 1e-6)
        throw StateError("reputation total drifted from anchor: total=" + fmt::display(post_sum) + ", anchor=1, drift=" + fmt::display(anchor));
}

static DecisionOutcome make_outcome(const DecisionId& id, uint32_t period, double value, double confidence, uint64_t total_votes,
                                    double total_weight, uint32_t min_votes, std::optional<std::optional<uint16_t>> cat)
{
    DecisionOutcome o;
    o.decision_id = id;
    o.period_id = period;
    o.outcome_value = value;
    o.min = 0.0;
    o.max = 1.0;
    o.confidence = std::clamp(confidence, 0.0, 1.0);
    o.total_votes = total_votes;
    o.total_reputation_weight = total_weight;
    o.finalized_at = 0;
    o.block_height = 0;
    o.is_consensus = true;
    o.res_status = 0;
    o.res_changed_at = 0;
    o.res_changed_height = 0;
    o.res_vote_count = 0;
    o.res_min_votes = min_votes;
    o.res_deadline = 0;
    o.res_outcome_ready = true;
    o.categorical_winner = cat;
    return o;
}

struct ConsensusResult {
    uint32_t period_id = 0;
    VotingPeriodStats stats;
    std::vector<DecisionOutcome> outcomes;
    std::vector<std::pair<DecisionId, double>> resolutions;
    std::map<Address, double> updated_reputations;
};

// mod.rs:108-375 compute_consensus
static std::optional<ConsensusResult> compute_consensus(const State& s, const Txn& t, uint32_t period, const std::vector<DecisionId>& decision_ids,
                                                        uint64_t ts, uint32_t h)
{
    if (!s.outcomes_for_period(t, period).empty()) return std::nullopt; // already calculated
    auto votes = s.votes_for_period(t, period);
    if (votes.empty()) {
        ConsensusResult r;
        r.period_id = period;
        for (auto& id : decision_ids) {
            r.outcomes.push_back(make_outcome(id, period, 0.5, 0.0, 0, 0.0, 0, std::nullopt));
            r.resolutions.emplace_back(id, 0.5);
        }
        r.stats.period_id = period;
        r.stats.calculated_at = 0;
        r.stats.certainty = 0.0;
        return r;
    }
    auto reputations = s.all_reputations(t);
    double total_weight = sum_values(reputations);
    if (reputations.empty() || total_weight <= 0.0) return std::nullopt;

    math::ConsensusInput in;
    for (auto& [a, rep] : reputations) in.reputations.emplace_back(a, rep);
    std::vector<DecisionId> decisions = decision_ids;
    std::sort(decisions.begin(), decisions.end());
    in.decisions = decisions;
    for (auto& id : decisions) {
        uint8_t scaled = 0;
        uint16_t catn = 0;
        if (auto e = s.decision_entry(t, id); e && e->decision) {
            if (e->decision->is_scaled()) scaled = 1;
            else if (auto n = e->decision->option_count()) catn = uint16_t(*n);
        }
        in.scaled.push_back(scaled);
        in.categorical_n.push_back(catn);
    }
    for (auto& [k, e] : votes)
        if (auto v = e.value.to_float()) in.votes.push_back({k.voter, k.decision_id, *v});
    std::string err;
    auto res = math::run_consensus(in, &err);
    if (!res) throw invalid_tx(err);

    ConsensusResult r;
    r.period_id = period;
    auto stats = s.period_stats(t, period).value_or(VotingPeriodStats{});
    if (!s.period_stats(t, period)) stats.period_id = period;
    stats.first_loading = res->first_loading;
    stats.certainty = res->certainty;
    std::map<Address, std::pair<double, double>> changes;
    for (size_t i = 0; i < in.reputations.size(); i++) {
        auto& a = in.reputations[i].first;
        r.updated_reputations[a] = res->updated_reputations[i];
        changes[a] = {reputations.count(a) ? reputations[a] : 0.0, res->updated_reputations[i]};
    }
    if (!changes.empty()) stats.reputation_changes = changes;
    r.stats = stats;
    // outcomes of the math in decision order (theirs: HashMap order; storage is keyed)
    std::set<DecisionId> resolved;
    for (size_t j = 0; j < decisions.size(); j++) {
        double v = res->outcomes[j].value_or(0.5);
        resolved.insert(decisions[j]);
        r.outcomes.push_back(make_outcome(decisions[j], period, v, 1.0, votes.size(), total_weight, 1, res->cat_winner[j]));
        r.resolutions.emplace_back(decisions[j], v);
    }
    for (auto& id : decision_ids)
        if (!resolved.count(id)) {
            r.outcomes.push_back(make_outcome(id, period, 0.5, 0.0, 0, 0.0, 0, std::nullopt));
            r.resolutions.emplace_back(id, 0.5);
        }
    return r;
}

std::optional<ConsensusUndoEntry> calculate_and_store_consensus(const State& s, Txn& t, uint32_t voting_period,
                                                                const std::vector<DecisionId>& decision_ids, uint64_t ts, uint32_t h)
{
    auto r = compute_consensus(s, t, voting_period, decision_ids, ts, h);
    if (!r) return std::nullopt;
    // validate_consensus_commit
    for (auto& [id, _] : r->resolutions)
        if (!s.decision_history(t, id))
            throw StateError("invalid decision ID: Pre-validation failed: DecisionEntry has no state history");
    ConsensusUndoEntry undo;
    undo.period_id = r->period_id;
    undo.had_period_stats = s.period_stats(t, r->period_id).has_value();
    for (auto& o : r->outcomes) undo.decision_outcome_ids.push_back(o.decision_id);
    for (auto& [id, _] : r->resolutions) undo.resolved_decision_ids.push_back(id);
    undo.pre_consensus_reputation = s.all_reputations(t);
    auto post = compute_post(undo.pre_consensus_reputation, r->updated_reputations);
    verify_conservation(undo.pre_consensus_reputation, post);
    reanchor(post);
    put_stats(t, r->stats);
    for (auto& o : r->outcomes) put_outcome(t, o);
    s.clear_and_restore_reputation(t, post);
    for (auto& [id, v] : r->resolutions) transition_decision_to_resolved(s, t, id, h, v);
    return undo;
}

// markets/block.rs:1381-1419
void revert_consensus(const State& s, Txn& t, const std::vector<ConsensusUndoEntry>& undo)
{
    for (auto it = undo.rbegin(); it != undo.rend(); ++it) {
        for (auto& id : it->decision_outcome_ids) t.del(State::T_DECISION_OUTCOMES, ByteSpan(id.b.data(), 3));
        if (!it->had_period_stats) t.del(State::T_PERIOD_STATS, u32_key(it->period_id));
        s.clear_and_restore_reputation(t, it->pre_consensus_reputation);
    }
}

} // namespace hivemind
