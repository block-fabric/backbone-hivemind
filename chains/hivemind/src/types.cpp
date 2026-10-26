// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <hivemind/types.h>

#include <bt/crypto.h>

#include <cmath>
#include <cstring>
#include <deque>

namespace hivemind {

using bt::DecodeError;
using bt::utreexo::NodeHash;

namespace {
/** borsh refuses NaN (borsh 1.6 `impl BorshSerialize for f64`) (the transaction cannot be hashed). */
struct BorshNaN : DecodeError {
    BorshNaN() : DecodeError("For portability reasons we do not allow to serialize NaNs") {}
};
} // namespace

const Address Address::ALL_ZEROS{};

std::string Address::to_base58() const { return bt::base58_encode(b.span()); }

std::optional<Address> Address::from_base58(std::string_view s)
{
    auto d = bt::base58_decode(s);
    if (!d || d->size() != 20) return std::nullopt;
    Address a;
    std::memcpy(a.b.data(), d->data(), 20);
    return a;
}

std::string Address::format_for_deposit() const
{
    std::string prefix = "s" + std::to_string(THIS_SIDECHAIN) + "_" + to_base58() + "_";
    auto d = bt::sha256(ByteSpan((const uint8_t*)prefix.data(), prefix.size()));
    return prefix + bt::hex_encode(ByteSpan(d.data(), 3));
}

std::optional<Address> Address::from_deposit_address(std::string_view s, std::string* err)
{
    std::string pre = "s" + std::to_string(THIS_SIDECHAIN) + "_";
    auto fail = [&](const std::string& m) -> std::optional<Address> {
        if (err) *err = m;
        return std::nullopt;
    };
    if (s.substr(0, pre.size()) != pre) return fail("`" + std::string(s) + "` has no `s<slot>_` prefix or no checksum");
    std::string_view rest = s.substr(pre.size());
    auto us = rest.rfind('_');
    if (us == std::string_view::npos) return fail("`" + std::string(s) + "` has no `s<slot>_` prefix or no checksum");
    std::string_view a = rest.substr(0, us);
    auto dec = bt::base58_decode(a);
    if (!dec) return fail("bs58 error");
    if (dec->size() != 20) return fail("wrong address length " + std::to_string(dec->size()) + " != 20");
    Address addr;
    std::memcpy(addr.b.data(), dec->data(), 20);
    std::string f = addr.format_for_deposit();
    // eq_ignore_ascii_case
    bool eq = f.size() == s.size();
    for (size_t i = 0; eq && i < f.size(); i++)
        if (std::tolower((unsigned char)f[i]) != std::tolower((unsigned char)s[i])) eq = false;
    if (!eq) return fail("deposit address `" + std::string(s) + "` carries a wrong checksum");
    return addr;
}

Address address_from_vk(const bt::frost::Point& vk)
{
    bt::Blake3 h;
    h.update(vk.span());
    Address a;
    h.finalize_xof(a.b.data(), 20);
    return a;
}

std::string MarketId::to_hex() const { return bt::hex_encode(ByteSpan(b.data(), b.size())); }

std::string DecisionId::to_hex() const { return bt::hex_encode(ByteSpan(b.data(), b.size())); }

std::optional<DecisionId> DecisionId::make(bool is_standard, uint32_t period, uint32_t index, std::string* err)
{
    if (period > 127) {
        if (err) *err = "Period " + std::to_string(period) + " exceeds maximum 127";
        return std::nullopt;
    }
    if (index > 65535) {
        if (err) *err = "Decision index " + std::to_string(index) + " exceeds maximum 65535";
        return std::nullopt;
    }
    uint32_t c = (uint32_t(is_standard) << 23) | (period << 16) | index;
    DecisionId d;
    d.b = {uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c)};
    return d;
}

std::optional<DecisionId> DecisionId::from_hex(std::string_view s, std::string* err)
{
    if (s.size() != 6) {
        if (err) *err = "Decision ID hex must be exactly 6 characters (3 bytes)";
        return std::nullopt;
    }
    DecisionId d;
    for (int i = 0; i < 3; i++) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        // u8::from_str_radix(s, 16) also takes a leading '+'
        char c0 = s[2 * i], c1 = s[2 * i + 1];
        int v;
        if (c0 == '+') {
            v = nib(c1);
        } else {
            int h = nib(c0), l = nib(c1);
            v = (h < 0 || l < 0) ? -1 : h * 16 + l;
        }
        if (v < 0) {
            if (err) *err = "Invalid decision ID hex format";
            return std::nullopt;
        }
        d.b[i] = uint8_t(v);
    }
    return d;
}

bool DecisionType::operator==(const DecisionType& o) const
{
    if (kind != o.kind) return false;
    auto teq = [](double a, double b) {
        int64_t x, y;
        std::memcpy(&x, &a, 8);
        std::memcpy(&y, &b, 8);
        return x == y; // total_cmp equality = bit equality
    };
    if (kind == Scaled) return teq(min, o.min) && teq(max, o.max) && teq(increment, o.increment);
    if (kind == Category) return options == o.options;
    return true;
}

const char* tx_data_name(size_t i)
{
    static const char* n[] = {"ClaimDecision", "CreateMarket", "Trade", "SubmitVote", "SubmitBallot", "TransferReputation", "AmplifyBeta"};
    return i < 7 ? n[i] : "?";
}

uint64_t Content::get_value() const
{
    if (kind == Withdrawal) {
        uint64_t s = value + main_fee;
        return s < value ? UINT64_MAX : s;
    }
    return value;
}

bool Content::operator==(const Content& o) const
{
    if (kind != o.kind || value != o.value) return false;
    if (kind == Withdrawal) return main_fee == o.main_fee && main_address == o.main_address;
    if (kind == MarketFunds) return market_id == o.market_id && is_fee == o.is_fee;
    return true;
}

std::strong_ordering operator<=>(const OutPoint& a, const OutPoint& b)
{
    if (auto c = a.kind <=> b.kind; c != 0) return c;
    if (a.kind == OutPoint::MarketFunds) {
        if (auto c = a.market_id <=> b.market_id; c != 0) return c;
        if (auto c = a.block_height <=> b.block_height; c != 0) return c;
        return a.is_fee <=> b.is_fee;
    }
    if (auto c = a.txid <=> b.txid; c != 0) return c;
    return a.vout <=> b.vout;
}

std::array<uint8_t, 37> OutPoint::key() const
{
    std::array<uint8_t, 37> k{};
    Writer w;
    borsh(w, *this);
    std::memcpy(k.data(), w.buf.data(), std::min<size_t>(w.buf.size(), 37));
    return k;
}

std::string OutPoint::to_string() const
{
    switch (kind) {
    case Regular: return "regular " + bt::hex_encode(txid) + " " + std::to_string(vout);
    case Coinbase: return "coinbase " + bt::hex_encode(txid) + " " + std::to_string(vout);
    case Deposit: return "deposit " + bt::hex_encode_rev(txid.span()) + " " + std::to_string(vout);
    case MarketFunds: return std::string(is_fee ? "market_fee " : "market ") + market_id.to_hex() + " " + std::to_string(block_height);
    case Payout: return "payout " + bt::hex_encode(txid) + " " + std::to_string(vout);
    }
    return {};
}

