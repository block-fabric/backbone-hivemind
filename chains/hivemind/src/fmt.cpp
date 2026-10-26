// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <hivemind/fmt.h>

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace hivemind::fmt {

namespace {
/** Shortest round-trip decimal digits of |x| (no leading/trailing zeros) and the decimal
 *  exponent `kk` such that |x| = 0.d1d2...dn × 10^kk. x must be finite and non-zero. */
void shortest(double x, std::string& digits, int& kk)
{
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, std::fabs(x), std::chars_format::scientific);
    std::string s(buf, r.ptr);
    auto e = s.find('e');
    std::string mant = s.substr(0, e);
    int exp = std::atoi(s.c_str() + e + 1);
    digits.clear();
    for (char c : mant)
        if (c != '.') digits.push_back(c);
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    kk = exp + 1;
}

std::string decimal(const std::string& d, int kk, size_t min_frac)
{
    std::string out;
    int len = int(d.size());
    if (kk <= 0) {
        out = "0.";
        out.append(size_t(-kk), '0');
        out += d;
    } else if (kk >= len) {
        out = d;
        out.append(size_t(kk - len), '0');
        if (min_frac) {
            out += '.';
            out.append(min_frac, '0');
        }
        return out;
    } else {
        out = d.substr(0, size_t(kk)) + "." + d.substr(size_t(kk));
    }
    auto dot = out.find('.');
    size_t frac = out.size() - dot - 1;
    if (frac < min_frac) out.append(min_frac - frac, '0');
    return out;
}

std::string exponential(const std::string& d, int kk)
{
    std::string out(1, d[0]);
    if (d.size() > 1) out += "." + d.substr(1);
    out += "e" + std::to_string(kk - 1);
    return out;
}
} // namespace

std::string display(double x)
{
    if (std::isnan(x)) return "NaN";
    std::string sign = std::signbit(x) ? "-" : "";
    if (std::isinf(x)) return sign + "inf";
    if (x == 0) return sign + "0";
    std::string d;
    int kk;
    shortest(x, d, kk);
    return sign + decimal(d, kk, 0);
}

std::string debug(double x)
{
    if (std::isnan(x)) return "NaN";
    std::string sign = std::signbit(x) ? "-" : "";
    if (std::isinf(x)) return sign + "inf";
    if (x == 0) return sign + "0.0";
    double a = std::fabs(x);
    std::string d;
    int kk;
    shortest(x, d, kk);
    if (a < 1e-4 || a >= 1e16) return sign + exponential(d, kk);
    return sign + decimal(d, kk, 1);
}

std::string json(double x)
{
    if (!std::isfinite(x)) return "null";
    std::string sign = std::signbit(x) ? "-" : "";
    if (x == 0) return sign + "0.0";
    std::string d;
    int kk;
    shortest(x, d, kk);
    int len = int(d.size());
    int k = kk - len; // exponent of the last digit
    // ryu::pretty::format64
    if (0 <= k && kk <= 16) {
        std::string out = d;
        out.append(size_t(k), '0');
        return sign + out + ".0";
    }
    if (0 < kk && kk <= 16) return sign + d.substr(0, size_t(kk)) + "." + d.substr(size_t(kk));
    if (-5 < kk && kk <= 0) {
        std::string out = "0.";
        out.append(size_t(-kk), '0');
        return sign + out + d;
    }
    // ryu (serde_json): an explicit '+' on positive exponents ("1e+16", "1.5e-7")
    std::string e = exponential(d, kk);
    auto pos = e.find('e');
    if (e[pos + 1] != '-') e.insert(pos + 1, "+");
    return sign + e;
}

} // namespace hivemind::fmt
