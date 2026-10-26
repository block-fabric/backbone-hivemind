// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// JSON -> Truthcoin types: the inverse of json.cpp (serde_json human-readable forms), for the
// RPC parameters (submit_transaction, connect_block, sign_transaction, ...). A value that
// does not deserialize throws ParseError, which the RPC layer answers as -32602.
#include <hivemind/json.h>

#include <bt/bech32.h>
#include <bt/parse.h>

#include <cstring>

#include <cmath>

namespace hivemind::json {

[[noreturn]] static void fail(const std::string& m) { throw ParseError(m); }

static const UniValue& field(const UniValue& v, const char* k)
{
    if (!v.isObject()) fail(std::string("invalid type: expected struct with field `") + k + "`");
    const UniValue& f = v.find_value(k);
    if (f.isNull() && !v.exists(k)) fail(std::string("missing field `") + k + "`");
    return f;
}

uint64_t parse_u64(const UniValue& v)
{
    if (!v.isNum()) fail("invalid type: expected u64");
    const std::string& s = v.getValStr();
    if (s.empty() || s[0] == '-' || s.find_first_not_of("0123456789") != std::string::npos) fail("invalid value: expected u64");
    errno = 0;
    char* e = nullptr;
    unsigned long long x = std::strtoull(s.c_str(), &e, 10);
    if (errno || *e) fail("invalid value: integer out of range for u64");
    return x;
}
uint32_t parse_u32(const UniValue& v)
{
    uint64_t x = parse_u64(v);
    if (x > UINT32_MAX) fail("invalid value: integer `" + std::to_string(x) + "`, expected u32");
    return uint32_t(x);
}
static uint8_t parse_u8(const UniValue& v)
{
    uint64_t x = parse_u64(v);
    if (x > 255) fail("invalid value: integer `" + std::to_string(x) + "`, expected u8");
    return uint8_t(x);
}
int64_t parse_i64(const UniValue& v)
{
    if (!v.isNum()) fail("invalid type: expected i64");
    const std::string& s = v.getValStr();
    errno = 0;
    char* e = nullptr;
    long long x = std::strtoll(s.c_str(), &e, 10);
    if (errno || *e || s.empty()) fail("invalid value: expected i64");
    return x;
}
double parse_f64(const UniValue& v)
{
    if (!v.isNum()) fail("invalid type: expected f64");
    return std::strtod(v.getValStr().c_str(), nullptr);
}
bool parse_bool(const UniValue& v)
{
    if (!v.isBool()) fail("invalid type: expected a boolean");
    return v.get_bool();
}
std::string parse_str(const UniValue& v)
{
    if (!v.isStr()) fail("invalid type: expected a string");
    return v.get_str();
}

Hash32 parse_hash(const UniValue& v)
{
    auto s = parse_str(v);
    auto h = bt::hex_to_array<32>(s);
    if (!h) fail("invalid value: string \"" + s + "\", expected a 32-byte hex string");
    return *h;
}
Hash32 parse_btc_hash(const UniValue& v)
{
    auto s = parse_str(v);
    auto h = bt::hash_from_rev_hex(s);
    if (!h) fail("invalid value: string \"" + s + "\", expected a bitcoin hash");
    return *h;
}
Address parse_address(const UniValue& v)
{
    auto s = parse_str(v);
    auto a = Address::from_base58(s);
    if (!a) fail("invalid value: string \"" + s + "\", expected an address");
    return *a;
}
static std::vector<uint8_t> parse_byte_array(const UniValue& v, size_t n)
{
    if (!v.isArray()) fail("invalid type: expected an array of " + std::to_string(n) + " bytes");
    if (v.size() != n) fail("invalid length " + std::to_string(v.size()) + ", expected an array of length " + std::to_string(n));
    std::vector<uint8_t> r;
    for (auto& x : v.getValues()) r.push_back(parse_u8(x));
    return r;
}
static Bytes parse_byte_vec(const UniValue& v)
{
    if (!v.isArray()) fail("invalid type: expected a sequence");
    Bytes r;
    for (auto& x : v.getValues()) r.push_back(parse_u8(x));
    return r;
}
MarketId parse_market_id(const UniValue& v)
{
    auto b = parse_byte_array(v, 6);
    MarketId m;
    std::memcpy(m.b.data(), b.data(), b.size());
    return m;
}
static std::array<uint8_t, 3> parse_did_bytes(const UniValue& v)
{
    auto b = parse_byte_array(v, 3);
    return {b[0], b[1], b[2]};
}
static std::optional<std::string> parse_opt_str(const UniValue& v)
{
    if (v.isNull()) return std::nullopt;
    return parse_str(v);
}
static std::vector<std::string> parse_strs(const UniValue& v)
{
    if (!v.isArray()) fail("invalid type: expected a sequence");
    std::vector<std::string> r;
    for (auto& x : v.getValues()) r.push_back(parse_str(x));
    return r;
}

// externally tagged enum: {"Variant": value} or "Variant" for a unit variant
static std::pair<std::string, const UniValue*> variant(const UniValue& v)
{
    if (v.isStr()) return {v.get_str(), nullptr};
    if (!v.isObject() || v.size() != 1) fail("invalid type: expected an enum");
    return {v.getKeys()[0], &v.getValues()[0]};
}

OutPoint parse_outpoint(const UniValue& v)
{
    auto [k, x] = variant(v);
    if (!x) fail("unknown variant `" + k + "`");
    OutPoint o;
    if (k == "Regular" || k == "Coinbase") {
        o.kind = k == "Regular" ? OutPoint::Regular : OutPoint::Coinbase;
        o.txid = parse_hash(field(*x, "txid"));
        o.vout = parse_u32(field(*x, "vout"));
    } else if (k == "Deposit") {
        auto s = parse_str(*x);
        auto c = s.rfind(':');
        if (c == std::string::npos) fail("invalid value: expected txid:vout");
        auto t = bt::hash_from_rev_hex(s.substr(0, c));
        auto n = bt::parse_u64(s.substr(c + 1));
        if (!t || !n || *n > UINT32_MAX) fail("invalid value: expected txid:vout");
        o.kind = OutPoint::Deposit;
        o.txid = *t;
        o.vout = uint32_t(*n);
    } else if (k == "MarketFunds") {
        o.kind = OutPoint::MarketFunds;
        o.market_id = parse_market_id(field(*x, "market_id"));
        o.block_height = parse_u32(field(*x, "block_height"));
        o.is_fee = parse_bool(field(*x, "is_fee"));
    } else if (k == "Payout") {
        o.kind = OutPoint::Payout;
        o.txid = parse_hash(field(*x, "hash"));
        o.vout = parse_u32(field(*x, "vout"));
    } else {
        fail("unknown variant `" + k + "`, expected one of `Regular`, `Coinbase`, `Deposit`, `MarketFunds`, `Payout`");
    }
    return o;
}

Content parse_content(const UniValue& v)
{
    auto [k, x] = variant(v);
    if (!x) fail("unknown variant `" + k + "`");
    Content c;
    if (k == "Value") {
        c.kind = Content::Value;
        c.value = parse_u64(*x);
    } else if (k == "Withdrawal") {
        c.kind = Content::Withdrawal;
        c.value = parse_u64(field(*x, "value_sats"));
        c.main_fee = parse_u64(field(*x, "main_fee_sats"));
        auto a = bt::btc::Address::parse(parse_str(field(*x, "main_address")));
        if (!a) fail("invalid value: expected a bitcoin address");
        c.main_address = *a;
    } else if (k == "MarketFunds") {
        c.kind = Content::MarketFunds;
        c.market_id = parse_market_id(field(*x, "market_id"));
        c.value = parse_u64(field(*x, "amount"));
        c.is_fee = parse_bool(field(*x, "is_fee"));
    } else {
        fail("unknown variant `" + k + "`, expected one of `Value`, `Withdrawal`, `MarketFunds`");
    }
    return c;
}

Output parse_output(const UniValue& v)
{
    Output o;
    o.address = parse_address(field(v, "address"));
    o.content = parse_content(field(v, "content"));
    return o;
}

static bt::utreexo::NodeHash parse_node_hash(const UniValue& v)
{
    auto [k, x] = variant(v);
    if (k == "Empty") return bt::utreexo::NodeHash::empty();
    if (k == "Placeholder") return bt::utreexo::NodeHash{bt::utreexo::NodeHash::Placeholder, {}};
    if (k == "Some" && x) {
        auto b = parse_byte_array(*x, 32);
        Hash32 h;
        std::memcpy(h.data(), b.data(), b.size());
        return bt::utreexo::NodeHash::some(h);
    }
    fail("unknown variant `" + k + "`");
}

static bt::utreexo::Proof parse_proof(const UniValue& v)
{
    bt::utreexo::Proof p;
    const UniValue& t = field(v, "targets");
    if (!t.isArray()) fail("invalid type: expected a sequence");
    for (auto& x : t.getValues()) p.targets.push_back(parse_u64(x));
    const UniValue& h = field(v, "hashes");
    if (!h.isArray()) fail("invalid type: expected a sequence");
    for (auto& x : h.getValues()) p.hashes.push_back(parse_node_hash(x));
    return p;
}

static DecisionType parse_decision_type(const UniValue& v)
{
    auto [k, x] = variant(v);
    DecisionType t;
    if (k == "Binary") t.kind = DecisionType::Binary;
    else if (k == "Scaled" && x) {
        t.kind = DecisionType::Scaled;
        t.min = parse_f64(field(*x, "min"));
        t.max = parse_f64(field(*x, "max"));
        t.increment = parse_f64(field(*x, "increment"));
    } else if (k == "Category" && x) {
        t.kind = DecisionType::Category;
        t.options = parse_strs(field(*x, "options"));
    } else
        fail("unknown variant `" + k + "`, expected one of `Binary`, `Scaled`, `Category`");
    return t;
}

ClaimDecisionPayload parse_claim_payload(const UniValue& v)
{
    ClaimDecisionPayload p;
    p.decision_type = parse_decision_type(field(v, "decision_type"));
    const UniValue& ds = field(v, "decisions");
    if (!ds.isArray()) fail("invalid type: expected a sequence");
    for (auto& d : ds.getValues()) {
        DecisionClaimEntry e;
        e.decision_id_bytes = parse_did_bytes(field(d, "decision_id_bytes"));
        e.header = parse_str(field(d, "header"));
        e.description = parse_str(field(d, "description"));
        e.option_0_label = parse_opt_str(d.find_value("option_0_label"));
        e.option_1_label = parse_opt_str(d.find_value("option_1_label"));
        if (!d.find_value("option_labels").isNull()) e.option_labels = parse_strs(d.find_value("option_labels"));
        if (!d.find_value("tags").isNull()) e.tags = parse_strs(d.find_value("tags"));
        p.decisions.push_back(std::move(e));
    }
    return p;
}

template <class T>
static std::optional<T> opt(const UniValue& v, T (*f)(const UniValue&))
{
    if (v.isNull()) return std::nullopt;
    return f(v);
}

TxData parse_tx_data(const UniValue& v)
{
    auto [k, x] = variant(v);
    if (!x) fail("unknown variant `" + k + "`");
    if (k == "ClaimDecision") return parse_claim_payload(*x);
    if (k == "CreateMarket") {
        TxCreateMarket m;
        m.title = parse_str(field(*x, "title"));
        m.description = parse_str(field(*x, "description"));
        const UniValue& ds = field(*x, "dimension_specs");
        if (!ds.isArray()) fail("invalid type: expected a sequence");
        for (auto& d : ds.getValues()) {
            auto [dk, dx] = variant(d);
            if (!dx || (dk != "Single" && dk != "Categorical")) fail("unknown variant `" + dk + "`");
            DimensionSpec s;
            s.kind = dk == "Single" ? DimensionSpec::Single : DimensionSpec::Categorical;
            auto b = parse_did_bytes(*dx);
            std::memcpy(s.id.b.data(), b.data(), b.size());
            m.dimension_specs.push_back(s);
        }
        const UniValue& nc = field(*x, "new_claims");
        if (!nc.isArray()) fail("invalid type: expected a sequence");
        for (auto& c : nc.getValues()) m.new_claims.push_back(parse_claim_payload(c));
        m.trading_fee = opt<double>(x->find_value("trading_fee"), parse_f64);
        if (!x->find_value("tx_pow_hash_selector").isNull()) m.tx_pow_hash_selector = parse_u8(x->find_value("tx_pow_hash_selector"));
        if (!x->find_value("tx_pow_ordering").isNull()) m.tx_pow_ordering = parse_u8(x->find_value("tx_pow_ordering"));
        if (!x->find_value("tx_pow_difficulty").isNull()) m.tx_pow_difficulty = parse_u8(x->find_value("tx_pow_difficulty"));
        return m;
    }
    if (k == "Trade") {
        TxTrade t;
        t.market_id = parse_market_id(field(*x, "market_id"));
        t.outcome_index = parse_u32(field(*x, "outcome_index"));
        t.shares = parse_i64(field(*x, "shares"));
        t.trader = parse_address(field(*x, "trader"));
        t.limit_sats = parse_u64(field(*x, "limit_sats"));
        t.tx_pow_nonce = opt<uint64_t>(x->find_value("tx_pow_nonce"), parse_u64);
        t.prev_block_hash = parse_hash(field(*x, "prev_block_hash"));
        return t;
    }
    if (k == "SubmitVote") {
        TxSubmitVote s;
        s.voter = parse_address(field(*x, "voter"));
        s.decision_id_bytes = parse_did_bytes(field(*x, "decision_id_bytes"));
        s.vote_value = parse_f64(field(*x, "vote_value"));
        s.voting_period = parse_u32(field(*x, "voting_period"));
        return s;
    }
    if (k == "SubmitBallot") {
        TxSubmitBallot s;
        s.voter = parse_address(field(*x, "voter"));
        const UniValue& vs = field(*x, "votes");
        if (!vs.isArray()) fail("invalid type: expected a sequence");
        for (auto& b : vs.getValues()) s.votes.push_back(BallotItem{parse_did_bytes(field(b, "decision_id_bytes")), parse_f64(field(b, "vote_value"))});
        s.voting_period = parse_u32(field(*x, "voting_period"));
        return s;
    }
    if (k == "TransferReputation") {
        TxTransferReputation t;
        t.sender = parse_address(field(*x, "sender"));
        t.dest = parse_address(field(*x, "dest"));
        t.amount = parse_f64(field(*x, "amount"));
        return t;
    }
    if (k == "AmplifyBeta") {
        TxAmplifyBeta a;
        a.market_id = parse_market_id(field(*x, "market_id"));
        a.amount = parse_u64(field(*x, "amount"));
        a.market_author = parse_address(field(*x, "market_author"));
        return a;
    }
    fail("unknown variant `" + k + "`");
}

Transaction parse_transaction(const UniValue& v)
{
    Transaction t;
    const UniValue& ins = field(v, "inputs");
    if (!ins.isArray()) fail("invalid type: expected a sequence");
    for (auto& p : ins.getValues()) {
        if (!p.isArray() || p.size() != 2) fail("invalid type: expected a tuple of size 2");
        auto b = parse_byte_array(p[1], 32);
        Hash32 h;
        std::memcpy(h.data(), b.data(), b.size());
        t.inputs.emplace_back(parse_outpoint(p[0]), h);
    }
    t.proof = parse_proof(field(v, "proof"));
    const UniValue& outs = field(v, "outputs");
    if (!outs.isArray()) fail("invalid type: expected a sequence");
    for (auto& o : outs.getValues()) t.outputs.push_back(parse_output(o));
    if (!v.find_value("data").isNull()) t.data = parse_tx_data(v.find_value("data"));
    return t;
}

bt::frost::Point parse_verifying_key(const UniValue& v)
{
    auto s = parse_str(v);
    auto b = bt::bech32::decode_bech32m_canonical(s, "ba-svk");
    if (!b || b->size() != 32) fail("invalid value: string \"" + s + "\", expected a verifying key");
    return bt::frost::Point::from_span(*b);
}

bt::frost::Signature parse_signature(const UniValue& v)
{
    auto s = parse_str(v);
    std::string h = s.rfind("0x", 0) == 0 ? s.substr(2) : s;
    auto b = bt::hex_decode(h);
    if (!b || b->size() != 64) fail("invalid value: string \"" + s + "\", expected a signature");
    bt::frost::Signature sig;
    std::memcpy(sig.R.data(), b->data(), 32);
    std::memcpy(sig.z.data(), b->data() + 32, 32);
    return sig;
}

Authorization parse_authorization(const UniValue& v)
{
    return Authorization{parse_verifying_key(field(v, "verifying_key")), parse_signature(field(v, "signature"))};
}

AuthorizedTransaction parse_authorized_transaction(const UniValue& v)
{
    AuthorizedTransaction a;
    a.transaction = parse_transaction(field(v, "transaction"));
    const UniValue& au = field(v, "authorizations");
    if (!au.isArray()) fail("invalid type: expected a sequence");
    for (auto& x : au.getValues()) a.authorizations.push_back(parse_authorization(x));
    if (!v.find_value("actor_proof").isNull()) a.actor_proof = parse_authorization(v.find_value("actor_proof"));
    return a;
}

Header parse_header(const UniValue& v)
{
    Header h;
    h.merkle_root = parse_hash(field(v, "merkle_root"));
    if (!v.find_value("prev_side_hash").isNull()) h.prev_side_hash = parse_hash(v.find_value("prev_side_hash"));
    h.prev_main_hash = parse_btc_hash(field(v, "prev_main_hash"));
    const UniValue& roots = field(v, "roots");
    if (!roots.isArray()) fail("invalid type: expected a sequence");
    for (auto& r : roots.getValues()) {
        auto s = parse_str(r);
        auto b = bt::hex_to_array<32>(s);
        if (!b) fail("invalid value: string \"" + s + "\", expected a node hash");
        h.roots.push_back(bt::utreexo::NodeHash::some(*b));
    }
    return h;
}

Body parse_body(const UniValue& v)
{
    Body b;
    const UniValue& cb = field(v, "coinbase");
    b.coinbase.memo = parse_byte_vec(field(cb, "memo"));
    const UniValue& co = field(cb, "outputs");
    if (!co.isArray()) fail("invalid type: expected a sequence");
    for (auto& o : co.getValues()) b.coinbase.outputs.push_back(parse_output(o));
    const UniValue& txs = field(v, "transactions");
    if (!txs.isArray()) fail("invalid type: expected a sequence");
    for (auto& t : txs.getValues()) b.transactions.push_back(parse_transaction(t));
    const UniValue& au = field(v, "authorizations");
    if (!au.isArray()) fail("invalid type: expected a sequence");
    for (auto& x : au.getValues()) b.authorizations.push_back(parse_authorization(x));
    const UniValue& ap = field(v, "actor_proofs");
    if (!ap.isArray()) fail("invalid type: expected a sequence");
    for (auto& x : ap.getValues()) b.actor_proofs.push_back(x.isNull() ? std::nullopt : std::optional<Authorization>(parse_authorization(x)));
    return b;
}

} // namespace hivemind::json
