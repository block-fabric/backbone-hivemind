// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Truthcoin's consensus math, called in THEIR compiled code (rust/hivemind_math_ffi over
// third_party/truthcoin-dc/lib/math, v0.20.0). See doc/hivemind.md "Floating point".
#pragma once

#include <hivemind/types.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hivemind::math {

/** Thrown where their math panics (their node cannot connect such a block). */
struct MathPanic : std::runtime_error {
    using std::runtime_error::runtime_error;
};

constexpr uint64_t TRADE_MINER_FEE_SATS = 1000;  // lib/math/trading.rs:15
constexpr uint64_t MIN_TRADING_FEE_SATS = 1000;  // lib/math/trading.rs:13
constexpr double DEFAULT_MARKET_BETA = 7.0;      // lib/state/markets/types.rs:12
constexpr double DEFAULT_TRADING_FEE = 0.005;    // lib/state/markets/types.rs:15
constexpr size_t MAX_OUTCOMES = 256;             // lib/math/lmsr.rs:13

/** calculate_update_cost(cur, new, beta); nullopt with `{LmsrError:?}` in *err. */
std::optional<double> update_cost(const std::vector<int64_t>& cur, const std::vector<int64_t>& next, double beta, std::string* err);
std::optional<std::vector<double>> prices(const std::vector<int64_t>& shares, double beta, std::string* err);
std::optional<double> treasury(const std::vector<int64_t>& shares, double beta, std::string* err);

struct BuyCost {
    uint64_t base_cost_sats = 0, trading_fee_sats = 0, total_cost_sats = 0;
    bool exceeds_limit(uint64_t limit) const { return total_cost_sats + TRADE_MINER_FEE_SATS > limit; }
};
struct SellProceeds {
    uint64_t gross_proceeds_sats = 0, trading_fee_sats = 0, net_proceeds_sats = 0;
};
/** nullopt with `{SatoshiError}` (Display) in *err. */
/** trading::validate_lmsr_parameters; false with their `{LmsrError:?}` text. */
bool validate_lmsr(const std::vector<int64_t>& shares, double beta, std::string* err);
std::optional<BuyCost> buy_cost(double base, double fee_pct, std::string* err);
std::optional<SellProceeds> sell_proceeds(double gross, double fee_pct, std::string* err);
double derive_beta_from_liquidity(uint64_t liquidity_sats, size_t num_outcomes);
double lmsr_liquidity(double beta, size_t num_outcomes);

double round_to_precision(double v, uint32_t decimals);
inline double round_outcome(double v) { return round_to_precision(v, 8); }
inline double round_reputation(double v) { return round_to_precision(v, 10); }
enum class Rounding : uint8_t { Up = 0, Down = 1, Nearest = 2 };
std::optional<uint64_t> to_sats(double v, Rounding m, std::string* err);

/** allocate_proportionally_u64 over participants 0..n-1: (participant index, amount) in
 *  their order; nullopt with `{SatoshiError}` in *err. */
std::optional<std::vector<std::pair<size_t, uint64_t>>> allocate_u64(const std::vector<double>& weights, uint64_t pool, std::string* err);

/** ndarray 0.15.6 `sum()` and `if sum > 0 { a /= sum }`. */
double ndarray_sum(const std::vector<double>& v);
void ndarray_normalize(std::vector<double>& v);

/** math::decisions::reprice (integer Q32/Q62). */
uint64_t reprice(uint64_t p_old, uint64_t sales, uint64_t available_at_open);

/** types/tx_pow.rs */
bool txpow_verify(uint8_t sel, uint8_t ord, uint8_t diff, ByteSpan data, uint64_t nonce);
bool txpow_validate(uint8_t sel, uint8_t ord, uint8_t diff);
Hash32 txpow_hash(uint8_t sel, uint8_t ord, ByteSpan data);
std::optional<uint64_t> txpow_mine(uint8_t sel, uint8_t ord, uint8_t diff, ByteSpan data, uint64_t start, uint64_t max_tries);

/** The voting math of one period (voting/mod.rs compute_consensus -> calculate_consensus). */
struct ConsensusInput {
    std::vector<std::pair<Address, double>> reputations; // sorted holders (rep > 0)
    std::vector<DecisionId> decisions;                     // sorted
    std::vector<uint8_t> scaled;                           // per decision
    std::vector<uint16_t> categorical_n;                   // per decision, 0 = not categorical
    struct Vote {
        Address voter;
        DecisionId decision;
        double value;
    };
    std::vector<Vote> votes; // non-abstain votes of the period
};
struct ConsensusOutput {
    std::vector<std::optional<double>> outcomes;                  // per decision
    std::vector<std::optional<std::optional<uint16_t>>> cat_winner; // per decision (outer = present in categorical_winners)
    std::vector<double> first_loading;
    double certainty = 0;
    std::vector<double> updated_reputations; // per voter
    std::vector<bool> outlier;               // per voter
};
/** nullopt with their error text in *err (block invalid); throws MathPanic on a panic. */
std::optional<ConsensusOutput> run_consensus(const ConsensusInput& in, std::string* err);

/** Vendored source description (for --version). */
const char* source();

} // namespace hivemind::math
