// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// `truthcoin-cli`: truthcoin-dc v0.20.0 cli/lib.rs over the node's JSON-RPC. Same
// subcommands, aliases, arguments, defaults, help texts (embedded from their release
// binary), clap-style argument errors (exit 2), output (serde_json pretty for structured
// results, Display for scalars, their custom text for market/decision/vote commands) and
// error report (`Error: ...` on stderr, exit 1).
#include <hivemind/fmt.h>
#include <hivemind/json.h>
#include <hivemind/types.h>

#include <bt/bech32.h>
#include <bt/bitcoin.h>
#include <bt/frost.h>
#include <bt/jsonrpc.h>
#include <bt/parse.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

#include "cli_help.inc"
#include "openapi_docs.inc"

const char* CLI_VERSION = "truthcoin_dc_app_cli 0.20.0";

// ------------------------------------------------------------------ output helpers
void json_str(std::string& o, const std::string& s)
{
    o += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        case '\b': o += "\\b"; break;
        case '\f': o += "\\f"; break;
        default:
            if (c < 0x20) {
                char b[8];
                std::snprintf(b, sizeof(b), "\\u%04x", c);
                o += b;
            } else {
                o += char(c);
            }
        }
    }
    o += '"';
}

// serde_json::to_string_pretty: two-space indent, "key": value, empty containers inline
void pretty(std::string& o, const UniValue& v, int indent)
{
    auto pad = [&](int n) { o.append(size_t(n) * 2, ' '); };
    if (v.isObject()) {
        if (v.empty()) {
            o += "{}";
            return;
        }
        o += "{\n";
        for (size_t i = 0; i < v.size(); i++) {
            pad(indent + 1);
            json_str(o, v.getKeys()[i]);
            o += ": ";
            pretty(o, v.getValues()[i], indent + 1);
            o += i + 1 < v.size() ? ",\n" : "\n";
        }
        pad(indent);
        o += "}";
    } else if (v.isArray()) {
        if (v.empty()) {
            o += "[]";
            return;
        }
        o += "[\n";
        for (size_t i = 0; i < v.size(); i++) {
            pad(indent + 1);
            pretty(o, v[i], indent + 1);
            o += i + 1 < v.size() ? ",\n" : "\n";
        }
        pad(indent);
        o += "]";
    } else if (v.isStr()) {
        json_str(o, v.get_str());
    } else {
        o += v.write();
    }
}
std::string pretty(const UniValue& v)
{
    std::string o;
    pretty(o, v, 0);
    return o;
}

// Rust Debug of a str
std::string rust_debug_str(const std::string& s)
{
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        case '\0': o += "\\0"; break;
        default:
            if (c < 0x20 || c == 0x7f) {
                char b[12];
                std::snprintf(b, sizeof b, "\\u{%x}", c);
                o += b;
            } else
                o += char(c);
        }
    }
    return o + "\"";
}

// Display of a scalar result (format!("{x}"))
std::string display(const UniValue& v)
{
    if (v.isStr()) return v.get_str();
    if (v.isNull()) return "null";
    if (v.isBool()) return v.get_bool() ? "true" : "false";
    if (v.isNum()) {
        const std::string& s = v.getValStr();
        if (s.find_first_of(".eE") != std::string::npos) return hivemind::fmt::display(std::strtod(s.c_str(), nullptr));
        return s;
    }
    return pretty(v);
}

// Rust `{:w}` / `{:>w}`: pad to w characters (Unicode scalar values)
size_t char_count(const std::string& s)
{
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) n++;
    return n;
}
std::string pad_left_align(const std::string& s, size_t w) { return s + std::string(w > char_count(s) ? w - char_count(s) : 0, ' '); }
std::string pad_right_align(const std::string& s, size_t w) { return std::string(w > char_count(s) ? w - char_count(s) : 0, ' ') + s; }

std::string fixed(double x, int prec)
{
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", prec, x);
    return b;
}

// ------------------------------------------------------------------ argument model
enum class Ty { Str, U64, U32, I64, Usize, U8, F64, Bool, Addr, SHash, MHash, Peer, Epk, Vk, Sig, Dst, JsonDests, JsonTx, JsonAtx, BtcAddr, StrList };

struct ArgDef {
    enum Kind { Pos, Opt, Flag } kind;
    std::string name; // positional id (snake) or long flag (kebab, without --)
    Ty ty = Ty::Str;
    bool required = false;
    bool multiple = false; // positional Vec
    std::optional<std::string> def;
};

struct CmdDef {
    std::string name;
    std::vector<std::string> aliases;
    std::vector<ArgDef> args;
};

ArgDef P(std::string n, Ty t = Ty::Str, bool req = true) { return ArgDef{ArgDef::Pos, std::move(n), t, req, false, std::nullopt}; }
ArgDef PM(std::string n, Ty t) { return ArgDef{ArgDef::Pos, std::move(n), t, true, true, std::nullopt}; }
ArgDef O(std::string n, Ty t = Ty::Str, bool req = false, std::optional<std::string> def = std::nullopt)
{
    return ArgDef{ArgDef::Opt, std::move(n), t, req, false, std::move(def)};
}
ArgDef F(std::string n) { return ArgDef{ArgDef::Flag, std::move(n), Ty::Bool, false, false, std::nullopt}; }