OutPoint OutPoint::regular(const Hash32& t, uint32_t v)
{
    OutPoint o;
    o.kind = Regular;
    o.txid = t;
    o.vout = v;
    return o;
}
OutPoint OutPoint::coinbase(const Hash32& t, uint32_t v)
{
    OutPoint o;
    o.kind = Coinbase;
    o.txid = t;
    o.vout = v;
    return o;
}
OutPoint OutPoint::market_funds(const MarketId& m, uint32_t h, bool fee)
{
    OutPoint o;
    o.kind = MarketFunds;
    o.market_id = m;
    o.block_height = h;
    o.is_fee = fee;
    return o;
}
OutPoint OutPoint::payout(const Hash32& h, uint32_t v)
{
    OutPoint o;
    o.kind = Payout;
    o.txid = h;
    o.vout = v;
    return o;
}

// ---------------------------------------------------------------- borsh
void borsh_f64(Writer& w, double v)
{
    if (std::isnan(v)) throw BorshNaN();
    uint64_t u;
    std::memcpy(&u, &v, 8);
    w.u64(u);
}

static void borsh_str(Writer& w, const std::string& s) { w.borsh_bytes(ByteSpan((const uint8_t*)s.data(), s.size())); }
static void borsh_opt_str(Writer& w, const std::optional<std::string>& s)
{
    if (!s) return w.u8(0);
    w.u8(1);
    borsh_str(w, *s);
}
static void borsh_strs(Writer& w, const std::vector<std::string>& v)
{
    w.borsh_len(v.size());
    for (auto& s : v) borsh_str(w, s);
}

void borsh(Writer& w, const Address& a) { w.raw(a.b); }

void borsh(Writer& w, const Content& c)
{
    w.u8(c.kind);
    if (c.kind == Content::Value) {
        w.u64(c.value);
    } else if (c.kind == Content::Withdrawal) {
        w.u64(c.value);
        w.u64(c.main_fee);
        w.borsh_bytes(c.main_address.script_pubkey()); // util.rs: bitcoin_address -> spk bytes
    } else {
        w.raw(ByteSpan(c.market_id.b.data(), 6));
        w.u64(c.value);
        w.u8(c.is_fee ? 1 : 0);
    }
}

void borsh(Writer& w, const Output& o)
{
    borsh(w, o.address);
    borsh(w, o.content);
}

void borsh(Writer& w, const OutPoint& o)
{
    w.u8(o.kind);
    if (o.kind == OutPoint::MarketFunds) {
        w.raw(ByteSpan(o.market_id.b.data(), 6));
        w.u32(o.block_height);
        w.u8(o.is_fee ? 1 : 0);
        return;
    }
    w.raw(o.txid);
    w.u32(o.vout);
}

void borsh(Writer& w, const DecisionType& t)
{
    w.u8(t.kind);
    if (t.kind == DecisionType::Scaled) {
        borsh_f64(w, t.min);
        borsh_f64(w, t.max);
        borsh_f64(w, t.increment);
    } else if (t.kind == DecisionType::Category) {
        borsh_strs(w, t.options);
    }
}

void borsh(Writer& w, const DimensionSpec& d)
{
    w.u8(d.kind);
    w.raw(ByteSpan(d.id.b.data(), 3));
}

void borsh(Writer& w, const ClaimDecisionPayload& p)
{
    borsh(w, p.decision_type);
    w.borsh_len(p.decisions.size());
    for (auto& e : p.decisions) {
        w.raw(ByteSpan(e.decision_id_bytes.data(), 3));
        borsh_str(w, e.header);
        borsh_str(w, e.description);
        borsh_opt_str(w, e.option_0_label);
        borsh_opt_str(w, e.option_1_label);
        if (e.option_labels) {
            w.u8(1);
            borsh_strs(w, *e.option_labels);
        } else {
            w.u8(0);
        }
        if (e.tags) {
            w.u8(1);
            borsh_strs(w, *e.tags);
        } else {
            w.u8(0);
        }
    }
}

template <class T, class F>
static void borsh_opt(Writer& w, const std::optional<T>& o, F f)
{
    if (!o) return w.u8(0);
    w.u8(1);
    f(*o);
}

void borsh(Writer& w, const TxData& d)
{
    w.u8(uint8_t(d.index()));
    std::visit(
        [&](auto&& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, ClaimDecisionPayload>) {
                borsh(w, x);
            } else if constexpr (std::is_same_v<T, TxCreateMarket>) {
                borsh_str(w, x.title);
                borsh_str(w, x.description);
                w.borsh_len(x.dimension_specs.size());
                for (auto& s : x.dimension_specs) borsh(w, s);
                w.borsh_len(x.new_claims.size());
                for (auto& c : x.new_claims) borsh(w, c);
                borsh_opt(w, x.trading_fee, [&](double v) { borsh_f64(w, v); });
                borsh_opt(w, x.tx_pow_hash_selector, [&](uint8_t v) { w.u8(v); });
                borsh_opt(w, x.tx_pow_ordering, [&](uint8_t v) { w.u8(v); });
                borsh_opt(w, x.tx_pow_difficulty, [&](uint8_t v) { w.u8(v); });
            } else if constexpr (std::is_same_v<T, TxTrade>) {
                w.raw(ByteSpan(x.market_id.b.data(), 6));
                w.u32(x.outcome_index);
                w.u64(uint64_t(x.shares));
                borsh(w, x.trader);
                w.u64(x.limit_sats);
                borsh_opt(w, x.tx_pow_nonce, [&](uint64_t v) { w.u64(v); });
                w.raw(x.prev_block_hash);
            } else if constexpr (std::is_same_v<T, TxSubmitVote>) {
                borsh(w, x.voter);
                w.raw(ByteSpan(x.decision_id_bytes.data(), 3));
                borsh_f64(w, x.vote_value);
                w.u32(x.voting_period);
            } else if constexpr (std::is_same_v<T, TxSubmitBallot>) {
                borsh(w, x.voter);
                w.borsh_len(x.votes.size());
                for (auto& v : x.votes) {
                    w.raw(ByteSpan(v.decision_id_bytes.data(), 3));
                    borsh_f64(w, v.vote_value);
                }
                w.u32(x.voting_period);
            } else if constexpr (std::is_same_v<T, TxTransferReputation>) {
                borsh(w, x.sender);
                borsh(w, x.dest);
                borsh_f64(w, x.amount);
            } else if constexpr (std::is_same_v<T, TxAmplifyBeta>) {
                w.raw(ByteSpan(x.market_id.b.data(), 6));
                w.u64(x.amount);
                borsh(w, x.market_author);
            }
        },
        d);
}

