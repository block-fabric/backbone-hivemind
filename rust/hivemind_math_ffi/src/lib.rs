//! C ABI over truthcoin-dc v0.20.0's own consensus math.
//!
//! The modules under `math`, `decision` and `tx_pow` are THEIR files, compiled unmodified
//! from `third_party/truthcoin-dc` (MIT, commit aa8b3ea; SHA256SUMS there). This crate
//! only supplies the few crate-root items those files name (`crate::types::Address`,
//! `crate::state::decisions::DecisionId`, `crate::state::markets::DEFAULT_MARKET_BETA`,
//! `crate::error::InvalidDecisionId`, `crate::BlockHash`) and the `extern "C"` wrappers.
//!
//! Why link their code instead of porting it: their consensus resolves reputation,
//! decision outcomes, LMSR prices and fee splits in IEEE f64 with `libm` (musl ports),
//! nalgebra 0.33's SVD and matrixmultiply's runtime-dispatched dgemm kernels, and
//! ndarray's unrolled reductions. Calling the same machine code reproduces every last bit
//! on the same CPU class, including the kernel choice their own nodes make.
//!
//! Every entry point catches panics: where their node would panic, the caller gets
//! status 2 and rejects the block (their node cannot connect it either).
#![allow(clippy::all)]
#![allow(dead_code)]

use std::collections::{BTreeMap, HashMap, HashSet};
use std::panic::{AssertUnwindSafe, catch_unwind};

pub mod error {
    #[derive(Debug, thiserror::Error)]
    #[error("invalid decision ID: {reason}")]
    pub struct InvalidDecisionId {
        pub reason: String,
    }
}

/// `types/decision.rs` (theirs): DecisionId, DecisionType, DecisionState.
#[path = "../../../third_party/truthcoin-dc/types/decision.rs"]
pub mod decision;

pub mod types {
    /// `types/lib.rs:54`
    pub const SECONDS_PER_QUARTER: u64 = 3600 * 24 * 91;

    /// `types/address.rs`: 20 raw bytes, `Ord` by bytes, borsh = the bytes.
    #[derive(borsh::BorshSerialize, Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
    pub struct Address(pub [u8; 20]);
}
pub use types::Address;

/// `types/hashes.rs` BlockHash: borsh = 32 raw bytes.
#[derive(borsh::BorshSerialize, Clone, Copy, Debug, Default, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct BlockHash(pub [u8; 32]);

/// `types/tx_pow.rs` (theirs).
#[path = "../../../third_party/truthcoin-dc/types/tx_pow.rs"]
pub mod tx_pow;

pub mod state {
    pub mod decisions {
        pub use crate::decision::DecisionId;
    }
    pub mod markets {
        /// `lib/state/markets/types.rs:12`
        pub const DEFAULT_MARKET_BETA: f64 = 7.0;
    }
}

/// `lib/math` (theirs).
#[path = "../../../third_party/truthcoin-dc/lib/math/mod.rs"]
pub mod math;

use decision::DecisionId;
use ndarray::Array1;

pub const HM_OK: i32 = 0;
pub const HM_ERR: i32 = 1;
pub const HM_PANIC: i32 = 2;

unsafe fn write_err(err: *mut u8, cap: usize, msg: &str) {
    if err.is_null() || cap == 0 {
        return;
    }
    let bytes = msg.as_bytes();
    let n = bytes.len().min(cap - 1);
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), err, n);
        *err.add(n) = 0;
    }
}

fn panic_text(p: Box<dyn std::any::Any + Send>) -> String {
    if let Some(s) = p.downcast_ref::<&str>() {
        format!("panic: {s}")
    } else if let Some(s) = p.downcast_ref::<String>() {
        format!("panic: {s}")
    } else {
        "panic".to_string()
    }
}

/// Run `f`; Ok -> HM_OK, Err(text) -> HM_ERR, panic -> HM_PANIC, writing the text to err.
fn guard<F>(err: *mut u8, cap: usize, f: F) -> i32
where
    F: FnOnce() -> Result<(), String>,
{
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(())) => HM_OK,
        Ok(Err(e)) => {
            unsafe { write_err(err, cap, &e) };
            HM_ERR
        }
        Err(p) => {
            unsafe { write_err(err, cap, &panic_text(p)) };
            HM_PANIC
        }
    }
}