const std::vector<CmdDef>& commands()
{
    static const std::vector<CmdDef> c = {
        {"balance", {}, {}},
        {"connect-block", {}, {P("block"), P("main_block_hash", Ty::MHash)}},
        {"connect-peer", {}, {P("addr", Ty::Peer)}},
        {"create-deposit", {}, {P("address", Ty::Addr), O("value-sats", Ty::U64, true), O("fee-sats", Ty::U64, true)}},
        {"create-transfer", {}, {P("dest", Ty::Addr), O("value-sats", Ty::U64, true), O("fee-sats", Ty::U64, true)}},
        {"create-transfer-many", {}, {P("dests", Ty::JsonDests), O("fee-sats", Ty::U64, true)}},
        {"create-withdrawal", {}, {P("mainchain_address", Ty::BtcAddr), O("amount-sats", Ty::U64, true), O("fee-sats", Ty::U64, true), O("mainchain-fee-sats", Ty::U64, true)}},
        {"format-deposit-address", {}, {P("address", Ty::Addr)}},
        {"forget-peer", {}, {P("addr", Ty::Peer)}},
        {"generate-mnemonic", {}, {}},
        {"get-best-mainchain-block-hash", {}, {}},
        {"get-best-sidechain-block-hash", {}, {}},
        {"get-block", {}, {P("block_hash", Ty::SHash), P("verbose", Ty::Bool, false)}},
        {"get-blockcount", {}, {}},
        {"get-block-hash", {}, {P("height", Ty::U32)}},
        {"get-block-index", {}, {P("block_hash", Ty::SHash)}},
        {"get-block-template", {}, {}},
        {"get-bmm-inclusions", {}, {P("block_hash", Ty::SHash)}},
        {"get-new-address", {}, {}},
        {"get-stxos", {}, {PM("addresses", Ty::Addr)}},
        {"get-transaction", {}, {P("txid", Ty::SHash)}},
        {"get-two-way-peg-events", {}, {P("block_hash", Ty::SHash)}},
        {"get-utxos", {}, {PM("addresses", Ty::Addr)}},
        {"get-wallet-addresses", {}, {}},
        {"get-wallet-utxos", {}, {}},
        {"get-unconfirmed-wallet-utxos", {}, {}},
        {"get-withdrawal-bundle", {}, {P("m6id", Ty::MHash)}},
        {"invalidate-block", {}, {P("block_hash", Ty::SHash)}},
        {"latest-failed-withdrawal-bundle-height", {}, {}},
        {"list-mempool", {}, {}},
        {"list-peers", {}, {}},
        {"list-utxos", {}, {}},
        {"mainchain-sync-progress", {}, {}},
        {"mine", {}, {O("fee-sats", Ty::U64)}},
        {"pending-withdrawal-bundle", {}, {}},
        {"openapi-schema", {}, {}},
        {"remove-from-mempool", {}, {P("txid", Ty::SHash)}},
        {"set-seed-from-mnemonic", {}, {P("mnemonic")}},
        {"sidechain-wealth", {}, {}},
        {"sign-transaction", {}, {P("transaction", Ty::JsonTx), ArgDef{ArgDef::Pos, "broadcast", Ty::Bool, false, false, std::string("false")}}},
        {"submit-transaction", {}, {P("transaction", Ty::JsonAtx)}},
        {"stop", {}, {}},
        {"get-voter-address", {}, {}},
        {"my-unconfirmed-utxos", {}, {}},
        {"get-transaction-info", {}, {P("txid", Ty::SHash)}},
        {"get-new-encryption-key", {}, {}},
        {"get-new-verifying-key", {}, {}},
        {"encrypt-msg", {"encrypt"}, {O("encryption-pubkey", Ty::Epk, true), O("msg", Ty::Str, true)}},
        {"decrypt-msg", {"decrypt"}, {O("encryption-pubkey", Ty::Epk, true), O("msg", Ty::Str, true), F("utf8")}},
        {"sign-arbitrary-msg", {"sign"}, {O("verifying-key", Ty::Vk, true), O("msg", Ty::Str, true)}},
        {"sign-arbitrary-msg-as-addr", {}, {O("address", Ty::Addr, true), O("msg", Ty::Str, true)}},
        {"verify-signature", {"verify"}, {O("signature", Ty::Sig, true), O("verifying-key", Ty::Vk, true), O("dst", Ty::Dst, true), O("msg", Ty::Str, true)}},
        {"decision-status", {}, {}},
        {"decision-list", {"decisions"}, {O("period", Ty::U32), O("status")}},
        {"decision-get", {}, {P("decision_id")}},
        {"decision-fee", {}, {P("period", Ty::U32)}},
        {"decision-fee-for-id", {}, {P("decision_id")}},
        {"decision-claim",
         {"claim"},
         {O("period-index", Ty::U32, true), O("decision-type", Ty::Str, true), O("header", Ty::Str, true), O("description"), O("min", Ty::F64), O("max", Ty::F64),
          O("increment", Ty::F64), O("option-0-label"), O("option-1-label"), O("option-labels", Ty::StrList), O("tx-fee-sats", Ty::U64, false, "1000"),
          O("max-listing-fee-sats", Ty::U64)}},
        {"market-create",
         {"cm"},
         {O("title", Ty::Str, true), O("description", Ty::Str, true), O("dimensions-json", Ty::Str, true), O("beta", Ty::F64), O("trading-fee", Ty::F64, false, "0.005"),
          O("initial-liquidity", Ty::U64), O("tx-pow-hash-selector", Ty::U8), O("tx-pow-ordering", Ty::U8), O("tx-pow-difficulty", Ty::U8),
          O("tx-fee-sats", Ty::U64, false, "1000"), O("max-listing-fee-sats", Ty::U64)}},
        {"list-open-periods", {"lop"}, {}},
        {"market-list", {"markets"}, {}},
        {"market-get", {}, {P("market_id")}},
        {"market-price-history", {}, {P("market_id")}},
        {"market-buy", {"buy"}, {O("market-id", Ty::Str, true), O("outcome-index", Ty::Usize, true), O("shares-amount", Ty::I64, true), O("max-cost", Ty::U64, true)}},
        {"market-sell",
         {"sell"},
         {O("market-id", Ty::Str, true), O("outcome-index", Ty::Usize, true), O("shares-amount", Ty::I64, true), O("seller-address", Ty::Addr, true),
          O("min-proceeds", Ty::U64, false, "0")}},
        {"market-amplify-beta", {}, {O("market-id", Ty::Str, true), O("amount-sats", Ty::U64, true)}},
        {"market-positions", {"positions"}, {O("address", Ty::Addr), O("market-id")}},
        {"calculate-share-cost", {"calc-cost"}, {O("market-id", Ty::Str, true), O("outcome-index", Ty::Usize, true), O("shares-amount", Ty::I64, true)}},
        {"calculate-initial-liquidity", {}, {O("beta", Ty::F64, true), O("dimensions"), O("num-outcomes", Ty::Usize)}},
        {"vote-voter", {}, {P("address", Ty::Addr)}},
        {"vote-voters", {"voters"}, {}},
        {"vote-submit", {"vote"}, {O("decision-id"), O("vote-value", Ty::F64), O("votes"), O("fee-sats", Ty::U64, false, "1000")}},
        {"vote-list", {}, {O("voter", Ty::Addr), O("decision-id"), O("period-id", Ty::U32)}},
        {"vote-period", {}, {O("period-id", Ty::U32)}},
        {"votecoin-transfer", {}, {P("dest", Ty::Addr), O("amount", Ty::F64, true), O("fee-sats", Ty::U64, false, "1000")}},
        {"votecoin-balance", {}, {P("address", Ty::Addr)}},
    };
    return c;
}

const CmdHelp* help_of(const std::string& cmd)
{
    for (auto& h : CMD_HELP)
        if (cmd == h.name) return &h;
    return nullptr;
}

// clap's error usage: the help's Usage line without " [OPTIONS]"
std::string usage_line(const std::string& cmd)
{
    auto* h = help_of(cmd);
    if (!h) return "Usage: truthcoin-cli [OPTIONS] <COMMAND>";
    std::string s = h->long_help;
    auto p = s.find("Usage: ");
    auto e = s.find('\n', p);
    std::string u = s.substr(p, e - p);
    if (auto o = u.find(" [OPTIONS]"); o != std::string::npos) u.erase(o, 10);
    return u;
}

// strsim::jaro (clap's did_you_mean: candidates with confidence > 0.7, best last)
double jaro(const std::string& a, const std::string& b)
{
    if (a.empty() && b.empty()) return 1.0;
    if (a.empty() || b.empty()) return 0.0;
    std::vector<char32_t> x(a.begin(), a.end()), y(b.begin(), b.end());
    if (x.size() == 1 && y.size() == 1) return x[0] == y[0] ? 1.0 : 0.0;
    size_t range = std::max(x.size(), y.size()) / 2;
    range = range > 0 ? range - 1 : 0;
    std::vector<bool> fm(x.size()), sm(y.size());
    double matches = 0;
    for (size_t i = 0; i < x.size(); i++) {
        size_t lo = i > range ? i - range : 0, hi = std::min(y.size(), i + range + 1);
        for (size_t j = lo; j < hi; j++)
            if (!sm[j] && x[i] == y[j]) {
                fm[i] = sm[j] = true;
                matches++;
                break;
            }
    }
    if (matches == 0) return 0.0;
    double transpositions = 0;
    size_t k = 0;
    for (size_t i = 0; i < x.size(); i++)
        if (fm[i]) {
            while (!sm[k]) k++;
            if (x[i] != y[k]) transpositions++;
            k++;
        }
    transpositions /= 2;
    return (matches / x.size() + matches / y.size() + (matches - transpositions) / matches) / 3.0;
}

std::string upper(const std::string& s)
{
    std::string r;
    for (char c : s) r += c == '-' ? '_' : char(std::toupper((unsigned char)c));
    return r;
}

std::string arg_display(const ArgDef& a)
{
    if (a.kind == ArgDef::Flag) return "--" + a.name;
    if (a.kind == ArgDef::Opt) return "--" + a.name + " <" + upper(a.name) + ">";
    std::string n = upper(a.name);
    if (a.multiple) return "<" + n + ">...";
    return a.required ? "<" + n + ">" : "[" + n + "]";
}

