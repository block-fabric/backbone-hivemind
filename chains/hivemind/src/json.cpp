// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <hivemind/fmt.h>
#include <hivemind/json.h>

#include <bt/bech32.h>

#include <cmath>

namespace hivemind::json {

static UniValue obj() { return UniValue(UniValue::VOBJ); }
static UniValue arr() { return UniValue(UniValue::VARR); }

UniValue f64(double x)
{
    if (!std::isfinite(x)) return UniValue();
    return UniValue(UniValue::VNUM, fmt::json(x));
}
UniValue hash(const Hash32& h) { return UniValue(bt::hex_encode(h)); }
UniValue btc_hash(const Hash32& h) { return UniValue(bt::hex_encode_rev(h.span())); }
UniValue bytes_array(ByteSpan b)
{
    UniValue a = arr();
    for (uint8_t x : b) a.push_back(UniValue(int(x)));
    return a;
}
UniValue address(const Address& a) { return UniValue(a.to_base58()); }
UniValue market_id(const MarketId& m) { return bytes_array(ByteSpan(m.b.data(), 6)); }
UniValue decision_id(const DecisionId& d) { return bytes_array(ByteSpan(d.b.data(), 3)); }
static UniValue opt_str(const std::optional<std::string>& s) { return s ? UniValue(*s) : UniValue(); }
static UniValue strs(const std::vector<std::string>& v)
{
    UniValue a = arr();
    for (auto& s : v) a.push_back(UniValue(s));
    return a;
}

UniValue outpoint(const OutPoint& o)
{
    UniValue r = obj();
    switch (o.kind) {
    case OutPoint::Deposit:
        r.pushKV("Deposit", bt::hex_encode_rev(o.txid.span()) + ":" + std::to_string(o.vout));
        return r;
    case OutPoint::MarketFunds: {
        UniValue i = obj();
        i.pushKV("market_id", market_id(o.market_id));
        i.pushKV("block_height", UniValue(uint64_t(o.block_height)));
        i.pushKV("is_fee", UniValue(o.is_fee));
        r.pushKV("MarketFunds", i);
        return r;
    }
    case OutPoint::Payout: {
        UniValue i = obj();
        i.pushKV("hash", hash(o.txid));
        i.pushKV("vout", UniValue(uint64_t(o.vout)));
        r.pushKV("Payout", i);
        return r;
    }
    default: {
        UniValue i = obj();
        i.pushKV("txid", hash(o.txid));
        i.pushKV("vout", UniValue(uint64_t(o.vout)));
        r.pushKV(o.kind == OutPoint::Regular ? "Regular" : "Coinbase", i);
        return r;
    }
    }
}

// output/content.rs: JSON is human readable -> HumanReadableRepr
UniValue content(const Content& c)
{
    UniValue r = obj();
    if (c.kind == Content::Value) {
        r.pushKV("Value", UniValue(c.value));
    } else if (c.kind == Content::Withdrawal) {
        UniValue w = obj();
        w.pushKV("value_sats", UniValue(c.value));
        w.pushKV("main_fee_sats", UniValue(c.main_fee));
        w.pushKV("main_address", c.main_address.to_string());
        r.pushKV("Withdrawal", w);
    } else {
        UniValue m = obj();
        m.pushKV("market_id", market_id(c.market_id));
        m.pushKV("amount", UniValue(c.value));
        m.pushKV("is_fee", UniValue(c.is_fee));
        r.pushKV("MarketFunds", m);
    }
    return r;
}

UniValue output(const Output& o)
{
    UniValue r = obj();
    r.pushKV("address", address(o.address));
    r.pushKV("content", content(o.content));
    return r;
}

UniValue pointed_output(const OutPoint& op, const Output& o)
{
    UniValue r = obj();
    r.pushKV("outpoint", outpoint(op));
    r.pushKV("output", output(o));
    return r;
}

UniValue inpoint(const InPoint& p)
{
    if (p.kind == InPoint::Redistribution) return UniValue("Redistribution");
    UniValue r = obj();
    UniValue i = obj();
    if (p.kind == InPoint::Regular) {
        i.pushKV("txid", hash(p.id));
        i.pushKV("vin", UniValue(uint64_t(p.vin)));
        r.pushKV("Regular", i);
    } else {
        i.pushKV("m6id", btc_hash(p.id));
        r.pushKV("Withdrawal", i);
    }
    return r;
}

UniValue spent_output(const SpentOutput& s)
{
    UniValue r = obj();
    r.pushKV("output", output(s.output));
    r.pushKV("inpoint", inpoint(s.inpoint));
    return r;
}

UniValue node_hash(const bt::utreexo::NodeHash& n)
{
    if (n.tag == bt::utreexo::NodeHash::Empty) return UniValue("Empty");
    if (n.tag == bt::utreexo::NodeHash::Placeholder) return UniValue("Placeholder");
    UniValue r = obj();
    r.pushKV("Some", bytes_array(n.h.span()));
    return r;
}

UniValue proof(const bt::utreexo::Proof& p)
{
    UniValue r = obj();
    UniValue t = arr(), h = arr();
    for (auto x : p.targets) t.push_back(UniValue(x));
    for (auto& x : p.hashes) h.push_back(node_hash(x));
    r.pushKV("targets", t);
    r.pushKV("hashes", h);
    return r;
}

UniValue decision_type(const DecisionType& t)
{
    if (t.kind == DecisionType::Binary) return UniValue("Binary");
    UniValue r = obj();
    UniValue i = obj();
    if (t.kind == DecisionType::Scaled) {
        i.pushKV("min", f64(t.min));
        i.pushKV("max", f64(t.max));
        i.pushKV("increment", f64(t.increment));
        r.pushKV("Scaled", i);
    } else {
        i.pushKV("options", strs(t.options));
        r.pushKV("Category", i);
    }
    return r;
}

UniValue claim_payload(const ClaimDecisionPayload& p)
{
    UniValue r = obj();
    r.pushKV("decision_type", decision_type(p.decision_type));
    UniValue ds = arr();
    for (auto& e : p.decisions) {
        UniValue x = obj();
        x.pushKV("decision_id_bytes", bytes_array(ByteSpan(e.decision_id_bytes.data(), 3)));
        x.pushKV("header", e.header);
        x.pushKV("description", e.description);
        x.pushKV("option_0_label", opt_str(e.option_0_label));
        x.pushKV("option_1_label", opt_str(e.option_1_label));
        x.pushKV("option_labels", e.option_labels ? strs(*e.option_labels) : UniValue());
        x.pushKV("tags", e.tags ? strs(*e.tags) : UniValue());
        ds.push_back(x);
    }
    r.pushKV("decisions", ds);
    return r;
}

template <class T>
static UniValue opt_num(const std::optional<T>& v)
{
    return v ? UniValue(uint64_t(*v)) : UniValue();
}

UniValue tx_data(const TxData& d)
{
    UniValue r = obj();
    UniValue i = obj();
    std::visit(
        [&](auto&& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, ClaimDecisionPayload>) {
                i = claim_payload(x);
            } else if constexpr (std::is_same_v<T, TxCreateMarket>) {
                i.pushKV("title", x.title);
                i.pushKV("description", x.description);
                UniValue ds = arr();
                for (auto& s : x.dimension_specs) {
                    UniValue o = obj();
                    o.pushKV(s.kind == DimensionSpec::Single ? "Single" : "Categorical", decision_id(s.id));
                    ds.push_back(o);
                }
                i.pushKV("dimension_specs", ds);
                UniValue nc = arr();
                for (auto& c : x.new_claims) nc.push_back(claim_payload(c));
                i.pushKV("new_claims", nc);
                i.pushKV("trading_fee", x.trading_fee ? f64(*x.trading_fee) : UniValue());
                i.pushKV("tx_pow_hash_selector", opt_num(x.tx_pow_hash_selector));
                i.pushKV("tx_pow_ordering", opt_num(x.tx_pow_ordering));
                i.pushKV("tx_pow_difficulty", opt_num(x.tx_pow_difficulty));
            } else if constexpr (std::is_same_v<T, TxTrade>) {
                i.pushKV("market_id", market_id(x.market_id));
                i.pushKV("outcome_index", UniValue(uint64_t(x.outcome_index)));
                i.pushKV("shares", UniValue(x.shares));
                i.pushKV("trader", address(x.trader));
                i.pushKV("limit_sats", UniValue(x.limit_sats));
                i.pushKV("tx_pow_nonce", opt_num(x.tx_pow_nonce));
                i.pushKV("prev_block_hash", hash(x.prev_block_hash));
            } else if constexpr (std::is_same_v<T, TxSubmitVote>) {
                i.pushKV("voter", address(x.voter));
                i.pushKV("decision_id_bytes", bytes_array(ByteSpan(x.decision_id_bytes.data(), 3)));
                i.pushKV("vote_value", f64(x.vote_value));
                i.pushKV("voting_period", UniValue(uint64_t(x.voting_period)));
            } else if constexpr (std::is_same_v<T, TxSubmitBallot>) {
                i.pushKV("voter", address(x.voter));
                UniValue vs = arr();
                for (auto& v : x.votes) {
                    UniValue o = obj();
                    o.pushKV("decision_id_bytes", bytes_array(ByteSpan(v.decision_id_bytes.data(), 3)));
                    o.pushKV("vote_value", f64(v.vote_value));
                    vs.push_back(o);
                }
                i.pushKV("votes", vs);
                i.pushKV("voting_period", UniValue(uint64_t(x.voting_period)));
            } else if constexpr (std::is_same_v<T, TxTransferReputation>) {
                i.pushKV("sender", address(x.sender));
                i.pushKV("dest", address(x.dest));
                i.pushKV("amount", f64(x.amount));
            } else if constexpr (std::is_same_v<T, TxAmplifyBeta>) {
                i.pushKV("market_id", market_id(x.market_id));
                i.pushKV("amount", UniValue(x.amount));
                i.pushKV("market_author", address(x.market_author));
            }
        },
        d);
    r.pushKV(tx_data_name(d.index()), i);
    return r;
}