unsafe fn slice<'a, T>(p: *const T, n: usize) -> &'a [T] {
    if n == 0 { &[] } else { unsafe { std::slice::from_raw_parts(p, n) } }
}

// ------------------------------------------------------------------ LMSR / trading

/// `math::trading::calculate_update_cost(current, new, beta)`. Error text: `{LmsrError:?}`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hm_update_cost(
    cur: *const i64,
    new: *const i64,
    n: usize,
    beta: f64,
    out: *mut f64,
    err: *mut u8,
    cap: usize,
) -> i32 {
    guard(err, cap, || {
        let c = Array1::from(unsafe { slice(cur, n) }.to_vec());
        let m = Array1::from(unsafe { slice(new, n) }.to_vec());
        let v = math::trading::calculate_update_cost(&c, &m, beta).map_err(|e| format!("{e:?}"))?;
        unsafe { *out = v };
        Ok(())
    })
}

/// `math::trading::calculate_prices(shares, beta)` (n outputs). Error text: `{LmsrError:?}`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hm_prices(
    shares: *const i64,
    n: usize,
    beta: f64,
    out: *mut f64,
    out_len: *mut usize,
    err: *mut u8,
    cap: usize,
) -> i32 {
    guard(err, cap, || {
        let s = Array1::from(unsafe { slice(shares, n) }.to_vec());
        let p = math::trading::calculate_prices(&s, beta).map_err(|e| format!("{e:?}"))?;
        let k = p.len().min(n);
        for (i, x) in p.iter().take(k).enumerate() {
            unsafe { *out.add(i) = *x };
        }
        unsafe { *out_len = p.len() };
        Ok(())
    })
}

/// `math::trading::calculate_treasury(shares, beta)`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hm_treasury(
    shares: *const i64,
    n: usize,
    beta: f64,
    out: *mut f64,
    err: *mut u8,
    cap: usize,
) -> i32 {
    guard(err, cap, || {
        let s = Array1::from(unsafe { slice(shares, n) }.to_vec());
        let v = math::trading::calculate_treasury(&s, beta).map_err(|e| format!("{e:?}"))?;
        unsafe { *out = v };
        Ok(())
    })
}

/// `math::trading::validate_lmsr_parameters` (node/mod.rs update_mempool_buy/sell).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hm_validate_lmsr(shares: *const i64, n: usize, beta: f64, err: *mut u8, cap: usize) -> i32 {
    guard(err, cap, || {
        let s = Array1::from(unsafe { slice(shares, n) }.to_vec());
        math::trading::validate_lmsr_parameters(beta, &s).map_err(|e| format!("{e:?}"))
    })
}

/// `math::trading::calculate_buy_cost` -> [base, fee, total]. Error text: `{SatoshiError}`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hm_buy_cost(base: f64, fee_pct: f64, out: *mut u64, err: *mut u8, cap: usize) -> i32 {
    guard(err, cap, || {
        let c = math::trading::calculate_buy_cost(base, fee_pct).map_err(|e| e.to_string())?;
        unsafe {
            *out = c.base_cost_sats;
            *out.add(1) = c.trading_fee_sats;
            *out.add(2) = c.total_cost_sats;
        }
        Ok(())
    })
}

/// `math::trading::calculate_sell_proceeds` -> [gross, fee, net]. Error text: `{SatoshiError}`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hm_sell_proceeds(gross: f64, fee_pct: f64, out: *mut u64, err: *mut u8, cap: usize) -> i32 {
    guard(err, cap, || {
        let c = math::trading::calculate_sell_proceeds(gross, fee_pct).map_err(|e| e.to_string())?;
        unsafe {
            *out = c.gross_proceeds_sats;
            *out.add(1) = c.trading_fee_sats;
            *out.add(2) = c.net_proceeds_sats;
        }
        Ok(())
    })
}