[[noreturn]] void clap_exit(const std::string& msg) // message without "error: "
{
    std::fprintf(stderr, "error: %s\n\nFor more information, try '--help'.\n", msg.c_str());
    std::exit(2);
}
[[noreturn]] void clap_exit_usage(const std::string& msg, const std::string& usage)
{
    std::fprintf(stderr, "error: %s\n\n%s\n\nFor more information, try '--help'.\n", msg.c_str(), usage.c_str());
    std::exit(2);
}

// ---- value parsers: nullopt = ok, else clap's error text after ': ' (or a full message for enums)
std::optional<std::string> int_error(const std::string& v, bool is_signed, long double lo, long double hi)
{
    if (v.empty()) return "cannot parse integer from empty string";
    size_t i = 0;
    if (v[0] == '+' || (is_signed && v[0] == '-')) i = 1;
    if (i == v.size()) return "invalid digit found in string";
    for (size_t k = i; k < v.size(); k++)
        if (!std::isdigit((unsigned char)v[k])) return "invalid digit found in string";
    errno = 0;
    long double x = std::strtold(v.c_str(), nullptr);
    char lo_s[64], hi_s[64];
    std::snprintf(lo_s, sizeof lo_s, "%.0Lf", lo);
    std::snprintf(hi_s, sizeof hi_s, "%.0Lf", hi);
    if (x < lo || x > hi) {
        // clap's ranged parsers parse an i64 first; beyond it, std's overflow message
        if (x > 9223372036854775807.0L || x < -9223372036854775808.0L) return x < 0 ? "number too small to fit in target type" : "number too large to fit in target type";
        return v + " is not in " + lo_s + ".." + "=" + hi_s;
    }
    return std::nullopt;
}

std::optional<std::string> value_error(Ty t, const std::string& v)
{
    switch (t) {
    case Ty::Str:
    case Ty::StrList: return std::nullopt;
    case Ty::U64: {
        if (v.empty()) return "cannot parse integer from empty string";
        for (char c : v)
            if (!std::isdigit((unsigned char)c) && c != '+') return "invalid digit found in string";
        if (!bt::parse_u64(v.size() && v[0] == '+' ? v.substr(1) : v)) return "number too large to fit in target type";
        return std::nullopt;
    }
    case Ty::U32: return int_error(v, false, 0, 4294967295.0L);
    case Ty::U8: return int_error(v, false, 0, 255);
    case Ty::Usize: {
        if (v.empty()) return "cannot parse integer from empty string";
        for (char c : v)
            if (!std::isdigit((unsigned char)c) && c != '+') return "invalid digit found in string";
        if (!bt::parse_u64(v.size() && v[0] == '+' ? v.substr(1) : v)) return "number too large to fit in target type";
        return std::nullopt;
    }
    case Ty::I64: {
        if (v.empty()) return "cannot parse integer from empty string";
        size_t i = (v[0] == '-' || v[0] == '+') ? 1 : 0;
        if (i == v.size()) return "invalid digit found in string";
        for (size_t k = i; k < v.size(); k++)
            if (!std::isdigit((unsigned char)v[k])) return "invalid digit found in string";
        errno = 0;
        (void)std::strtoll(v.c_str(), nullptr, 10);
        if (errno == ERANGE) return v[0] == '-' ? "number too small to fit in target type" : "number too large to fit in target type";
        return std::nullopt;
    }
    case Ty::F64: {
        if (v.empty()) return "cannot parse float from empty string";
        char* e = nullptr;
        (void)std::strtod(v.c_str(), &e);
        std::string lv;
        for (char c : v) lv += char(std::tolower((unsigned char)c));
        bool special = lv == "inf" || lv == "+inf" || lv == "-inf" || lv == "infinity" || lv == "+infinity" || lv == "-infinity" || lv == "nan" ||
                       lv == "+nan" || lv == "-nan";
        bool hex = lv.find("0x") != std::string::npos;
        if ((*e || hex) && !special) return "invalid float literal";
        return std::nullopt;
    }
    case Ty::Bool: return std::nullopt; // handled by caller (possible values)
    case Ty::Addr: {
        auto a = hivemind::Address::from_base58(v);
        if (a) return std::nullopt;
        // bs58 decode then length check, as address.rs FromStr
        static const std::string alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
        for (size_t i = 0; i < v.size(); i++)
            if (alphabet.find(v[i]) == std::string::npos)
                return "provided string contained invalid character '" + std::string(1, v[i]) + "' at byte " + std::to_string(i);
        // decode length
        std::vector<uint8_t> b;
        for (char c : v) {
            int carry = int(alphabet.find(c));
            for (auto& x : b) {
                carry += 58 * x;
                x = uint8_t(carry & 0xff);
                carry >>= 8;
            }
            while (carry) {
                b.push_back(uint8_t(carry & 0xff));
                carry >>= 8;
            }
        }
        size_t zeros = 0;
        while (zeros < v.size() && v[zeros] == '1') zeros++;
        return "wrong address length " + std::to_string(b.size() + zeros) + " != 20";
    }
    case Ty::SHash:
    case Ty::Sig: {
        size_t n = t == Ty::Sig ? 128 : 64;
        std::string s = v;
        if (t == Ty::Sig && s.rfind("0x", 0) == 0) s = s.substr(2);
        if (s.size() != n) return "invalid string length";
        for (size_t i = 0; i < s.size(); i++)
            if (!std::isxdigit((unsigned char)s[i])) return "invalid character " + rust_debug_str(std::string(1, s[i])).replace(0, 1, "'").replace(2, 1, "'") + " at position " + std::to_string(i);
        return std::nullopt;
    }
    case Ty::MHash: {
        if (!bt::hash_from_rev_hex(v)) return "failed to parse hex";
        return std::nullopt;
    }
    case Ty::Peer: return std::nullopt;
    case Ty::Epk: {
        auto b = bt::bech32::decode_bech32m_canonical(v, "ba-enc");
        if (!b || b->size() != 32) return "parsing failed";
        return std::nullopt;
    }
    case Ty::Vk: {
        auto b = bt::bech32::decode_bech32m_canonical(v, "ba-svk");
        if (!b || b->size() != 32) return "parsing failed";
        return std::nullopt;
    }
    case Ty::Dst: return std::nullopt; // handled by caller
    case Ty::BtcAddr: {
        if (!bt::btc::Address::parse(v)) return "base58 error";
        return std::nullopt;
    }
    case Ty::JsonDests:
    case Ty::JsonTx:
    case Ty::JsonAtx: {
        UniValue j;
        if (!j.read(v)) return "expected value at line 1 column 1";
        try {
            if (t == Ty::JsonTx) hivemind::json::parse_transaction(j);
            else if (t == Ty::JsonAtx) hivemind::json::parse_authorized_transaction(j);
            else {
                if (!j.isObject()) return "invalid type: expected a map at line 1 column 1";
                std::set<hivemind::Address> seen;
                for (size_t i = 0; i < j.size(); i++) {
                    auto a = hivemind::Address::from_base58(j.getKeys()[i]);
                    if (!a) return "invalid value: expected an address";
                    if (!seen.insert(*a).second) return "invalid entry: found duplicate key";
                    hivemind::json::parse_u64(j.getValues()[i]);
                }
            }
        } catch (const std::exception& e) {
            return e.what();
        }
        return std::nullopt;
    }
    }
    return std::nullopt;
}

struct Parsed {
    std::map<std::string, std::vector<std::string>> v;
    bool has(const std::string& k) const { return v.count(k) > 0; }
    std::string get(const std::string& k) const { return v.at(k).front(); }
    std::optional<std::string> opt(const std::string& k) const { return has(k) ? std::optional<std::string>(get(k)) : std::nullopt; }
};

bool is_help_flag(const std::string& a) { return a == "-h" || a == "--help"; }