static void tx_fields(UniValue& r, const Transaction& t)
{
    UniValue ins = arr();
    for (auto& [op, h] : t.inputs) {
        UniValue pair = arr();
        pair.push_back(outpoint(op));
        pair.push_back(bytes_array(h.span())); // Hash = [u8; 32]
        ins.push_back(pair);
    }
    r.pushKV("inputs", ins);
    r.pushKV("proof", proof(t.proof));
    UniValue outs = arr();
    for (auto& o : t.outputs) outs.push_back(output(o));
    r.pushKV("outputs", outs);
    r.pushKV("data", t.data ? tx_data(*t.data) : UniValue());
}

UniValue transaction(const Transaction& t)
{
    UniValue r = obj();
    tx_fields(r, t);
    return r;
}

std::string verifying_key_str(const bt::frost::Point& vk) { return *bt::bech32::encode("ba-svk", vk.span(), bt::bech32::Variant::Bech32m); }
std::string encryption_pubkey_str(const bt::ByteArray<32>& epk) { return *bt::bech32::encode("ba-enc", epk.span(), bt::bech32::Variant::Bech32m); }

std::string signature_str(const bt::frost::Signature& s)
{
    Bytes sig(s.R.begin(), s.R.end());
    sig.insert(sig.end(), s.z.begin(), s.z.end());
    return "0x" + bt::hex_encode(sig); // const_hex::serde::serialize: 0x-prefixed
}

