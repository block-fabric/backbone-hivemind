// Hivemind's app layer (doc/app.md): the LayerTwo JSON-RPC is the same with the app layer running (same methods, same
// bytes for every reply); the app RPC's reads are the LayerTwo handlers' own JSON; the backup phrase's flows (made, its
// words only in a question, restored, refused over another) and the unified wallet's phrase (BIP-85 vectors); wallet
// ownership (positions, Votecoin, ballots, "mine") only for a wallet reader; every spend refused before any question
// when its numbers are not finite, wrap, pass 21,000,000 ECX or cannot be paid; the manifest of app/manifest.json is
// the node's.
#include "test.h"

#include <hivemind/app.h>
#include <hivemind/appui.h>

#include <bt/appui/auth.h>
#include <bt/appui/phrase.h>
#include <bt/frost.h>
#include <bt/hd.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <set>

using namespace hivemind;

namespace {
const char* ABANDON = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";

struct Harness {
    std::string dir;
    std::unique_ptr<Node> node;
    std::unique_ptr<Wallet> wallet;
    std::unique_ptr<App> app;
    explicit Harness(const std::string& name)
    {
        bt::frost::init();
        dir = (std::filesystem::temp_directory_path() / ("bt-hivemind-appui-" + name + "-" + bt::appui::random_hex(6))).string();
        std::filesystem::create_directories(dir);
        NodeConfig cfg;
        cfg.datadir = dir;
        cfg.network = Network::Regtest;
        cfg.p2p = false;
        cfg.use_seeds = false;
        cfg.mainchain_grpc_url = "http://127.0.0.1:1";
        cfg.decision_config_testing = 6;
        node = std::make_unique<Node>(cfg);
        std::string err;
        CHECK(node->start(err));
        wallet = std::make_unique<Wallet>(dir + "/backbone-truthcoin-wallet.sqlite", *node);
        app = std::make_unique<App>(*node, *wallet);
    }
    ~Harness()
    {
        app.reset();
        wallet.reset();
        if (node) node->stop();
        node.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

std::string reply(bt::jsonrpc::Server& s, const std::string& method, const std::string& params)
{
    return s.handle(R"({"jsonrpc":"2.0","id":7,"method":")" + method + R"(","params":)" + params + "}");
}

UniValue host_call(AppLayer& layer, const std::string& user, const std::string& method, const std::string& params, const std::string& token = "")
{
    UniValue req{UniValue::VOBJ};
    req.pushKV("kind", "host");
    req.pushKV("method", method);
    UniValue p;
    CHECK(p.read(params));
    req.pushKV("params", p);
    return layer.host().run(req, token, "", UniValue{}, user);
}

bt::appui::AppOptions options()
{
    bt::appui::AppOptions o;
    o.enabled = true;
    o.port = 0;
    // The app's user (a reader of the chain and the wallet), a reader of the chain only, the cookie unrestricted.
    o.whitelist = {"app:webuihost,describeconfirmation,webuievents,getblockchaininfo,getbalances,get_wallet_addresses,dropin.checkAddress,dropin.block,dropin.seed.status,"
                   "dropin.seed.prepare,dropin.seed.create,dropin.seed.restore,dropin.seed.show,create_transfer,balance,get_block,market_list,market_get,decision_list,"
                   "decision_get,market_positions,get_voter_address,votecoin_balance,vote_list,decision_status",
                   "chainonly:webuihost,getblockchaininfo,get_block,dropin.checkAddress,market_list,market_get,decision_list,decision_get,decision_status"};
    o.whitelist_default = false;
    return o;
}

bool refused_with(AppLayer& layer, const std::string& method, const std::string& params, const std::string& words, const std::string& user = "__cookie__")
{
    UniValue p;
    CHECK(p.read(params));
    try {
        layer.host().describe(method, p, user);
    } catch (const std::exception& e) {
        const bool ok{std::string{e.what()}.find(words) != std::string::npos};
        if (!ok) std::fprintf(stderr, "  %s %s: %s\n", method.c_str(), params.c_str(), e.what());
        return ok;
    }
    std::fprintf(stderr, "  %s %s: not refused\n", method.c_str(), params.c_str());
    return false;
}
} // namespace

TEST(hm_appui_l2l_rpc_byte_identical_with_and_without_the_app_layer)
{
    Harness h("l2l");
    // Without the app layer: the LayerTwo server as main.cpp makes it.
    bt::jsonrpc::Server before;
    register_rpc(before, *h.app, true, true);
    // With it running (its own server, its pollers, its store) beside the same node and wallet.
    AppLayer layer(*h.app, "regtest", h.dir, options());
    std::string err;
    CHECK(layer.start(err));
    bt::jsonrpc::Server after;
    register_rpc(after, *h.app, true, true);
    CHECK(before.methods() == after.methods());
    // LayerTwo's 81 methods of rpc-api, openapi_schema once on a server of both APIs.
    CHECK_EQ(before.methods().size(), size_t{80});
    // Every method, with no parameters, with wrong ones, and (the reads) with right ones: the same bytes.
    const std::vector<std::string> bad{"[]", "[1,2,3,4,5]", R"(["x"])", R"({"nope":1})", "[null]", R"([{"market_id":"zz"}])", R"([{}])"};
    static const std::set<std::string> changes{"stop", "mine", "get_new_address", "generate_mnemonic", "set_seed_from_mnemonic", "create_deposit", "create_transfer",
                                               "create_transfer_many", "create_withdrawal", "connect_peer", "forget_peer", "invalidate_block", "remove_from_mempool",
                                               "submit_transaction", "connect_block", "get_block_template", "sign_transaction", "push_tx", "sync_to_tip",
                                               "get_new_encryption_key", "get_new_verifying_key", "transfer_votecoin", "decision_claim", "market_create", "market_buy",
                                               "market_sell", "market_amplify_beta", "vote_submit", "create_trade", "await_block_height"};
    size_t compared{0};
    for (const std::string& m : before.methods()) {
        for (const std::string& p : bad) {
            if (changes.contains(m) && (p == "[]" || p == "[null]")) continue;
            // A fresh phrase each call (it ignores its parameters): the same form, not the same words.
            if (m == "generate_mnemonic") {
                UniValue a, b;
                CHECK(a.read(reply(before, m, p)));
                CHECK(b.read(reply(after, m, p)));
                CHECK(a["result"].isStr() && b["result"].isStr() && a.getKeys() == b.getKeys());
                continue;
            }
            const std::string a{reply(before, m, p)}, b{reply(after, m, p)};
            if (a != b) std::fprintf(stderr, "  %s %s:\n    %s\n    %s\n", m.c_str(), p.c_str(), a.c_str(), b.c_str());
            CHECK(a == b);
            ++compared;
        }
    }
    CHECK(compared > 80 * 4);
    const std::vector<std::pair<std::string, std::string>> reads{
        {"getblockcount", "[]"}, {"get_best_sidechain_block_hash", "[]"}, {"list_utxos", "[]"}, {"list_peers", "[]"}, {"pending_withdrawal_bundle", "[]"},
        {"sidechain_wealth", "[]"}, {"balance", "[]"}, {"get_wallet_addresses", "[]"}, {"get_wallet_utxos", "[]"}, {"openapi_schema", "[]"},
        {"latest_failed_withdrawal_bundle_height", "[]"}, {"get_block_hash", "[0]"}, {"format_deposit_address", R"(["4XrMw8bmnhchFspSyP4XRUuB5vaR"])"},
        {"get_utxos", R"([["4XrMw8bmnhchFspSyP4XRUuB5vaR"]])"}, {"get_stxos", R"([["4XrMw8bmnhchFspSyP4XRUuB5vaR"]])"},
        {"get_transaction", R"(["0000000000000000000000000000000000000000000000000000000000000000"])"}, {"decision_status", "[]"}, {"market_list", "[]"},
        {"decision_list", "[]"}, {"list_open_periods_with_pricing", "[]"}, {"vote_voters", "[]"}, {"vote_period", "[null]"}, {"mainchain_sync_progress", "[]"},
        {"votecoin_balance", R"(["4XrMw8bmnhchFspSyP4XRUuB5vaR"])"}, {"market_positions", R"(["4XrMw8bmnhchFspSyP4XRUuB5vaR", null])"},
        {"vote_voter", R"(["4XrMw8bmnhchFspSyP4XRUuB5vaR"])"}, {"vote_list", R"([{"voter":"4XrMw8bmnhchFspSyP4XRUuB5vaR"}])"}, {"list_mempool", "[]"},
        {"market_get", R"(["000000000000"])"}, {"decision_get", R"(["000000"])"}, {"calculate_initial_liquidity", R"([{"beta":7.0,"num_outcomes":2}])"}};
    for (const auto& [m, p] : reads) CHECK(reply(before, m, p) == reply(after, m, p));
    // The app RPC's own read of a LayerTwo method answers its result, the same JSON; an error of LayerTwo's an error.
    for (const auto& [m, p] : reads) {
        UniValue r;
        CHECK(r.read(reply(before, m, p)));
        bt::appui::Call c;
        c.method = m;
        CHECK(c.params.read(p));
        c.user = "__cookie__";
        if (r.exists("error")) {
            bool refused{false};
            try {
                layer.server().dispatch(c);
            } catch (const bt::appui::RpcError&) {
                refused = true;
            }
            CHECK(refused);
            continue;
        }
        CHECK_EQ(layer.server().dispatch(c).write(), r.find_value("result").write());
    }
    layer.stop();
}

TEST(hm_appui_phrase_made_in_a_question_restored_refused_over_another)
{
    Harness h("seed");
    AppLayer layer(*h.app, "regtest", h.dir, options());
    std::string err;
    CHECK(layer.start(err));
    // Made: an id for the page, the words only in the question, the wallet set on the yes.
    const UniValue prep{host_call(layer, "app", "dropin.seed.prepare", "{}")["result"]};
    const std::string id{prep["id"].get_str()};
    CHECK(id.size() == 32);
    CHECK(!prep.exists("phrase") && !prep.exists("words_list"));
    const std::string create{"{\"id\":\"" + id + "\"}"};
    const UniValue q{host_call(layer, "app", "dropin.seed.create", create)["confirm"]};
    CHECK_EQ(q["kind"].get_str(), std::string{"phrase.create"});
    CHECK(q["details"].get_str().find(" 1. ") != std::string::npos);
    CHECK(!h.wallet->has_seed());
    const UniValue done{host_call(layer, "app", "dropin.seed.create", create, q["token"].get_str())["result"]};
    CHECK(h.wallet->has_seed());
    const std::string fp{done["fingerprint"].get_str()};
    CHECK_EQ(host_call(layer, "app", "dropin.seed.status", "{}")["result"]["fingerprint"].get_str(), fp);
    CHECK(host_call(layer, "app", "dropin.seed.status", "{}")["result"]["phraseKept"].isTrue());
    // Shown again: a question, never a result with words.
    const UniValue shown{host_call(layer, "app", "dropin.seed.show", "{}")};
    CHECK(shown.exists("confirm") && !shown.exists("result"));
    // Another phrase over it: refused before any question.
    bool refused{false};
    try {
        host_call(layer, "app", "dropin.seed.restore", std::string{R"({"phrase":")"} + ABANDON + "\"}");
    } catch (const bt::appui::RpcError&) {
        refused = true;
    }
    CHECK(refused);
    // The unified wallet's setup (its own user, the cookie): the same refusal over another phrase.
    bt::appui::Call c;
    c.method = "setwalletphrase";
    c.params.push_back(ABANDON);
    c.user = "__cookie__";
    refused = false;
    try {
        layer.server().dispatch(c);
    } catch (const bt::appui::RpcError&) {
        refused = true;
    }
    CHECK(refused);
    // The app's user may not call it at all.
    c.user = "app";
    refused = false;
    try {
        layer.server().dispatch(c);
    } catch (const bt::appui::RpcError&) {
        refused = true;
    }
    CHECK(refused);
    layer.stop();
}

TEST(hm_appui_unified_phrase_bip85_vectors)
{
    // BIP-85 of the master "abandon ×11 about", label sidechain/dropin/v1, stem hivemind: the same as an independent
    // implementation (Python, hardened BIP-32 + BIP-85, itself checked against BIP-85's own vector).
    auto master{bt::hd::parse_mnemonic(ABANDON)};
    const auto seed{bt::hd::mnemonic_to_seed(*master)};
    CHECK_EQ(bt::appui::dropin_phrase_index("hivemind", "betanet"), uint32_t{1777411230});
    CHECK_EQ(bt::appui::dropin_phrase_index("hivemind", "regtest"), uint32_t{91414691});
    const std::string betanet{bt::appui::dropin_phrase(seed.span(), "hivemind", "betanet")};
    const std::string regtest{bt::appui::dropin_phrase(seed.span(), "hivemind", "regtest")};
    CHECK_EQ(betanet, std::string{"essence lesson earth fever power inner edge only transfer almost hint upon"});
    CHECK_EQ(regtest, std::string{"fragile evidence confirm dune dizzy end blame infant pencil enrich evil airport"});
    CHECK_EQ(bt::appui::seed_fingerprint(bt::hd::mnemonic_to_seed(*bt::hd::parse_mnemonic(betanet)).span()), std::string{"02749253da7ef11f"});
    CHECK_EQ(bt::appui::seed_fingerprint(bt::hd::mnemonic_to_seed(*bt::hd::parse_mnemonic(regtest)).span()), std::string{"dd6edf265dd6471d"});

    Harness h("unified");
    AppLayer layer(*h.app, "regtest", h.dir, options());
    std::string err;
    CHECK(layer.start(err));
    bt::appui::Call c;
    c.method = "setwalletphrase";
    c.params.push_back(regtest);
    c.user = "__cookie__";
    CHECK_EQ(layer.server().dispatch(c)["fingerprint"].get_str(), std::string{"dd6edf265dd6471d"});
    bt::appui::Call info;
    info.method = "getwalletphraseinfo";
    info.user = "__cookie__";
    const UniValue i{layer.server().dispatch(info)};
    CHECK_EQ(i["fingerprint"].get_str(), std::string{"dd6edf265dd6471d"});
    CHECK_EQ(i["from"].get_str(), std::string{"unified"});
    // The same phrase again: accepted (the same seed); the wallet's keys are truthcoin's derivation of it.
    CHECK(layer.server().dispatch(c)["fingerprint"].get_str() == "dd6edf265dd6471d");
    CHECK(!h.wallet->voter_address().to_base58().empty());
    // A bad phrase: tiny-bip39's words.
    bt::appui::Call bad{c};
    bad.params = UniValue{UniValue::VARR};
    bad.params.push_back("abandon abandon");
    bool refused{false};
    try {
        layer.server().dispatch(bad);
    } catch (const bt::appui::RpcError& e) {
        refused = std::string{e.what()}.find("word") != std::string::npos || std::string{e.what()}.find("invalid") != std::string::npos;
    }
    CHECK(refused);
    layer.stop();
}

TEST(hm_appui_wallet_data_only_for_a_wallet_reader)
{
    Harness h("mine");
    h.wallet->set_seed_from_mnemonic(ABANDON);
    const std::string own{h.wallet->get_new_address().to_base58()};
    AppLayer layer(*h.app, "regtest", h.dir, options());
    std::string err;
    CHECK(layer.start(err));
    const std::string p{"{\"address\":\"" + own + "\"}"};
    const UniValue reader{host_call(layer, "app", "dropin.checkAddress", p)["result"]};
    CHECK(reader["valid"].isTrue());
    CHECK(reader["mine"].isTrue());
    const UniValue chain_only{host_call(layer, "chainonly", "dropin.checkAddress", p)["result"]};
    CHECK(chain_only["valid"].isTrue());
    CHECK(!chain_only.exists("mine"));
    CHECK(!chain_only.exists("error"));
    // The chain's markets and decisions: to both (no "mine" for the reader of the chain only).
    CHECK(host_call(layer, "chainonly", "hivemind.markets", "{}")["result"].isArray());
    CHECK(host_call(layer, "chainonly", "hivemind.decisions", "{}")["result"].isArray());
    CHECK(host_call(layer, "chainonly", "hivemind.status", "{}")["result"]["period"].isObject());
    // What is the wallet's (positions, Votecoin, ballots, payouts, history): never to a reader of the chain only.
    for (const char* m : {"hivemind.positions", "hivemind.votecoin", "hivemind.ballots", "hivemind.payouts", "dropin.history", "dropin.addresses"}) {
        bool refused{false};
        try {
            host_call(layer, "chainonly", m, "{}");
        } catch (const bt::appui::RpcError&) {
            refused = true;
        }
        if (!refused) std::fprintf(stderr, "  %s reached a reader of the chain only\n", m);
        CHECK(refused);
    }
    // ...and to the wallet's reader.
    CHECK(host_call(layer, "app", "hivemind.positions", "{}")["result"].isArray());
    const UniValue vc{host_call(layer, "app", "hivemind.votecoin", "{}")["result"]};
    CHECK_EQ(vc["voter"].get_str(), h.wallet->voter_address().to_base58());
    CHECK(host_call(layer, "app", "hivemind.ballots", "{}")["result"].isArray());
    // The phrase is shown only to a user that names it (no privilege stands for it).
    bool refused{false};
    try {
        host_call(layer, "chainonly", "dropin.seed.show", "{}");
    } catch (const bt::appui::RpcError&) {
        refused = true;
    }
    CHECK(refused);
    layer.stop();
}

TEST(hm_appui_spends_refused_before_any_question)
{
    Harness h("refuse");
    h.wallet->set_seed_from_mnemonic(ABANDON);
    AppLayer layer(*h.app, "regtest", h.dir, options());
    std::string err;
    CHECK(layer.start(err));
    const std::string a{"4XrMw8bmnhchFspSyP4XRUuB5vaR"}, b{"4TXCaVb28BDdZbpF93Gg4GDMhxd9"}, main{"bcrt1qqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5phstwt"};
    // Sums that wrap past their first part, or pass 21,000,000 ECX.
    CHECK(refused_with(layer, "create_withdrawal", "[\"" + main + "\", 9223372036854775808, 9223372036854775808, 9223372036854775809]", "amount overflow"));
    CHECK(refused_with(layer, "create_withdrawal", "[\"" + main + "\", 18446744073709551606, 5, 20]", "amount overflow"));
    CHECK(refused_with(layer, "create_transfer", "[\"" + a + "\", 18446744073709551615, 1]", "amount overflow"));
    CHECK(refused_with(layer, "create_transfer", "[\"" + a + "\", 2100000000000001, 0]", "amount overflow"));
    CHECK(refused_with(layer, "create_transfer_many", "[{\"" + a + "\": 9223372036854775808, \"" + b + "\": 9223372036854775808}, 1000]", "amount overflow"));
    CHECK(refused_with(layer, "create_deposit", "[\"" + a + "\", 18446744073709551615, 1000]", "amount overflow"));
    CHECK(refused_with(layer, "mine", "[2100000000000001]", "amount overflow"));
    // What the wallet cannot pay.
    CHECK(refused_with(layer, "create_transfer", "[\"" + a + "\", 1000, 10]", "Not enough"));
    // Votecoin: a number, finite, above nothing, with at most 10 decimals, held by the voter address.
    CHECK(refused_with(layer, "transfer_votecoin", "[\"" + a + "\", 1e400, 1000]", "finite"));
    CHECK(refused_with(layer, "transfer_votecoin", "[\"" + a + "\", \"0.1\", 1000]", "a number"));
    CHECK(refused_with(layer, "transfer_votecoin", "[\"" + a + "\", -0.5, 1000]", "more than nothing"));
    CHECK(refused_with(layer, "transfer_votecoin", "[\"" + a + "\", 0.12345678901234, 1000]", "10 decimals"));
    CHECK(refused_with(layer, "transfer_votecoin", "[\"" + a + "\", 0.25, 1000]", "Not enough Votecoin"));
    // Ballots: finite values, decisions that exist and are being voted on, one period, no decision twice.
    CHECK(refused_with(layer, "vote_submit", R"([[{"decision_id":"800001","vote_value":1e400}], 1000])", "finite"));
    CHECK(refused_with(layer, "vote_submit", R"([[], 1000])", "1 to 1000"));
    CHECK(refused_with(layer, "vote_submit", R"([[{"decision_id":"zz","vote_value":1}], 1000])", "Not a decision id"));
    CHECK(refused_with(layer, "vote_submit", R"([[{"decision_id":"800001","vote_value":1}], 1000])", "not claimed"));
    CHECK(refused_with(layer, "vote_submit", R"([[{"decision_id":"800001","vote_value":1},{"decision_id":"800001","vote_value":0}], 1000])", "twice"));
    CHECK(refused_with(layer, "vote_submit", R"([[{"decision_id":"800001","vote_value":1},{"decision_id":"810001","vote_value":0}], 1000])", "one period"));
    // Trades: no dry run as a spend, a share count in range, a market that exists.
    CHECK(refused_with(layer, "market_buy", R"([{"market_id":"000000000000","outcome_index":0,"shares_amount":10,"max_cost":1000,"dry_run":true}])", "quote"));
    CHECK(refused_with(layer, "market_buy", R"([{"market_id":"000000000000","outcome_index":0,"shares_amount":9223372036854775808,"max_cost":1000}])", "too large"));
    CHECK(refused_with(layer, "market_buy", R"([{"market_id":"000000000000","outcome_index":0,"shares_amount":0,"max_cost":1000}])", "at least one share"));
    CHECK(refused_with(layer, "market_buy", R"([{"market_id":"000000000000","outcome_index":0,"shares_amount":-5,"max_cost":1000}])", "whole number"));
    CHECK(refused_with(layer, "market_buy", R"([{"market_id":"000000000000","outcome_index":0,"shares_amount":10,"max_cost":1000}])", "No such market"));
    CHECK(refused_with(layer, "market_sell", "[{\"market_id\":\"000000000000\",\"outcome_index\":0,\"shares_amount\":10,\"seller_address\":\"" + a + "\",\"min_proceeds\":0}]", "No such market"));
    CHECK(refused_with(layer, "market_amplify_beta", R"([{"market_id":"000000000000","amount_sats":1000}])", "No such market"));
    // Decisions and markets: finite numbers, a period with slots, both beta and liquidity refused.
    CHECK(refused_with(layer, "decision_claim", R"([{"decision_type":"scaled","decisions":[{"period_index":0,"header":"T"}],"min":1e400,"max":2,"tx_fee_sats":1000}])", "finite"));
    CHECK(refused_with(layer, "decision_claim", R"([{"decision_type":"binary","decisions":[{"period_index":200,"header":"T"}],"tx_fee_sats":1000}])", "127"));
    CHECK(refused_with(layer, "decision_claim", R"([{"decision_type":"binary","decisions":[{"period_index":5,"header":"T"}],"tx_fee_sats":1000}])", "No decision slots"));
    CHECK(refused_with(layer, "decision_claim", R"([{"decision_type":"wrong","decisions":[{"period_index":0,"header":"T"}],"tx_fee_sats":1000}])", "binary, scaled or category"));
    // Text the trusted dialog shows (a question, a title, a label): core's display_text_problem, a description may break lines.
    CHECK(refused_with(layer, "decision_claim", R"([{"decision_type":"binary","decisions":[{"period_index":0,"header":"Yes\u202eoN"}],"tx_fee_sats":1000}])", "U+202E"));
    CHECK(refused_with(layer, "decision_claim", R"([{"decision_type":"binary","decisions":[{"period_index":0,"header":"T","option_1_label":"a\u200bb"}],"tx_fee_sats":1000}])", "U+200B"));
    CHECK(refused_with(layer, "market_create", R"([{"title":"t\nu","description":"d","dimensions":[{"type":"existing","id":"800001"}],"tx_fee_sats":1000}])", "U+000A"));
    CHECK(refused_with(layer, "market_create", R"([{"title":"t","description":"line one\nline two","dimensions":[{"type":"existing","id":"800001"}],"tx_fee_sats":1000}])", "not claimed"));
    CHECK(refused_with(layer, "market_create", R"([{"title":"t","description":"d","dimensions":[{"type":"existing","id":"800001"}],"beta":7,"initial_liquidity":100,"tx_fee_sats":1000}])", "not both"));
    CHECK(refused_with(layer, "market_create", R"([{"title":"t","description":"d","dimensions":[{"type":"existing","id":"800001"}],"trading_fee":1e400,"tx_fee_sats":1000}])", "finite"));
    CHECK(refused_with(layer, "market_create", R"([{"title":"t","description":"d","dimensions":[{"type":"existing","id":"800001"}],"tx_fee_sats":1000}])", "not claimed"));
    // Quotes: a share count in range, a market that exists.
    bool refused{false};
    try {
        host_call(layer, "app", "hivemind.quote", R"({"market":"000000000000","outcome":0,"shares":10})");
    } catch (const bt::appui::RpcError& e) {
        refused = std::string{e.what()}.find("No such market") != std::string::npos;
    }
    CHECK(refused);
    layer.stop();
}

TEST(hm_appui_manifest_is_the_nodes)
{
    Harness h("manifest");
    AppLayer layer(*h.app, "regtest", h.dir, options());
    std::string err;
    CHECK(layer.start(err));
    std::ifstream f(std::string{BT_SOURCE_DIR} + "/app/manifest.json");
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    UniValue file;
    CHECK(file.read(text));
    const UniValue node{layer.manifest()};
    for (const char* group : {"reads", "confirmed", "host", "protocol"}) {
        std::set<std::string> a, b;
        for (const UniValue& v : file[group].getValues()) a.insert(v.get_str());
        for (const UniValue& v : node[group].getValues()) b.insert(v.get_str());
        if (a != b) std::fprintf(stderr, "  manifest %s differs\n", group);
        CHECK(a == b);
    }
    // Every read and protocol method is served by the app RPC; every confirmed method is a LayerTwo method.
    for (const UniValue& v : file["reads"].getValues()) CHECK(layer.server().has(v.get_str()));
    for (const UniValue& v : file["protocol"].getValues()) CHECK(layer.server().has(v.get_str()));
    bt::jsonrpc::Server l2l;
    register_rpc(l2l, *h.app, true, true);
    const auto methods{l2l.methods()};
    for (const UniValue& v : file["confirmed"].getValues()) CHECK(std::find(methods.begin(), methods.end(), v.get_str()) != methods.end());
    CHECK_EQ(file["program"].get_str(), std::string{"truthcoin"});
    CHECK_EQ(file["datadirName"].get_str(), std::string{"truthcoin_dc"});
    layer.stop();
}