void borsh(Writer& w, const Transaction& t)
{
    w.borsh_len(t.inputs.size());
    for (auto& [op, h] : t.inputs) {
        borsh(w, op);
        w.raw(h);
    }
    w.borsh_len(t.outputs.size());
    for (auto& o : t.outputs) borsh(w, o);
    if (t.data) {
        w.u8(1);
        borsh(w, *t.data);
    } else {
        w.u8(0);
    }
}

void borsh(Writer& w, const Authorization& a)
{
    w.raw(a.verifying_key);
    w.raw(a.signature.R);
    w.raw(a.signature.z);
}

void borsh(Writer& w, const Coinbase& c)
{
    w.borsh_bytes(c.memo);
    w.borsh_len(c.outputs.size());
    for (auto& o : c.outputs) borsh(w, o);
}

void borsh(Writer& w, const Body& b)
{
    borsh(w, b.coinbase);
    w.borsh_len(b.transactions.size());
    for (auto& t : b.transactions) borsh(w, t);
    w.borsh_len(b.authorizations.size());
    for (auto& a : b.authorizations) borsh(w, a);
    w.borsh_len(b.actor_proofs.size());
    for (auto& p : b.actor_proofs) {
        if (p) {
            w.u8(1);
            borsh(w, *p);
        } else {
            w.u8(0);
        }
    }
}

void borsh(Writer& w, const Header& h)
{
    w.raw(h.merkle_root);
    if (h.prev_side_hash) {
        w.u8(1);
        w.raw(*h.prev_side_hash);
    } else {
        w.u8(0);
    }
    w.raw(h.prev_main_hash);
    w.borsh_len(h.roots.size());
    for (auto& r : h.roots) w.raw(r.bytes()); // Deref: Empty/Placeholder -> zeros
}

Bytes Transaction::canonical_bytes() const { return borsh_bytes(*this); }
Hash32 Transaction::txid() const { return bt::blake3(canonical_bytes()); }

bool Transaction::has_nan() const
{
    if (!data) return false;
    bool nan = false;
    auto f = [&](double v) {
        if (std::isnan(v)) nan = true;
    };
    auto dt = [&](const DecisionType& t) {
        if (t.kind == DecisionType::Scaled) {
            f(t.min);
            f(t.max);
            f(t.increment);
        }
    };
    std::visit(
        [&](auto&& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, ClaimDecisionPayload>) {
                dt(x.decision_type);
            } else if constexpr (std::is_same_v<T, TxCreateMarket>) {
                for (auto& c : x.new_claims) dt(c.decision_type);
                if (x.trading_fee) f(*x.trading_fee);
            } else if constexpr (std::is_same_v<T, TxSubmitVote>) {
                f(x.vote_value);
            } else if constexpr (std::is_same_v<T, TxSubmitBallot>) {
                for (auto& v : x.votes) f(v.vote_value);
            } else if constexpr (std::is_same_v<T, TxTransferReputation>) {
                f(x.amount);
            }
        },
        *data);
    return nan;
}

Hash32 Header::hash() const { return hivemind::hash(*this); }
Hash32 Header::coinbase_txid() const { return compute_coinbase_txid(merkle_root, prev_main_hash, prev_side_hash); }

Hash32 compute_coinbase_txid(const Hash32& merkle_root, const Hash32& prev_main_hash, const std::optional<Hash32>& prev_side)
{
    Writer w;
    w.raw(merkle_root);
    w.raw(prev_main_hash);
    if (prev_side) {
        w.u8(1);
        w.raw(*prev_side);
    } else {
        w.u8(0);
    }
    return bt::blake3(w.buf);
}

Hash32 pointed_output_hash(const OutPoint& op, const Output& out)
{
    Writer w;
    borsh(w, op);
    borsh(w, out);
    return bt::blake3(w.buf);
}

size_t Body::inputs_len() const
{
    size_t n = 0;
    for (auto& t : transactions) n += t.inputs.size();
    return n;
}

std::vector<AuthorizedTransaction> Body::authorized_transactions() const
{
    std::vector<AuthorizedTransaction> r;
    size_t k = 0;
    for (size_t i = 0; i < transactions.size(); i++) {
        auto& t = transactions[i];
        AuthorizedTransaction at;
        at.transaction = t;
        for (size_t j = 0; j < t.inputs.size() && k < authorizations.size(); j++) at.authorizations.push_back(authorizations[k++]);
        if (i < actor_proofs.size()) at.actor_proof = actor_proofs[i];
        r.push_back(std::move(at));
    }
    return r;
}

Body Body::from_authorized(std::vector<AuthorizedTransaction> txs, Coinbase cb)
{
    Body b;
    b.coinbase = std::move(cb);
    for (auto& t : txs) {
        for (auto& a : t.authorizations) b.authorizations.push_back(a);
        b.actor_proofs.push_back(t.actor_proof);
        b.transactions.push_back(std::move(t.transaction));
    }
    return b;
}

std::optional<uint64_t> FilledTransaction::value_in() const
{
    uint64_t in = 0;
    for (auto& o : spent_utxos) {
        uint64_t v = o.get_value();
        if (in + v < in) return std::nullopt;
        in += v;
    }
    return in;
}

std::optional<uint64_t> FilledTransaction::fee() const
{
    auto in = value_in();
    if (!in) return std::nullopt;
    uint64_t out = 0;
    for (auto& o : tx->outputs) {
        uint64_t v = o.get_value();
        if (out + v < out) return std::nullopt;
        out += v;
    }
    if (*in < out) return std::nullopt; // Underfunded
    return *in - out;
}

// ---------------------------------------------------------------- bincode
void bincode_f64(Writer& w, double v)
{
    uint64_t u;
    std::memcpy(&u, &v, 8);
    w.u64(u);
}
double de_f64(Reader& r)
{
    uint64_t u = r.u64();
    double v;
    std::memcpy(&v, &u, 8);
    return v;
}
static void bincode_strs(Writer& w, const std::vector<std::string>& v)
{
    w.bincode_len(v.size());
    for (auto& s : v) w.bincode_str(s);
}
static void bincode_opt_str(Writer& w, const std::optional<std::string>& s)
{
    if (!s) return w.u8(0);
    w.u8(1);
    w.bincode_str(*s);
}

void bincode(Writer& w, const Address& a) { w.raw(a.b); }

void bincode(Writer& w, const Content& c)
{
    w.u32(c.kind); // DefaultRepr enum
    if (c.kind == Content::Value) {
        w.u64(c.value);
    } else if (c.kind == Content::Withdrawal) {
        w.u64(c.value);
        w.u64(c.main_fee);
        w.bincode_str(c.main_address.to_string()); // Address serde = collect_str(Display)
    } else {
        w.raw(ByteSpan(c.market_id.b.data(), 6));
        w.u64(c.value);
        w.u8(c.is_fee ? 1 : 0);
    }
}

void bincode(Writer& w, const Output& o)
{
    bincode(w, o.address);
    bincode(w, o.content);
}