/// `math::trading::derive_beta_from_liquidity(liquidity_sats, num_outcomes)`.
#[unsafe(no_mangle)]
pub extern "C" fn hm_derive_beta(liquidity_sats: u64, num_outcomes: usize) -> f64 {
    math::trading::derive_beta_from_liquidity(liquidity_sats, num_outcomes)
}

/// `math::trading::calculate_lmsr_liquidity(beta, num_outcomes)`.
#[unsafe(no_mangle)]
pub extern "C" fn hm_lmsr_liquidity(beta: f64, num_outcomes: usize) -> f64 {
    math::trading::calculate_lmsr_liquidity(beta, num_outcomes)
}

// ------------------------------------------------------------------ safe math / allocation

/// `math::safe_math::round_to_precision(value, decimals)`.
#[unsafe(no_mangle)]
pub extern "C" fn hm_round_to_precision(value: f64, decimals: u32) -> f64 {
    math::safe_math::round_to_precision(value, decimals)
}

/// `math::safe_math::to_sats(value, mode)`; mode 0 Up, 1 Down, 2 Nearest.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hm_to_sats(value: f64, mode: u8, out: *mut u64, err: *mut u8, cap: usize) -> i32 {
    use math::safe_math::Rounding;
    guard(err, cap, || {
        let m = match mode {
            0 => Rounding::Up,
            1 => Rounding::Down,
            _ => Rounding::Nearest,
        };
        let v = math::safe_math::to_sats(value, m).map_err(|e| e.to_string())?;
        unsafe { *out = v };
        Ok(())
    })
}

/// `math::allocation::allocate_proportionally_u64` with participant keys 0..n (the
/// algorithm looks only at weights and positions). Writes the allocations in THEIR order
/// (out_idx[i], out_amt[i]) for i < *out_n (≤ n). Error text: `{SatoshiError}`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hm_allocate_u64(
    weights: *const f64,
    n: usize,
    pool: u64,
    out_idx: *mut usize,
    out_amt: *mut u64,
    out_n: *mut usize,
    err: *mut u8,
    cap: usize,
) -> i32 {
    guard(err, cap, || {
        let w = unsafe { slice(weights, n) };
        let parts: Vec<(usize, f64)> = w.iter().copied().enumerate().collect();
        let r = math::allocation::allocate_proportionally_u64(parts, pool).map_err(|e| e.to_string())?;
        if r.allocations.len() > n {
            return Err("allocation longer than its participants".into());
        }
        for (i, (k, a)) in r.allocations.iter().enumerate() {
            unsafe {
                *out_idx.add(i) = *k;
                *out_amt.add(i) = *a;
            }
        }
        unsafe { *out_n = r.allocations.len() };
        Ok(())
    })
}

/// ndarray 0.15.6 `Array1<f64>::sum()` (8-way unrolled fold), as `Market::calculate_final_prices`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hm_ndarray_sum(p: *const f64, n: usize) -> f64 {
    let a = Array1::from(unsafe { slice(p, n) }.to_vec());
    a.sum()
}

/// `let sum = prices.sum(); if sum > 0.0 { prices /= sum }` (market.rs:590-593), in place.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hm_ndarray_normalize(p: *mut f64, n: usize) {
    if n == 0 {
        return;
    }
    let mut a = Array1::from(unsafe { std::slice::from_raw_parts(p, n) }.to_vec());
    let sum: f64 = a.sum();
    if sum > 0.0 {
        a /= sum;
    }
    for (i, x) in a.iter().enumerate() {
        unsafe { *p.add(i) = *x };
    }
}

// ------------------------------------------------------------------ decision pricing

/// `math::decisions::reprice(p_old, sales, available_at_open)` (Q32/Q62 fixed point).
#[unsafe(no_mangle)]
pub extern "C" fn hm_reprice(p_old: u64, sales: u64, available_at_open: u64) -> u64 {
    math::decisions::reprice(p_old, sales, available_at_open)
}

// ------------------------------------------------------------------ tx-pow

