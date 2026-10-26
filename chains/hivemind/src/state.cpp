// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Tables of the state (lib/state/mod.rs, reputation.rs, decisions/database.rs,
// markets/database.rs, voting/database.rs): getters, writers and value encodings. Keys
// are their SerdeBincode keys, so our memcmp-ordered store iterates them in LMDB order.
#include <hivemind/fmt.h>
#include <hivemind/math.h>
#include <hivemind/state.h>

#include <bt/crypto.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hivemind {

using bt::kv::Txn;
using bt::utreexo::NodeHash;

StateError invalid_tx(const std::string& reason) { return StateError("invalid transaction: " + reason); }

const char* to_string(BundleStatus s)
{
    switch (s) {
    case BundleStatus::Confirmed: return "Confirmed";
    case BundleStatus::Dropped: return "Dropped";
    case BundleStatus::Failed: return "Failed";
    case BundleStatus::Pending: return "Pending";
    case BundleStatus::Submitted: return "Submitted";
    case BundleStatus::SubmittedUnexpected: return "SubmittedUnexpected";
    }
    return "?";
}

const char* to_string(DecisionState s)
{
    switch (s) {
    case DecisionState::Created: return "Created";
    case DecisionState::Claimed: return "Claimed";
    case DecisionState::Voting: return "Voting";
    case DecisionState::Resolved: return "Resolved";
    case DecisionState::Invalid: return "Invalid";
    }
    return "?";
}

const char* to_string(MarketState s)
{
    switch (s) {
    case MarketState::Trading: return "Trading";
    case MarketState::Cancelled: return "Cancelled";
    case MarketState::Invalid: return "Invalid";
    case MarketState::Settled: return "Settled";
    }
    return "?";
}

// types/market.rs MarketState::can_transition_to
bool can_transition(MarketState from, MarketState to)
{
    using M = MarketState;
    if (from == to) return true;
    if (from == M::Trading) return true;
    if (from == M::Invalid && to == M::Settled) return true;
    return false;
}

// ------------------------------------------------------------------ AccumulatorDiff
void AccumulatorDiff::insert(const NodeHash& h)
{
    auto it = idx.find(h);
    if (it != idx.end()) {
        if (!it->second->second) { // removal pending: cancel it
            order.erase(it->second);
            idx.erase(it);
        }
        return; // inserting twice -> one insertion
    }
    order.emplace_back(h, true);
    idx[h] = std::prev(order.end());
}

void AccumulatorDiff::remove(const NodeHash& h)
{
    auto it = idx.find(h);
    if (it != idx.end()) {
        if (it->second->second) { // insertion pending: cancel it
            order.erase(it->second);
            idx.erase(it);
        }
        return;
    }
    order.emplace_back(h, false);
    idx[h] = std::prev(order.end());
}

void AccumulatorDiff::split(std::vector<NodeHash>& ins, std::vector<NodeHash>& dels) const
{
    for (auto& [h, add] : order) (add ? ins : dels).push_back(h);
}

// ------------------------------------------------------------------ withdrawal bundle
static uint32_t varint_size(uint64_t n) { return uint32_t(bt::btc::compact_size_len(n)); }

static std::optional<uint32_t> txout_size(uint32_t spk_size)
{
    uint64_t s = 8ull + varint_size(spk_size) + spk_size;
    if (s > UINT32_MAX) return std::nullopt;
    return uint32_t(s);
}

bool predict_weight_ok(uint32_t n_outputs, uint32_t sum_txout_sizes)
{
    const uint64_t TXIN = 36 + 1 + 0 + 4; // treasury input, empty scriptSig
    uint64_t no = uint64_t(n_outputs) + 2;
    if (no > UINT32_MAX) return false;
    uint64_t sum = uint64_t(*txout_size(34)) + *txout_size(10) + sum_txout_sizes;
    if (sum > UINT32_MAX) return false;
    uint64_t base = 4 + (varint_size(1) + TXIN) + (varint_size(no) + sum) + 4;
    if (base > UINT32_MAX) return false;
    return base * 4 <= 400000; // MAX_STANDARD_WEIGHT
}

std::optional<WithdrawalBundle> make_withdrawal_bundle(uint32_t block_height, uint64_t fee, std::map<OutPoint, Output> spend_utxos,
                                                       std::vector<bt::btc::TxOut> payouts)
{
    // inputs commitment: hash(Vec<OutPoint> = sorted keys ++ [Regular{0^32, block_height}])  types/lib.rs
    Writer w;
    w.borsh_len(spend_utxos.size() + 1);
    for (auto& [op, _] : spend_utxos) borsh(w, op);
    borsh(w, OutPoint::regular(Hash32{}, block_height));
    Hash32 commitment = bt::blake3(w.buf);
    Writer fw;
    fw.u64be(fee);
    WithdrawalBundle b;
    b.spend_utxos = std::move(spend_utxos);
    b.outputs.push_back({0, bt::btc::op_return_push(fw.buf)});
    b.outputs.push_back({0, bt::btc::op_return_push(commitment.span())});
    for (auto& p : payouts) b.outputs.push_back(std::move(p));
    if (bt::btc::inputless_tx_weight(b.outputs) > 400000) return std::nullopt; // BundleTooHeavy
    return b;
}

// ------------------------------------------------------------------ encodings
static Bytes u32le(uint32_t v)
{
    Writer w;
    w.u32(v);
    return w.buf;
}
static const Bytes UNIT{};
static Bytes span_bytes(ByteSpan s) { return Bytes(s.begin(), s.end()); }

static void enc_str(Writer& w, const std::string& s) { w.bincode_str(s); }
static void enc_strs(Writer& w, const std::vector<std::string>& v)
{
    w.u64(v.size());
    for (auto& s : v) w.bincode_str(s);
}
static std::vector<std::string> dec_strs(Reader& r)
{
    std::vector<std::string> v;
    uint64_t n = r.u64();
    for (uint64_t i = 0; i < n; i++) v.push_back(r.bincode_str());
    return v;
}
static void enc_opt_str(Writer& w, const std::optional<std::string>& s)
{
    if (!s) return w.u8(0);
    w.u8(1);
    w.bincode_str(*s);
}
static std::optional<std::string> dec_opt_str(Reader& r)
{
    if (!r.u8()) return std::nullopt;
    return r.bincode_str();
}
static void enc_f64(Writer& w, double d) { bincode_f64(w, d); }
static double dec_f64(Reader& r) { return de_f64(r); }
static void enc_mid(Writer& w, const MarketId& m) { w.raw(ByteSpan(m.b.data(), 6)); }
static MarketId dec_mid(Reader& r)
{
    MarketId m;
    auto a = r.array<6>();
    std::memcpy(m.b.data(), a.data(), 6);
    return m;
}
static void enc_did(Writer& w, const DecisionId& d) { w.raw(ByteSpan(d.b.data(), 3)); }
static DecisionId dec_did(Reader& r)
{
    DecisionId d;
    auto a = r.array<3>();
    std::memcpy(d.b.data(), a.data(), 3);
    return d;
}

void enc_spent(Writer& w, const SpentOutput& s)
{
    bincode(w, s.output);
    bincode(w, s.inpoint);
}
SpentOutput dec_spent(Reader& r)
{
    SpentOutput s;
    s.output = de_output(r);
    s.inpoint = de_inpoint(r);
    return s;
}

static void enc_utxo_map(Writer& w, const std::map<OutPoint, Output>& m)
{
    w.u64(m.size());
    for (auto& [op, o] : m) {
        bincode(w, op);
        bincode(w, o);
    }
}
static std::map<OutPoint, Output> dec_utxo_map(Reader& r)
{
    std::map<OutPoint, Output> m;
    uint64_t n = r.u64();
    for (uint64_t i = 0; i < n; i++) {
        OutPoint op = de_outpoint(r);
        m[op] = de_output(r);
    }
    return m;
}

void enc_bundle_info(Writer& w, const WithdrawalBundleInfo& i)
{
    w.u8(i.kind);
    if (i.kind == WithdrawalBundleInfo::Known) {
        enc_utxo_map(w, i.bundle.spend_utxos);
        w.u64(i.bundle.outputs.size());
        for (auto& o : i.bundle.outputs) {
            w.u64(o.value);
            w.bincode_bytes(o.script_pubkey);
        }
    } else if (i.kind == WithdrawalBundleInfo::UnknownConfirmed) {
        enc_utxo_map(w, i.spend_utxos);
    }
}
WithdrawalBundleInfo dec_bundle_info(Reader& r)
{
    WithdrawalBundleInfo i;
    i.kind = WithdrawalBundleInfo::Kind(r.u8());
    if (i.kind == WithdrawalBundleInfo::Known) {
        i.bundle.spend_utxos = dec_utxo_map(r);
        uint64_t n = r.u64();
        for (uint64_t k = 0; k < n; k++) {
            bt::btc::TxOut o;
            o.value = r.u64();
            o.script_pubkey = r.bincode_bytes();
            i.bundle.outputs.push_back(std::move(o));
        }
    } else if (i.kind == WithdrawalBundleInfo::UnknownConfirmed) {
        i.spend_utxos = dec_utxo_map(r);
    }
    return i;
}