Parsed parse_args(const CmdDef& cmd, const std::vector<std::string>& argv)
{
    Parsed p;
    std::string usage = usage_line(cmd.name);
    std::vector<const ArgDef*> pos;
    for (auto& a : cmd.args)
        if (a.kind == ArgDef::Pos) pos.push_back(&a);
    size_t pi = 0;
    bool only_pos = false;
    auto set_value = [&](const ArgDef& a, const std::string& val) {
        if (a.ty == Ty::Bool || a.ty == Ty::Dst) {
            std::vector<std::string> pv = a.ty == Ty::Bool ? std::vector<std::string>{"true", "false"} : std::vector<std::string>{"transaction", "arbitrary"};
            bool ok = false;
            for (auto& x : pv)
                if (x == val) ok = true;
            if (!ok) {
                std::fprintf(stderr, "error: invalid value '%s' for '%s'\n  [possible values: %s, %s]\n\nFor more information, try '--help'.\n", val.c_str(),
                             arg_display(a).c_str(), pv[0].c_str(), pv[1].c_str());
                std::exit(2);
            }
        } else if (a.ty == Ty::StrList) {
            // value_delimiter = ','
            std::string cur;
            for (char c : val) {
                if (c == ',') {
                    p.v[a.name].push_back(cur);
                    cur.clear();
                } else
                    cur += c;
            }
            p.v[a.name].push_back(cur);
            return;
        } else if (auto e = value_error(a.ty, val)) {
            clap_exit("invalid value '" + val + "' for '" + arg_display(a) + "': " + *e);
        }
        p.v[a.name].push_back(val);
    };
    for (size_t i = 0; i < argv.size(); i++) {
        const std::string& tok = argv[i];
        if (!only_pos && tok == "--") {
            only_pos = true;
            continue;
        }
        if (!only_pos && is_help_flag(tok)) {
            auto* h = help_of(cmd.name);
            std::fputs(tok == "-h" && h->short_help ? h->short_help : h->long_help, stdout);
            std::exit(0);
        }
        if (!only_pos && tok.size() > 2 && tok.rfind("--", 0) == 0) {
            std::string name = tok.substr(2), val;
            bool inline_val = false;
            if (auto eq = name.find('='); eq != std::string::npos) {
                val = name.substr(eq + 1);
                name = name.substr(0, eq);
                inline_val = true;
            }
            const ArgDef* a = nullptr;
            for (auto& x : cmd.args)
                if (x.kind != ArgDef::Pos && x.name == name) a = &x;
            if (!a) {
                std::fprintf(stderr, "error: unexpected argument '%s' found\n\n  tip: to pass '%s' as a value, use '-- %s'\n\n%s\n\nFor more information, try '--help'.\n",
                             tok.c_str(), tok.c_str(), tok.c_str(), usage.c_str());
                std::exit(2);
            }
            if (a->kind == ArgDef::Flag) {
                if (inline_val) clap_exit("unexpected value '" + val + "' for '" + arg_display(*a) + "' found; no more were expected");
                if (p.has(a->name)) clap_exit_usage("the argument '" + arg_display(*a) + "' cannot be used multiple times", usage);
                p.v[a->name].push_back("true");
                continue;
            }
            if (p.has(a->name) && a->ty != Ty::StrList) clap_exit_usage("the argument '" + arg_display(*a) + "' cannot be used multiple times", usage);
            if (!inline_val) {
                if (i + 1 >= argv.size()) clap_exit("a value is required for '" + arg_display(*a) + "' but none was supplied");
                const std::string& nx = argv[i + 1];
                if (nx.size() > 1 && nx[0] == '-') {
                    std::fprintf(stderr, "error: unexpected argument '%s' found\n\n  tip: to pass '%s' as a value, use '-- %s'\n\n%s\n\nFor more information, try '--help'.\n",
                                 nx.c_str(), nx.c_str(), nx.c_str(), usage.c_str());
                    std::exit(2);
                }
                val = nx;
                i++;
            }
            set_value(*a, val);
            continue;
        }
        if (!only_pos && tok.size() > 1 && tok[0] == '-') {
            std::fprintf(stderr, "error: unexpected argument '%s' found\n\n  tip: to pass '%s' as a value, use '-- %s'\n\n%s\n\nFor more information, try '--help'.\n", tok.c_str(),
                         tok.c_str(), tok.c_str(), usage.c_str());
            std::exit(2);
        }
        if (pi >= pos.size()) clap_exit_usage("unexpected argument '" + tok + "' found", usage);
        const ArgDef& a = *pos[pi];
        set_value(a, tok);
        if (!a.multiple) pi++;
    }
    // missing required (usage order: options first, then positionals)
    std::vector<std::string> missing;
    for (auto& a : cmd.args)
        if (a.kind == ArgDef::Opt && a.required && !p.has(a.name)) missing.push_back(arg_display(a));
    for (auto& a : cmd.args)
        if (a.kind == ArgDef::Pos && a.required && !p.has(a.name)) missing.push_back(arg_display(a));
    if (!missing.empty()) {
        std::string m = "the following required arguments were not provided:";
        for (auto& x : missing) m += "\n  " + x;
        clap_exit_usage(m, usage);
    }
    for (auto& a : cmd.args)
        if (a.def && !p.has(a.name)) p.v[a.name].push_back(*a.def);
    return p;
}

// ------------------------------------------------------------------ RPC
struct Rpc {
    std::string host;
    uint16_t port = 0;
};

std::string error_object_debug(const UniValue& e)
{
    int code = e["code"].isNum() ? e["code"].getInt<int>() : 0;
    std::string c;
    switch (code) {
    case -32700: c = "ParseError"; break;
    case -32600: c = "InvalidRequest"; break;
    case -32601: c = "MethodNotFound"; break;
    case -32602: c = "InvalidParams"; break;
    case -32603: c = "InternalError"; break;
    default: c = "ServerError(" + std::to_string(code) + ")";
    }
    std::string data = "None";
    if (e.exists("data") && !e["data"].isNull()) data = "Some(RawValue(" + e["data"].write() + "))";
    return "ErrorObject { code: " + c + ", message: " + rust_debug_str(e["message"].getValStr()) + ", data: " + data + " }";
}

[[noreturn]] void fail_rpc(const UniValue& e)
{
    std::string d = error_object_debug(e);
    std::fprintf(stderr, "Error: %s\n\nCaused by:\n    %s\n", d.c_str(), d.c_str());
    std::exit(1);
}

[[noreturn]] void fail_msg(const std::string& m)
{
    std::fprintf(stderr, "Error: %s\n", m.c_str());
    std::exit(1);
}

UniValue call(const Rpc& r, const std::string& method, const UniValue& params)
{
    std::string err;
    UniValue resp = bt::jsonrpc::call(r.host, r.port, method, params, err);
    if (!err.empty()) {
        if (err.rfind("cannot connect", 0) == 0) {
            std::fprintf(stderr, "Error: client error (Connect)\n\nCaused by:\n    0: tcp connect error\n    1: Connection refused (os error 111)\n");
            std::exit(1);
        }
        fail_msg(err);
    }
    if (resp.exists("error") && !resp["error"].isNull()) fail_rpc(resp["error"]);
    return resp["result"];
}

UniValue arr() { return UniValue(UniValue::VARR); }
UniValue obj() { return UniValue(UniValue::VOBJ); }
UniValue num_text(const std::string& s) { return UniValue(UniValue::VNUM, s); }
UniValue f64v(const std::string& s) { return hivemind::json::f64(std::strtod(s.c_str(), nullptr)); }
UniValue u64v(const std::string& s) { return num_text(s.size() && s[0] == '+' ? s.substr(1) : s); }
UniValue opt_u64(const Parsed& p, const std::string& k) { return p.has(k) ? u64v(p.get(k)) : UniValue(); }
UniValue opt_f64(const Parsed& p, const std::string& k) { return p.has(k) ? f64v(p.get(k)) : UniValue(); }
UniValue opt_str(const Parsed& p, const std::string& k) { return p.has(k) ? UniValue(p.get(k)) : UniValue(); }
UniValue jparse(const std::string& s)
{
    UniValue v;
    v.read(s);
    return v;
}

