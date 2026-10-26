// Truthcoin codec/hash known answers: truthcoin-dc v0.20.0's own fixed encodings
// (lib/types/encoding_tests.rs, copied to tests/data/encoding_vectors.json) and their
// lib/math through the C ABI.
#include "test.h"

#include <hivemind/fmt.h>
#include <hivemind/json.h>
#include <hivemind/math.h>
#include <hivemind/types.h>

#include <bt/crypto.h>

#include <univalue.h>

#include <cmath>

using namespace hivemind;

namespace {
Hash32 fill32(uint8_t b)
{
    Hash32 h;
    for (auto& x : h.v) x = b;
    return h;
}
bt::Hash20 fill20(uint8_t b)
{
    bt::Hash20 h;
    for (auto& x : h.v) x = b;
    return h;
}

UniValue vectors()
{
    UniValue v;
    v.read(bttest::read_file(bttest::data_dir() + "/encoding_vectors.json"));
    return v;
}

std::vector<OutPoint> outpoints()
{
    std::vector<OutPoint> v;
    v.push_back(OutPoint::regular(fill32(1), 2));
    v.push_back(OutPoint::coinbase(fill32(3), 4));
    OutPoint d;
    d.kind = OutPoint::Deposit;
    d.txid = fill32(5);
    d.vout = 6;
    v.push_back(d);
    MarketId m;
    m.b.fill(7);
    v.push_back(OutPoint::market_funds(m, 8, true));
    v.push_back(OutPoint::payout(fill32(9), 10));
    return v;
}

Address addr(uint8_t b)
{
    Address a;
    a.b = fill20(b);
    return a;
}

std::vector<Output> outputs(uint8_t a0, uint64_t v0, uint8_t a1, uint64_t v1, uint64_t f1, uint8_t a2, uint8_t m2, uint64_t v2, bool fee2)
{
    std::vector<Output> r(3);
    r[0].address = addr(a0);
    r[0].content.kind = Content::Value;
    r[0].content.value = v0;
    r[1].address = addr(a1);
    r[1].content.kind = Content::Withdrawal;
    r[1].content.value = v1;
    r[1].content.main_fee = f1;
    r[1].content.main_address = *bt::btc::Address::parse("bc1qar0srrr7xfkvy5l643lydnw9re59gtzzwf5mdq");
    r[2].address = addr(a2);
    r[2].content.kind = Content::MarketFunds;
    r[2].content.market_id.b.fill(m2);
    r[2].content.value = v2;
    r[2].content.is_fee = fee2;
    return r;
}

std::vector<Transaction> transactions()
{
    auto ops = outpoints();
    Transaction trade;
    trade.inputs = {{ops[0], fill32(44)}, {ops[1], fill32(45)}, {ops[2], fill32(46)}};
    trade.outputs = outputs(11, 12, 13, 14, 15, 18, 19, 20, false);
    TxTrade tr;
    tr.market_id.b.fill(22);
    tr.outcome_index = 23;
    tr.shares = -24;
    tr.trader = addr(25);
    tr.limit_sats = 26;
    tr.tx_pow_nonce = 27;
    tr.prev_block_hash = fill32(28);
    trade.data = tr;
    Transaction cm;
    cm.inputs = {{ops[3], fill32(47)}, {ops[4], fill32(48)}};
    TxCreateMarket c;
    c.title = "title";
    c.description = "description";
    c.dimension_specs.push_back({DimensionSpec::Single, *DecisionId::make(true, 1, 0)});
    c.dimension_specs.push_back({DimensionSpec::Categorical, *DecisionId::make(false, 2, 3)});
    ClaimDecisionPayload p;
    p.decision_type.kind = DecisionType::Scaled;
    p.decision_type.min = 0.0;
    p.decision_type.max = 100.0;
    p.decision_type.increment = 0.5;
    DecisionClaimEntry e;
    e.decision_id_bytes = {0x81, 0, 0};
    e.header = "header";
    e.description = "description";
    e.option_0_label = "no";
    e.option_labels = std::vector<std::string>{"a", "b"};
    p.decisions.push_back(e);
    c.new_claims.push_back(p);
    c.trading_fee = 0.01;
    c.tx_pow_hash_selector = 29;
    c.tx_pow_difficulty = 30;
    cm.data = c;
    return {trade, cm};
}

Block block()
{
    Block b;
    b.header.merkle_root = fill32(31);
    b.header.prev_side_hash = fill32(32);
    b.header.prev_main_hash = fill32(33);
    b.body.coinbase.memo = {16, 17};
    b.body.coinbase.outputs = outputs(11, 12, 13, 14, 15, 18, 19, 20, false);
    b.body.transactions = transactions();
    b.body.actor_proofs = {std::nullopt, std::nullopt};
    return b;
}

template <class F>
std::string hexw(F f)
{
    Writer w;
    f(w);
    return bt::hex_encode(w.buf);
}
} // namespace