template <class T, class F>
static void enc_rollback(Writer& w, const RollBack<T>& rb, F encv)
{
    w.u64(rb.v.size());
    for (auto& hs : rb.v) {
        encv(w, hs.value);
        w.u32(hs.height);
    }
}
template <class T, class F>
static RollBack<T> dec_rollback(Reader& r, F decv)
{
    RollBack<T> rb;
    uint64_t n = r.u64();
    for (uint64_t i = 0; i < n; i++) {
        T v = decv(r);
        uint32_t h = r.u32();
        rb.v.push_back({v, h});
    }
    if (rb.v.empty()) throw bt::DecodeError("empty RollBack");
    return rb;
}

static Bytes m6id_key(const Hash32& m)
{
    Writer w;
    w.bincode_bytes(m.span()); // SerdeBincode<M6id>: u64 length + 32 bytes
    return w.buf;
}

void enc_decision(Writer& w, const Decision& d)
{
    w.raw(d.market_maker_pubkey_hash);
    bincode(w, d.decision_type);
    enc_str(w, d.header);
    enc_str(w, d.description);
    enc_opt_str(w, d.option_0_label);
    enc_opt_str(w, d.option_1_label);
    enc_strs(w, d.tags);
}
Decision dec_decision(Reader& r)
{
    Decision d;
    d.market_maker_pubkey_hash = r.array<20>();
    d.decision_type = de_decision_type(r);
    d.header = r.bincode_str();
    d.description = r.bincode_str();
    d.option_0_label = dec_opt_str(r);
    d.option_1_label = dec_opt_str(r);
    d.tags = dec_strs(r);
    return d;
}

static void enc_entries(Writer& w, const std::vector<DecisionEntry>& v)
{
    w.u64(v.size());
    for (auto& e : v) {
        enc_did(w, e.decision_id);
        if (e.decision) {
            w.u8(1);
            enc_decision(w, *e.decision);
        } else {
            w.u8(0);
        }
        w.raw(e.claiming_txid);
    }
}
static std::vector<DecisionEntry> dec_entries(Reader& r)
{
    std::vector<DecisionEntry> v;
    uint64_t n = r.u64();
    for (uint64_t i = 0; i < n; i++) {
        DecisionEntry e;
        e.decision_id = dec_did(r);
        if (r.u8()) e.decision = dec_decision(r);
        e.claiming_txid = r.array<32>();
        v.push_back(std::move(e));
    }
    return v;
}

static void enc_history(Writer& w, const DecisionStateHistory& h)
{
    w.u64(h.state_history.size());
    for (auto& it : h.state_history) {
        w.u32(uint32_t(it.state));
        w.u32(it.height);
        if (it.outcome) {
            w.u8(1);
            enc_f64(w, *it.outcome);
        } else {
            w.u8(0);
        }
    }
}
static DecisionStateHistory dec_history(Reader& r)
{
    DecisionStateHistory h;
    uint64_t n = r.u64();
    for (uint64_t i = 0; i < n; i++) {
        DecisionStateHistory::Item it;
        it.state = DecisionState(r.u32());
        it.height = r.u32();
        if (r.u8()) it.outcome = dec_f64(r);
        h.state_history.push_back(it);
    }
    return h;
}

static void enc_pricing(Writer& w, const PeriodPricing& p)
{
    w.u64(p.p_period);
    w.u64(p.p_floor);
    w.u64(p.mints);
    w.u64(p.claimed);
    w.u32(p.last_reprice_block);
    w.u64(p.window_open_claimed);
    w.u64(p.window_open_mints);
}
static PeriodPricing dec_pricing(Reader& r)
{
    PeriodPricing p;
    p.p_period = r.u64();
    p.p_floor = r.u64();
    p.mints = r.u64();
    p.claimed = r.u64();
    p.last_reprice_block = r.u32();
    p.window_open_claimed = r.u64();
    p.window_open_mints = r.u64();
    return p;
}

void enc_market(Writer& w, const Market& m)
{
    enc_mid(w, m.id);
    enc_str(w, m.title);
    enc_str(w, m.description);
    enc_strs(w, m.tags);
    w.raw(m.creator_address.b);
    w.u64(m.dimension_specs.size());
    for (auto& d : m.dimension_specs) {
        w.u32(d.kind);
        enc_did(w, d.id);
    }
    w.u64(m.decision_ids.size());
    for (auto& d : m.decision_ids) enc_did(w, d);
    w.u64(m.state_combos.size());
    for (auto& c : m.state_combos) {
        w.u64(c.size());
        for (auto x : c) w.u64(x);
    }
    w.u32(m.created_at_height);
    if (m.expires_at_height) {
        w.u8(1);
        w.u32(*m.expires_at_height);
    } else {
        w.u8(0);
    }
    w.u8(m.tau_from_now);
    w.u64(m.storage_fee_sats);
    w.u32(uint32_t(m.market_state));
    enc_f64(w, m.trading_fee);
    w.u64(m.liquidity_base_sats);
    w.u64(m.shares.size());
    for (auto s : m.shares) w.u64(uint64_t(s));
    w.u64(m.final_prices.size());
    for (auto p : m.final_prices) enc_f64(w, p);
    w.u64(m.version);
    w.u32(m.last_updated_height);
    w.u64(m.total_volume_sats);
    w.u64(m.outcome_volumes_sats.size());
    for (auto v : m.outcome_volumes_sats) w.u64(v);
    w.u8(m.tx_pow_hash_selector);
    w.u8(m.tx_pow_ordering);
    w.u8(m.tx_pow_difficulty);
}

Market dec_market(Reader& r)
{
    Market m;
    m.id = dec_mid(r);
    m.title = r.bincode_str();
    m.description = r.bincode_str();
    m.tags = dec_strs(r);
    m.creator_address.b = r.array<20>();
    uint64_t n = r.u64();
    for (uint64_t i = 0; i < n; i++) {
        DimensionSpec d;
        d.kind = DimensionSpec::Kind(r.u32());
        d.id = dec_did(r);
        m.dimension_specs.push_back(d);
    }
    n = r.u64();
    for (uint64_t i = 0; i < n; i++) m.decision_ids.push_back(dec_did(r));
    n = r.u64();
    for (uint64_t i = 0; i < n; i++) {
        std::vector<uint64_t> c;
        uint64_t k = r.u64();
        for (uint64_t j = 0; j < k; j++) c.push_back(r.u64());
        m.state_combos.push_back(std::move(c));
    }
    m.created_at_height = r.u32();
    if (r.u8()) m.expires_at_height = r.u32();
    m.tau_from_now = r.u8();
    m.storage_fee_sats = r.u64();
    m.market_state = MarketState(r.u32());
    m.trading_fee = dec_f64(r);
    m.liquidity_base_sats = r.u64();
    n = r.u64();
    for (uint64_t i = 0; i < n; i++) m.shares.push_back(int64_t(r.u64()));
    n = r.u64();
    for (uint64_t i = 0; i < n; i++) m.final_prices.push_back(dec_f64(r));
    m.version = r.u64();
    m.last_updated_height = r.u32();
    m.total_volume_sats = r.u64();
    n = r.u64();
    for (uint64_t i = 0; i < n; i++) m.outcome_volumes_sats.push_back(r.u64());
    m.tx_pow_hash_selector = r.u8();
    m.tx_pow_ordering = r.u8();
    m.tx_pow_difficulty = r.u8();
    return m;
}

static void enc_account(Writer& w, const ShareAccount& a)
{
    w.u64(a.positions.size());
    for (auto& [k, v] : a.positions) {
        enc_mid(w, k.first);
        w.u32(k.second);
        w.u64(uint64_t(v));
    }
    w.u32(a.last_updated_height);
}
static ShareAccount dec_account(Reader& r)
{
    ShareAccount a;
    uint64_t n = r.u64();
    for (uint64_t i = 0; i < n; i++) {
        MarketId m = dec_mid(r);
        uint32_t o = r.u32();
        a.positions[{m, o}] = int64_t(r.u64());
    }
    a.last_updated_height = r.u32();
    return a;
}

static void enc_vote_value(Writer& w, const VoteValue& v)
{
    w.u32(v.kind);
    if (v.kind == VoteValue::Binary) w.u8(v.b);
    else if (v.kind == VoteValue::Scalar) enc_f64(w, v.s);
    else if (v.kind == VoteValue::Categorical) w.u16(v.c);
}
static VoteValue dec_vote_value(Reader& r)
{
    VoteValue v;
    v.kind = VoteValue::Kind(r.u32());
    if (v.kind == VoteValue::Binary) v.b = r.u8() != 0;
    else if (v.kind == VoteValue::Scalar) v.s = dec_f64(r);
    else if (v.kind == VoteValue::Categorical) v.c = r.u16();
    return v;
}

static Bytes vote_key(uint32_t period, const Address& a, const DecisionId& d)
{
    Writer w;
    w.u32(period); // VotingPeriodId(u32): bincode little-endian
    w.raw(a.b);
    enc_did(w, d);
    return w.buf;
}

