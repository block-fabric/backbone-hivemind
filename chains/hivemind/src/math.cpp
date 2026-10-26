// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <hivemind/math.h>

#include <cstring>

extern "C" {
int hm_update_cost(const int64_t* cur, const int64_t* nw, size_t n, double beta, double* out, char* err, size_t cap);
int hm_prices(const int64_t* shares, size_t n, double beta, double* out, size_t* out_len, char* err, size_t cap);
int hm_treasury(const int64_t* shares, size_t n, double beta, double* out, char* err, size_t cap);
int hm_buy_cost(double base, double fee, uint64_t* out, char* err, size_t cap);
int hm_validate_lmsr(const int64_t* shares, size_t n, double beta, char* err, size_t cap);
int hm_sell_proceeds(double gross, double fee, uint64_t* out, char* err, size_t cap);
double hm_derive_beta(uint64_t liq, size_t n);
double hm_lmsr_liquidity(double beta, size_t n);
double hm_round_to_precision(double v, uint32_t d);
int hm_to_sats(double v, uint8_t mode, uint64_t* out, char* err, size_t cap);
int hm_allocate_u64(const double* w, size_t n, uint64_t pool, size_t* out_idx, uint64_t* out_amt, size_t* out_n, char* err, size_t cap);
double hm_ndarray_sum(const double* p, size_t n);
void hm_ndarray_normalize(double* p, size_t n);
uint64_t hm_reprice(uint64_t p, uint64_t s, uint64_t a);
bool hm_txpow_verify(uint8_t, uint8_t, uint8_t, const uint8_t*, size_t, uint64_t);
bool hm_txpow_validate(uint8_t, uint8_t, uint8_t);
void hm_txpow_hash(uint8_t, uint8_t, const uint8_t*, size_t, uint8_t*);
bool hm_txpow_mine(uint8_t, uint8_t, uint8_t, const uint8_t*, size_t, uint64_t, uint64_t, uint64_t*);
int hm_run_consensus(const uint8_t* voters, size_t nv, const double* reps, const uint8_t* decisions, size_t nd, const uint8_t* scaled,
                     const uint16_t* catn, const uint8_t* vv, const uint8_t* vd, const double* vals, size_t nvotes, uint8_t* o_tag,
                     double* o_val, uint8_t* c_tag, uint16_t* c_win, double* fl, size_t* fl_len, double* cert, double* upd, uint8_t* upd_tag,
                     uint8_t* outlier, char* err, size_t cap);
const char* hm_math_source();
}