UniValue authorization(const Authorization& a)
{
    UniValue r = obj();
    r.pushKV("verifying_key", verifying_key_str(a.verifying_key));
    r.pushKV("signature", signature_str(a.signature));
    return r;
}

UniValue authorized_transaction(const AuthorizedTransaction& t)
{
    UniValue r = obj();
    r.pushKV("transaction", transaction(t.transaction));
    UniValue a = arr();
    for (auto& x : t.authorizations) a.push_back(authorization(x));
    r.pushKV("authorizations", a);
    r.pushKV("actor_proof", t.actor_proof ? authorization(*t.actor_proof) : UniValue());
    return r;
}

UniValue coinbase(const Coinbase& c)
{
    UniValue r = obj();
    r.pushKV("memo", bytes_array(c.memo));
    UniValue o = arr();
    for (auto& x : c.outputs) o.push_back(output(x));
    r.pushKV("outputs", o);
    return r;
}

UniValue body(const Body& b)
{
    UniValue r = obj();
    r.pushKV("coinbase", coinbase(b.coinbase));
    UniValue txs = arr();
    for (auto& t : b.transactions) txs.push_back(transaction(t));
    r.pushKV("transactions", txs);
    UniValue a = arr();
    for (auto& x : b.authorizations) a.push_back(authorization(x));
    r.pushKV("authorizations", a);
    UniValue ap = arr();
    for (auto& p : b.actor_proofs) ap.push_back(p ? authorization(*p) : UniValue());
    r.pushKV("actor_proofs", ap);
    return r;
}

UniValue header(const Header& h)
{
    UniValue r = obj();
    r.pushKV("merkle_root", hash(h.merkle_root));
    r.pushKV("prev_side_hash", h.prev_side_hash ? hash(*h.prev_side_hash) : UniValue());
    r.pushKV("prev_main_hash", btc_hash(h.prev_main_hash));
    UniValue roots = arr();
    for (auto& x : h.roots) roots.push_back(UniValue(x.to_string())); // DisplayFromStr
    r.pushKV("roots", roots);
    return r;
}

UniValue block(const Block& b)
{
    UniValue r = obj();
    r.pushKV("header", header(b.header));
    r.pushKV("body", body(b.body));
    return r;
}

UniValue bitcoin_tx(const std::vector<bt::btc::TxOut>& outs)
{
    UniValue r = obj();
    r.pushKV("version", 2);
    r.pushKV("lock_time", 0);
    r.pushKV("input", arr());
    UniValue o = arr();
    for (auto& x : outs) {
        UniValue t = obj();
        t.pushKV("value", UniValue(x.value));
        t.pushKV("script_pubkey", bt::hex_encode(x.script_pubkey));
        o.push_back(t);
    }
    r.pushKV("output", o);
    return r;
}

UniValue withdrawal_bundle(const WithdrawalBundle& w)
{
    UniValue r = obj();
    UniValue su = arr();
    for (auto& [op, o] : w.spend_utxos) {
        UniValue p = arr();
        p.push_back(outpoint(op));
        p.push_back(output(o));
        su.push_back(p);
    }
    r.pushKV("spend_utxos", su);
    r.pushKV("tx", bitcoin_tx(w.outputs));
    return r;
}

UniValue withdrawal_bundle_info(const WithdrawalBundleInfo& i)
{
    if (i.kind == WithdrawalBundleInfo::Unknown) return UniValue("Unknown");
    UniValue r = obj();
    if (i.kind == WithdrawalBundleInfo::Known) {
        r.pushKV("Known", withdrawal_bundle(i.bundle));
    } else {
        // serde_json cannot serialize a map with OutPoint keys; theirs fails here
        throw std::runtime_error("key must be a string");
    }
    return r;
}

} // namespace hivemind::json