static void enc_outcome(Writer& w, const DecisionOutcome& o)
{
    enc_did(w, o.decision_id);
    w.u32(o.period_id);
    enc_f64(w, o.outcome_value);
    enc_f64(w, o.min);
    enc_f64(w, o.max);
    enc_f64(w, o.confidence);
    w.u64(o.total_votes);
    enc_f64(w, o.total_reputation_weight);
    w.u64(o.finalized_at);
    w.u32(o.block_height);
    w.u8(o.is_consensus);
    // DecisionResolution (serde bincode, as their decision_outcomes table holds it)
    enc_did(w, o.decision_id);
    w.u32(o.period_id);
    w.u32(o.res_status);
    w.u64(o.res_changed_at);
    w.u32(o.res_changed_height);
    w.u32(o.res_vote_count);
    w.u32(o.res_min_votes);
    w.u64(o.res_deadline);
    w.u8(o.res_outcome_ready);
    w.u8(0); // reason: None
    if (!o.categorical_winner) {
        w.u8(0);
    } else {
        w.u8(1);
        if (*o.categorical_winner) {
            w.u8(1);
            w.u16(**o.categorical_winner);
        } else {
            w.u8(0);
        }
    }
}
static DecisionOutcome dec_outcome(Reader& r)
{
    DecisionOutcome o;
    o.decision_id = dec_did(r);
    o.period_id = r.u32();
    o.outcome_value = dec_f64(r);
    o.min = dec_f64(r);
    o.max = dec_f64(r);
    o.confidence = dec_f64(r);
    o.total_votes = r.u64();
    o.total_reputation_weight = dec_f64(r);
    o.finalized_at = r.u64();
    o.block_height = r.u32();
    o.is_consensus = r.u8();
    dec_did(r);
    r.u32();
    o.res_status = uint8_t(r.u32());
    o.res_changed_at = r.u64();
    o.res_changed_height = r.u32();
    o.res_vote_count = r.u32();
    o.res_min_votes = r.u32();
    o.res_deadline = r.u64();
    o.res_outcome_ready = r.u8();
    if (r.u8()) r.bincode_str();
    if (r.u8()) {
        if (r.u8()) o.categorical_winner = std::optional<uint16_t>(r.u16());
        else o.categorical_winner = std::optional<uint16_t>();
    }
    return o;
}

static void enc_stats(Writer& w, const VotingPeriodStats& s)
{
    w.u32(s.period_id);
    w.u64(s.total_voters);
    w.u64(s.total_votes);
    w.u64(s.total_decisions);
    enc_f64(w, s.avg_participation_rate);
    enc_f64(w, s.total_reputation_weight);
    w.u64(s.consensus_decisions);
    w.u64(s.calculated_at);
    if (s.first_loading) {
        w.u8(1);
        w.u64(s.first_loading->size());
        for (auto x : *s.first_loading) enc_f64(w, x);
    } else {
        w.u8(0);
    }
    if (s.certainty) {
        w.u8(1);
        enc_f64(w, *s.certainty);
    } else {
        w.u8(0);
    }
    if (s.reputation_changes) {
        w.u8(1);
        w.u64(s.reputation_changes->size());
        for (auto& [a, p] : *s.reputation_changes) {
            w.raw(a.b);
            enc_f64(w, p.first);
            enc_f64(w, p.second);
        }
    } else {
        w.u8(0);
    }
}
static VotingPeriodStats dec_stats(Reader& r)
{
    VotingPeriodStats s;
    s.period_id = r.u32();
    s.total_voters = r.u64();
    s.total_votes = r.u64();
    s.total_decisions = r.u64();
    s.avg_participation_rate = dec_f64(r);
    s.total_reputation_weight = dec_f64(r);
    s.consensus_decisions = r.u64();
    s.calculated_at = r.u64();
    if (r.u8()) {
        std::vector<double> v;
        uint64_t n = r.u64();
        for (uint64_t i = 0; i < n; i++) v.push_back(dec_f64(r));
        s.first_loading = v;
    }
    if (r.u8()) s.certainty = dec_f64(r);
    if (r.u8()) {
        std::map<Address, std::pair<double, double>> m;
        uint64_t n = r.u64();
        for (uint64_t i = 0; i < n; i++) {
            Address a;
            a.b = r.array<20>();
            double x = dec_f64(r), y = dec_f64(r);
            m[a] = {x, y};
        }
        s.reputation_changes = m;
    }
    return s;
}

static void enc_twpd_events(Writer& w, const std::vector<TwoWayPegEvent>& v)
{
    w.u64(v.size());
    for (auto& e : v) {
        w.u32(e.kind);
        bincode(w, e.outpoint);
        if (e.kind == TwoWayPegEvent::Deposit) {
            bincode(w, e.output);
        } else if (e.kind == TwoWayPegEvent::BundleSpend) {
            w.bincode_bytes(e.m6id.span());
        } else {
            bincode(w, e.output);
            w.bincode_bytes(e.m6id.span());
        }
    }
}
static std::vector<TwoWayPegEvent> dec_twpd_events(Reader& r)
{
    std::vector<TwoWayPegEvent> v;
    uint64_t n = r.u64();
    for (uint64_t i = 0; i < n; i++) {
        TwoWayPegEvent e;
        e.kind = TwoWayPegEvent::Kind(r.u32());
        e.outpoint = de_outpoint(r);
        if (e.kind == TwoWayPegEvent::Deposit) {
            e.output = de_output(r);
        } else if (e.kind == TwoWayPegEvent::BundleSpend) {
            e.m6id = Hash32::from_span(r.bincode_bytes());
        } else {
            e.output = de_output(r);
            e.m6id = Hash32::from_span(r.bincode_bytes());
        }
        v.push_back(e);
    }
    return v;
}

// ------------------------------------------------------------------ decisions (types.rs)
std::optional<Decision> Decision::make(const bt::Hash20& maker, DecisionType t, std::string header, std::string description,
                                       std::optional<std::string> l0, std::optional<std::string> l1, std::vector<std::string> tags,
                                       std::string* err)
{
    auto fail = [&](const std::string& m) -> std::optional<Decision> {
        if (err) *err = m;
        return std::nullopt;
    };
    if (header.size() > 100) return fail("invalid decision ID: Header must be 100 bytes or less");
    if (description.size() > 2000) return fail("invalid decision ID: Description must be 2000 bytes or less");
    if (tags.size() > 10) return fail("invalid decision ID: Maximum 10 tags allowed");
    for (auto& tag : tags)
        if (tag.size() > 50) return fail("invalid decision ID: Tag '" + tag + "' exceeds 50 byte limit");
    if (t.kind == DecisionType::Scaled) {
        double mn = t.min, mx = t.max, inc = t.increment;
        if (!std::isfinite(mn) || !std::isfinite(mx) || !std::isfinite(inc) || mn >= mx || inc <= 0.0)
            return fail("invalid scaled decision: invalid bounds: min=" + fmt::display(mn) + ", max=" + fmt::display(mx) +
                        ", increment=" + fmt::display(inc));
        // is_step_aligned(max - min, increment) (types.rs:8-12)
        double delta = mx - mn;
        double k = std::round(delta / inc);
        double tol = std::fmax(std::fabs(inc) * 1e-9, 1e-12);
        if (!(std::fabs(delta - k * inc) <= tol))
            return fail("invalid scaled decision: (max - min) = " + fmt::display(delta) + " is not an integer multiple of increment " +
                        fmt::display(inc));
    } else if (t.kind == DecisionType::Category) {
        if (t.options.size() < 2) return fail("invalid decision ID: Category decisions require at least 2 options");
    }
    Decision d;
    d.market_maker_pubkey_hash = maker;
    d.decision_type = std::move(t);
    d.header = std::move(header);
    d.description = std::move(description);
    d.option_0_label = std::move(l0);
    d.option_1_label = std::move(l1);
    d.tags = std::move(tags);
    return d;
}

std::optional<size_t> Decision::option_count() const
{
    if (decision_type.kind == DecisionType::Category) return decision_type.options.size();
    return std::nullopt;
}
std::optional<uint16_t> Decision::inconclusive_index() const
{
    auto n = option_count();
    if (!n) return std::nullopt;
    return uint16_t(*n);
}
std::pair<std::string, std::string> Decision::binary_labels() const
{
    return {option_0_label.value_or("No"), option_1_label.value_or("Yes")};
}
double Decision::normalize_value(double v) const
{
    if (std::isnan(v)) return v;
    if (decision_type.kind == DecisionType::Scaled) {
        double range = decision_type.max - decision_type.min;
        if (range == 0.0) return 0.5;
        return (v - decision_type.min) / range;
    }
    return v;
}
double Decision::denormalize_value(double v) const
{
    if (std::isnan(v)) return v;
    if (decision_type.kind == DecisionType::Scaled) {
        double range = decision_type.max - decision_type.min;
        return v * range + decision_type.min;
    }
    return v;
}

static int cmp_f64_total(double a, double b)
{
    // f64::total_cmp
    int64_t x, y;
    std::memcpy(&x, &a, 8);
    std::memcpy(&y, &b, 8);
    x ^= int64_t(uint64_t(x >> 63) >> 1);
    y ^= int64_t(uint64_t(y >> 63) >> 1);
    return x < y ? -1 : x > y ? 1 : 0;
}
static int cmp_type(const DecisionType& a, const DecisionType& b)
{
    if (a.kind != b.kind) return a.kind < b.kind ? -1 : 1;
    if (a.kind == DecisionType::Scaled) {
        if (int c = cmp_f64_total(a.min, b.min)) return c;
        if (int c = cmp_f64_total(a.max, b.max)) return c;
        return cmp_f64_total(a.increment, b.increment);
    }
    if (a.kind == DecisionType::Category) return a.options < b.options ? -1 : b.options < a.options ? 1 : 0;
    return 0;
}
template <class T>
static int cmp3(const T& a, const T& b)
{
    return a < b ? -1 : b < a ? 1 : 0;
}
int compare(const Decision& a, const Decision& b)
{
    if (int c = cmp3(a.market_maker_pubkey_hash, b.market_maker_pubkey_hash)) return c;
    if (int c = cmp_type(a.decision_type, b.decision_type)) return c;
    if (int c = cmp3(a.header, b.header)) return c;
    if (int c = cmp3(a.description, b.description)) return c;
    if (int c = cmp3(a.option_0_label, b.option_0_label)) return c;
    if (int c = cmp3(a.option_1_label, b.option_1_label)) return c;
    return cmp3(a.tags, b.tags);
}
bool operator<(const DecisionEntry& a, const DecisionEntry& b)
{
    if (a.decision_id != b.decision_id) return a.decision_id < b.decision_id;
    if (a.decision.has_value() != b.decision.has_value()) return !a.decision.has_value();
    if (a.decision) {
        int c = compare(*a.decision, *b.decision);
        if (c) return c < 0;
    }
    return a.claiming_txid < b.claiming_txid;
}