std::string format_with_commas(uint64_t n)
{
    std::string s = std::to_string(n), out;
    for (size_t i = 0; i < s.size(); i++) {
        size_t from_end = s.size() - i;
        if (i > 0 && from_end % 3 == 0) out += ',';
        out += s[i];
    }
    return out;
}

std::string render_period_slot_grid(uint32_t period, const UniValue& info, const std::vector<uint32_t>& claimed)
{
    uint64_t mints = info["mints"].getInt<uint64_t>();
    std::string out = "Period " + std::to_string(period) + "\np_period: " + info["p_period"].getValStr() + " sats   mints: " + std::to_string(mints) +
                      "/20   last reprice: block " + info["last_reprice_block"].getValStr() + "\n";
    size_t cells = size_t(mints * 5);
    std::set<uint32_t> cs(claimed.begin(), claimed.end());
    size_t w = 0;
    for (size_t i = 0; i < 5; i++) w = std::max(w, format_with_commas(info["tier_prices"][i].getInt<uint64_t>()).size());
    out += pad_right_align("sats", w) + "   " + pad_left_align("slots", cells) + "      claimed\n";
    for (uint32_t tier = 0; tier < 5; tier++) {
        std::string row;
        uint64_t c = 0;
        for (uint32_t pos = 0; pos < cells; pos++) {
            if (cs.count(tier * 100 + pos)) {
                row += "\xE2\x96\x88";
                c++;
            } else
                row += "\xE2\x96\x91";
        }
        out += pad_right_align(format_with_commas(info["tier_prices"][tier].getInt<uint64_t>()), w) + "   " + row + "      " + std::to_string(c) + "/" +
               std::to_string(cells) + "\n";
    }
    out += "\nPeriod total: " + info["claimed"].getValStr() + " / " + info["period_capacity"].getValStr() + "\n";
    return out;
}

std::string debug_u32_list(const UniValue& a)
{
    std::string s = "[";
    for (size_t i = 0; i < a.size(); i++) s += (i ? ", " : "") + a[i].getValStr();
    return s + "]";
}