void bincode(Writer& w, const OutPoint& o)
{
    w.u32(o.kind);
    switch (o.kind) {
    case OutPoint::Deposit:
        w.bincode_bytes(o.txid.span()); // bitcoin::Txid serde = serialize_bytes
        w.u32(o.vout);
        break;
    case OutPoint::MarketFunds:
        w.raw(ByteSpan(o.market_id.b.data(), 6));
        w.u32(o.block_height);
        w.u8(o.is_fee ? 1 : 0);
        break;
    default:
        w.raw(o.txid); // [u8; 32] as a tuple
        w.u32(o.vout);
    }
}

void bincode(Writer& w, const InPoint& o)
{
    w.u32(o.kind);
    if (o.kind == InPoint::Regular) {
        w.raw(o.id);
        w.u32(o.vin);
    } else if (o.kind == InPoint::Withdrawal) {
        w.bincode_bytes(o.id.span());
    }
}

void bincode(Writer& w, const DecisionType& t)
{
    w.u32(t.kind);
    if (t.kind == DecisionType::Scaled) {
        bincode_f64(w, t.min);
        bincode_f64(w, t.max);
        bincode_f64(w, t.increment);
    } else if (t.kind == DecisionType::Category) {
        bincode_strs(w, t.options);
    }
}

void bincode(Writer& w, const ClaimDecisionPayload& p)
{
    bincode(w, p.decision_type);
    w.bincode_len(p.decisions.size());
    for (auto& e : p.decisions) {
        w.raw(ByteSpan(e.decision_id_bytes.data(), 3));
        w.bincode_str(e.header);
        w.bincode_str(e.description);
        bincode_opt_str(w, e.option_0_label);
        bincode_opt_str(w, e.option_1_label);
        if (e.option_labels) {
            w.u8(1);
            bincode_strs(w, *e.option_labels);
        } else {
            w.u8(0);
        }
        if (e.tags) {
            w.u8(1);
            bincode_strs(w, *e.tags);
        } else {
            w.u8(0);
        }
    }
}

void bincode(Writer& w, const TxData& d)
{
    w.u32(uint32_t(d.index()));
    std::visit(
        [&](auto&& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, ClaimDecisionPayload>) {
                bincode(w, x);
            } else if constexpr (std::is_same_v<T, TxCreateMarket>) {
                w.bincode_str(x.title);
                w.bincode_str(x.description);
                w.bincode_len(x.dimension_specs.size());
                for (auto& s : x.dimension_specs) {
                    w.u32(s.kind);
                    w.raw(ByteSpan(s.id.b.data(), 3));
                }
                w.bincode_len(x.new_claims.size());
                for (auto& c : x.new_claims) bincode(w, c);
                if (x.trading_fee) {
                    w.u8(1);
                    bincode_f64(w, *x.trading_fee);
                } else {
                    w.u8(0);
                }
                for (auto* o : {&x.tx_pow_hash_selector, &x.tx_pow_ordering, &x.tx_pow_difficulty}) {
                    if (*o) {
                        w.u8(1);
                        w.u8(**o);
                    } else {
                        w.u8(0);
                    }
                }
            } else if constexpr (std::is_same_v<T, TxTrade>) {
                w.raw(ByteSpan(x.market_id.b.data(), 6));
                w.u32(x.outcome_index);
                w.u64(uint64_t(x.shares));
                bincode(w, x.trader);
                w.u64(x.limit_sats);
                if (x.tx_pow_nonce) {
                    w.u8(1);
                    w.u64(*x.tx_pow_nonce);
                } else {
                    w.u8(0);
                }
                w.raw(x.prev_block_hash);
            } else if constexpr (std::is_same_v<T, TxSubmitVote>) {
                bincode(w, x.voter);
                w.raw(ByteSpan(x.decision_id_bytes.data(), 3));
                bincode_f64(w, x.vote_value);
                w.u32(x.voting_period);
            } else if constexpr (std::is_same_v<T, TxSubmitBallot>) {
                bincode(w, x.voter);
                w.bincode_len(x.votes.size());
                for (auto& v : x.votes) {
                    w.raw(ByteSpan(v.decision_id_bytes.data(), 3));
                    bincode_f64(w, v.vote_value);
                }
                w.u32(x.voting_period);
            } else if constexpr (std::is_same_v<T, TxTransferReputation>) {
                bincode(w, x.sender);
                bincode(w, x.dest);
                bincode_f64(w, x.amount);
            } else if constexpr (std::is_same_v<T, TxAmplifyBeta>) {
                w.raw(ByteSpan(x.market_id.b.data(), 6));
                w.u64(x.amount);
                bincode(w, x.market_author);
            }
        },
        d);
}

void bincode(Writer& w, const NodeHash& h)
{
    w.u32(h.tag);
    if (h.tag == NodeHash::Some) w.raw(h.h);
}

void bincode(Writer& w, const bt::utreexo::Proof& p)
{
    w.bincode_len(p.targets.size());
    for (auto t : p.targets) w.u64(t);
    w.bincode_len(p.hashes.size());
    for (auto& h : p.hashes) bincode(w, h);
}

void bincode(Writer& w, const Transaction& t)
{
    w.bincode_len(t.inputs.size());
    for (auto& [op, h] : t.inputs) {
        bincode(w, op);
        w.raw(h);
    }
    bincode(w, t.proof);
    w.bincode_len(t.outputs.size());
    for (auto& o : t.outputs) bincode(w, o);
    if (t.data) {
        w.u8(1);
        bincode(w, *t.data);
    } else {
        w.u8(0);
    }
}

void bincode(Writer& w, const Authorization& a)
{
    w.raw(a.verifying_key); // serdect::array -> tuple, 32 bytes
    w.bincode_len(64);      // serdect::slice -> bytes with u64 length
    w.raw(a.signature.R);
    w.raw(a.signature.z);
}

void bincode(Writer& w, const AuthorizedTransaction& t)
{
    bincode(w, t.transaction);
    w.bincode_len(t.authorizations.size());
    for (auto& a : t.authorizations) bincode(w, a);
    if (t.actor_proof) {
        w.u8(1);
        bincode(w, *t.actor_proof);
    } else {
        w.u8(0);
    }
}

void bincode(Writer& w, const Coinbase& c)
{
    w.bincode_bytes(c.memo);
    w.bincode_len(c.outputs.size());
    for (auto& o : c.outputs) bincode(w, o);
}

void bincode(Writer& w, const Body& b)
{
    bincode(w, b.coinbase);
    w.bincode_len(b.transactions.size());
    for (auto& t : b.transactions) bincode(w, t);
    w.bincode_len(b.authorizations.size());
    for (auto& a : b.authorizations) bincode(w, a);
    w.bincode_len(b.actor_proofs.size());
    for (auto& p : b.actor_proofs) {
        if (p) {
            w.u8(1);
            bincode(w, *p);
        } else {
            w.u8(0);
        }
    }
}

void bincode(Writer& w, const Header& h)
{
    w.raw(h.merkle_root);
    if (h.prev_side_hash) {
        w.u8(1);
        w.raw(*h.prev_side_hash);
    } else {
        w.u8(0);
    }
    w.bincode_bytes(h.prev_main_hash.span()); // bitcoin::BlockHash serde = bytes
    w.bincode_len(h.roots.size());
    for (auto& r : h.roots) bincode(w, r);
}