TEST(hm_outpoint_encodings)
{
    auto exp = vectors()["outpoint_encodings"];
    auto ops = outpoints();
    size_t i = 0;
    for (auto& op : ops) {
        CHECK_EQ(hexw([&](Writer& w) { borsh(w, op); }), exp[i++].get_str());
        CHECK_EQ(hexw([&](Writer& w) { bincode(w, op); }), exp[i++].get_str());
        auto k = op.key();
        CHECK_EQ(bt::hex_encode(ByteSpan(k.data(), k.size())), exp[i++].get_str());
        // decoders round-trip
        Writer w;
        bincode(w, op);
        Reader r(w.buf);
        CHECK(de_outpoint(r) == op);
        Writer wb;
        borsh(wb, op);
        Reader rb(wb.buf);
        CHECK(borsh_de_outpoint(rb) == op);
    }
}

TEST(hm_transaction_encodings)
{
    auto exp = vectors()["transaction_encodings"];
    size_t i = 0;
    for (auto& tx : transactions()) {
        CHECK_EQ(bt::hex_encode(tx.txid()), exp[i++].get_str());
        CHECK_EQ(bt::hex_encode(tx.canonical_bytes()), exp[i++].get_str());
        std::string bc = hexw([&](Writer& w) { bincode(w, tx); });
        CHECK_EQ(bc, exp[i - 1 + 1].get_str());
        i++;
        // bincode and borsh decoders reproduce the same transaction
        Writer w;
        bincode(w, tx);
        Reader r(w.buf);
        Transaction back = de_transaction(r);
        CHECK(r.empty());
        CHECK_EQ(bt::hex_encode(back.canonical_bytes()), bt::hex_encode(tx.canonical_bytes()));
        auto cb = tx.canonical_bytes();
        Reader rb(cb);
        Transaction back2 = borsh_de_transaction(rb);
        CHECK(rb.empty());
        CHECK_EQ(bt::hex_encode(back2.txid()), bt::hex_encode(tx.txid()));
    }
}

TEST(hm_block_encodings)
{
    auto exp = vectors()["block_encodings"];
    Block b = block();
    CHECK_EQ(bt::hex_encode(b.header.hash()), exp[0].get_str());
    CHECK_EQ(hexw([&](Writer& w) { borsh(w, b.header); }), exp[1].get_str());
    CHECK_EQ(hexw([&](Writer& w) { bincode(w, b.header); }), exp[2].get_str());
    CHECK_EQ(hexw([&](Writer& w) { borsh(w, b.body); }), exp[3].get_str());
    CHECK_EQ(hexw([&](Writer& w) { bincode(w, b.body); }), exp[4].get_str());
    CHECK_EQ(hexw([&](Writer& w) { bincode(w, b); }), exp[5].get_str());
    CHECK_EQ(json::block(b).write(), exp[6].get_str());
    Writer w;
    bincode(w, b.body);
    Reader r(w.buf);
    Body back = de_body(r);
    CHECK(r.empty());
    CHECK_EQ(hexw([&](Writer& ww) { bincode(ww, back); }), exp[4].get_str());
}

TEST(hm_spent_output_encodings)
{
    auto exp = vectors()["spent_output_encodings"];
    auto outs = outputs(35, 36, 37, 38, 39, 40, 41, 42, true);
    for (size_t i = 0; i < 3; i++) {
        SpentOutput s{outs[i], InPoint{InPoint::Withdrawal, fill32(43), 0}};
        Writer w;
        bincode(w, s.output);
        bincode(w, s.inpoint);
        CHECK_EQ(bt::hex_encode(w.buf), exp[i].get_str());
    }
}