// cli/lib.rs handle_command
std::string handle(const Rpc& r, const std::string& name, const Parsed& p)
{
    auto none = arr();
    auto simple_pretty = [&](const std::string& m, UniValue params) { return pretty(call(r, m, params)); };
    auto params = [&](std::initializer_list<UniValue> l) {
        UniValue a = arr();
        for (auto& x : l) a.push_back(x);
        return a;
    };
    if (name == "balance") return simple_pretty("balance", none);
    if (name == "connect-block") {
        UniValue b;
        if (!b.read(p.get("block"))) fail_msg("expected value at line 1 column 1");
        return display(call(r, "connect_block", params({b, UniValue(p.get("main_block_hash"))})));
    }
    if (name == "get-block-hash") return simple_pretty("get_block_hash", params({u64v(p.get("height"))}));
    if (name == "connect-peer") {
        call(r, "connect_peer", params({UniValue(p.get("addr"))}));
        return "";
    }
    if (name == "create-deposit")
        return display(call(r, "create_deposit", params({UniValue(p.get("address")), u64v(p.get("value-sats")), u64v(p.get("fee-sats"))})));
    if (name == "create-transfer")
        return display(call(r, "create_transfer", params({UniValue(p.get("dest")), u64v(p.get("value-sats")), u64v(p.get("fee-sats"))})));
    if (name == "create-transfer-many") return display(call(r, "create_transfer_many", params({jparse(p.get("dests")), u64v(p.get("fee-sats"))})));
    if (name == "create-withdrawal")
        return display(call(r, "create_withdrawal",
                            params({UniValue(p.get("mainchain_address")), u64v(p.get("amount-sats")), u64v(p.get("fee-sats")), u64v(p.get("mainchain-fee-sats"))})));
    if (name == "format-deposit-address") return display(call(r, "format_deposit_address", params({UniValue(p.get("address"))})));
    if (name == "forget-peer") {
        call(r, "forget_peer", params({UniValue(p.get("addr"))}));
        return "";
    }
    if (name == "get-best-mainchain-block-hash") return simple_pretty("get_best_mainchain_block_hash", none);
    if (name == "get-best-sidechain-block-hash") return simple_pretty("get_best_sidechain_block_hash", none);
    if (name == "get-block") {
        bool v = p.opt("verbose").value_or("false") == "true";
        return simple_pretty("get_block", params({UniValue(p.get("block_hash")), UniValue(v)}));
    }
    if (name == "get-blockcount") return display(call(r, "getblockcount", none));
    if (name == "get-block-index") return simple_pretty("get_block_index", params({UniValue(p.get("block_hash"))}));
    if (name == "get-block-template") return simple_pretty("get_block_template", none);
    if (name == "get-bmm-inclusions") return simple_pretty("get_bmm_inclusions", params({UniValue(p.get("block_hash"))}));
    if (name == "generate-mnemonic") return display(call(r, "generate_mnemonic", none));
    if (name == "get-new-address") return display(call(r, "get_new_address", none));
    if (name == "get-stxos" || name == "get-utxos") {
        UniValue a = arr();
        std::set<std::string> seen;
        for (auto& x : p.v.at("addresses"))
            if (seen.insert(x).second) a.push_back(x); // HashSet
        return simple_pretty(name == "get-stxos" ? "get_stxos" : "get_utxos", params({a}));
    }
    if (name == "get-transaction") return simple_pretty("get_transaction", params({UniValue(p.get("txid"))}));
    if (name == "get-two-way-peg-events") return simple_pretty("get_two_way_peg_events", params({UniValue(p.get("block_hash"))}));
    if (name == "get-wallet-addresses") return simple_pretty("get_wallet_addresses", none);
    if (name == "get-wallet-utxos") return simple_pretty("get_wallet_utxos", none);
    if (name == "get-unconfirmed-wallet-utxos") return simple_pretty("get_unconfirmed_wallet_utxos", none);
    if (name == "get-withdrawal-bundle") return simple_pretty("get_withdrawal_bundle", params({UniValue(p.get("m6id"))}));
    if (name == "invalidate-block") {
        call(r, "invalidate_block", params({UniValue(p.get("block_hash"))}));
        return "";
    }
    if (name == "latest-failed-withdrawal-bundle-height") return simple_pretty("latest_failed_withdrawal_bundle_height", none);
    if (name == "list-mempool") return simple_pretty("list_mempool", none);
    if (name == "list-peers") return simple_pretty("list_peers", none);
    if (name == "list-utxos") return simple_pretty("list_utxos", none);
    if (name == "mainchain-sync-progress") return simple_pretty("mainchain_sync_progress", none);
    if (name == "mine") {
        call(r, "mine", params({opt_u64(p, "fee-sats")}));
        return "";
    }
    if (name == "pending-withdrawal-bundle") return simple_pretty("pending_withdrawal_bundle", none);
    if (name == "openapi-schema") {
        // built locally from their rpc-api crate (the combined document), as theirs does
        UniValue v;
        v.read(OPENAPI_ALL);
        return pretty(v);
    }
    if (name == "remove-from-mempool") {
        call(r, "remove_from_mempool", params({UniValue(p.get("txid"))}));
        return "";
    }
    if (name == "set-seed-from-mnemonic") {
        call(r, "set_seed_from_mnemonic", params({UniValue(p.get("mnemonic"))}));
        return "";
    }
    if (name == "sidechain-wealth") return display(call(r, "sidechain_wealth", none));
    if (name == "sign-transaction")
        return simple_pretty("sign_transaction", params({jparse(p.get("transaction")), UniValue(p.get("broadcast") == "true")}));
    if (name == "submit-transaction") return display(call(r, "submit_transaction", params({jparse(p.get("transaction"))})));
    if (name == "stop") {
        call(r, "stop", none);
        return "";
    }
    if (name == "get-voter-address") return display(call(r, "get_voter_address", none));
    if (name == "my-unconfirmed-utxos") return simple_pretty("my_unconfirmed_utxos", none);
    if (name == "get-transaction-info") return simple_pretty("get_transaction_info", params({UniValue(p.get("txid"))}));
    if (name == "get-new-encryption-key") return display(call(r, "get_new_encryption_key", none));
    if (name == "get-new-verifying-key") return display(call(r, "get_new_verifying_key", none));
    if (name == "encrypt-msg") return display(call(r, "encrypt_msg", params({UniValue(p.get("encryption-pubkey")), UniValue(p.get("msg"))})));
    if (name == "decrypt-msg") {
        std::string h = display(call(r, "decrypt_msg", params({UniValue(p.get("encryption-pubkey")), UniValue(p.get("msg"))})));
        if (p.has("utf8")) {
            auto b = bt::hex_decode(h);
            if (!b) fail_msg("Odd number of digits");
            return std::string(b->begin(), b->end());
        }
        return h;
    }
    if (name == "sign-arbitrary-msg") return display(call(r, "sign_arbitrary_msg", params({UniValue(p.get("verifying-key")), UniValue(p.get("msg"))})));
    if (name == "sign-arbitrary-msg-as-addr") return simple_pretty("sign_arbitrary_msg_as_addr", params({UniValue(p.get("address")), UniValue(p.get("msg"))}));
    if (name == "verify-signature") {
        std::string sig = p.get("signature");
        if (sig.rfind("0x", 0) != 0) sig = "0x" + sig;
        return display(call(r, "verify_signature", params({UniValue(sig), UniValue(p.get("verifying-key")), UniValue(p.get("dst")), UniValue(p.get("msg"))})));
    }
    if (name == "decision-status") return simple_pretty("decision_status", none);
    if (name == "decision-list") {
        std::optional<std::string> st;
        if (p.has("status")) {
            std::string s;
            for (char c : p.get("status")) s += char(std::tolower((unsigned char)c));
            if (s == "created" || s == "available") st = "Created";
            else if (s == "claimed") st = "Claimed";
            else if (s == "voting") st = "Voting";
            else if (s == "resolved" || s == "settled") st = "Resolved";
            else if (s == "invalid") st = "Invalid";
            else fail_msg("Unrecognized decision status: '" + s + "'. Valid values: created, claimed, voting, resolved, invalid");
        }
        UniValue filter;
        if (p.has("period") || st) {
            filter = obj();
            filter.pushKV("period", opt_u64(p, "period"));
            filter.pushKV("status", st ? UniValue(*st) : UniValue());
        }
        UniValue decisions = call(r, "decision_list", params({filter}));
        if (p.has("period")) {
            uint32_t per = uint32_t(std::stoul(p.get("period")));
            std::string out;
            std::string err;
            UniValue rq(UniValue::VOBJ);
            UniValue resp = bt::jsonrpc::call(r.host, r.port, "decision_listing_fee", params({u64v(p.get("period"))}), err);
            if (err.empty() && resp["error"].isNull() && resp["result"].isObject()) {
                std::vector<uint32_t> claimed;
                for (auto& d : decisions.getValues())
                    if (d["period_index"].getInt<uint32_t>() == per) claimed.push_back(d["decision_index"].getInt<uint32_t>());
                out += render_period_slot_grid(per, resp["result"], claimed);
                out += "\n";
            }
            return out + pretty(decisions);
        }
        return pretty(decisions);
    }
    if (name == "decision-get") return simple_pretty("decision_get", params({UniValue(p.get("decision_id"))}));
    if (name == "decision-fee") return simple_pretty("decision_listing_fee", params({u64v(p.get("period"))}));
    if (name == "decision-fee-for-id") return display(call(r, "decision_fee_for_id", params({UniValue(p.get("decision_id"))}))) + " sats";
    if (name == "decision-claim") {
        std::string dt = p.get("decision-type");
        if (dt != "binary" && dt != "scaled" && dt != "category")
            fail_msg("Invalid --decision-type '" + dt + "': must be 'binary', 'scaled', or 'category'");
        UniValue labels;
        if (p.has("option-labels")) {
            auto& ls = p.v.at("option-labels");
            if (ls.size() < 2) fail_msg("Category decisions require at least 2 option labels, got " + std::to_string(ls.size()));
            labels = arr();
            for (auto& l : ls) labels.push_back(l);
        }
        UniValue item = obj();
        item.pushKV("period_index", u64v(p.get("period-index")));
        item.pushKV("header", p.get("header"));
        item.pushKV("description", opt_str(p, "description"));
        item.pushKV("option_0_label", opt_str(p, "option-0-label"));
        item.pushKV("option_1_label", opt_str(p, "option-1-label"));
        item.pushKV("option_labels", labels);
        item.pushKV("tags", UniValue());
        UniValue req = obj();
        req.pushKV("decision_type", dt);
        req.pushKV("decisions", params({item}));
        req.pushKV("min", opt_f64(p, "min"));
        req.pushKV("max", opt_f64(p, "max"));
        req.pushKV("increment", opt_f64(p, "increment"));
        req.pushKV("tx_fee_sats", u64v(p.get("tx-fee-sats")));
        req.pushKV("max_listing_fee_sats", opt_u64(p, "max-listing-fee-sats"));
        UniValue resp = call(r, "decision_claim", params({req}));
        std::string ids;
        for (size_t i = 0; i < resp["decision_ids"].size(); i++) ids += (i ? ", " : "") + resp["decision_ids"][i].get_str();
        return "Decision claimed:\n  txid: " + display(resp["txid"]) + "\n  decision_ids: " + ids + "\n  listing_fee_paid_sats: " + resp["listing_fee_paid_sats"].getValStr();
    }
    if (name == "market-create") {
        UniValue dims;
        if (!dims.read(p.get("dimensions-json")) || !dims.isArray()) fail_msg("failed to parse --dimensions-json: expected value at line 1 column 1");
        UniValue req = obj();
        req.pushKV("title", p.get("title"));
        req.pushKV("description", p.get("description"));
        req.pushKV("dimensions", dims);
        req.pushKV("beta", opt_f64(p, "beta"));
        req.pushKV("trading_fee", f64v(p.get("trading-fee")));
        req.pushKV("initial_liquidity", opt_u64(p, "initial-liquidity"));
        req.pushKV("tx_pow_hash_selector", opt_u64(p, "tx-pow-hash-selector"));
        req.pushKV("tx_pow_ordering", opt_u64(p, "tx-pow-ordering"));
        req.pushKV("tx_pow_difficulty", opt_u64(p, "tx-pow-difficulty"));
        req.pushKV("tx_fee_sats", u64v(p.get("tx-fee-sats")));
        req.pushKV("max_listing_fee_sats", opt_u64(p, "max-listing-fee-sats"));
        UniValue resp = call(r, "market_create", params({req}));
        std::string out = "Market '" + p.get("title") + "' created: txid=" + display(resp["txid"]) + ", market_id=" + display(resp["market_id"]);
        if (!resp["claimed_decisions"].empty()) {
            out += "\nClaimed decisions:";
            for (auto& d : resp["claimed_decisions"].getValues())
                out += "\n  - id=" + d["id"].get_str() + " period=" + d["period_index"].getValStr() + " fee_sats=" + d["listing_fee_paid_sats"].getValStr();
        }
        return out;
    }
    if (name == "list-open-periods") {
        UniValue ps = call(r, "list_open_periods_with_pricing", none);
        if (ps.empty()) return "No open periods.";
        std::string out = "Open periods:\n";
        for (auto& x : ps.getValues())
            out += "  period " + x["period_index"].getValStr() + " \xE2\x80\x94 cheapest: " + x["cheapest_available_slot_sats"].getValStr() + " sats (tier " +
                   x["cheapest_available_tier"].getValStr() + "), slots/tier " + debug_u32_list(x["slots_available_by_tier"]) + "\n";
        return out;
    }
    if (name == "market-list") {
        UniValue ms = call(r, "market_list", none);
        if (ms.empty()) return "No markets in Trading state found.";
        std::string o = "Markets in Trading State:\n";
        o += "\xE2\x94\x8C" + [] {
            std::string s;
            const int w[] = {18, 26, 25, 14, 12};
            for (int i = 0; i < 5; i++) {
                for (int k = 0; k < w[i]; k++) s += "\xE2\x94\x80";
                s += i < 4 ? "\xE2\x94\xAC" : "\xE2\x94\x90";
            }
            return s;
        }() + "\n";
        o += "\xE2\x94\x82 Market ID        \xE2\x94\x82 Title                    \xE2\x94\x82 Outcomes                \xE2\x94\x82 Volume       \xE2\x94\x82 State      \xE2\x94\x82\n";
        auto rule = [](const char* l, const char* m, const char* rr) {
            std::string s = l;
            const int w[] = {18, 26, 25, 14, 12};
            for (int i = 0; i < 5; i++) {
                for (int k = 0; k < w[i]; k++) s += "\xE2\x94\x80";
                s += i < 4 ? m : rr;
            }
            return s;
        };
        o += rule("\xE2\x94\x9C", "\xE2\x94\xBC", "\xE2\x94\xA4") + "\n";
        for (auto& m : ms.getValues()) {
            std::string title = m["title"].get_str();
            if (title.size() > 22) title = title.substr(0, 19) + "...";
            std::string outs = m["outcome_count"].getValStr() + " outcomes";
            if (outs.size() > 21) outs = outs.substr(0, 18) + "...";
            std::string vol = m["volume_sats"].getValStr();
            if (vol.size() > 12) vol = vol.substr(0, 9) + "...";
            std::string bar = "\xE2\x94\x82";
            o += bar + " " + pad_left_align(m["market_id"].get_str(), 16) + " " + bar + " " + pad_left_align(title, 24) + " " + bar + " " + pad_left_align(outs, 23) + " " +
                 bar + " " + pad_left_align(vol, 12) + " " + bar + " " + pad_left_align(m["state"].get_str(), 10) + " " + bar + "\n";
        }
        o += rule("\xE2\x94\x94", "\xE2\x94\xB4", "\xE2\x94\x98") + "\n";
        o += "\nTotal markets: " + std::to_string(ms.size());
        return o;
    }
    if (name == "market-get") return simple_pretty("market_get", params({UniValue(p.get("market_id"))}));
    if (name == "market-price-history") return simple_pretty("market_price_history", params({UniValue(p.get("market_id"))}));
    if (name == "market-buy") {
        UniValue req = obj();
        req.pushKV("market_id", p.get("market-id"));
        req.pushKV("outcome_index", u64v(p.get("outcome-index")));
        req.pushKV("shares_amount", num_text(p.get("shares-amount")));
        req.pushKV("max_cost", u64v(p.get("max-cost")));
        req.pushKV("dry_run", false);
        UniValue res = call(r, "market_buy", params({req}));
        return "Successfully submitted buy shares transaction!\nMarket: " + p.get("market-id") + "\nOutcome Index: " + std::to_string(std::stoull(p.get("outcome-index"))) +
               "\nShares: " + std::to_string(std::stoll(p.get("shares-amount"))) + "\nCost: " + res["cost_sats"].getValStr() +
               " sats\nTransaction ID: " + (res["txid"].isStr() ? res["txid"].get_str() : "");
    }
    if (name == "market-sell") {
        UniValue req = obj();
        req.pushKV("market_id", p.get("market-id"));
        req.pushKV("outcome_index", u64v(p.get("outcome-index")));
        req.pushKV("shares_amount", num_text(p.get("shares-amount")));
        req.pushKV("seller_address", p.get("seller-address"));
        req.pushKV("min_proceeds", u64v(p.get("min-proceeds")));
        req.pushKV("dry_run", false);
        UniValue res = call(r, "market_sell", params({req}));
        std::string txid = res["txid"].isStr() ? res["txid"].get_str() : "None (dry run)";
        return "Successfully submitted sell shares transaction!\nMarket: " + p.get("market-id") + "\nOutcome Index: " + std::to_string(std::stoull(p.get("outcome-index"))) +
               "\nShares Sold: " + std::to_string(std::stoll(p.get("shares-amount"))) + "\nGross Proceeds: " + res["proceeds_sats"].getValStr() +
               " sats\nTrading Fee: " + res["trading_fee_sats"].getValStr() + " sats\nNet Proceeds: " + res["net_proceeds_sats"].getValStr() +
               " sats\nTransaction ID: " + txid;
    }
    if (name == "market-amplify-beta") {
        UniValue req = obj();
        req.pushKV("market_id", p.get("market-id"));
        req.pushKV("amount_sats", u64v(p.get("amount-sats")));
        return "AmplifyBeta submitted for market " + p.get("market-id") + ": " + display(call(r, "market_amplify_beta", params({req})));
    }
    if (name == "market-positions") {
        std::string addr = p.has("address") ? p.get("address") : display(call(r, "get_new_address", none));
        return simple_pretty("market_positions", params({UniValue(addr), opt_str(p, "market-id")}));
    }
    if (name == "calculate-share-cost") {
        UniValue req = obj();
        req.pushKV("market_id", p.get("market-id"));
        req.pushKV("outcome_index", u64v(p.get("outcome-index")));
        req.pushKV("shares_amount", num_text(p.get("shares-amount")));
        req.pushKV("max_cost", UniValue());
        req.pushKV("dry_run", true);
        UniValue res = call(r, "market_buy", params({req}));
        uint64_t cost = res["cost_sats"].getInt<uint64_t>();
        return "Share Purchase Cost Calculation:\nMarket: " + p.get("market-id") + "\nOutcome Index: " + std::to_string(std::stoull(p.get("outcome-index"))) +
               "\nShares Amount: " + std::to_string(std::stoll(p.get("shares-amount"))) + "\nEstimated Cost: " + std::to_string(cost) + " sats (" +
               fixed(double(cost) / 100000000.0, 8) + " BTC)";
    }
    if (name == "calculate-initial-liquidity") {
        UniValue req = obj();
        req.pushKV("beta", f64v(p.get("beta")));
        req.pushKV("dimensions", opt_str(p, "dimensions"));
        req.pushKV("num_outcomes", opt_u64(p, "num-outcomes"));
        UniValue res = call(r, "calculate_initial_liquidity", params({req}));
        double beta = std::strtod(res["beta"].getValStr().c_str(), nullptr);
        uint64_t n = res["num_outcomes"].getInt<uint64_t>();
        double liq = n <= 1 ? 0.0 : beta * std::log(double(n));
        std::string bar;
        for (int i = 0; i < 80; i++) bar += "\xE2\x94\x81";
        return "Initial Liquidity Calculation:\n" + bar + "\nBeta Parameter:       " + fixed(beta, 2) + "\nNumber of Outcomes:   " + std::to_string(n) +
               "\nMarket Configuration: " + res["market_config"].get_str() + "\n\nCalculation Details:\n" + res["outcome_breakdown"].get_str() +
               "\n\nRequired Initial Liquidity: " + res["initial_liquidity_sats"].getValStr() + " sats\nMinimum Treasury:          " +
               res["min_treasury_sats"].getValStr() + " sats\n\nFormula Used: Initial Liquidity = \xCE\xB2 \xC3\x97 ln(Number of States)\n= " +
               fixed(beta, 2) + " \xC3\x97 ln(" + std::to_string(n) + ") = " + fixed(liq, 6) + " \xE2\x89\x88 " + res["initial_liquidity_sats"].getValStr() + " sats\n" + bar;
    }
    if (name == "vote-voter") return simple_pretty("vote_voter", params({UniValue(p.get("address"))}));
    if (name == "vote-voters") return simple_pretty("vote_voters", none);
    if (name == "vote-submit") {
        UniValue items = arr();
        if (p.has("votes")) {
            std::string batch = p.get("votes"), cur;
            std::vector<std::string> pairs;
            for (char c : batch) {
                if (c == ',') {
                    pairs.push_back(cur);
                    cur.clear();
                } else
                    cur += c;
            }
            pairs.push_back(cur);
            for (auto& pr : pairs) {
                // pair.trim().split(':') with exactly two parts and an f64 value
                size_t a = 0, b = pr.size();
                while (a < b && std::isspace((unsigned char)pr[a])) a++;
                while (b > a && std::isspace((unsigned char)pr[b - 1])) b--;
                std::string t = pr.substr(a, b - a);
                if (std::count(t.begin(), t.end(), ':') != 1) continue;
                auto c = t.find(':');
                std::string val = t.substr(c + 1);
                if (value_error(Ty::F64, val)) continue;
                UniValue it = obj();
                it.pushKV("decision_id", t.substr(0, c));
                it.pushKV("vote_value", f64v(val));
                items.push_back(it);
            }
        } else if (p.has("decision-id") && p.has("vote-value")) {
            UniValue it = obj();
            it.pushKV("decision_id", p.get("decision-id"));
            it.pushKV("vote_value", f64v(p.get("vote-value")));
            items.push_back(it);
        } else {
            return "Error: provide --decision-id and --vote-value, or --votes";
        }
        return "Vote submitted: " + display(call(r, "vote_submit", params({items, u64v(p.get("fee-sats"))})));
    }
    if (name == "vote-list") {
        UniValue f = obj();
        f.pushKV("voter", opt_str(p, "voter"));
        f.pushKV("decision_id", opt_str(p, "decision-id"));
        f.pushKV("period_id", opt_u64(p, "period-id"));
        return simple_pretty("vote_list", params({f}));
    }
    if (name == "vote-period") return simple_pretty("vote_period", params({opt_u64(p, "period-id")}));
    if (name == "votecoin-transfer")
        return "Votecoin transferred: " + display(call(r, "transfer_votecoin", params({UniValue(p.get("dest")), f64v(p.get("amount")), u64v(p.get("fee-sats"))})));
    if (name == "votecoin-balance") return display(call(r, "votecoin_balance", params({UniValue(p.get("address"))})));
    fail_msg("unhandled command " + name);
}