void bincode(Writer& w, const Block& b)
{
    bincode(w, b.header);
    bincode(w, b.body);
}

static uint8_t opt_tag(Reader& r)
{
    uint8_t t = r.u8();
    if (t > 1) throw DecodeError("invalid value: integer `" + std::to_string(t) + "`, expected an option");
    return t;
}

static std::vector<std::string> de_strs(Reader& r)
{
    std::vector<std::string> v;
    size_t n = r.bincode_len(8);
    for (size_t i = 0; i < n; i++) v.push_back(r.bincode_str());
    return v;
}
static std::optional<std::string> de_opt_str(Reader& r)
{
    if (!opt_tag(r)) return std::nullopt;
    return r.bincode_str();
}
static std::array<uint8_t, 3> de_arr3(Reader& r)
{
    auto a = r.array<3>();
    return {a[0], a[1], a[2]};
}
static MarketId de_market_id(Reader& r)
{
    auto a = r.array<6>();
    MarketId m;
    std::memcpy(m.b.data(), a.data(), 6);
    return m;
}

Address de_address(Reader& r)
{
    Address a;
    a.b = r.array<20>();
    return a;
}

Content de_content(Reader& r)
{
    Content c;
    uint32_t tag = r.u32();
    if (tag == 0) {
        c.kind = Content::Value;
        c.value = r.u64();
    } else if (tag == 1) {
        c.kind = Content::Withdrawal;
        c.value = r.u64();
        c.main_fee = r.u64();
        std::string s = r.bincode_str();
        auto a = bt::btc::Address::parse(s);
        if (!a) throw DecodeError("invalid main_address");
        c.main_address = *a;
    } else if (tag == 2) {
        c.kind = Content::MarketFunds;
        c.market_id = de_market_id(r);
        c.value = r.u64();
        c.is_fee = r.boolean();
    } else {
        throw DecodeError("invalid Content tag");
    }
    return c;
}

Output de_output(Reader& r)
{
    Output o;
    o.address = de_address(r);
    o.content = de_content(r);
    return o;
}

OutPoint de_outpoint(Reader& r)
{
    OutPoint o;
    uint32_t tag = r.u32();
    if (tag > 4) throw DecodeError("invalid OutPoint tag");
    o.kind = OutPoint::Kind(tag);
    if (o.kind == OutPoint::Deposit) {
        Bytes b = r.bincode_bytes();
        if (b.size() != 32) throw DecodeError("invalid txid length");
        o.txid = Hash32::from_span(b);
        o.vout = r.u32();
    } else if (o.kind == OutPoint::MarketFunds) {
        o.market_id = de_market_id(r);
        o.block_height = r.u32();
        o.is_fee = r.boolean();
    } else {
        o.txid = r.array<32>();
        o.vout = r.u32();
    }
    return o;
}

InPoint de_inpoint(Reader& r)
{
    InPoint p;
    uint32_t tag = r.u32();
    if (tag > 2) throw DecodeError("invalid InPoint tag");
    p.kind = InPoint::Kind(tag);
    if (p.kind == InPoint::Regular) {
        p.id = r.array<32>();
        p.vin = r.u32();
    } else if (p.kind == InPoint::Withdrawal) {
        Bytes b = r.bincode_bytes();
        if (b.size() != 32) throw DecodeError("invalid m6id length");
        p.id = Hash32::from_span(b);
    }
    return p;
}

DecisionType de_decision_type(Reader& r)
{
    DecisionType t;
    uint32_t tag = r.u32();
    if (tag > 2) throw DecodeError("invalid DecisionType tag");
    t.kind = DecisionType::Kind(tag);
    if (t.kind == DecisionType::Scaled) {
        t.min = de_f64(r);
        t.max = de_f64(r);
        t.increment = de_f64(r);
    } else if (t.kind == DecisionType::Category) {
        t.options = de_strs(r);
    }
    return t;
}

ClaimDecisionPayload de_claim_payload(Reader& r)
{
    ClaimDecisionPayload p;
    p.decision_type = de_decision_type(r);
    size_t n = r.bincode_len(3 + 16 + 4);
    for (size_t i = 0; i < n; i++) {
        DecisionClaimEntry e;
        e.decision_id_bytes = de_arr3(r);
        e.header = r.bincode_str();
        e.description = r.bincode_str();
        e.option_0_label = de_opt_str(r);
        e.option_1_label = de_opt_str(r);
        if (opt_tag(r)) e.option_labels = de_strs(r);
        if (opt_tag(r)) e.tags = de_strs(r);
        p.decisions.push_back(std::move(e));
    }
    return p;
}

TxData de_tx_data(Reader& r)
{
    uint32_t tag = r.u32();
    switch (tag) {
    case CLAIM_DECISION: return de_claim_payload(r);
    case CREATE_MARKET: {
        TxCreateMarket m;
        m.title = r.bincode_str();
        m.description = r.bincode_str();
        size_t n = r.bincode_len(7);
        for (size_t i = 0; i < n; i++) {
            DimensionSpec s;
            uint32_t k = r.u32();
            if (k > 1) throw DecodeError("invalid DimensionSpec tag");
            s.kind = DimensionSpec::Kind(k);
            s.id.b = de_arr3(r);
            m.dimension_specs.push_back(s);
        }
        size_t c = r.bincode_len(12);
        for (size_t i = 0; i < c; i++) m.new_claims.push_back(de_claim_payload(r));
        if (opt_tag(r)) m.trading_fee = de_f64(r);
        if (opt_tag(r)) m.tx_pow_hash_selector = r.u8();
        if (opt_tag(r)) m.tx_pow_ordering = r.u8();
        if (opt_tag(r)) m.tx_pow_difficulty = r.u8();
        return m;
    }
    case TRADE: {
        TxTrade t;
        t.market_id = de_market_id(r);
        t.outcome_index = r.u32();
        t.shares = int64_t(r.u64());
        t.trader = de_address(r);
        t.limit_sats = r.u64();
        if (opt_tag(r)) t.tx_pow_nonce = r.u64();
        t.prev_block_hash = r.array<32>();
        return t;
    }
    case SUBMIT_VOTE: {
        TxSubmitVote v;
        v.voter = de_address(r);
        v.decision_id_bytes = de_arr3(r);
        v.vote_value = de_f64(r);
        v.voting_period = r.u32();
        return v;
    }
    case SUBMIT_BALLOT: {
        TxSubmitBallot b;
        b.voter = de_address(r);
        size_t n = r.bincode_len(11);
        for (size_t i = 0; i < n; i++) {
            BallotItem it;
            it.decision_id_bytes = de_arr3(r);
            it.vote_value = de_f64(r);
            b.votes.push_back(it);
        }
        b.voting_period = r.u32();
        return b;
    }
    case TRANSFER_REPUTATION: {
        TxTransferReputation t;
        t.sender = de_address(r);
        t.dest = de_address(r);
        t.amount = de_f64(r);
        return t;
    }
    case AMPLIFY_BETA: {
        TxAmplifyBeta a;
        a.market_id = de_market_id(r);
        a.amount = r.u64();
        a.market_author = de_address(r);
        return a;
    }
    }
    throw DecodeError("invalid TransactionData tag");
}