/// `TxPowConfig::verify(data, nonce)`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hm_txpow_verify(sel: u8, ord: u8, diff: u8, data: *const u8, len: usize, nonce: u64) -> bool {
    let cfg = tx_pow::TxPowConfig { hash_selector: sel, ordering: ord, difficulty: diff };
    catch_unwind(AssertUnwindSafe(|| cfg.verify(unsafe { slice(data, len) }, nonce))).unwrap_or(false)
}

/// `TxPowConfig::validate()`.
#[unsafe(no_mangle)]
pub extern "C" fn hm_txpow_validate(sel: u8, ord: u8, diff: u8) -> bool {
    tx_pow::TxPowConfig { hash_selector: sel, ordering: ord, difficulty: diff }.validate()
}

/// `TxPowConfig::compute_hash(data)` into out[32].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hm_txpow_hash(sel: u8, ord: u8, data: *const u8, len: usize, out: *mut u8) {
    let cfg = tx_pow::TxPowConfig { hash_selector: sel, ordering: ord, difficulty: 0 };
    let h = cfg.compute_hash(unsafe { slice(data, len) });
    unsafe { std::ptr::copy_nonoverlapping(h.as_ptr(), out, 32) };
}

/// `TxPowConfig::mine(data)`, bounded: tries nonces from `start` for at most `max_tries`;
/// returns true and writes the nonce when found (their wallet's `mine` loops forever).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hm_txpow_mine(
    sel: u8,
    ord: u8,
    diff: u8,
    data: *const u8,
    len: usize,
    start: u64,
    max_tries: u64,
    out: *mut u64,
) -> bool {
    let cfg = tx_pow::TxPowConfig { hash_selector: sel, ordering: ord, difficulty: diff };
    let d = unsafe { slice(data, len) };
    if !cfg.is_enabled() {
        unsafe { *out = 0 };
        return true;
    }
    let mut nonce = start;
    for _ in 0..max_tries {
        if cfg.verify(d, nonce) {
            unsafe { *out = nonce };
            return true;
        }
        nonce = nonce.wrapping_add(1);
    }
    false
}

// ------------------------------------------------------------------ consensus