std::optional<double> DecisionStateHistory::consensus_outcome() const
{
    for (auto& it : state_history)
        if (it.state == DecisionState::Resolved) return it.outcome;
    return std::nullopt;
}
std::optional<uint32_t> DecisionStateHistory::state_height(DecisionState s) const
{
    for (auto& it : state_history)
        if (it.state == s) return it.height;
    return std::nullopt;
}

PeriodPricing PeriodPricing::seeded(uint64_t p_seed, uint32_t h)
{
    PeriodPricing p;
    p.p_period = p_seed;
    p.p_floor = p_seed / 16;
    p.mints = 1;
    p.claimed = 0;
    p.last_reprice_block = h;
    p.window_open_claimed = 0;
    p.window_open_mints = 1;
    return p;
}

// period_calculator.rs
uint32_t get_current_period(uint64_t ts, std::optional<uint32_t> height, uint64_t genesis_ts, const DecisionConfig& c)
{
    if (c.is_blocks) {
        uint32_t h = height.value_or(0);
        if (c.quantity == 0) return 0;
        return h / c.quantity + 1;
    }
    if (genesis_ts == 0 || ts < genesis_ts) return 0;
    return uint32_t((ts - genesis_ts) / uint64_t(c.quantity)) + 1;
}

std::pair<uint64_t, uint64_t> period_boundaries(uint32_t p, const DecisionConfig& c, uint64_t genesis_ts)
{
    if (p == 0) return {0, 0};
    if (c.is_blocks) return {uint64_t(uint32_t((p - 1) * c.quantity)), uint64_t(uint32_t(p * c.quantity))};
    uint64_t start = genesis_ts + uint64_t(p - 1) * c.quantity;
    return {start, start + c.quantity};
}

std::string period_to_name(uint32_t period)
{
    if (period == 0) return "Genesis";
    uint32_t year = 2009 + (period - 1) / 4;
    uint32_t q = ((period - 1) % 4) + 1;
    return "Q" + std::to_string(q) + " Y" + std::to_string(year);
}

uint32_t reprice_interval(const DecisionConfig& c)
{
    uint32_t bp = c.is_blocks ? c.quantity : uint32_t(uint64_t(c.quantity) / 600);
    return std::max<uint32_t>(bp / 5, 1);
}

bool slot_unlocked(uint32_t idx, uint64_t mints)
{
    if (uint64_t(idx) >= 500) return false;
    return uint64_t(idx) % 100 < mints * 5;
}

uint64_t slot_price(uint64_t p_period, size_t tier)
{
    static const uint64_t NUM[5] = {1, 2, 4, 8, 16};
    unsigned __int128 m = (unsigned __int128)p_period * NUM[tier];
    uint64_t sat = m > UINT64_MAX ? UINT64_MAX : uint64_t(m); // saturating_mul
    return sat / 4;
}

std::optional<uint64_t> fee_for_index(uint64_t p_period, uint64_t mints, uint32_t idx, std::string* err)
{
    if (uint64_t(idx) >= 500) {
        if (err) *err = "decision_index " + std::to_string(idx) + " is out of range (max 499)";
        return std::nullopt;
    }
    if (!slot_unlocked(idx, mints)) {
        if (err) *err = "slot at decision_index " + std::to_string(idx) + " is not yet minted (mints=" + std::to_string(mints) + ")";
        return std::nullopt;
    }
    return slot_price(p_period, idx / 100);
}

// ------------------------------------------------------------------ core getters
std::optional<Hash32> State::tip(const Txn& t) const
{
    auto v = t.get(T_TIP, UNIT);
    if (!v) return std::nullopt;
    return Hash32::from_span(*v);
}

std::optional<uint32_t> State::height(const Txn& t) const
{
    auto v = t.get(T_HEIGHT, UNIT);
    if (!v) return std::nullopt;
    Reader r(*v);
    return r.u32();
}

std::optional<Output> State::utxo(const Txn& t, const OutPoint& op) const
{
    auto k = op.key();
    auto v = t.get(T_UTXOS, ByteSpan(k.data(), k.size()));
    if (!v) return std::nullopt;
    Reader r(*v);
    return de_output(r);
}

std::optional<SpentOutput> State::stxo(const Txn& t, const OutPoint& op) const
{
    auto k = op.key();
    auto v = t.get(T_STXOS, ByteSpan(k.data(), k.size()));
    if (!v) return std::nullopt;
    Reader r(*v);
    return dec_spent(r);
}

static OutPoint op_from_key(ByteSpan k)
{
    Reader r(k);
    return borsh_de_outpoint(r); // OutPointKey -> OutPoint (zero padding ignored)
}

std::vector<std::pair<OutPoint, Output>> State::utxos(const Txn& t) const
{
    std::vector<std::pair<OutPoint, Output>> r;
    t.scan(T_UTXOS, [&](ByteSpan k, ByteSpan v) {
        Reader rd(v);
        r.emplace_back(op_from_key(k), de_output(rd));
        return true;
    });
    return r;
}

std::vector<std::pair<OutPoint, SpentOutput>> State::stxos(const Txn& t) const
{
    std::vector<std::pair<OutPoint, SpentOutput>> r;
    t.scan(T_STXOS, [&](ByteSpan k, ByteSpan v) {
        Reader rd(v);
        r.emplace_back(op_from_key(k), dec_spent(rd));
        return true;
    });
    return r;
}

bt::utreexo::MemForest State::accumulator(const Txn& t) const
{
    auto v = t.get(T_ACCUMULATOR, UNIT);
    if (!v) return {};
    auto f = bt::utreexo::MemForest::deserialize(*v);
    if (!f) throw StateError("corrupt accumulator", true);
    return std::move(*f);
}

void State::put_accumulator(Txn& t, const bt::utreexo::MemForest& f) const { t.put(T_ACCUMULATOR, UNIT, f.serialize()); }

void State::apply_diff(Txn& t, const AccumulatorDiff& d) const
{
    auto acc = accumulator(t);
    std::vector<NodeHash> ins, dels;
    d.split(ins, dels);
    if (auto e = acc.modify(ins, dels)) throw StateError("Utreexo error: " + *e);
    put_accumulator(t, acc);
}

std::optional<Hash32> State::pending_bundle_m6id(const Txn& t) const
{
    auto v = t.get(T_PENDING_BUNDLE, UNIT);
    if (!v) return std::nullopt;
    return Hash32::from_span(*v);
}

static BundleStatus dec_status(Reader& r) { return BundleStatus(r.u8()); }
static void enc_status(Writer& w, BundleStatus s) { w.u8(uint8_t(s)); }

std::optional<std::pair<WithdrawalBundleInfo, RollBack<BundleStatus>>> State::bundle(const Txn& t, const Hash32& m6id) const
{
    auto v = t.get(T_BUNDLES, m6id_key(m6id));
    if (!v) return std::nullopt;
    Reader r(*v);
    auto info = dec_bundle_info(r);
    auto st = dec_rollback<BundleStatus>(r, dec_status);
    return std::make_pair(std::move(info), std::move(st));
}

std::vector<std::pair<Hash32, std::pair<WithdrawalBundleInfo, RollBack<BundleStatus>>>> State::bundles(const Txn& t) const
{
    std::vector<std::pair<Hash32, std::pair<WithdrawalBundleInfo, RollBack<BundleStatus>>>> res;
    t.scan(T_BUNDLES, [&](ByteSpan k, ByteSpan v) {
        Reader r(v);
        auto info = dec_bundle_info(r);
        auto st = dec_rollback<BundleStatus>(r, dec_status);
        res.emplace_back(Hash32::from_span(k.subspan(8)), std::make_pair(std::move(info), std::move(st)));
        return true;
    });
    return res;
}

void State::put_bundle(Txn& t, const Hash32& m6id, const WithdrawalBundleInfo& info, const RollBack<BundleStatus>& st) const
{
    Writer w;
    enc_bundle_info(w, info);
    enc_rollback(w, st, enc_status);
    t.put(T_BUNDLES, m6id_key(m6id), w.buf);
}

std::optional<RollBack<Hash32>> State::latest_failed_rollback(const Txn& t) const
{
    auto v = t.get(T_LATEST_FAILED, UNIT);
    if (!v) return std::nullopt;
    Reader r(*v);
    return dec_rollback<Hash32>(r, [](Reader& rd) { return rd.array<32>(); });
}