NodeHash de_nodehash(Reader& r)
{
    uint32_t tag = r.u32();
    if (tag == 0) return NodeHash::empty();
    if (tag == 1) return NodeHash{NodeHash::Placeholder, {}};
    if (tag == 2) return NodeHash::some(r.array<32>());
    throw DecodeError("invalid NodeHash tag");
}

bt::utreexo::Proof de_proof(Reader& r)
{
    bt::utreexo::Proof p;
    size_t n = r.bincode_len(8);
    p.targets.reserve(n);
    for (size_t i = 0; i < n; i++) p.targets.push_back(r.u64());
    size_t m = r.bincode_len(4);
    for (size_t i = 0; i < m; i++) p.hashes.push_back(de_nodehash(r));
    return p;
}

Transaction de_transaction(Reader& r)
{
    Transaction t;
    size_t n = r.bincode_len(4 + 4 + 32);
    for (size_t i = 0; i < n; i++) {
        OutPoint op = de_outpoint(r);
        Hash32 h = r.array<32>();
        t.inputs.emplace_back(op, h);
    }
    t.proof = de_proof(r);
    size_t m = r.bincode_len(20 + 4 + 8);
    for (size_t i = 0; i < m; i++) t.outputs.push_back(de_output(r));
    if (opt_tag(r)) t.data = de_tx_data(r);
    return t;
}

Authorization de_authorization(Reader& r)
{
    Authorization a;
    a.verifying_key = r.array<32>();
    if (!bt::frost::valid_element(a.verifying_key)) throw DecodeError("invalid verifying key");
    Bytes sig = r.bincode_bytes();
    if (sig.size() != 64) throw DecodeError("MalformedSignature");
    std::memcpy(a.signature.R.data(), sig.data(), 32);
    std::memcpy(a.signature.z.data(), sig.data() + 32, 32);
    if (!bt::frost::valid_element(a.signature.R) || !bt::frost::canonical_scalar(a.signature.z))
        throw DecodeError("invalid signature encoding");
    return a;
}

AuthorizedTransaction de_authorized_transaction(Reader& r)
{
    AuthorizedTransaction t;
    t.transaction = de_transaction(r);
    size_t n = r.bincode_len(32 + 8 + 64);
    for (size_t i = 0; i < n; i++) t.authorizations.push_back(de_authorization(r));
    if (opt_tag(r)) t.actor_proof = de_authorization(r);
    return t;
}

Coinbase de_coinbase(Reader& r)
{
    Coinbase c;
    c.memo = r.bincode_bytes();
    size_t n = r.bincode_len(20 + 4 + 8);
    for (size_t i = 0; i < n; i++) c.outputs.push_back(de_output(r));
    return c;
}

Body de_body(Reader& r)
{
    Body b;
    b.coinbase = de_coinbase(r);
    size_t n = r.bincode_len(8 + 16 + 8 + 1);
    for (size_t i = 0; i < n; i++) b.transactions.push_back(de_transaction(r));
    size_t m = r.bincode_len(32 + 8 + 64);
    for (size_t i = 0; i < m; i++) b.authorizations.push_back(de_authorization(r));
    size_t k = r.bincode_len(1);
    for (size_t i = 0; i < k; i++) {
        if (opt_tag(r)) b.actor_proofs.push_back(de_authorization(r));
        else b.actor_proofs.push_back(std::nullopt);
    }
    return b;
}

Header de_header(Reader& r)
{
    Header h;
    h.merkle_root = r.array<32>();
    uint8_t opt = r.u8();
    if (opt == 1) h.prev_side_hash = r.array<32>();
    else if (opt != 0) throw DecodeError("invalid Option tag");
    Bytes pm = r.bincode_bytes();
    if (pm.size() != 32) throw DecodeError("invalid block hash length");
    h.prev_main_hash = Hash32::from_span(pm);
    size_t n = r.bincode_len(4);
    for (size_t i = 0; i < n; i++) h.roots.push_back(de_nodehash(r));
    return h;
}

// ---------------------------------------------------------------- borsh decode
static uint8_t borsh_opt_tag(Reader& r)
{
    uint8_t t = r.u8();
    if (t > 1) throw DecodeError("Invalid Option representation: " + std::to_string(t) + ". The first byte must be 0 or 1");
    return t;
}
static std::string borsh_de_str(Reader& r)
{
    Bytes b = r.borsh_bytes();
    std::string s(b.begin(), b.end());
    if (!Reader::utf8_valid(s)) throw DecodeError("invalid utf-8 sequence");
    return s;
}
static std::vector<std::string> borsh_de_strs(Reader& r)
{
    std::vector<std::string> v;
    size_t n = r.borsh_len(4);
    for (size_t i = 0; i < n; i++) v.push_back(borsh_de_str(r));
    return v;
}
static double borsh_de_f64(Reader& r)
{
    double v = de_f64(r);
    if (std::isnan(v)) throw DecodeError("For portability reasons we do not allow to deserialize NaNs");
    return v;
}
static ClaimDecisionPayload borsh_de_claim(Reader& r)
{
    ClaimDecisionPayload p;
    uint8_t k = r.u8();
    if (k > 2) throw DecodeError("Unexpected variant index: " + std::to_string(k));
    p.decision_type.kind = DecisionType::Kind(k);
    if (k == DecisionType::Scaled) {
        p.decision_type.min = borsh_de_f64(r);
        p.decision_type.max = borsh_de_f64(r);
        p.decision_type.increment = borsh_de_f64(r);
    } else if (k == DecisionType::Category) {
        p.decision_type.options = borsh_de_strs(r);
    }
    size_t n = r.borsh_len(3 + 8 + 4);
    for (size_t i = 0; i < n; i++) {
        DecisionClaimEntry e;
        e.decision_id_bytes = de_arr3(r);
        e.header = borsh_de_str(r);
        e.description = borsh_de_str(r);
        if (borsh_opt_tag(r)) e.option_0_label = borsh_de_str(r);
        if (borsh_opt_tag(r)) e.option_1_label = borsh_de_str(r);
        if (borsh_opt_tag(r)) e.option_labels = borsh_de_strs(r);
        if (borsh_opt_tag(r)) e.tags = borsh_de_strs(r);
        p.decisions.push_back(std::move(e));
    }
    return p;
}

