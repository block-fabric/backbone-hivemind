// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// f64 text as Rust writes it: `{}` (Display), `{:?}` (Debug) and serde_json (ryu). Their
// RPC replies and error texts carry these forms, so a drop-in must print them the same.
#pragma once

#include <string>

namespace hivemind::fmt {

/** Rust `format!("{}", x)`: shortest round-trip digits, never an exponent ("1", "0.5",
 *  "0.0000001", "1000000000000000000000", "NaN", "inf", "-0"). */
std::string display(double x);
/** Rust `format!("{:?}", x)`: decimal with at least one fractional digit for
 *  1e-4 <= |x| < 1e16 or 0 ("1.0", "0.5"), shortest exponential otherwise ("1e16", "1.5e-7"). */
std::string debug(double x);
/** serde_json / ryu: "1.0", "0.005", "1e16", "1e-7", "1.234e-5"; non-finite -> "null". */
std::string json(double x);

} // namespace hivemind::fmt