std::optional<std::pair<uint32_t, Hash32>> State::latest_failed_bundle(const Txn& t) const
{
    auto rb = latest_failed_rollback(t);
    if (!rb) return std::nullopt;
    Hash32 m6id = rb->latest().value;
    auto b = bundle(t, m6id);
    if (!b) throw StateError("Inconsistent DBs: latest failed m6id missing", true);
    for (auto it = b->second.v.rbegin(); it != b->second.v.rend(); ++it)
        if (it->value == BundleStatus::Failed) return std::make_pair(it->height, m6id);
    throw StateError("missing failure status", true);
}

std::optional<std::pair<WithdrawalBundle, uint32_t>> State::pending_bundle(const Txn& t) const
{
    auto m = pending_bundle_m6id(t);
    if (!m) return std::nullopt;
    auto b = bundle(t, *m);
    if (!b) throw StateError("pending bundle missing from withdrawal_bundles");
    if (b->first.kind != WithdrawalBundleInfo::Known) throw StateError("PendingWithdrawalBundleUnknown");
    return std::make_pair(b->first.bundle, b->second.latest().height);
}

static std::vector<std::pair<uint32_t, std::pair<Hash32, uint32_t>>> seq_table(const Txn& t, uint8_t table)
{
    std::vector<std::pair<uint32_t, std::pair<Hash32, uint32_t>>> r;
    t.scan(table, [&](ByteSpan k, ByteSpan v) {
        Reader kr(k), vr(v);
        uint32_t seq = kr.u32();
        Bytes hb = vr.bincode_bytes();
        uint32_t h = vr.u32();
        r.emplace_back(seq, std::make_pair(Hash32::from_span(hb), h));
        return true;
    });
    return r;
}
std::vector<std::pair<uint32_t, std::pair<Hash32, uint32_t>>> State::deposit_blocks(const Txn& t) const { return seq_table(t, T_DEPOSIT_BLOCKS); }
std::vector<std::pair<uint32_t, std::pair<Hash32, uint32_t>>> State::bundle_event_blocks(const Txn& t) const
{
    return seq_table(t, T_BUNDLE_EVENT_BLOCKS);
}

std::vector<TwoWayPegEvent> State::two_way_peg_events(const Txn& t, uint32_t height) const
{
    auto v = t.get(T_TWPD_EVENTS, u32le(height));
    if (!v) return {};
    Reader r(*v);
    return dec_twpd_events(r);
}

uint64_t State::sidechain_wealth(const Txn& t) const
{
    uint64_t dep_utxo = 0, dep_stxo = 0, wd_stxo = 0;
    for (auto& [op, o] : utxos(t))
        if (op.kind == OutPoint::Deposit) {
            if (dep_utxo + o.get_value() < dep_utxo) throw StateError("amount overflow");
            dep_utxo += o.get_value();
        }
    for (auto& [op, s] : stxos(t)) {
        if (op.kind == OutPoint::Deposit) {
            if (dep_stxo + s.output.get_value() < dep_stxo) throw StateError("amount overflow");
            dep_stxo += s.output.get_value();
        }
        if (s.inpoint.kind == InPoint::Withdrawal) {
            if (wd_stxo + s.output.get_value() < wd_stxo) throw StateError("amount overflow");
            wd_stxo += s.output.get_value();
        }
    }
    uint64_t total = dep_utxo + dep_stxo;
    if (total < dep_utxo || total < wd_stxo) throw StateError("amount overflow");
    return total - wd_stxo;
}

std::optional<uint64_t> State::mainchain_timestamp(const Txn& t) const
{
    auto v = t.get(T_MAIN_TS, UNIT);
    if (!v) return std::nullopt;
    Reader r(*v);
    return r.u64();
}
std::optional<uint64_t> State::genesis_timestamp(const Txn& t) const
{
    auto v = t.get(T_GENESIS_TS, UNIT);
    if (!v) return std::nullopt;
    Reader r(*v);
    return r.u64();
}
std::tuple<uint64_t, std::optional<uint32_t>, uint64_t> State::period_context(const Txn& t) const
{
    return {mainchain_timestamp(t).value_or(0), height(t), genesis_timestamp(t).value_or(0)};
}

// ------------------------------------------------------------------ utxo writes (markets/utxos.rs)
void State::insert_utxo(Txn& t, const OutPoint& op, const Output& o, AccumulatorDiff& d) const
{
    auto k = op.key();
    Writer w;
    bincode(w, o);
    t.put(T_UTXOS, ByteSpan(k.data(), k.size()), w.buf);
    d.insert(NodeHash::some(pointed_output_hash(op, o)));
}

bool State::delete_utxo(Txn& t, const OutPoint& op, AccumulatorDiff& d) const
{
    auto o = utxo(t, op);
    if (!o) return false;
    auto k = op.key();
    if (!t.del(T_UTXOS, ByteSpan(k.data(), k.size()))) return false;
    d.remove(NodeHash::some(pointed_output_hash(op, *o)));
    return true;
}

// ------------------------------------------------------------------ reputation.rs
double State::reputation(const Txn& t, const Address& a) const
{
    auto v = t.get(T_REPUTATION, a.b.span());
    if (!v) return 0.0;
    Reader r(*v);
    return dec_f64(r);
}

std::map<Address, double> State::all_reputations(const Txn& t) const
{
    std::map<Address, double> m;
    t.scan(T_REPUTATION, [&](ByteSpan k, ByteSpan v) {
        Reader r(v);
        double rep = dec_f64(r);
        if (rep > 0.0) {
            Address a;
            std::memcpy(a.b.data(), k.data(), 20);
            m[a] = rep;
        }
        return true;
    });
    return m;
}

void State::set_reputation(Txn& t, const Address& a, double rep) const
{
    if (rep <= 0.0) {
        t.del(T_REPUTATION, a.b.span());
    } else {
        Writer w;
        enc_f64(w, rep);
        t.put(T_REPUTATION, a.b.span(), w.buf);
    }
}

void State::clear_and_restore_reputation(Txn& t, const std::map<Address, double>& snap) const
{
    t.clear(T_REPUTATION);
    for (auto& [a, rep] : snap)
        if (rep > 0.0) {
            Writer w;
            enc_f64(w, rep);
            t.put(T_REPUTATION, a.b.span(), w.buf);
        }
}

// ------------------------------------------------------------------ decisions/database.rs
std::vector<DecisionEntry> State::period_decisions(const Txn& t, uint32_t period) const
{
    auto v = t.get(T_PERIOD_DECISIONS, u32le(period));
    if (!v) return {};
    Reader r(*v);
    return dec_entries(r);
}

std::vector<std::pair<uint32_t, std::vector<DecisionEntry>>> State::all_period_decisions(const Txn& t) const
{
    std::vector<std::pair<uint32_t, std::vector<DecisionEntry>>> res;
    t.scan(T_PERIOD_DECISIONS, [&](ByteSpan k, ByteSpan v) {
        Reader kr(k), r(v);
        uint32_t p = kr.u32();
        res.emplace_back(p, dec_entries(r));
        return true;
    });
    return res;
}

void State::put_period_decisions(Txn& t, uint32_t period, const std::vector<DecisionEntry>& set) const
{
    Writer w;
    enc_entries(w, set);
    t.put(T_PERIOD_DECISIONS, u32le(period), w.buf);
}

std::optional<DecisionEntry> State::decision_entry(const Txn& t, const DecisionId& id) const
{
    // database.rs:716-745: the set is ordered by decision_id first; the first entry with
    // this id is what `get` / the range scan finds.
    for (auto& e : period_decisions(t, id.period_index())) {
        if (e.decision_id == id) return e;
        if (id < e.decision_id) break;
    }
    return std::nullopt;
}

std::set<DecisionId> State::claimed_ids_for_period(const Txn& t, uint32_t period) const
{
    std::set<DecisionId> s;
    for (auto& e : period_decisions(t, period))
        if (e.decision) s.insert(e.decision_id);
    return s;
}

uint64_t State::standard_claimed_count(const Txn& t, uint32_t period) const
{
    uint64_t n = 0;
    for (auto& id : claimed_ids_for_period(t, period))
        if (id.is_standard()) n++;
    return n;
}

std::optional<DecisionStateHistory> State::decision_history(const Txn& t, const DecisionId& id) const
{
    auto v = t.get(T_DECISION_HISTORIES, ByteSpan(id.b.data(), 3));
    if (!v) return std::nullopt;
    Reader r(*v);
    return dec_history(r);
}

std::vector<std::pair<DecisionId, DecisionStateHistory>> State::all_decision_histories(const Txn& t) const
{
    std::vector<std::pair<DecisionId, DecisionStateHistory>> res;
    t.scan(T_DECISION_HISTORIES, [&](ByteSpan k, ByteSpan v) {
        DecisionId id;
        std::memcpy(id.b.data(), k.data(), 3);
        Reader r(v);
        res.emplace_back(id, dec_history(r));
        return true;
    });
    return res;
}

void State::put_decision_history(Txn& t, const DecisionId& id, const DecisionStateHistory& h) const
{
    Writer w;
    enc_history(w, h);
    t.put(T_DECISION_HISTORIES, ByteSpan(id.b.data(), 3), w.buf);
}

DecisionState State::decision_state(const Txn& t, const DecisionId& id) const
{
    auto h = decision_history(t, id);
    return h ? h->current_state() : DecisionState::Created;
}