/// `lib/state/voting/mod.rs` compute_consensus' call into the math:
/// `SparseVoteMatrix::new(voters, decisions)`, `set_vote` for every vote, the weight
/// vector from the reputation map, then `calculate_consensus` (oracle::run_consensus).
///
/// Inputs: voters (20 bytes each, the sorted reputation holders), their reputations,
/// decisions (3 bytes each, sorted), per-decision scaled flag and categorical option count
/// (0 = not categorical), and the votes as (voter bytes, decision bytes, value). Votes are
/// set in the given order (theirs iterate a HashMap; any order reaches the same matrix,
/// and the first error is the same error kind).
///
/// Outputs (caller-allocated): per decision: outcome_tag (0 = None, 1 = Some) and outcome;
/// cat_tag (0 = not in categorical_winners, 1 = Some(None), 2 = Some(Some(w))) and cat_win;
/// first_loading (len written to *fl_len, at most nd); certainty; per voter: updated
/// reputation (all voters get one) and outlier flag.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hm_run_consensus(
    voters: *const u8,
    nv: usize,
    reputations: *const f64,
    decisions: *const u8,
    nd: usize,
    scaled: *const u8,
    categorical_n: *const u16,
    vote_voters: *const u8,
    vote_decisions: *const u8,
    vote_values: *const f64,
    nvotes: usize,
    out_outcome_tag: *mut u8,
    out_outcome: *mut f64,
    out_cat_tag: *mut u8,
    out_cat_win: *mut u16,
    out_first_loading: *mut f64,
    out_fl_len: *mut usize,
    out_certainty: *mut f64,
    out_updated: *mut f64,
    out_updated_tag: *mut u8,
    out_outlier: *mut u8,
    err: *mut u8,
    cap: usize,
) -> i32 {
    use math::voting::{SparseVoteMatrix, VotingWeightVector, calculate_consensus};
    guard(err, cap, || {
        let vb = unsafe { slice(voters, nv * 20) };
        let voter_list: Vec<Address> = vb
            .chunks_exact(20)
            .map(|c| Address(c.try_into().expect("20 bytes")))
            .collect();
        let reps = unsafe { slice(reputations, nv) };
        let db = unsafe { slice(decisions, nd * 3) };
        let mut decision_list = Vec::with_capacity(nd);
        for c in db.chunks_exact(3) {
            let id = DecisionId::from_bytes(c.try_into().expect("3 bytes")).map_err(|e| format!("{e:?}"))?;
            decision_list.push(id);
        }
        let scaled_flags = unsafe { slice(scaled, nd) };
        let cat_counts = unsafe { slice(categorical_n, nd) };
        let mut scaled_set: HashSet<DecisionId> = HashSet::new();
        let mut categorical: HashMap<DecisionId, u16> = HashMap::new();
        for (j, id) in decision_list.iter().enumerate() {
            if scaled_flags[j] != 0 {
                scaled_set.insert(*id);
            } else if cat_counts[j] != 0 {
                categorical.insert(*id, cat_counts[j]);
            }
        }
        let mut reputation_map: BTreeMap<Address, f64> = BTreeMap::new();
        for (a, r) in voter_list.iter().zip(reps.iter()) {
            reputation_map.insert(*a, *r);
        }
        let mut matrix = SparseVoteMatrix::new(voter_list.clone(), decision_list.clone());
        let vv = unsafe { slice(vote_voters, nvotes * 20) };
        let vd = unsafe { slice(vote_decisions, nvotes * 3) };
        let vals = unsafe { slice(vote_values, nvotes) };
        for k in 0..nvotes {
            let a = Address(vv[k * 20..k * 20 + 20].try_into().expect("20 bytes"));
            let d = DecisionId::from_bytes(vd[k * 3..k * 3 + 3].try_into().expect("3 bytes"))
                .map_err(|e| format!("{e:?}"))?;
            matrix
                .set_vote(a, d, vals[k])
                .map_err(|e| format!("Failed to set vote in matrix: {e:?}"))?;
        }
        let weights = VotingWeightVector::from_reputation_map(&reputation_map);
        let r = calculate_consensus(&matrix, &weights, &scaled_set, &categorical)
            .map_err(|e| format!("Failed to calculate consensus: {e:?}"))?;
        for (j, id) in decision_list.iter().enumerate() {
            match r.outcomes.get(id) {
                Some(Some(v)) => unsafe {
                    *out_outcome_tag.add(j) = 1;
                    *out_outcome.add(j) = *v;
                },
                _ => unsafe {
                    *out_outcome_tag.add(j) = 0;
                    *out_outcome.add(j) = 0.0;
                },
            }
            match r.categorical_winners.get(id) {
                None => unsafe {
                    *out_cat_tag.add(j) = 0;
                    *out_cat_win.add(j) = 0;
                },
                Some(None) => unsafe {
                    *out_cat_tag.add(j) = 1;
                    *out_cat_win.add(j) = 0;
                },
                Some(Some(w)) => unsafe {
                    *out_cat_tag.add(j) = 2;
                    *out_cat_win.add(j) = *w;
                },
            }
        }
        let k = r.first_loading.len().min(nd);
        for (i, x) in r.first_loading.iter().take(k).enumerate() {
            unsafe { *out_first_loading.add(i) = *x };
        }
        unsafe {
            *out_fl_len = r.first_loading.len();
            *out_certainty = r.certainty;
        }
        let outliers: HashSet<Address> = r.outliers.iter().copied().collect();
        for (i, a) in voter_list.iter().enumerate() {
            match r.updated_reputations.get(a) {
                Some(v) => unsafe {
                    *out_updated_tag.add(i) = 1;
                    *out_updated.add(i) = *v;
                },
                None => unsafe {
                    *out_updated_tag.add(i) = 0;
                    *out_updated.add(i) = 0.0;
                },
            }
            unsafe { *out_outlier.add(i) = outliers.contains(a) as u8 };
        }
        Ok(())
    })
}

/// Version string of the vendored sources, for logs and `--version` output.
#[unsafe(no_mangle)]
pub extern "C" fn hm_math_source() -> *const u8 {
    b"truthcoin-dc v0.20.0 (aa8b3ea) lib/math, types/tx_pow.rs, types/decision.rs\0".as_ptr()
}