bool parse_url(const std::string& url, std::string& host, uint16_t& port)
{
    std::string r = url;
    if (r.rfind("http://", 0) == 0) r = r.substr(7);
    else if (r.rfind("https://", 0) == 0) return false;
    auto slash = r.find('/');
    if (slash != std::string::npos) r = r.substr(0, slash);
    auto hp = bt::split_hostport(r, uint16_t(80));
    if (!hp) return false;
    host = hp->first;
    port = hp->second;
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string url = "http://localhost:6013";
    size_t i = 0;
    const std::string main_usage = "Usage: truthcoin-cli [OPTIONS] <COMMAND>";
    for (; i < args.size(); i++) {
        const std::string& a = args[i];
        if (a == "--rpc-url" || a == "--timeout") {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "error: a value is required for '%s' but none was supplied\n\nFor more information, try '--help'.\n",
                             a == "--rpc-url" ? "--rpc-url <RPC_URL>" : "--timeout <TIMEOUT>");
                return 2;
            }
            if (a == "--rpc-url") url = args[++i];
            else {
                std::string v = args[++i];
                if (auto e = value_error(Ty::U64, v)) {
                    std::fprintf(stderr, "error: invalid value '%s' for '--timeout <TIMEOUT>': %s\n\nFor more information, try '--help'.\n", v.c_str(), e->c_str());
                    return 2;
                }
            }
        } else if (a.rfind("--rpc-url=", 0) == 0) {
            url = a.substr(10);
        } else if (a.rfind("--timeout=", 0) == 0) {
        } else if (a == "-v" || a == "--verbose") {
        } else if (a == "-h" || a == "--help") {
            std::fputs(HELP_MAIN, stdout);
            return 0;
        } else if (a == "-V" || a == "--version") {
            std::printf("%s\n", CLI_VERSION);
            return 0;
        } else
            break;
    }
    if (args.empty()) { // arg_required_else_help
        std::fputs(HELP_MAIN, stderr);
        return 2;
    }
    if (i >= args.size()) {
        std::string list;
        for (auto& c : commands()) {
            list += (list.empty() ? "" : ", ") + c.name;
            for (auto& al : c.aliases) list += ", " + al;
        }
        list += ", help";
        std::fprintf(stderr, "error: 'truthcoin-cli' requires a subcommand but one was not provided\n  [subcommands: %s]\n\n%s\n\nFor more information, try '--help'.\n",
                     list.c_str(), main_usage.c_str());
        return 2;
    }
    std::string name = args[i++];
    if (name == "help") {
        if (i >= args.size()) {
            std::fputs(HELP_MAIN, stdout);
            return 0;
        }
        std::string sub = args[i];
        for (auto& c : commands()) {
            bool m = c.name == sub;
            for (auto& al : c.aliases) m |= al == sub;
            if (m) {
                std::fputs(help_of(c.name)->long_help, stdout);
                return 0;
            }
        }
        if (sub == "help") {
            std::fputs(help_of("help")->long_help, stdout);
            return 0;
        }
        std::fprintf(stderr, "error: unrecognized subcommand '%s'\n\n%s\n\nFor more information, try '--help'.\n", sub.c_str(), main_usage.c_str());
        return 2;
    }
    const CmdDef* cmd = nullptr;
    for (auto& c : commands()) {
        if (c.name == name) cmd = &c;
        for (auto& al : c.aliases)
            if (al == name) cmd = &c;
    }
    if (!cmd) {
        if (name.size() > 1 && name[0] == '-')
            std::fprintf(stderr, "error: unexpected argument '%s' found\n\n%s\n\nFor more information, try '--help'.\n", name.c_str(), main_usage.c_str());
        else {
            std::vector<std::pair<double, std::string>> sim;
            for (auto& c : commands()) {
                std::vector<std::string> names{c.name};
                names.insert(names.end(), c.aliases.begin(), c.aliases.end());
                for (auto& nm : names)
                    if (double conf = jaro(name, nm); conf > 0.7) sim.emplace_back(conf, nm);
            }
            if (double conf = jaro(name, "help"); conf > 0.7) sim.emplace_back(conf, "help");
            std::stable_sort(sim.begin(), sim.end(), [](auto& x, auto& y) { return x.first < y.first; });
            std::string tip;
            if (sim.size() == 1) tip = "  tip: a similar subcommand exists: '" + sim[0].second + "'\n\n";
            else if (sim.size() > 1) {
                tip = "  tip: some similar subcommands exist: ";
                for (size_t k = 0; k < sim.size(); k++) tip += (k ? ", '" : "'") + sim[k].second + "'";
                tip += "\n\n";
            }
            std::fprintf(stderr, "error: unrecognized subcommand '%s'\n\n%s%s\n\nFor more information, try '--help'.\n", name.c_str(), tip.c_str(), main_usage.c_str());
        }
        return 2;
    }
    Parsed p = parse_args(*cmd, std::vector<std::string>(args.begin() + long(i), args.end()));
    Rpc r;
    if (!parse_url(url, r.host, r.port)) {
        std::fprintf(stderr, "error: invalid value '%s' for '--rpc-url <RPC_URL>': relative URL without a base\n\nFor more information, try '--help'.\n", url.c_str());
        return 2;
    }
    std::string out = handle(r, cmd->name, p);
    std::printf("%s\n", out.c_str());
    return 0;
}