std::optional<PeriodPricing> State::period_pricing(const Txn& t, uint32_t period) const
{
    auto v = t.get(T_PERIOD_PRICING, u32le(period));
    if (!v) return std::nullopt;
    Reader r(*v);
    return dec_pricing(r);
}

void State::put_period_pricing(Txn& t, uint32_t period, const PeriodPricing& p) const
{
    Writer w;
    enc_pricing(w, p);
    t.put(T_PERIOD_PRICING, u32le(period), w.buf);
}

std::optional<uint32_t> State::highest_minted_period(const Txn& t) const
{
    std::optional<uint32_t> hi;
    t.scan(T_PERIOD_DECISIONS, [&](ByteSpan k, ByteSpan) {
        Reader r(k);
        uint32_t p = r.u32();
        hi = hi ? std::max(*hi, p) : p;
        return true;
    });
    return hi;
}

uint64_t State::fee_for_decision_id(const Txn& t, const DecisionId& id) const
{
    uint32_t period = id.period_index();
    auto p = period_pricing(t, period);
    if (!p) throw invalid_tx("no pricing record for period " + std::to_string(period));
    std::string err;
    auto f = fee_for_index(p->p_period, p->mints, id.decision_index(), &err);
    if (!f) throw invalid_tx(err);
    return *f;
}

static std::string did_debug(const DecisionId& d)
{
    return "DecisionId([" + std::to_string(d.b[0]) + ", " + std::to_string(d.b[1]) + ", " + std::to_string(d.b[2]) + "])";
}

uint64_t State::total_for(uint32_t period, uint64_t ts, std::optional<uint32_t> h, uint64_t genesis_ts) const
{
    uint32_t cur = get_current_period(ts, h, genesis_ts, cfg_);
    if (period < cur) return 0;
    uint32_t off = period - cur;
    if (off >= FUTURE_PERIODS) return 0;
    uint64_t sub = uint64_t(off) * 25;
    return sub > INITIAL_DECISIONS_PER_PERIOD ? 0 : INITIAL_DECISIONS_PER_PERIOD - sub;
}

/** Error::DecisionNotAvailable { decision_id, reason }; validate_claim_payload maps it to
 *  InvalidDecisionId { reason } ("invalid decision ID: {reason}"). */
static StateError not_available(const std::string& reason) { return StateError("invalid decision ID: " + reason); }

void State::validate_decision_claim(const Txn& t, const DecisionId& id, uint64_t ts, std::optional<uint32_t> h, uint64_t genesis_ts) const
{
    uint32_t period_index = id.period_index();
    uint32_t decision_index = id.decision_index();
    uint32_t current = get_current_period(ts, h, genesis_ts, cfg_);
    // is_decision_settled: current > period + 7
    if (current > period_index + 7) throw not_available("Decision period " + std::to_string(period_index) + " is settled");
    if (decision_state(t, id) == DecisionState::Voting)
        throw not_available("Decision " + did_debug(id) + " is in voting state - no new decisions can be claimed");
    if (period_index > current + FUTURE_PERIODS - 1)
        throw not_available("Decision period " + std::to_string(period_index) + " exceeds maximum allowed period " +
                            std::to_string(current + FUTURE_PERIODS - 1) + " (current + 20)");
    if (id.is_standard()) {
        uint32_t start = current, end = current + FUTURE_PERIODS - 1;
        if (period_index < start || period_index > end)
            throw not_available("Period " + std::to_string(period_index) + " is not in active window for new decisions (" +
                                std::to_string(start) + "-" + std::to_string(end) + ")");
        auto p = period_pricing(t, period_index);
        if (!p) throw not_available("no pricing record for period " + std::to_string(period_index));
        if (!slot_unlocked(decision_index, p->mints))
            throw not_available("decision_index " + std::to_string(decision_index) + " not yet unlocked in period " +
                                std::to_string(period_index) + " (mints=" + std::to_string(p->mints) + ")");
    }
    if (claimed_ids_for_period(t, period_index).count(id))
        throw StateError("invalid decision ID: Decision " + id.to_hex() + " already claimed");
}

void State::claim_decision(Txn& t, const DecisionId& id, const Decision& d, const Hash32& txid, std::optional<uint32_t> h) const
{
    uint32_t period = id.period_index();
    auto set = period_decisions(t, period);
    DecisionEntry ne{id, d, txid};
    // BTreeSet::insert: no-op when an equal entry exists
    auto it = std::lower_bound(set.begin(), set.end(), ne);
    if (it == set.end() || ne < *it) set.insert(it, ne);
    put_period_decisions(t, period, set);
    DecisionStateHistory hist;
    hist.state_history.push_back({DecisionState::Claimed, h.value_or(0), std::nullopt});
    put_decision_history(t, id, hist);
    if (id.is_standard()) {
        auto p = period_pricing(t, period);
        if (!p) throw invalid_tx("no pricing record for period " + std::to_string(period));
        p->claimed = p->claimed + 1 < p->claimed ? p->claimed : p->claimed + 1; // saturating_add
        put_period_pricing(t, period, *p);
    }
}

void State::revert_decision_claim(Txn& t, const DecisionId& id) const
{
    uint32_t period = id.period_index();
    auto set = period_decisions(t, period);
    auto it = std::find_if(set.begin(), set.end(), [&](const DecisionEntry& e) { return e.decision_id == id; });
    if (it == set.end()) return; // "Attempted to revert decision ... that wasn't found"
    set.erase(it);
    if (set.empty()) t.del(T_PERIOD_DECISIONS, u32le(period));
    else put_period_decisions(t, period, set);
}

std::vector<DecisionId> State::available_decisions_in_period(const Txn& t, uint32_t period) const
{
    auto [ts, h, g] = period_context(t);
    if (total_for(period, ts, h, g) == 0) return {};
    auto p = period_pricing(t, period);
    if (!p) return {};
    auto claimed = claimed_ids_for_period(t, period);
    uint64_t unlocked = p->mints * 5;
    std::vector<DecisionId> res;
    for (uint64_t tier = 0; tier < 5; tier++)
        for (uint64_t pos = 0; pos < unlocked; pos++) {
            uint32_t idx = uint32_t(tier * 100 + pos);
            auto id = DecisionId::make(true, period, idx);
            if (!id) throw StateError("invalid decision ID");
            if (!claimed.count(*id)) res.push_back(*id);
        }
    return res;
}

// ------------------------------------------------------------------ markets/database.rs
std::optional<Market> State::market(const Txn& t, const MarketId& id) const
{
    auto v = t.get(T_MARKETS, ByteSpan(id.b.data(), 6));
    if (!v) return std::nullopt;
    Reader r(*v);
    return dec_market(r);
}

std::vector<Market> State::all_markets(const Txn& t) const
{
    std::vector<Market> res;
    t.scan(T_MARKETS, [&](ByteSpan, ByteSpan v) {
        Reader r(v);
        res.push_back(dec_market(r));
        return true;
    });
    return res;
}

static Bytes state_key(MarketState s) { return u32le(uint32_t(s)); }

static std::vector<MarketId> dec_ids(const std::optional<Bytes>& v)
{
    std::vector<MarketId> ids;
    if (!v) return ids;
    Reader r(*v);
    uint64_t n = r.u64();
    for (uint64_t i = 0; i < n; i++) ids.push_back(dec_mid(r));
    return ids;
}
static Bytes enc_ids(const std::vector<MarketId>& ids)
{
    Writer w;
    w.u64(ids.size());
    for (auto& m : ids) enc_mid(w, m);
    return w.buf;
}

std::vector<Market> State::markets_by_state(const Txn& t, MarketState s) const
{
    std::vector<Market> res;
    for (auto& id : dec_ids(t.get(T_MARKETS_BY_STATE, state_key(s))))
        if (auto m = market(t, id)) res.push_back(std::move(*m));
    return res;
}

std::vector<MarketId> State::markets_by_decision(const Txn& t, const DecisionId& d) const
{
    return dec_ids(t.get(T_MARKETS_BY_DECISION, ByteSpan(d.b.data(), 3)));
}

static void index_update(Txn& t, uint8_t table, const Bytes& key, const MarketId& id, bool add)
{
    auto ids = dec_ids(t.get(table, key));
    if (add) {
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
            ids.push_back(id);
            t.put(table, key, enc_ids(ids));
        }
    } else {
        ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
        if (ids.empty()) t.del(table, key);
        else t.put(table, key, enc_ids(ids));
    }
}

void State::put_market(Txn& t, const Market& m) const
{
    Writer w;
    enc_market(w, m);
    t.put(T_MARKETS, ByteSpan(m.id.b.data(), 6), w.buf);
}

void State::add_market(Txn& t, const Market& m) const
{
    put_market(t, m);
    index_update(t, T_MARKETS_BY_STATE, state_key(m.market_state), m.id, true);
    if (m.expires_at_height) index_update(t, T_MARKETS_BY_EXPIRY, u32le(*m.expires_at_height), m.id, true);
    for (auto& d : m.decision_ids) index_update(t, T_MARKETS_BY_DECISION, span_bytes(ByteSpan(d.b.data(), 3)), m.id, true);
}

void State::delete_market(Txn& t, const MarketId& id) const
{
    auto m = market(t, id);
    if (!m) return;
    index_update(t, T_MARKETS_BY_STATE, state_key(m->market_state), id, false);
    if (m->expires_at_height) index_update(t, T_MARKETS_BY_EXPIRY, u32le(*m->expires_at_height), id, false);
    for (auto& d : m->decision_ids) index_update(t, T_MARKETS_BY_DECISION, span_bytes(ByteSpan(d.b.data(), 3)), id, false);
    clear_market_funds_utxo(t, id, false);
    clear_market_funds_utxo(t, id, true);
    t.del(T_MARKETS, ByteSpan(id.b.data(), 6));
}