TEST(hm_nan_refused_by_borsh)
{
    Transaction t;
    TxSubmitVote v;
    v.vote_value = std::nan("");
    t.data = v;
    CHECK(t.has_nan());
    bool threw = false;
    try {
        t.txid();
    } catch (const bt::DecodeError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(hm_decision_id_bits)
{
    auto d = DecisionId::make(true, 127, 65535);
    REQUIRE(d.has_value());
    CHECK(d->is_standard());
    CHECK_EQ(d->period_index(), 127u);
    CHECK_EQ(d->decision_index(), 65535u);
    CHECK_EQ(d->voting_period(), 128u);
    CHECK(!DecisionId::make(true, 128, 0));
    CHECK_EQ(DecisionId::from_hex("810001")->to_hex(), std::string("810001"));
    CHECK(!DecisionId::from_hex("81000"));
}

TEST(hm_deposit_address)
{
    Address a = addr(7);
    std::string f = a.format_for_deposit();
    CHECK(f.rfind("s13_", 0) == 0);
    CHECK(*Address::from_deposit_address(f) == a);
    CHECK(!Address::from_deposit_address(f.substr(0, f.size() - 1)));
    CHECK(!Address::from_deposit_address(a.to_base58()));
}

TEST(hm_rust_float_text)
{
    CHECK_EQ(fmt::display(1.0), std::string("1"));
    CHECK_EQ(fmt::display(0.005), std::string("0.005"));
    CHECK_EQ(fmt::display(1e-7), std::string("0.0000001"));
    CHECK_EQ(fmt::display(1e21), std::string("1000000000000000000000"));
    CHECK_EQ(fmt::debug(1.0), std::string("1.0"));
    CHECK_EQ(fmt::debug(1e16), std::string("1e16"));
    CHECK_EQ(fmt::debug(1.5e-7), std::string("1.5e-7"));
    CHECK_EQ(fmt::debug(0.0001), std::string("0.0001"));
    CHECK_EQ(fmt::json(0.01), std::string("0.01"));
    CHECK_EQ(fmt::json(100.0), std::string("100.0"));
    CHECK_EQ(fmt::json(1e16), std::string("1e+16"));
    CHECK_EQ(fmt::json(1e15), std::string("1000000000000000.0"));
    CHECK_EQ(fmt::json(1e-7), std::string("1e-7"));
    CHECK_EQ(fmt::json(0.00001), std::string("0.00001"));
    CHECK_EQ(fmt::json(123.456), std::string("123.456"));
}

// lib/math/lmsr.rs:303-337 bit patterns, through the C ABI
TEST(hm_math_lmsr_known_bits)
{
    std::string err;
    auto c = math::treasury({0, 0}, 1000.0, &err);
    REQUIRE(c.has_value());
    // C = beta * ln(2) for two empty outcomes
    CHECK(std::fabs(*c - 1000.0 * std::log(2.0)) < 1e-9);
    auto p = math::prices({0, 0}, 1000.0, &err);
    REQUIRE(p.has_value());
    CHECK_EQ((*p)[0], 0.5);
    auto bad = math::treasury({0, 0}, 0.0, &err);
    CHECK(!bad.has_value());
    CHECK(err.find("InvalidBeta") != std::string::npos);
    auto bc = math::buy_cost(1234.5, 0.005, &err);
    REQUIRE(bc.has_value());
    CHECK_EQ(bc->base_cost_sats, 1235u);
    CHECK_EQ(bc->trading_fee_sats, 1000u); // max(round(6.17), MIN 1000)
    auto alloc = math::allocate_u64({2.0, 1.0, 1.0}, 1001, &err);
    REQUIRE(alloc.has_value());
    uint64_t sum = 0;
    for (auto& [k, v] : *alloc) sum += v;
    CHECK_EQ(sum, 1001u);
    CHECK_EQ(math::reprice(10000, 25, 25), 20000u);
    CHECK_EQ(math::round_reputation(0.12345678901234), 0.123456789);
}