TxData borsh_de_tx_data(Reader& r)
{
    uint8_t tag = r.u8();
    switch (tag) {
    case CLAIM_DECISION: return borsh_de_claim(r);
    case CREATE_MARKET: {
        TxCreateMarket m;
        m.title = borsh_de_str(r);
        m.description = borsh_de_str(r);
        size_t n = r.borsh_len(4);
        for (size_t i = 0; i < n; i++) {
            DimensionSpec s;
            uint8_t k = r.u8();
            if (k > 1) throw DecodeError("Unexpected variant index: " + std::to_string(k));
            s.kind = DimensionSpec::Kind(k);
            s.id.b = de_arr3(r);
            m.dimension_specs.push_back(s);
        }
        size_t c = r.borsh_len(5);
        for (size_t i = 0; i < c; i++) m.new_claims.push_back(borsh_de_claim(r));
        if (borsh_opt_tag(r)) m.trading_fee = borsh_de_f64(r);
        if (borsh_opt_tag(r)) m.tx_pow_hash_selector = r.u8();
        if (borsh_opt_tag(r)) m.tx_pow_ordering = r.u8();
        if (borsh_opt_tag(r)) m.tx_pow_difficulty = r.u8();
        return m;
    }
    case TRADE: {
        TxTrade t;
        t.market_id = de_market_id(r);
        t.outcome_index = r.u32();
        t.shares = int64_t(r.u64());
        t.trader = de_address(r);
        t.limit_sats = r.u64();
        if (borsh_opt_tag(r)) t.tx_pow_nonce = r.u64();
        t.prev_block_hash = r.array<32>();
        return t;
    }
    case SUBMIT_VOTE: {
        TxSubmitVote v;
        v.voter = de_address(r);
        v.decision_id_bytes = de_arr3(r);
        v.vote_value = borsh_de_f64(r);
        v.voting_period = r.u32();
        return v;
    }
    case SUBMIT_BALLOT: {
        TxSubmitBallot b;
        b.voter = de_address(r);
        size_t n = r.borsh_len(11);
        for (size_t i = 0; i < n; i++) {
            BallotItem it;
            it.decision_id_bytes = de_arr3(r);
            it.vote_value = borsh_de_f64(r);
            b.votes.push_back(it);
        }
        b.voting_period = r.u32();
        return b;
    }
    case TRANSFER_REPUTATION: {
        TxTransferReputation t;
        t.sender = de_address(r);
        t.dest = de_address(r);
        t.amount = borsh_de_f64(r);
        return t;
    }
    case AMPLIFY_BETA: {
        TxAmplifyBeta a;
        a.market_id = de_market_id(r);
        a.amount = r.u64();
        a.market_author = de_address(r);
        return a;
    }
    }
    throw DecodeError("Unexpected variant index: " + std::to_string(tag));
}

OutPoint borsh_de_outpoint(Reader& r)
{
    OutPoint o;
    uint8_t tag = r.u8();
    if (tag > 4) throw DecodeError("Unexpected variant index: " + std::to_string(tag));
    o.kind = OutPoint::Kind(tag);
    if (o.kind == OutPoint::MarketFunds) {
        o.market_id = de_market_id(r);
        o.block_height = r.u32();
        o.is_fee = r.boolean();
        return o;
    }
    o.txid = r.array<32>();
    o.vout = r.u32();
    return o;
}

Output borsh_de_output(Reader& r)
{
    Output o;
    o.address.b = r.array<20>();
    uint8_t tag = r.u8();
    if (tag == 0) {
        o.content.kind = Content::Value;
        o.content.value = r.u64();
    } else if (tag == 1) {
        o.content.kind = Content::Withdrawal;
        o.content.value = r.u64();
        o.content.main_fee = r.u64();
        Bytes spk = r.borsh_bytes();
        // borsh carries the scriptPubKey; recover an address of the same script (network
        // information is not in borsh, mainnet forms are used).
        bt::btc::Address a;
        if (spk.size() == 25 && spk[0] == 0x76 && spk[1] == 0xa9 && spk[2] == 0x14 && spk[23] == 0x88 && spk[24] == 0xac) {
            a.kind = bt::btc::Address::Kind::P2pkh;
            a.program.assign(spk.begin() + 3, spk.begin() + 23);
        } else if (spk.size() == 23 && spk[0] == 0xa9 && spk[1] == 0x14 && spk[22] == 0x87) {
            a.kind = bt::btc::Address::Kind::P2sh;
            a.program.assign(spk.begin() + 2, spk.begin() + 22);
        } else if (spk.size() >= 4 && (spk[0] == 0 || (spk[0] >= 0x51 && spk[0] <= 0x60)) && spk[1] + 2u == spk.size()) {
            a.kind = bt::btc::Address::Kind::Segwit;
            a.witver = spk[0] == 0 ? 0 : uint8_t(spk[0] - 0x50);
            a.program.assign(spk.begin() + 2, spk.end());
        } else {
            throw DecodeError("withdrawal script is not an address");
        }
        o.content.main_address = a;
    } else if (tag == 2) {
        o.content.kind = Content::MarketFunds;
        o.content.market_id = de_market_id(r);
        o.content.value = r.u64();
        o.content.is_fee = r.boolean();
    } else {
        throw DecodeError("Unexpected variant index: " + std::to_string(tag));
    }
    return o;
}

Transaction borsh_de_transaction(Reader& r)
{
    Transaction t;
    size_t n = r.borsh_len(37 + 32);
    for (size_t i = 0; i < n; i++) {
        OutPoint op = borsh_de_outpoint(r);
        Hash32 h = r.array<32>();
        t.inputs.emplace_back(op, h);
    }
    size_t m = r.borsh_len(29);
    for (size_t i = 0; i < m; i++) t.outputs.push_back(borsh_de_output(r));
    if (borsh_opt_tag(r)) t.data = borsh_de_tx_data(r);
    return t;
}

// ---------------------------------------------------------------- merkle (CKB CBMT, merkle-cbt 0.3.2)
namespace {
// merkle_cbt::CBMT::build_merkle_root, generic over a merge that may fail.
template <class T, class Merge>
std::optional<T> cbmt_root(const std::vector<T>& leaves, Merge merge, const T& dflt)
{
    if (leaves.empty()) return dflt;
    std::deque<T> q;
    size_t n = leaves.size();
    // rchunks_exact(2): pairs from the end
    size_t i = n;
    while (i >= 2) {
        auto m = merge(leaves[i - 2], leaves[i - 1]);
        if (!m) return std::nullopt;
        q.push_back(*m);
        i -= 2;
    }
    if (i == 1) q.push_front(leaves[0]);
    while (q.size() > 1) {
        T right = q.front();
        q.pop_front();
        T left = q.front();
        q.pop_front();
        auto m = merge(left, right);
        if (!m) return std::nullopt;
        q.push_back(*m);
    }
    return q.front();
}

struct ValueNode {
    Hash32 commitment;
    uint64_t value = 0;
    uint64_t size = 0;
};
} // namespace

Hash32 inputs_merkle_root(const std::vector<std::pair<OutPoint, Hash32>>& ins)
{
    std::vector<Hash32> leaves;
    for (auto& [op, h] : ins) {
        Writer w;
        borsh(w, op);
        w.raw(h);
        leaves.push_back(bt::blake3(w.buf));
    }
    auto r = cbmt_root<Hash32>(
        leaves,
        [](const Hash32& l, const Hash32& r) -> std::optional<Hash32> {
            Writer w;
            w.raw(l);
            w.raw(r);
            return bt::blake3(w.buf);
        },
        Hash32{});
    return *r;
}