static void write_market_and_indexes(const State& s, Txn& t, const Market& m, const std::optional<Market>& old)
{
    s.put_market(t, m);
    if (!old) return;
    if (old->market_state != m.market_state) {
        index_update(t, State::T_MARKETS_BY_STATE, state_key(old->market_state), m.id, false);
        index_update(t, State::T_MARKETS_BY_STATE, state_key(m.market_state), m.id, true);
    }
    if (old->expires_at_height != m.expires_at_height) {
        if (old->expires_at_height) index_update(t, State::T_MARKETS_BY_EXPIRY, u32le(*old->expires_at_height), m.id, false);
        if (m.expires_at_height) index_update(t, State::T_MARKETS_BY_EXPIRY, u32le(*m.expires_at_height), m.id, true);
    }
    std::set<DecisionId> od(old->decision_ids.begin(), old->decision_ids.end()), nd(m.decision_ids.begin(), m.decision_ids.end());
    for (auto& d : od)
        if (!nd.count(d)) index_update(t, State::T_MARKETS_BY_DECISION, span_bytes(ByteSpan(d.b.data(), 3)), m.id, false);
    for (auto& d : nd)
        if (!od.count(d)) index_update(t, State::T_MARKETS_BY_DECISION, span_bytes(ByteSpan(d.b.data(), 3)), m.id, true);
}

void State::update_market(Txn& t, const Market& m) const
{
    auto old = market(t, m.id);
    if (old && !can_transition(old->market_state, m.market_state))
        throw invalid_tx(std::string("Invalid market state transition from ") + to_string(old->market_state) + " to " +
                         to_string(m.market_state));
    write_market_and_indexes(*this, t, m, old);
}

void State::restore_market(Txn& t, const Market& m) const { write_market_and_indexes(*this, t, m, market(t, m.id)); }

static Bytes funds_key(const MarketId& id, bool is_fee)
{
    Bytes k(id.b.begin(), id.b.end());
    k.push_back(is_fee ? 1 : 0);
    return k;
}

std::optional<OutPoint> State::market_funds_utxo(const Txn& t, const MarketId& id, bool is_fee) const
{
    auto v = t.get(T_MARKET_FUNDS, funds_key(id, is_fee));
    if (!v) return std::nullopt;
    Reader r(*v);
    return de_outpoint(r);
}

void State::set_market_funds_utxo(Txn& t, const MarketId& id, bool is_fee, const OutPoint& op) const
{
    Writer w;
    bincode(w, op);
    t.put(T_MARKET_FUNDS, funds_key(id, is_fee), w.buf);
}

void State::clear_market_funds_utxo(Txn& t, const MarketId& id, bool is_fee) const { t.del(T_MARKET_FUNDS, funds_key(id, is_fee)); }

uint64_t State::market_funds_sats(const Txn& t, const MarketId& id, bool is_fee) const
{
    auto op = market_funds_utxo(t, id, is_fee);
    if (!op) return 0;
    auto u = utxo(t, *op);
    if (!u) throw StateError("utxo " + op->to_string() + " doesn't exist");
    return u->get_value();
}

std::optional<std::vector<int64_t>> State::mempool_shares(const Txn& t, const MarketId& id) const
{
    auto v = t.get(T_MEMPOOL_SHARES, ByteSpan(id.b.data(), 6));
    if (!v) return std::nullopt;
    Reader r(*v);
    std::vector<int64_t> s;
    uint64_t n = r.u64();
    for (uint64_t i = 0; i < n; i++) s.push_back(int64_t(r.u64()));
    return s;
}
void State::put_mempool_shares(Txn& t, const MarketId& id, const std::vector<int64_t>& s) const
{
    Writer w;
    w.u64(s.size());
    for (auto x : s) w.u64(uint64_t(x));
    t.put(T_MEMPOOL_SHARES, ByteSpan(id.b.data(), 6), w.buf);
}
void State::clear_mempool_shares(Txn& t, const MarketId& id) const { t.del(T_MEMPOOL_SHARES, ByteSpan(id.b.data(), 6)); }

std::optional<ShareAccount> State::share_account(const Txn& t, const Address& a) const
{
    auto v = t.get(T_SHARE_ACCOUNTS, a.b.span());
    if (!v) return std::nullopt;
    Reader r(*v);
    return dec_account(r);
}

std::vector<std::pair<Address, ShareAccount>> State::all_share_accounts(const Txn& t) const
{
    std::vector<std::pair<Address, ShareAccount>> res;
    t.scan(T_SHARE_ACCOUNTS, [&](ByteSpan k, ByteSpan v) {
        Address a;
        std::memcpy(a.b.data(), k.data(), 20);
        Reader r(v);
        res.emplace_back(a, dec_account(r));
        return true;
    });
    return res;
}

void State::add_shares(Txn& t, const Address& a, const MarketId& m, uint32_t outcome, int64_t shares, uint32_t h) const
{
    auto acc = share_account(t, a).value_or(ShareAccount{});
    auto key = std::make_pair(m, outcome);
    int64_t cur = acc.positions.count(key) ? acc.positions[key] : 0;
    acc.positions[key] = int64_t(uint64_t(cur) + uint64_t(shares)); // i64 `+` wraps in release
    acc.last_updated_height = h;
    Writer w;
    enc_account(w, acc);
    t.put(T_SHARE_ACCOUNTS, a.b.span(), w.buf);
}

void State::remove_shares(Txn& t, const Address& a, const MarketId& m, uint32_t outcome, int64_t shares, uint32_t h) const
{
    auto acc = share_account(t, a);
    if (!acc) throw invalid_tx("No share account found for address");
    auto key = std::make_pair(m, outcome);
    int64_t cur = acc->positions.count(key) ? acc->positions[key] : 0;
    if (shares > cur) throw invalid_tx("Insufficient shares for sell transaction");
    int64_t nw = cur - shares;
    if (nw > 0) acc->positions[key] = nw;
    else acc->positions.erase(key);
    acc->last_updated_height = h;
    if (acc->positions.empty()) {
        t.del(T_SHARE_ACCOUNTS, a.b.span());
    } else {
        Writer w;
        enc_account(w, *acc);
        t.put(T_SHARE_ACCOUNTS, a.b.span(), w.buf);
    }
}

static std::string mid_debug(const MarketId& m)
{
    std::string s = "MarketId([";
    for (int i = 0; i < 6; i++) s += (i ? ", " : "") + std::to_string(m.b[i]);
    return s + "])";
}

void State::verify_share_invariant(const Txn& t, const MarketId& id) const
{
    auto m = market(t, id);
    if (!m) throw invalid_tx("Market " + mid_debug(id) + " not found for invariant check");
    size_t n = m->shares.size();
    std::vector<int64_t> totals(n, 0);
    for (auto& [a, acc] : all_share_accounts(t))
        for (auto& [k, v] : acc.positions)
            if (k.first == id && k.second < n) totals[k.second] = int64_t(uint64_t(totals[k.second]) + uint64_t(v));
    for (size_t i = 0; i < n; i++)
        if (m->shares[i] != totals[i])
            throw invalid_tx("Share invariant violation for market " + mid_debug(id) + " outcome " + std::to_string(i) +
                             ": market.shares=" + std::to_string(m->shares[i]) + ", sum(accounts)=" + std::to_string(totals[i]));
}

// ------------------------------------------------------------------ voting/database.rs
std::optional<double> VoteValue::to_float() const
{
    switch (kind) {
    case Binary: return b ? 1.0 : 0.0;
    case Scalar: return s;
    case Categorical: return double(c);
    case Abstain: return std::nullopt;
    }
    return std::nullopt;
}

VoteValue convert_vote_value(double v, const Decision* d)
{
    VoteValue r;
    if (std::isnan(v)) {
        r.kind = VoteValue::Abstain;
        return r;
    }
    if (d && d->is_categorical()) {
        r.kind = VoteValue::Categorical;
        double x = std::round(v);
        // `as u16`: saturating, NaN -> 0
        r.c = x <= 0 ? 0 : x >= 65535.0 ? 65535 : uint16_t(x);
        return r;
    }
    if (v == 0.0 || v == 1.0) {
        r.kind = VoteValue::Binary;
        r.b = v == 1.0;
    } else {
        r.kind = VoteValue::Scalar;
        r.s = v;
    }
    return r;
}

std::optional<VoteEntry> State::vote(const Txn& t, uint32_t period, const Address& voter, const DecisionId& d) const
{
    auto v = t.get(T_VOTES, vote_key(period, voter, d));
    if (!v) return std::nullopt;
    Reader r(*v);
    VoteEntry e;
    e.value = dec_vote_value(r);
    e.timestamp = r.u64();
    e.block_height = r.u32();
    return e;
}

void State::put_vote(Txn& t, const VoteKey& k, const VoteEntry& e) const
{
    Writer w;
    enc_vote_value(w, e.value);
    w.u64(e.timestamp);
    w.u32(e.block_height);
    t.put(T_VOTES, vote_key(k.period_id, k.voter, k.decision_id), w.buf);
}