namespace hivemind::math {

namespace {
constexpr size_t ERRCAP = 2048;
struct Err {
    char buf[ERRCAP] = {0};
};
/** status 0 ok, 1 error (text to *err), 2 panic (throw). */
bool check(int st, const Err& e, std::string* err)
{
    if (st == 0) return true;
    if (st == 2) throw MathPanic(e.buf);
    if (err) *err = e.buf;
    return false;
}
} // namespace

std::optional<double> update_cost(const std::vector<int64_t>& cur, const std::vector<int64_t>& next, double beta, std::string* err)
{
    if (cur.size() != next.size()) {
        if (err) *err = "DimensionMismatch { expected: " + std::to_string(cur.size()) + ", actual: " + std::to_string(next.size()) + " }";
        return std::nullopt;
    }
    Err e;
    double out = 0;
    if (!check(hm_update_cost(cur.data(), next.data(), cur.size(), beta, &out, e.buf, ERRCAP), e, err)) return std::nullopt;
    return out;
}

std::optional<std::vector<double>> prices(const std::vector<int64_t>& shares, double beta, std::string* err)
{
    Err e;
    std::vector<double> out(shares.size());
    size_t len = 0;
    if (!check(hm_prices(shares.data(), shares.size(), beta, out.data(), &len, e.buf, ERRCAP), e, err)) return std::nullopt;
    out.resize(std::min(len, out.size()));
    return out;
}

std::optional<double> treasury(const std::vector<int64_t>& shares, double beta, std::string* err)
{
    Err e;
    double out = 0;
    if (!check(hm_treasury(shares.data(), shares.size(), beta, &out, e.buf, ERRCAP), e, err)) return std::nullopt;
    return out;
}

bool validate_lmsr(const std::vector<int64_t>& shares, double beta, std::string* err)
{
    Err e;
    return check(hm_validate_lmsr(shares.data(), shares.size(), beta, e.buf, ERRCAP), e, err);
}

std::optional<BuyCost> buy_cost(double base, double fee_pct, std::string* err)
{
    Err e;
    uint64_t o[3] = {0, 0, 0};
    if (!check(hm_buy_cost(base, fee_pct, o, e.buf, ERRCAP), e, err)) return std::nullopt;
    return BuyCost{o[0], o[1], o[2]};
}

std::optional<SellProceeds> sell_proceeds(double gross, double fee_pct, std::string* err)
{
    Err e;
    uint64_t o[3] = {0, 0, 0};
    if (!check(hm_sell_proceeds(gross, fee_pct, o, e.buf, ERRCAP), e, err)) return std::nullopt;
    return SellProceeds{o[0], o[1], o[2]};
}

double derive_beta_from_liquidity(uint64_t liquidity_sats, size_t n) { return hm_derive_beta(liquidity_sats, n); }
double lmsr_liquidity(double beta, size_t n) { return hm_lmsr_liquidity(beta, n); }
double round_to_precision(double v, uint32_t d) { return hm_round_to_precision(v, d); }

std::optional<uint64_t> to_sats(double v, Rounding m, std::string* err)
{
    Err e;
    uint64_t out = 0;
    if (!check(hm_to_sats(v, uint8_t(m), &out, e.buf, ERRCAP), e, err)) return std::nullopt;
    return out;
}

std::optional<std::vector<std::pair<size_t, uint64_t>>> allocate_u64(const std::vector<double>& weights, uint64_t pool, std::string* err)
{
    Err e;
    std::vector<size_t> idx(weights.size());
    std::vector<uint64_t> amt(weights.size());
    size_t n = 0;
    if (!check(hm_allocate_u64(weights.data(), weights.size(), pool, idx.data(), amt.data(), &n, e.buf, ERRCAP), e, err)) return std::nullopt;
    std::vector<std::pair<size_t, uint64_t>> r;
    for (size_t i = 0; i < n; i++) r.emplace_back(idx[i], amt[i]);
    return r;
}

double ndarray_sum(const std::vector<double>& v) { return hm_ndarray_sum(v.data(), v.size()); }
void ndarray_normalize(std::vector<double>& v) { hm_ndarray_normalize(v.data(), v.size()); }
uint64_t reprice(uint64_t p, uint64_t s, uint64_t a) { return hm_reprice(p, s, a); }

bool txpow_verify(uint8_t sel, uint8_t ord, uint8_t diff, ByteSpan data, uint64_t nonce)
{
    return hm_txpow_verify(sel, ord, diff, data.data(), data.size(), nonce);
}
bool txpow_validate(uint8_t sel, uint8_t ord, uint8_t diff) { return hm_txpow_validate(sel, ord, diff); }
Hash32 txpow_hash(uint8_t sel, uint8_t ord, ByteSpan data)
{
    Hash32 h;
    hm_txpow_hash(sel, ord, data.data(), data.size(), h.data());
    return h;
}
std::optional<uint64_t> txpow_mine(uint8_t sel, uint8_t ord, uint8_t diff, ByteSpan data, uint64_t start, uint64_t max_tries)
{
    uint64_t n = 0;
    if (!hm_txpow_mine(sel, ord, diff, data.data(), data.size(), start, max_tries, &n)) return std::nullopt;
    return n;
}

std::optional<ConsensusOutput> run_consensus(const ConsensusInput& in, std::string* err)
{
    size_t nv = in.reputations.size(), nd = in.decisions.size(), nvotes = in.votes.size();
    std::vector<uint8_t> voters(nv * 20), decisions(nd * 3), vv(nvotes * 20), vd(nvotes * 3);
    std::vector<double> reps(nv), vals(nvotes);
    for (size_t i = 0; i < nv; i++) {
        std::memcpy(voters.data() + 20 * i, in.reputations[i].first.b.data(), 20);
        reps[i] = in.reputations[i].second;
    }
    for (size_t j = 0; j < nd; j++) std::memcpy(decisions.data() + 3 * j, in.decisions[j].b.data(), 3);
    for (size_t k = 0; k < nvotes; k++) {
        std::memcpy(vv.data() + 20 * k, in.votes[k].voter.b.data(), 20);
        std::memcpy(vd.data() + 3 * k, in.votes[k].decision.b.data(), 3);
        vals[k] = in.votes[k].value;
    }
    std::vector<uint8_t> o_tag(nd), c_tag(nd), upd_tag(nv), outl(nv);
    std::vector<double> o_val(nd), fl(nd), upd(nv);
    std::vector<uint16_t> c_win(nd);
    size_t fl_len = 0;
    double cert = 0;
    Err e;
    int st = hm_run_consensus(voters.data(), nv, reps.data(), decisions.data(), nd, in.scaled.data(), in.categorical_n.data(), vv.data(),
                              vd.data(), vals.data(), nvotes, o_tag.data(), o_val.data(), c_tag.data(), c_win.data(), fl.data(), &fl_len,
                              &cert, upd.data(), upd_tag.data(), outl.data(), e.buf, ERRCAP);
    if (!check(st, e, err)) return std::nullopt;
    ConsensusOutput out;
    for (size_t j = 0; j < nd; j++) {
        out.outcomes.push_back(o_tag[j] ? std::optional<double>(o_val[j]) : std::nullopt);
        if (c_tag[j] == 0) out.cat_winner.push_back(std::nullopt);
        else if (c_tag[j] == 1) out.cat_winner.push_back(std::optional<uint16_t>());
        else out.cat_winner.push_back(std::optional<uint16_t>(c_win[j]));
    }
    fl.resize(std::min(fl_len, fl.size()));
    out.first_loading = fl;
    out.certainty = cert;
    out.updated_reputations = upd;
    for (size_t i = 0; i < nv; i++) out.outlier.push_back(outl[i] != 0);
    return out;
}

const char* source() { return hm_math_source(); }

} // namespace hivemind::math