std::optional<Hash32> outputs_merkle_root(const std::vector<Output>& outs)
{
    std::vector<ValueNode> leaves;
    for (auto& o : outs) {
        ValueNode n;
        n.value = o.get_value();
        n.size = borsh_bytes(o).size();
        Writer w;
        w.u64(n.value);
        w.u64(n.size);
        borsh(w, o);
        n.commitment = bt::blake3(w.buf);
        leaves.push_back(n);
    }
    auto r = cbmt_root<ValueNode>(
        leaves,
        [](const ValueNode& l, const ValueNode& r) -> std::optional<ValueNode> {
            ValueNode n;
            n.value = l.value + r.value;
            if (n.value < l.value) return std::nullopt; // AmountOverflow
            n.size = l.size + r.size;
            if (n.size < l.size) return std::nullopt; // SizeOverflow
            Writer w;
            w.raw(l.commitment);
            w.u64(n.value);
            w.u64(n.size);
            w.raw(r.commitment);
            n.commitment = bt::blake3(w.buf);
            return n;
        },
        ValueNode{});
    if (!r) return std::nullopt;
    return r->commitment;
}

std::optional<Hash32> coinbase_merkle_root(const Coinbase& cb)
{
    auto oc = outputs_merkle_root(cb.outputs);
    if (!oc) return std::nullopt;
    Writer w;
    w.borsh_bytes(cb.memo);
    w.raw(*oc);
    return bt::blake3(w.buf);
}

std::optional<Hash32> tx_merkle_root(const Transaction& t)
{
    auto outs = outputs_merkle_root(t.outputs);
    if (!outs) return std::nullopt;
    Writer data;
    if (t.data) {
        data.u8(1);
        borsh(data, *t.data);
    } else {
        data.u8(0);
    }
    Writer tm;
    tm.raw(inputs_merkle_root(t.inputs));
    tm.raw(*outs);
    tm.raw(bt::blake3(data.buf));
    return bt::blake3(tm.buf);
}

std::optional<Hash32> compute_merkle_root(const Coinbase& cb, const std::vector<FilledTransaction>& txs)
{
    std::vector<ValueNode> leaves;
    for (auto& ft : txs) {
        auto fee = ft.fee();
        if (!fee) return std::nullopt;
        uint64_t size = ft.tx->canonical_size();
        auto tmr = tx_merkle_root(*ft.tx);
        if (!tmr) return std::nullopt;
        Writer w;
        w.u64(*fee);
        w.u64(size);
        w.raw(*tmr);
        leaves.push_back(ValueNode{bt::blake3(w.buf), *fee, size});
    }
    auto r = cbmt_root<ValueNode>(
        leaves,
        [](const ValueNode& l, const ValueNode& r) -> std::optional<ValueNode> {
            ValueNode n;
            n.value = l.value + r.value;
            if (n.value < l.value) return std::nullopt; // Amount `+` panics in their node
            n.size = l.size + r.size;                  // plain u64 `+`: wraps in release
            Writer w;
            w.raw(l.commitment);
            w.u64(n.value);
            w.u64(n.size);
            w.raw(r.commitment);
            n.commitment = bt::blake3(w.buf);
            return n;
        },
        ValueNode{});
    if (!r) return std::nullopt;
    auto cbr = coinbase_merkle_root(cb);
    if (!cbr) return std::nullopt;
    Writer w;
    w.raw(*cbr);
    w.raw(r->commitment);
    return bt::blake3(w.buf);
}

// ---------------------------------------------------------------- limits
double powi(double a, int b)
{
    const bool recip = b < 0;
    double r = 1;
    while (true) {
        if (b & 1) r *= a;
        b /= 2;
        if (b == 0) break;
        a *= a;
    }
    return recip ? 1 / r : r;
}

static uint64_t growing_limit(uint64_t start, uint32_t height)
{
    uint32_t month = height / (6 * 24 * 30);
    if (month < 120) {
        double v = double(start) * powi(1.04, int(month)); // START as f64 * 1.04f64.powi(month)
        return uint64_t(std::floor(v));
    }
    return start * 111;
}

uint64_t body_sigops_limit(uint32_t height) { return growing_limit(42800, height); }
uint64_t body_size_limit(uint32_t height) { return growing_limit(8ull * 1024 * 1024, height); }

// ---------------------------------------------------------------- authorizations
Bytes tx_msg_canonical(const Transaction& tx)
{
    Bytes m;
    m.push_back(uint8_t(Dst::Transaction));
    append(m, tx.canonical_bytes());
    return m;
}

bool verify_dst(const bt::frost::Signature& sig, const bt::frost::Point& vk, Dst dst, ByteSpan msg)
{
    Bytes m;
    m.push_back(uint8_t(dst));
    append(m, msg);
    return bt::frost::verify(vk, sig, m);
}

static bool verify_actor_proof(const Transaction& tx, const std::optional<Authorization>& proof)
{
    if (!proof) return true;
    return bt::frost::verify(proof->verifying_key, proof->signature, tx_msg_canonical(tx));
}

bool verify_body(const Body& body)
{
    size_t required = body.inputs_len();
    if (body.authorizations.size() != required) return false;
    size_t k = 0;
    for (auto& tx : body.transactions) {
        if (tx.inputs.empty()) continue;
        Bytes msg = tx_msg_canonical(tx);
        for (size_t i = 0; i < tx.inputs.size(); i++, k++) {
            auto& a = body.authorizations[k];
            if (!bt::frost::verify(a.verifying_key, a.signature, msg)) return false;
        }
    }
    for (size_t i = 0; i < body.transactions.size(); i++) {
        std::optional<Authorization> p;
        if (i < body.actor_proofs.size()) p = body.actor_proofs[i];
        if (!verify_actor_proof(body.transactions[i], p)) return false;
    }
    return true;
}

bool verify_transaction(const AuthorizedTransaction& tx)
{
    if (tx.authorizations.size() != tx.transaction.inputs.size()) return false;
    if (tx.authorizations.empty()) return false; // frost-core batch::Verifier::verify: n == 0 -> InvalidSignature
    Bytes msg = tx_msg_canonical(tx.transaction);
    for (auto& a : tx.authorizations)
        if (!bt::frost::verify(a.verifying_key, a.signature, msg)) return false;
    return verify_actor_proof(tx.transaction, tx.actor_proof);
}

Bytes serialize_trade_for_pow(const MarketId& m, uint32_t outcome_index, int64_t shares, const Address& trader, uint64_t limit_sats,
                              const Hash32& prev_block_hash)
{
    Writer w;
    w.raw(ByteSpan(m.b.data(), 6));
    w.u32(outcome_index);
    w.u64(uint64_t(shares));
    w.raw(trader.b);
    w.u64(limit_sats);
    w.raw(prev_block_hash);
    return w.buf;
}

} // namespace hivemind