bool State::delete_vote(Txn& t, uint32_t period, const Address& voter, const DecisionId& d) const
{
    return t.del(T_VOTES, vote_key(period, voter, d));
}

std::vector<std::pair<VoteKey, VoteEntry>> State::all_votes(const Txn& t) const
{
    std::vector<std::pair<VoteKey, VoteEntry>> res;
    t.scan(T_VOTES, [&](ByteSpan k, ByteSpan v) {
        Reader kr(k), r(v);
        VoteKey key;
        key.period_id = kr.u32();
        key.voter.b = kr.array<20>();
        key.decision_id = dec_did(kr);
        VoteEntry e;
        e.value = dec_vote_value(r);
        e.timestamp = r.u64();
        e.block_height = r.u32();
        res.emplace_back(key, e);
        return true;
    });
    return res;
}

std::vector<std::pair<VoteKey, VoteEntry>> State::votes_for_period(const Txn& t, uint32_t period) const
{
    // voting/database.rs:106-148: scan in key order, skip lower periods, stop at the first
    // higher one. Keys are little-endian, so the order is numeric only below 256 (the
    // DecisionId period field caps voting periods at 128).
    std::vector<std::pair<VoteKey, VoteEntry>> res;
    t.scan(T_VOTES, [&](ByteSpan k, ByteSpan v) {
        Reader kr(k);
        uint32_t p = kr.u32();
        if (p < period) return true;
        if (p > period) return false;
        VoteKey key;
        key.period_id = p;
        key.voter.b = kr.array<20>();
        key.decision_id = dec_did(kr);
        Reader r(v);
        VoteEntry e;
        e.value = dec_vote_value(r);
        e.timestamp = r.u64();
        e.block_height = r.u32();
        res.emplace_back(key, e);
        return true;
    });
    return res;
}

std::optional<DecisionOutcome> State::decision_outcome(const Txn& t, const DecisionId& d) const
{
    auto v = t.get(T_DECISION_OUTCOMES, ByteSpan(d.b.data(), 3));
    if (!v) return std::nullopt;
    Reader r(*v);
    return dec_outcome(r);
}

std::vector<DecisionOutcome> State::outcomes_for_period(const Txn& t, uint32_t period) const
{
    std::vector<DecisionOutcome> res;
    uint32_t target = period == 0 ? 0 : period - 1;
    t.scan(T_DECISION_OUTCOMES, [&](ByteSpan k, ByteSpan v) {
        DecisionId id;
        std::memcpy(id.b.data(), k.data(), 3);
        Reader r(v);
        auto o = dec_outcome(r);
        if (id.period_index() == target && o.period_id == period) res.push_back(o);
        return true;
    });
    return res;
}

std::optional<VotingPeriodStats> State::period_stats(const Txn& t, uint32_t period) const
{
    auto v = t.get(T_PERIOD_STATS, u32le(period));
    if (!v) return std::nullopt;
    Reader r(*v);
    return dec_stats(r);
}

// exported for voting.cpp / market_block.cpp
void put_outcome(Txn& t, const DecisionOutcome& o)
{
    Writer w;
    enc_outcome(w, o);
    t.put(State::T_DECISION_OUTCOMES, ByteSpan(o.decision_id.b.data(), 3), w.buf);
}
void put_stats(Txn& t, const VotingPeriodStats& s)
{
    Writer w;
    enc_stats(w, s);
    t.put(State::T_PERIOD_STATS, u32le(s.period_id), w.buf);
}
void put_twpd_events(Txn& t, uint32_t height, const std::vector<TwoWayPegEvent>& ev)
{
    Writer w;
    enc_twpd_events(w, ev);
    t.put(State::T_TWPD_EVENTS, u32le(height), w.buf);
}
Bytes pricing_bytes(const PeriodPricing& p)
{
    Writer w;
    enc_pricing(w, p);
    return w.buf;
}
PeriodPricing pricing_from(ByteSpan b)
{
    Reader r(b);
    return dec_pricing(r);
}

// ---------------------------------------------------------------- replay digest
// The same summary tests/oracle (hm_oracle replay) prints from THEIR State after each block:
// table hashes over (key, bincode value) pairs in their order (see oracle main.rs digest()).
namespace {
struct TableHash {
    bt::Blake3 h;
    size_t n = 0;
    void add(ByteSpan k, ByteSpan v)
    {
        uint64_t kl = k.size(), vl = v.size();
        h.update(&kl, 8);
        h.update(k);
        h.update(&vl, 8);
        h.update(v);
        n++;
    }
    UniValue json() const
    {
        UniValue a(UniValue::VARR);
        a.push_back(uint64_t(n));
        a.push_back(bt::hex_encode(h.finalize()));
        return a;
    }
};
} // namespace

UniValue State::digest(const Txn& t) const
{
    UniValue o(UniValue::VOBJ);
    auto tp = tip(t);
    auto ht = height(t);
    o.pushKV("height", ht ? UniValue(uint64_t(*ht)) : UniValue());
    o.pushKV("tip", tp ? UniValue(bt::hex_encode(*tp)) : UniValue());
    UniValue roots(UniValue::VARR);
    for (auto& r : accumulator(t).get_roots()) roots.push_back(r.tag == bt::utreexo::NodeHash::Some ? bt::hex_encode(r.h) : std::string("empty"));
    o.pushKV("roots", roots);
    TableHash utx;
    t.scan(T_UTXOS, [&](ByteSpan k, ByteSpan v) {
        utx.add(k, v);
        return true;
    });
    o.pushKV("utxos", utx.json());
    UniValue rep(UniValue::VARR);
    for (auto& [a, r] : all_reputations(t)) {
        UniValue p(UniValue::VARR);
        p.push_back(a.to_base58());
        uint64_t bits;
        std::memcpy(&bits, &r, 8);
        char buf[17];
        std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)bits);
        p.push_back(std::string(buf));
        rep.push_back(p);
    }
    o.pushKV("rep", rep);
    // claimed entries regrouped by their id's period (BTreeMap), in table order
    std::map<uint32_t, std::vector<DecisionEntry>> periods;
    t.scan(T_PERIOD_DECISIONS, [&](ByteSpan, ByteSpan v) {
        Reader r(v);
        for (auto& e : dec_entries(r))
            if (e.decision) periods[e.decision_id.period_index()].push_back(e);
        return true;
    });
    TableHash dec, hist, outc;
    std::vector<std::pair<Bytes, Bytes>> hrows, orows;
    for (auto& [p, v] : periods) {
        Writer w;
        enc_entries(w, v);
        dec.add(u32le(p), w.buf);
        for (auto& e : v) {
            ByteSpan id(e.decision_id.b.data(), 3);
            if (auto h = t.get(T_DECISION_HISTORIES, id)) hrows.emplace_back(Bytes(id.begin(), id.end()), *h);
            if (auto x = t.get(T_DECISION_OUTCOMES, id)) orows.emplace_back(Bytes(id.begin(), id.end()), *x);
        }
    }
    std::sort(hrows.begin(), hrows.end());
    std::sort(orows.begin(), orows.end());
    for (auto& [k, v] : hrows) hist.add(k, v);
    for (auto& [k, v] : orows) outc.add(k, v);
    o.pushKV("decisions", dec.json());
    o.pushKV("histories", hist.json());
    TableHash pr, st;
    for (uint32_t p = 0; p < 200; p++) {
        if (auto v = t.get(T_PERIOD_PRICING, u32le(p))) pr.add(u32le(p), *v);
        if (auto v = t.get(T_PERIOD_STATS, u32le(p))) st.add(u32le(p), *v);
    }
    o.pushKV("pricing", pr.json());
    TableHash mk, fu, ac, vo;
    std::vector<Bytes> ids;
    t.scan(T_MARKETS, [&](ByteSpan k, ByteSpan v) {
        mk.add(k, v);
        ids.emplace_back(k.begin(), k.end());
        return true;
    });
    for (auto& id : ids)
        for (bool fee : {false, true}) {
            MarketId m;
            std::memcpy(m.b.data(), id.data(), 6);
            if (auto v = t.get(T_MARKET_FUNDS, funds_key(m, fee))) {
                Bytes k = id;
                k.push_back(uint8_t(fee));
                fu.add(k, *v);
            }
        }
    o.pushKV("markets", mk.json());
    o.pushKV("funds", fu.json());
    t.scan(T_SHARE_ACCOUNTS, [&](ByteSpan k, ByteSpan v) {
        Reader r(v);
        auto a = dec_account(r);
        if (a.positions.empty()) return true;
        Writer w;
        w.u64(a.positions.size());
        for (auto& [mo, sh] : a.positions) {
            enc_mid(w, mo.first);
            w.u32(mo.second);
            w.u64(uint64_t(sh));
        }
        ac.add(k, w.buf);
        return true;
    });
    o.pushKV("accounts", ac.json());
    t.scan(T_VOTES, [&](ByteSpan k, ByteSpan v) {
        Reader kr(k);
        if (kr.u32() < 200) vo.add(k, v);
        return true;
    });
    o.pushKV("votes", vo.json());
    o.pushKV("outcomes", outc.json());
    o.pushKV("stats", st.json());
    auto mts = mainchain_timestamp(t);
    auto gts = genesis_timestamp(t);
    o.pushKV("main_ts", mts ? UniValue(*mts) : UniValue());
    o.pushKV("genesis_ts", gts ? UniValue(*gts) : UniValue());
    return o;
}

} // namespace hivemind
