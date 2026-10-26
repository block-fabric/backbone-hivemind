// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <hivemind/appui.h>
#include <hivemind/fmt.h>
#include <hivemind/json.h>
#include <hivemind/math.h>

#include "state_internal.h"

#include <bt/appui/phrase.h>
#include <bt/appui/text.h>
#include <bt/fsperm.h>
#include <bt/hd.h>
#include <bt/log.h>
#include <bt/parse.h>

#include <sodium.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace hivemind {

using bt::appui::Confirmed;
using bt::appui::HostEnv;
using bt::appui::HostError;
using bt::appui::Question;
using bt::appui::Row;
using bt::appui::RpcError;
using bt::kv::Txn;

namespace {
const char* WALLET_NAME = "main";
/** The LayerTwo read that stands for knowing the wallet's addresses: what tells a caller which outputs, positions,
 *  votes and Votecoin are the wallet's. */
const char* WALLET_READ = "get_wallet_addresses";
const char* VERSION_TEXT = "0.1.0 (truthcoin 0.20.0 compatible)";

const UniValue NULLV;
const UniValue& param(const UniValue& p, const std::string& key) { return p.isObject() ? p.find_value(key) : NULLV; }

std::string str_param(const UniValue& p, const std::string& key)
{
    const UniValue& v{param(p, key)};
    if (!v.isStr()) throw HostError(bt::appui::HOST_INVALID_REQUEST, key + ": a string is needed");
    return v.get_str();
}

/** A whole number of JSON (no sign, no fraction, no exponent), at most UINT64_MAX. */
uint64_t u64_of(const UniValue& v, const std::string& what)
{
    if (!v.isNum()) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": a whole number is needed");
    const std::string s{v.getValStr()};
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": a whole number is needed");
    auto n{bt::parse_u64(s)};
    if (!n) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": too large");
    return *n;
}

/** A positive count of shares (LayerTwo takes an i64): 1 .. INT64_MAX. */
int64_t shares_of(const UniValue& v, const std::string& what)
{
    const uint64_t n{u64_of(v, what)};
    if (n == 0) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": at least one share");
    if (n > uint64_t(INT64_MAX)) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": too large");
    return static_cast<int64_t>(n);
}

/** A finite JSON number (a vote, Votecoin, a beta, a fee rate, the ends of a scale): never NaN or infinite. */
double f64_of(const UniValue& v, const std::string& what)
{
    if (!v.isNum()) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": a number is needed");
    double d{0};
    try {
        d = json::parse_f64(v);
    } catch (const std::exception&) {
        throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": a number is needed");
    }
    if (!std::isfinite(d)) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": a finite number is needed");
    return d;
}

/** A text of a request, at most `max_bytes`: what core's display_text_problem takes (valid UTF-8, no control, format,
 *  separator, private-use, unassigned or default-ignorable code point: what a trusted dialog may show as it is); a
 *  multi-line one (a description) may also break lines. LayerTwo's rules take any text; our app's spends take these. */
std::string text_of(const UniValue& v, const std::string& what, size_t max_bytes, bool multiline = false)
{
    if (!v.isStr()) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": a text is needed");
    if (v.get_str().size() > max_bytes) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": at most " + std::to_string(max_bytes) + " bytes");
    std::string check{v.get_str()};
    if (multiline) std::replace(check.begin(), check.end(), '\n', ' ');
    if (const std::string why{bt::appui::display_text_problem(check)}; !why.empty()) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": " + why);
    return v.get_str();
}

std::string coins(uint64_t sats) { return bt::appui::format_sats(sats) + " ECX"; }

std::string net_label(const std::string& network) { return network == "betanet" ? "" : network; }

/** Whether a mainchain address is of the mainchain network this node follows (betanet and forknet follow eCash's
 *  betanet, whose addresses are Bitcoin's main ones; regtest's are regtest ones, signet's test ones). */
bool main_address_of_network(const bt::btc::Address& a, const std::string& network)
{
    using A = bt::btc::Address;
    if (a.kind == A::Kind::Segwit) {
        if (network == "regtest") return a.hrp == A::Hrp::Regtest;
        if (network == "signet") return a.hrp == A::Hrp::Testnets;
        return a.hrp == A::Hrp::Mainnet;
    }
    if (network == "regtest" || network == "signet") return a.net == A::NetKind::Test;
    return a.net == A::NetKind::Main;
}

UniValue n64(uint64_t v) { return UniValue(v); }
UniValue fnum(double d) { return std::isfinite(d) ? UniValue(d) : UniValue{}; }

/** Votecoin as text: up to 10 decimals (the chain's precision), no trailing zeros. */
std::string votecoin_text(double v)
{
    if (!std::isfinite(v)) return "?";
    char b[64];
    std::snprintf(b, sizeof b, "%.10f", v);
    std::string s{b};
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

std::string pct_text(double p)
{
    if (!std::isfinite(p)) return "?";
    char b[32];
    std::snprintf(b, sizeof b, "%.1f%%", p * 100.0);
    return b;
}

/** A number of a scale as the chain's Display writes it. */
std::string num_text(double v) { return fmt::display(v); }

std::string count_text(uint64_t n)
{
    std::string s{std::to_string(n)};
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), " ");
    return s;
}

const char* market_state_word(MarketState s)
{
    switch (s) {
    case MarketState::Trading: return "trading";
    case MarketState::Cancelled: return "cancelled";
    case MarketState::Invalid: return "invalid";
    case MarketState::Settled: return "settled";
    }
    return "trading";
}

const char* decision_state_word(DecisionState s)
{
    switch (s) {
    case DecisionState::Created: return "created";
    case DecisionState::Claimed: return "claimed";
    case DecisionState::Voting: return "voting";
    case DecisionState::Resolved: return "resolved";
    case DecisionState::Invalid: return "invalid";
    }
    return "claimed";
}

std::optional<MarketId> market_id_of(const std::string& s)
{
    auto b{bt::hex_decode(s)};
    if (!b || b->size() != 6) return std::nullopt;
    MarketId m;
    std::copy(b->begin(), b->end(), m.b.begin());
    return m;
}

std::optional<DecisionId> decision_id_of(const std::string& s) { return DecisionId::from_hex(s); }

Address author_of(const Decision& d)
{
    Address a;
    a.b = d.market_maker_pubkey_hash;
    return a;
}

/** The value of one dimension of an outcome in words (market.rs describe_outcome, one part). */
std::string dimension_value(const Decision& d, uint64_t pos)
{
    if (d.is_categorical()) {
        auto& labels{d.decision_type.options};
        return pos < labels.size() ? labels[pos] : std::string{"Inconclusive"};
    }
    if (d.is_scaled()) return pos == 0 ? num_text(d.decision_type.min) + " (Min)" : pos == 1 ? num_text(d.decision_type.max) + " (Max)" : "Abstain";
    auto [l0, l1] = d.binary_labels();
    return pos == 0 ? l0 : pos == 1 ? l1 : "Abstain";
}

/** A ballot's answer in words: the label of the option or the number of the scale. */
std::string answer_text(const Decision& d, double v)
{
    if (d.is_categorical()) {
        const size_t n{d.decision_type.options.size()};
        const size_t i{static_cast<size_t>(v)};
        return i < n ? d.decision_type.options[i] : std::string{"Inconclusive"};
    }
    if (d.is_scaled()) return num_text(v);
    auto [l0, l1] = d.binary_labels();
    if (v == 1.0) return l1;
    if (v == 0.0) return l0;
    return "Unsure (" + num_text(v) + ")";
}
} // namespace

struct AppLayer::Impl {
    App& app;
    Node& node;
    Wallet& wallet;
    std::string network, datadir;
    bt::appui::AppOptions options;
    bt::jsonrpc::Server l2l;
    std::unique_ptr<bt::appui::AppStore> store;
    std::unique_ptr<bt::appui::LinkMonitor> link;
    std::unique_ptr<bt::appui::Poller> poller;
    bt::appui::PendingPhrases phrases;
    std::chrono::system_clock::time_point started{std::chrono::system_clock::now()};

    Impl(App& a, std::string net, std::string dir, bt::appui::AppOptions o)
        : app(a), node(a.node), wallet(a.wallet), network(std::move(net)), datadir(std::move(dir)), options(std::move(o))
    {
        // Every LayerTwo method, on a server that does not listen: the reads the pages make and the spends run after
        // a yes go through the very handlers (and JSON) of the LayerTwo RPC.
        register_rpc(l2l, app, true, true);
    }

    /** One LayerTwo method, through its own handler: its result, or its error as an app RPC error. */
    UniValue l2l_call(const std::string& method, const UniValue& params)
    {
        UniValue req{UniValue::VOBJ};
        req.pushKV("jsonrpc", "2.0");
        req.pushKV("id", 1);
        req.pushKV("method", method);
        req.pushKV("params", params.isNull() ? UniValue{UniValue::VARR} : params);
        UniValue resp;
        if (!resp.read(l2l.handle(req.write())) || !resp.isObject()) throw RpcError(bt::appui::RPC_INTERNAL_ERROR, "no answer from " + method);
        const UniValue& err{resp.find_value("error")};
        if (err.isObject()) {
            const UniValue& data{err.find_value("data")};
            throw RpcError(err.find_value("code").isNum() ? err.find_value("code").getInt<int>() : -1,
                           err.find_value("message").getValStr() + (data.isStr() ? ": " + data.get_str() : std::string{}));
        }
        return resp.find_value("result");
    }

    std::set<Address> addresses()
    {
        if (!wallet.has_seed()) return {};
        return wallet.addresses();
    }

    template <class F>
    auto state(F f)
    {
        return node.with_state(f);
    }

    std::optional<std::pair<uint32_t, Hash32>> block_of(const Hash32& block)
    {
        return state([&](const State&, const Archive& a, const Txn& t) -> std::optional<std::pair<uint32_t, Hash32>> {
            auto h{a.height(t, block)};
            auto hd{a.header(t, block)};
            if (!h || !hd) return std::nullopt;
            return std::make_pair(*h, hd->prev_main_hash);
        });
    }

    /** The time of a mainchain block (its header, as the node stored it; else as the link asks for it). */
    std::optional<uint64_t> main_time(const Hash32& main)
    {
        auto ts{state([&](const State&, const Archive& a, const Txn& t) -> std::optional<uint64_t> {
            if (auto m{a.main_header(t, main)}) return m->timestamp;
            return std::nullopt;
        })};
        if (ts) return ts;
        return link ? link->block_time(main) : std::nullopt;
    }

    std::optional<std::pair<Hash32, std::optional<uint32_t>>> followed()
    {
        auto tip{node.tip()};
        if (!tip) return std::nullopt;
        auto hd{node.header(*tip)};
        if (!hd) return std::nullopt;
        return std::make_pair(hd->prev_main_hash, link ? link->block_height(hd->prev_main_hash) : std::nullopt);
    }

    // ---------------------------------------------------------- wallet
    struct Balances {
        uint64_t available{0}, withdrawing{0}, pending_in{0}, pending_out{0};
    };

    Balances balances()
    {
        Balances b;
        if (!wallet.has_seed()) return b;
        const Balance w{wallet.balance()};
        b.available = w.available;
        b.pending_in = w.unconfirmed;
        const auto spent{wallet.mempool_spent()};
        for (auto& [op, o] : wallet.utxos()) {
            if (spent.contains(op)) {
                b.pending_out += o.get_value();
                continue;
            }
            if (o.content.kind == Content::Withdrawal) b.withdrawing += o.get_value();
        }
        return b;
    }

    std::string wallet_id() { return seed_fingerprint(); }

    /** bt::appui::seed_fingerprint of the wallet's seed (the same definition), "" without one. */
    std::string seed_fingerprint()
    {
        auto d{wallet.seed_digest("sidechain/dropin/fingerprint/v1")};
        return d ? bt::hex_encode(bt::ByteSpan{d->data(), 8}) : std::string{};
    }

    /** Set the wallet's seed from a phrase checked before; the phrase kept for the backup (owner-only store). */
    UniValue set_phrase(const std::string& phrase, const std::string& from)
    {
        auto p{bt::hd::parse_mnemonic(phrase)};
        if (!p) throw HostError(bt::hd::mnemonic_error(phrase));
        const bt::appui::WipeOnExit wipe_parsed{*p};
        auto seed{bt::hd::mnemonic_to_seed(*p)};
        const std::string fp{bt::appui::seed_fingerprint(seed.span())};
        try {
            wallet.set_seed(seed);
        } catch (const std::exception& e) {
            sodium_memzero(seed.data(), seed.size());
            throw HostError(bt::appui::RPC_WALLET_ERROR, std::string{"This wallet has a backup phrase already ("} + e.what() + ")");
        }
        sodium_memzero(seed.data(), seed.size());
        store->put("phrase", *p);
        store->put("phrase.from", from);
        if (poller) poller->poke();
        UniValue out{UniValue::VOBJ};
        out.pushKV("fingerprint", fp);
        return out;
    }

    /** The phrase the app layer kept, only when it is the one of the wallet's seed now (a wallet file replaced since: none). */
    std::optional<std::string> kept_phrase()
    {
        auto phrase{store->get("phrase")};
        if (!phrase || !wallet.has_seed()) return std::nullopt;
        auto p{bt::hd::parse_mnemonic(*phrase)};
        if (!p) {
            sodium_memzero(phrase->data(), phrase->size());
            return std::nullopt;
        }
        auto seed{bt::hd::mnemonic_to_seed(*p)};
        sodium_memzero(p->data(), p->size());
        const bool same{bt::appui::seed_fingerprint(seed.span()) == seed_fingerprint()};
        sodium_memzero(seed.data(), seed.size());
        if (!same) {
            sodium_memzero(phrase->data(), phrase->size());
            return std::nullopt;
        }
        return phrase;
    }

    UniValue seed_status()
    {
        UniValue s{UniValue::VOBJ};
        const bool has{wallet.has_seed()};
        std::optional<std::string> kept{has ? kept_phrase() : std::nullopt};
        s.pushKV("hasSeed", has);
        s.pushKV("phraseKept", kept.has_value());
        if (kept) sodium_memzero(kept->data(), kept->size());
        s.pushKV("from", has ? store->get("phrase.from").value_or("outside") : "");
        s.pushKV("fingerprint", has ? seed_fingerprint() : "");
        s.pushKV("derivation", "truthcoin 0.20.0");
        return s;
    }

    static std::string words_block(const std::string& phrase)
    {
        std::string out;
        size_t n{0}, at{0};
        while (at < phrase.size()) {
            size_t sp{phrase.find(' ', at)};
            if (sp == std::string::npos) sp = phrase.size();
            ++n;
            out += (n < 10 ? " " : "") + std::to_string(n) + ". " + phrase.substr(at, sp - at) + (n % 3 == 0 ? "\n" : "    ");
            at = sp + 1;
        }
        return out;
    }

    // ---------------------------------------------------------- markets and decisions as the pages see them
    struct MarketNames {
        std::string title;
        std::vector<std::string> outcomes;
    };

    std::map<DecisionId, Decision> market_decisions(const State& st, const Txn& t, const Market& m)
    {
        std::map<DecisionId, Decision> r;
        for (auto& id : m.decision_ids)
            if (auto e{st.decision_entry(t, id)}; e && e->decision) r[id] = *e->decision;
        return r;
    }

    static double market_beta(const Market& m) { return math::derive_beta_from_liquidity(m.liquidity_base_sats, m.shares.size()); }

    static std::vector<double> prices_or_uniform(const std::vector<int64_t>& shares, double beta)
    {
        try {
            if (auto p{math::prices(shares, beta, nullptr)}) return *p;
        } catch (const std::exception&) {
        }
        const size_t n{shares.size()};
        return n ? std::vector<double>(n, 1.0 / double(n)) : std::vector<double>{};
    }

    /** The words of each tradeable outcome of a market: "<decision>: <value>" a dimension, joined. */
    std::vector<std::string> outcome_labels(const Market& m, const std::map<DecisionId, Decision>& ds)
    {
        std::vector<std::string> out;
        for (auto& [full, combo] : m.valid_state_combos()) {
            std::string s;
            for (size_t i{0}; i < m.decision_ids.size() && i < combo->size(); ++i) {
                auto it{ds.find(m.decision_ids[i])};
                if (it == ds.end()) {
                    s = "Outcome " + std::to_string(full);
                    break;
                }
                s += (i ? ", " : "") + (m.decision_ids.size() > 1 ? it->second.header + ": " : std::string{}) + dimension_value(it->second, (*combo)[i]);
            }
            out.push_back(s.empty() ? "Outcome " + std::to_string(full) : s);
        }
        return out;
    }

    MarketNames names_of(const MarketId& id)
    {
        return state([&](const State& st, const Archive&, const Txn& t) {
            MarketNames n;
            if (auto m{st.market(t, id)}) {
                n.title = m->title;
                n.outcomes = outcome_labels(*m, market_decisions(st, t, *m));
            }
            return n;
        });
    }

    UniValue decision_type_json(const Decision& d)
    {
        UniValue o{UniValue::VOBJ};
        if (d.is_scaled()) {
            o.pushKV("kind", "scaled");
            o.pushKV("min", fnum(d.decision_type.min));
            o.pushKV("max", fnum(d.decision_type.max));
            o.pushKV("increment", fnum(d.decision_type.increment));
            o.pushKV("lowLabel", d.option_0_label.value_or("Lower basis"));
            o.pushKV("highLabel", d.option_1_label.value_or("Upper basis"));
        } else if (d.is_categorical()) {
            o.pushKV("kind", "category");
            UniValue opts{UniValue::VARR};
            for (auto& s : d.decision_type.options) opts.push_back(s);
            o.pushKV("options", opts);
        } else {
            auto [l0, l1] = d.binary_labels();
            o.pushKV("kind", "binary");
            o.pushKV("noLabel", l0);
            o.pushKV("yesLabel", l1);
        }
        return o;
    }

    /** A claimed decision: what it asks, its period, its state, its outcome once resolved, its markets. */
    UniValue decision_json(const State& st, const Txn& t, const DecisionEntry& e, const std::optional<std::set<Address>>& mine)
    {
        const Decision& d{*e.decision};
        UniValue o{UniValue::VOBJ};
        o.pushKV("id", e.decision_id.to_hex());
        o.pushKV("period", static_cast<uint64_t>(e.decision_id.period_index()));
        o.pushKV("index", static_cast<uint64_t>(e.decision_id.decision_index()));
        o.pushKV("votingPeriod", static_cast<uint64_t>(e.decision_id.voting_period()));
        o.pushKV("standard", e.decision_id.is_standard());
        o.pushKV("header", d.header);
        o.pushKV("description", d.description);
        UniValue tags{UniValue::VARR};
        for (auto& s : d.tags) tags.push_back(s);
        o.pushKV("tags", tags);
        o.pushKV("type", decision_type_json(d));
        o.pushKV("author", author_of(d).to_base58());
        o.pushKV("claimTxid", bt::hex_encode(e.claiming_txid));
        const DecisionState s{st.decision_state(t, e.decision_id)};
        o.pushKV("state", decision_state_word(s));
        if (auto h{st.decision_history(t, e.decision_id)}) {
            if (auto at{h->state_height(DecisionState::Resolved)}) o.pushKV("resolvedHeight", static_cast<uint64_t>(*at));
        }
        if (auto out{st.decision_outcome(t, e.decision_id)}) {
            UniValue r{UniValue::VOBJ};
            r.pushKV("value", fnum(out->outcome_value));
            r.pushKV("answer", fnum(d.denormalize_value(out->outcome_value)));
            r.pushKV("confidence", fnum(out->confidence));
            r.pushKV("votes", n64(out->total_votes));
            if (out->categorical_winner) {
                if (*out->categorical_winner) r.pushKV("winner", static_cast<uint64_t>(**out->categorical_winner));
                else r.pushKV("winner", UniValue{});
            }
            o.pushKV("outcome", r);
        }
        UniValue markets{UniValue::VARR};
        for (auto& m : st.markets_by_decision(t, e.decision_id)) markets.push_back(m.to_hex());
        o.pushKV("markets", markets);
        if (mine) o.pushKV("mine", mine->contains(author_of(d)));
        return o;
    }

    /** A market: its decisions, its outcomes with their prices (now, or its last before it settled), what it paid. */
    UniValue market_json(const State& st, const Txn& t, const Market& m, const std::optional<std::set<Address>>& mine)
    {
        auto ds{market_decisions(st, t, m)};
        const double beta{market_beta(m)};
        auto mshares{m.market_state == MarketState::Trading ? st.mempool_shares(t, m.id) : std::nullopt};
        const auto& shares{mshares ? *mshares : m.shares};
        const auto prices{prices_or_uniform(shares, beta)};
        const auto labels{outcome_labels(m, ds)};
        UniValue o{UniValue::VOBJ};
        o.pushKV("id", m.id.to_hex());
        o.pushKV("title", m.title);
        o.pushKV("description", m.description);
        UniValue tags{UniValue::VARR};
        for (auto& s : m.tags) tags.push_back(s);
        o.pushKV("tags", tags);
        o.pushKV("creator", m.creator_address.to_base58());
        o.pushKV("createdHeight", static_cast<uint64_t>(m.created_at_height));
        o.pushKV("expiresHeight", m.expires_at_height ? UniValue(static_cast<uint64_t>(*m.expires_at_height)) : UniValue{});
        o.pushKV("state", market_state_word(m.market_state));
        o.pushKV("updatedHeight", static_cast<uint64_t>(m.last_updated_height));
        if (m.market_state == MarketState::Settled) o.pushKV("settledHeight", static_cast<uint64_t>(m.last_updated_height));
        o.pushKV("beta", fnum(beta));
        o.pushKV("liquidity", n64(m.liquidity_base_sats));
        o.pushKV("treasury", n64(st.market_funds_sats(t, m.id, false)));
        o.pushKV("fees", n64(st.market_funds_sats(t, m.id, true)));
        o.pushKV("volume", n64(m.total_volume_sats));
        o.pushKV("feeRate", fnum(m.trading_fee));
        o.pushKV("txPow", m.txpow_enabled());
        UniValue decisions{UniValue::VARR};
        for (auto& id : m.decision_ids) {
            UniValue d{UniValue::VOBJ};
            d.pushKV("id", id.to_hex());
            auto it{ds.find(id)};
            d.pushKV("header", it != ds.end() ? it->second.header : "");
            if (it != ds.end()) d.pushKV("type", decision_type_json(it->second));
            d.pushKV("state", decision_state_word(st.decision_state(t, id)));
            if (auto out{st.decision_outcome(t, id)}; out && it != ds.end()) {
                d.pushKV("outcome", fnum(out->outcome_value));
                d.pushKV("answer", fnum(it->second.denormalize_value(out->outcome_value)));
            }
            d.pushKV("votingPeriod", static_cast<uint64_t>(id.voting_period()));
            decisions.push_back(d);
        }
        o.pushKV("decisions", decisions);
        UniValue outcomes{UniValue::VARR};
        const auto valid{m.valid_state_combos()};
        for (size_t i{0}; i < valid.size(); ++i) {
            UniValue x{UniValue::VOBJ};
            x.pushKV("index", static_cast<uint64_t>(i));
            x.pushKV("label", i < labels.size() ? labels[i] : "Outcome " + std::to_string(i));
            UniValue parts{UniValue::VARR}, coords{UniValue::VARR};
            for (size_t k{0}; k < valid[i].second->size(); ++k) {
                coords.push_back(n64((*valid[i].second)[k]));
                auto it{k < m.decision_ids.size() ? ds.find(m.decision_ids[k]) : ds.end()};
                parts.push_back(it != ds.end() ? dimension_value(it->second, (*valid[i].second)[k]) : std::string{"?"});
            }
            x.pushKV("parts", parts);
            x.pushKV("coordinates", coords);
            x.pushKV("price", fnum(i < prices.size() ? prices[i] : 0.0));
            x.pushKV("shares", UniValue(static_cast<int64_t>(i < m.shares.size() ? m.shares[i] : 0)));
            x.pushKV("volume", n64(i < m.outcome_volumes_sats.size() ? m.outcome_volumes_sats[i] : 0));
            if (m.market_state == MarketState::Settled && i < m.final_prices.size()) x.pushKV("finalPrice", fnum(m.final_prices[i]));
            outcomes.push_back(x);
        }
        o.pushKV("outcomes", outcomes);
        if (mine) o.pushKV("mine", mine->contains(m.creator_address));
        return o;
    }

    using Mine = std::optional<std::set<Address>>;
    Mine mine_for(HostEnv& env) { return env.may_call(WALLET_READ) ? Mine{addresses()} : std::nullopt; }

    /** The period clock: the period now, its bounds (blocks on a chain counted in blocks, else seconds), the next. */
    UniValue period_json(const State& st, const Txn& t)
    {
        auto [ts, h, g] = st.period_context(t);
        const auto& cfg{st.config()};
        const uint32_t cur{get_current_period(ts, h, g, cfg)};
        auto [start, end] = period_boundaries(cur, cfg, g);
        UniValue o{UniValue::VOBJ};
        o.pushKV("current", static_cast<uint64_t>(cur));
        o.pushKV("name", period_to_name(cur));
        o.pushKV("unit", cfg.is_blocks ? "blocks" : "seconds");
        o.pushKV("length", static_cast<uint64_t>(cfg.quantity));
        o.pushKV("start", n64(start));
        o.pushKV("end", n64(end));
        o.pushKV("now", cfg.is_blocks ? n64(h ? uint64_t(*h) : 0) : n64(ts));
        o.pushKV("height", h ? UniValue(static_cast<uint64_t>(*h)) : UniValue{});
        return o;
    }

    // ---------------------------------------------------------- the trades of a market, priced as the node prices them
    struct Quote {
        uint64_t cost{0}, fee{0}, net{0};
        double price_before{0}, price_after{0};
    };

    /** market_buy's (dry_run) and market_sell's arithmetic, without the wallet: the same math, the same order. */
    Quote quote(const Market& m, size_t outcome, int64_t shares, bool buy)
    {
        auto cur{state([&](const State& st, const Archive&, const Txn& t) {
            auto ms{st.mempool_shares(t, m.id)};
            return ms ? *ms : m.shares;
        })};
        if (outcome >= cur.size()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "No such outcome in this market");
        const double beta{market_beta(m)};
        auto next{cur};
        int64_t moved{0};
        if (__builtin_add_overflow(next[outcome], buy ? shares : -shares, &moved)) throw HostError(bt::appui::HOST_INVALID_REQUEST, "amount overflow");
        next[outcome] = moved;
        Quote q;
        std::string err;
        try {
            const auto before{prices_or_uniform(cur, beta)};
            q.price_before = before[outcome];
            if (auto pr{math::prices(next, beta, nullptr)}; pr && outcome < pr->size()) q.price_after = (*pr)[outcome];
            if (buy) {
                auto cost{math::update_cost(cur, next, beta, &err)};
                if (!cost) throw HostError("The market cannot price this trade: " + err);
                auto bc{math::buy_cost(*cost, m.trading_fee, &err)};
                if (!bc) throw HostError("The market cannot price this trade: " + err);
                q.cost = bc->total_cost_sats;
                q.fee = bc->trading_fee_sats;
            } else {
                auto oldc{math::treasury(cur, beta, &err)};
                if (!oldc) throw HostError("The market cannot price this trade: " + err);
                auto newc{math::treasury(next, beta, &err)};
                if (!newc) throw HostError("The market cannot price this trade: " + err);
                auto sp{math::sell_proceeds(*oldc - *newc, m.trading_fee, &err)};
                if (!sp) throw HostError("The market cannot price this trade: " + err);
                q.cost = sp->gross_proceeds_sats;
                q.fee = sp->trading_fee_sats;
                q.net = sp->net_proceeds_sats;
            }
        } catch (const math::MathPanic& e) {
            throw HostError(std::string{"The market cannot price this trade: "} + e.what());
        }
        return q;
    }

    Market market_or_refuse(const UniValue& v)
    {
        if (!v.isStr()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "market_id: the market's id (12 hex digits)");
        auto id{market_id_of(v.get_str())};
        if (!id) throw HostError(bt::appui::HOST_INVALID_REQUEST, "market_id: the market's id (12 hex digits)");
        auto m{state([&](const State& st, const Archive&, const Txn& t) { return st.market(t, *id); })};
        if (!m) throw HostError("No such market");
        return *m;
    }

    /** The shares an address holds of an outcome of a market. */
    int64_t held(const Address& a, const MarketId& id, uint32_t outcome)
    {
        return state([&](const State& st, const Archive&, const Txn& t) -> int64_t {
            if (auto acc{st.share_account(t, a)}) {
                auto it{acc->positions.find({id, outcome})};
                if (it != acc->positions.end()) return it->second;
            }
            return 0;
        });
    }

    // ---------------------------------------------------------- history
    struct Hist {
        std::string id, kind;
        int64_t delta{0};
        uint64_t received{0}, spent{0}, withdrawing{0};
        bool pending{false}, ours_in{false};
        std::optional<Hash32> txid, m6id;
        std::vector<std::pair<std::string, uint64_t>> to; // outputs to others
        std::optional<uint64_t> fee;
        std::string main_address;
        //! The wallet's address it paid (what a receipt was received at).
        std::string own_address;
        //! Hivemind's: the market (id, title), the outcome and shares of a trade, the decisions claimed or voted on, Votecoin moved.
        std::string market, market_title, outcome_label;
        int64_t shares{0};
        std::vector<std::string> decisions;
        std::optional<double> votecoin;
        std::string role;
    };

    std::string seen_key(const std::string& id) { return "seen:" + id; }

    std::optional<std::pair<int64_t, int64_t>> seen(const std::string& id)
    {
        auto v{store->get(seen_key(id))};
        if (!v) return std::nullopt;
        const size_t c{v->find(':')};
        if (c == std::string::npos) return std::nullopt;
        return std::make_pair(std::atoll(v->substr(0, c).c_str()), std::atoll(v->substr(c + 1).c_str()));
    }

    void note_seen(const std::string& id)
    {
        if (store->get(seen_key(id))) return;
        auto h{node.height()};
        const int64_t now{std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()};
        store->put(seen_key(id), std::to_string(h ? int64_t(*h) : int64_t{-1}) + ":" + std::to_string(now));
    }

    /** What a transaction of the wallet is, from its data: a trade, a claim, a market, a ballot, Votecoin moved. */
    void classify(Hist& h, const Transaction& tx, const std::set<Address>& mine, std::map<OutPoint, std::string>& payout_rows)
    {
        if (!tx.data) return;
        const Hash32 txid{tx.txid()};
        std::visit(
            [&](const auto& d) {
                using T = std::decay_t<decltype(d)>;
                if constexpr (std::is_same_v<T, ClaimDecisionPayload>) {
                    h.kind = "claim";
                    for (auto& e : d.decisions) h.decisions.push_back(e.header);
                } else if constexpr (std::is_same_v<T, TxCreateMarket>) {
                    h.kind = "market";
                    h.market_title = d.title;
                    for (auto& c : d.new_claims)
                        for (auto& e : c.decisions) h.decisions.push_back(e.header);
                } else if constexpr (std::is_same_v<T, TxTrade>) {
                    h.kind = d.is_buy() ? "buy" : "sell";
                    h.market = d.market_id.to_hex();
                    const MarketNames n{names_of(d.market_id)};
                    h.market_title = n.title;
                    if (d.outcome_index < n.outcomes.size()) h.outcome_label = n.outcomes[d.outcome_index];
                    h.shares = d.shares;
                    // What the block that takes the trade pays back (a buy's change, a sale's proceeds): this row's.
                    payout_rows[buy_change_outpoint(d.market_id, d.trader, txid)] = h.id;
                    payout_rows[sell_payout_outpoint(d.market_id, d.trader, txid)] = h.id;
                    payout_rows[sell_input_change_outpoint(d.trader, txid)] = h.id;
                } else if constexpr (std::is_same_v<T, TxSubmitVote>) {
                    h.kind = "vote";
                    DecisionId id;
                    id.b = d.decision_id_bytes;
                    h.decisions.push_back(id.to_hex());
                } else if constexpr (std::is_same_v<T, TxSubmitBallot>) {
                    h.kind = "vote";
                    for (auto& v : d.votes) {
                        DecisionId id;
                        id.b = v.decision_id_bytes;
                        h.decisions.push_back(id.to_hex());
                    }
                } else if constexpr (std::is_same_v<T, TxTransferReputation>) {
                    h.kind = mine.contains(d.sender) ? "votecoin-sent" : "votecoin-received";
                    h.votecoin = d.amount;
                } else if constexpr (std::is_same_v<T, TxAmplifyBeta>) {
                    h.kind = "amplify";
                    h.market = d.market_id.to_hex();
                    h.market_title = names_of(d.market_id).title;
                }
            },
            *tx.data);
    }

    /** The settlement payouts of the wallet (the settlement records of the blocks that paid its markets). */
    void settlement_payouts(const std::set<Address>& mine, std::map<OutPoint, std::tuple<std::string, std::string, std::string>>& out)
    {
        state([&](const State& st, const Archive&, const Txn& t) {
            for (auto& m : st.markets_by_state(t, MarketState::Settled)) {
                auto v{t.get(State::T_SETTLEMENT_UNDO, u32_key(m.last_updated_height))};
                if (!v) continue;
                for (auto& e : dec_settlement_undo(*v)) {
                    const auto& s{e.payout_summary};
                    if (s.market_id != m.id) continue;
                    uint32_t seq{0};
                    for (auto& p : s.payouts) {
                        if (p.payout_sats > 0 && mine.contains(p.address)) out[share_payout_outpoint(s.market_id, p.address, s.block_height, seq)] = {m.id.to_hex(), m.title, "shares"};
                        ++seq;
                    }
                    for (auto& f : s.fee_payouts) {
                        if (mine.contains(f.address))
                            out[share_payout_outpoint(s.market_id, f.address, s.block_height, seq)] = {
                                m.id.to_hex(), m.title, f.fee_role == FeeRole::CorrectVoter ? "voter" : f.fee_role == FeeRole::DecisionAuthor ? "decision-author" : "market-author"};
                        ++seq;
                    }
                    if (s.creator_refund && mine.contains(s.creator_refund->first)) out[share_payout_outpoint(s.market_id, s.creator_refund->first, s.block_height, seq)] = {m.id.to_hex(), m.title, "refund"};
                }
            }
            return 0;
        });
    }

    std::map<std::string, Hist> gather(const std::set<Address>& mine)
    {
        std::map<std::string, Hist> rows;
        std::map<OutPoint, std::string> payout_rows;
        std::set<Hash32> classified;
        auto tx_row = [&](const Hash32& txid) -> Hist& {
            const std::string id{bt::hex_encode(txid)};
            Hist& h{rows[id]};
            if (h.id.empty()) {
                h.id = id;
                h.txid = txid;
            }
            if (classified.insert(txid).second) {
                if (auto t{node.transaction(txid)}) classify(h, t->first, mine, payout_rows);
                else if (auto mt{node.mempool_tx(txid)}) classify(h, mt->transaction, mine, payout_rows);
            }
            return h;
        };
        const auto utxos{node.utxos_by_addresses(mine)};
        const auto stxos{node.stxos_by_addresses(mine)};
        // The transactions of the wallet first (a trade names its payouts), then what they paid it.
        for (auto& [op, s] : stxos)
            if (s.inpoint.kind == InPoint::Regular) tx_row(s.inpoint.id);
        for (auto& [op, o] : utxos)
            if (op.kind == OutPoint::Regular) tx_row(op.txid);
        for (auto& [op, s] : stxos)
            if (op.kind == OutPoint::Regular) tx_row(op.txid);
        std::map<OutPoint, std::tuple<std::string, std::string, std::string>> settled;
        settlement_payouts(mine, settled);
        auto row_of = [&](const OutPoint& op) -> Hist& {
            std::string id, kind;
            if (op.kind == OutPoint::Deposit) {
                id = "deposit:" + bt::hex_encode_rev(op.txid.span());
                kind = "deposit";
            } else if (op.kind == OutPoint::Coinbase) {
                id = "coinbase:" + bt::hex_encode(op.txid);
                kind = "mined";
            } else if (op.kind == OutPoint::Payout) {
                if (auto it{payout_rows.find(op)}; it != payout_rows.end()) return rows[it->second];
                id = "payout:" + bt::hex_encode(op.txid);
                kind = "payout";
            } else if (op.kind == OutPoint::MarketFunds) {
                id = "funds:" + op.market_id.to_hex();
                kind = "market-funds";
            } else {
                return tx_row(op.txid);
            }
            Hist& h{rows[id]};
            if (h.id.empty()) {
                h.id = id;
                h.kind = kind;
                if (op.kind == OutPoint::Payout) {
                    if (auto it{settled.find(op)}; it != settled.end()) {
                        h.market = std::get<0>(it->second);
                        h.market_title = std::get<1>(it->second);
                        h.role = std::get<2>(it->second);
                    }
                }
            }
            return h;
        };
        auto credit = [&](const OutPoint& op, const Output& o) {
            Hist& h{row_of(op)};
            if (o.content.kind == Content::Withdrawal) {
                h.withdrawing += o.get_value();
                h.main_address = o.content.main_address.to_string();
            } else if (o.content.kind == Content::Value) {
                h.received += o.get_value();
                h.delta += static_cast<int64_t>(o.get_value());
                if (h.own_address.empty()) h.own_address = o.address.to_base58();
            }
        };
        for (auto& [op, o] : utxos) credit(op, o);
        for (auto& [op, s] : stxos) {
            credit(op, s.output);
            if (s.inpoint.kind == InPoint::Withdrawal) {
                const std::string id{"bundle:" + bt::hex_encode_rev(s.inpoint.id.span())};
                Hist& h{rows[id]};
                if (h.id.empty()) {
                    h.id = id;
                    h.kind = "bundle";
                    h.m6id = s.inpoint.id;
                }
                // Our withdrawal paid out of the chain into a bundle: counted when it was asked for.
                if (s.output.content.kind == Content::Withdrawal) h.withdrawing += s.output.get_value();
                else {
                    h.spent += s.output.get_value();
                    h.delta -= static_cast<int64_t>(s.output.get_value());
                }
                continue;
            }
            if (s.inpoint.kind != InPoint::Regular) continue;
            Hist& h{tx_row(s.inpoint.id)};
            h.ours_in = true;
            if (s.output.content.kind == Content::Value) {
                h.spent += s.output.get_value();
                h.delta -= static_cast<int64_t>(s.output.get_value());
            }
        }
        // What waits in the mempool: our spends and what comes in.
        for (const AuthorizedTransaction& atx : node.mempool()) {
            const Transaction& tx{atx.transaction};
            const Hash32 txid{tx.txid()};
            auto filled{node.filled_transaction(txid)};
            const std::vector<Output> spent{filled ? filled->first.second : std::vector<Output>{}};
            bool touches{false};
            for (const Output& s : spent) touches = touches || mine.contains(s.address);
            for (const Output& o : tx.outputs) touches = touches || mine.contains(o.address);
            if (const auto* tr{tx.data ? std::get_if<TxTrade>(&*tx.data) : nullptr}) touches = touches || mine.contains(tr->trader);
            if (!touches) continue;
            Hist& h{tx_row(txid)};
            h.pending = true;
            h.received = h.spent = h.withdrawing = 0;
            h.delta = 0;
            uint64_t in_total{0}, out_total{0};
            bool all_inputs_ours{true};
            for (const Output& s : spent) {
                in_total += s.get_value();
                if (mine.contains(s.address)) {
                    h.ours_in = true;
                    if (s.content.kind == Content::Value) {
                        h.spent += s.get_value();
                        h.delta -= static_cast<int64_t>(s.get_value());
                    }
                } else {
                    all_inputs_ours = false;
                }
            }
            h.to.clear();
            for (const Output& o : tx.outputs) {
                out_total += o.get_value();
                if (mine.contains(o.address)) {
                    if (o.content.kind == Content::Withdrawal) {
                        h.withdrawing += o.get_value();
                        h.main_address = o.content.main_address.to_string();
                    } else if (o.content.kind == Content::Value) {
                        h.received += o.get_value();
                        h.delta += static_cast<int64_t>(o.get_value());
                        if (h.own_address.empty()) h.own_address = o.address.to_base58();
                    }
                } else if (o.content.kind != Content::MarketFunds) {
                    h.to.emplace_back(o.address.to_base58(), o.get_value());
                }
            }
            if (all_inputs_ours && in_total >= out_total && !tx.data) h.fee = in_total - out_total;
        }
        return rows;
    }

    UniValue history(size_t limit)
    {
        const auto mine{addresses()};
        auto rows{gather(mine)};
        struct Out {
            UniValue v;
            int64_t order_height;
            int64_t order_time;
        };
        std::vector<Out> list;
        for (auto& [id, h] : rows) {
            note_seen(id);
            std::optional<uint32_t> height;
            std::optional<uint64_t> time;
            std::string block_hex;
            // A transaction of a block: where, when (the time of the mainchain block it was merge-mined in), its other
            // outputs and its fee.
            if (h.txid && !h.pending) {
                if (auto t{node.transaction(*h.txid)}) {
                    if (t->second) {
                        block_hex = bt::hex_encode(*t->second);
                        if (auto b{block_of(*t->second)}) {
                            height = b->first;
                            time = main_time(b->second);
                        }
                    }
                    h.to.clear();
                    uint64_t out_total{0};
                    for (const Output& o : t->first.outputs) {
                        out_total += o.get_value();
                        if (!mine.contains(o.address) && o.content.kind != Content::MarketFunds) h.to.emplace_back(o.address.to_base58(), o.get_value());
                    }
                    // Every input ours, a plain payment: its fee (a market transaction's cost is its own, in its row).
                    if (h.ours_in && !t->first.data && t->first.inputs.size() > 0 && h.spent >= out_total) h.fee = h.spent - out_total;
                }
            }
            bool approx{false};
            if (!height || !time) {
                if (auto s{seen(id)}) {
                    if (!height && s->first >= 0) height = static_cast<uint32_t>(s->first);
                    if (!time) time = static_cast<uint64_t>(s->second);
                    approx = true;
                }
            }
            std::string kind{h.kind};
            if (kind.empty()) {
                if (h.withdrawing > 0) kind = "withdrawal";
                else if (!h.ours_in) kind = "receive";
                else if (h.to.empty()) kind = "self";
                else kind = "send";
            }
            UniValue o{UniValue::VOBJ};
            o.pushKV("id", id);
            o.pushKV("kind", kind);
            if (h.txid) o.pushKV("txid", bt::hex_encode(*h.txid));
            if (h.m6id) {
                o.pushKV("m6id", bt::hex_encode_rev(h.m6id->span()));
                if (auto b{node.withdrawal_bundle(*h.m6id)}) o.pushKV("bundleStatus", to_string(b->second.latest().value));
            }
            o.pushKV("amount", static_cast<int64_t>(h.delta));
            o.pushKV("received", n64(h.received));
            o.pushKV("sent", n64(h.spent));
            if (h.withdrawing) o.pushKV("withdrawing", n64(h.withdrawing));
            if (!h.main_address.empty()) o.pushKV("mainAddress", h.main_address);
            if (!h.own_address.empty()) o.pushKV("ownAddress", h.own_address);
            if (h.fee) o.pushKV("fee", n64(*h.fee));
            UniValue to{UniValue::VARR};
            for (auto& [a, v] : h.to) {
                UniValue t{UniValue::VOBJ};
                t.pushKV("address", a);
                t.pushKV("amount", n64(v));
                to.push_back(t);
                if (to.size() >= 50) break;
            }
            o.pushKV("to", to);
            o.pushKV("recipients", static_cast<uint64_t>(h.to.size()));
            o.pushKV("pending", h.pending);
            o.pushKV("height", height ? UniValue(uint64_t(*height)) : UniValue{});
            o.pushKV("time", time ? UniValue(*time) : UniValue{});
            o.pushKV("approxTime", approx);
            if (!block_hex.empty()) o.pushKV("block", block_hex);
            if (!h.market.empty()) o.pushKV("market", h.market);
            if (!h.market_title.empty()) o.pushKV("marketTitle", h.market_title);
            if (!h.outcome_label.empty()) o.pushKV("outcomeLabel", h.outcome_label);
            if (h.shares) o.pushKV("shares", UniValue(h.shares < 0 ? -h.shares : h.shares));
            if (!h.decisions.empty()) {
                UniValue ds{UniValue::VARR};
                for (auto& d : h.decisions) ds.push_back(d);
                o.pushKV("decisions", ds);
            }
            if (h.votecoin) o.pushKV("votecoin", fnum(*h.votecoin));
            if (!h.role.empty()) o.pushKV("role", h.role);
            list.push_back(Out{o, h.pending ? INT64_MAX : height ? int64_t(*height) : -1, time ? int64_t(*time) : 0});
        }
        std::sort(list.begin(), list.end(), [](const Out& a, const Out& b) {
            if (a.order_height != b.order_height) return a.order_height > b.order_height;
            if (a.order_time != b.order_time) return a.order_time > b.order_time;
            return a.v.find_value("id").get_str() < b.v.find_value("id").get_str();
        });
        UniValue out{UniValue::VARR};
        for (auto& o : list) {
            if (out.size() >= limit) break;
            out.push_back(std::move(o.v));
        }
        return out;
    }

    // ---------------------------------------------------------- the node as events
    bt::appui::NodeLook look()
    {
        bt::appui::NodeLook l;
        l.height = node.height();
        if (auto t{node.tip()}) {
            l.tip = bt::hex_encode(*t);
            if (auto hd{node.header(*t)}) {
                if (auto tm{main_time(hd->prev_main_hash)}) l.tip_time = *tm;
            }
        }
        // In step with eCash: as fresh as eCash's tip (blocks of this chain come only when merge-mined); behind it, as old
        // as what this chain last acted on.
        if (link && link->connected() && link->info().find_value("state").getValStr() == "ok") {
            if (auto tm{link->tip_time()}) l.tip_time = std::max(l.tip_time, *tm);
        }
        for (auto& p : node.peers()) l.connections += p.connected ? 1 : 0;
        l.wallet = wallet.has_seed();
        if (l.wallet) {
            const auto mine{addresses()};
            const Balances b{balances()};
            l.trusted = b.available;
            l.pending = b.pending_in;
            l.immature = b.withdrawing;
            for (auto& [id, h] : gather(mine)) {
                l.txs[id] = !h.pending;
                note_seen(id);
            }
        }
        if (link && !link->connected()) l.warnings.push_back("The mainchain interface does not answer: this chain cannot follow it.");
        return l;
    }

    UniValue info()
    {
        UniValue i{UniValue::VOBJ};
        i.pushKV("name", "Hivemind");
        i.pushKV("version", VERSION_TEXT);
        i.pushKV("bugreport", "https://github.com/block-fabric/backbone-hivemind/issues");
        i.pushKV("network", network);
        i.pushKV("networkLabel", net_label(network));
        i.pushKV("datadir", "");
        i.pushKV("blockSpacing", 600);
        if (auto h{node.height()}) i.pushKV("blockWeight", n64(body_size_limit(*h + 1)));
        i.pushKV("walletEnabled", true);
        i.pushKV("devtools", false);
        i.pushKV("language", "");
        i.pushKV("test", false);
        i.pushKV("platform", "other");
        i.pushKV("dropin", "hivemind");
        i.pushKV("slot", static_cast<int>(THIS_SIDECHAIN));
        return i;
    }

    // ---------------------------------------------------------- explorer
    UniValue block_summary(uint32_t height, const Hash32& hash, const Header& hd, const Body& b)
    {
        UniValue o{UniValue::VOBJ};
        o.pushKV("height", static_cast<uint64_t>(height));
        o.pushKV("hash", bt::hex_encode(hash));
        o.pushKV("mainHash", bt::hex_encode_rev(hd.prev_main_hash.span()));
        if (auto t{main_time(hd.prev_main_hash)}) o.pushKV("time", *t);
        uint64_t fees{0};
        for (const Output& c : b.coinbase.outputs) fees += c.get_value();
        o.pushKV("txs", static_cast<uint64_t>(b.transactions.size()));
        o.pushKV("inputs", static_cast<uint64_t>(b.inputs_len()));
        size_t outs{0};
        for (const auto& tx : b.transactions) outs += tx.outputs.size();
        o.pushKV("outputs", static_cast<uint64_t>(outs));
        o.pushKV("fees", n64(fees));
        Writer w;
        borsh(w, b);
        o.pushKV("bytes", static_cast<uint64_t>(w.buf.size()));
        o.pushKV("limit", n64(body_size_limit(height)));
        return o;
    }

    UniValue output_json(const Output& o, const Mine& mine)
    {
        UniValue r{UniValue::VOBJ};
        r.pushKV("address", o.address.to_base58());
        r.pushKV("amount", n64(o.get_value()));
        r.pushKV("kind", o.content.kind == Content::Withdrawal ? "withdrawal" : o.content.kind == Content::MarketFunds ? "market-funds" : "value");
        if (o.content.kind == Content::Withdrawal) {
            r.pushKV("mainAddress", o.content.main_address.to_string());
            r.pushKV("mainFee", n64(o.content.main_fee));
        }
        if (o.content.kind == Content::MarketFunds) {
            r.pushKV("market", o.content.market_id.to_hex());
            r.pushKV("isFee", o.content.is_fee);
        }
        if (mine) r.pushKV("mine", mine->contains(o.address));
        return r;
    }

    std::optional<Hash32> parse_hash(const std::string& s)
    {
        auto b{bt::hex_to_array<32>(s)};
        if (!b) return std::nullopt;
        return Hash32{*b};
    }

    /** What a transaction does besides moving coins (its data), in words, for the explorer. */
    UniValue data_json(const Transaction& tx)
    {
        if (!tx.data) return UniValue{};
        UniValue o{UniValue::VOBJ};
        o.pushKV("kind", tx_data_name(tx.data->index()));
        if (const auto* tr{std::get_if<TxTrade>(&*tx.data)}) {
            o.pushKV("market", tr->market_id.to_hex());
            o.pushKV("outcome", static_cast<uint64_t>(tr->outcome_index));
            o.pushKV("shares", UniValue(tr->shares));
        } else if (const auto* cm{std::get_if<TxCreateMarket>(&*tx.data)}) {
            o.pushKV("title", cm->title);
        } else if (const auto* am{std::get_if<TxAmplifyBeta>(&*tx.data)}) {
            o.pushKV("market", am->market_id.to_hex());
            o.pushKV("amount", n64(am->amount));
        } else if (const auto* rt{std::get_if<TxTransferReputation>(&*tx.data)}) {
            o.pushKV("votecoin", fnum(rt->amount));
            o.pushKV("to", rt->dest.to_base58());
        } else if (const auto* b{std::get_if<TxSubmitBallot>(&*tx.data)}) {
            o.pushKV("votes", static_cast<uint64_t>(b->votes.size()));
            o.pushKV("period", static_cast<uint64_t>(b->voting_period));
        } else if (const auto* c{std::get_if<ClaimDecisionPayload>(&*tx.data)}) {
            o.pushKV("decisions", static_cast<uint64_t>(c->decisions.size()));
        }
        return o;
    }

    UniValue block_detail(const Hash32& hash, const Mine& mine)
    {
        auto hd{node.header(hash)};
        auto b{node.body(hash)};
        auto at{block_of(hash)};
        if (!hd || !b || !at) throw HostError("No such block");
        UniValue o{block_summary(at->first, hash, *hd, *b)};
        o.pushKV("prevHash", hd->prev_side_hash ? bt::hex_encode(*hd->prev_side_hash) : "");
        o.pushKV("merkleRoot", bt::hex_encode(hd->merkle_root));
        o.pushKV("onChain", node.block_hash_at(at->first) == hash);
        UniValue cb{UniValue::VARR};
        for (const Output& c : b->coinbase.outputs) cb.push_back(output_json(c, mine));
        o.pushKV("coinbase", cb);
        UniValue txs{UniValue::VARR};
        for (const auto& tx : b->transactions) {
            if (txs.size() >= 1000) break;
            UniValue t{UniValue::VOBJ};
            t.pushKV("txid", bt::hex_encode(tx.txid()));
            t.pushKV("inputs", static_cast<uint64_t>(tx.inputs.size()));
            UniValue outs{UniValue::VARR};
            for (const Output& out : tx.outputs) outs.push_back(output_json(out, mine));
            t.pushKV("outputs", outs);
            if (tx.data) t.pushKV("data", data_json(tx));
            txs.push_back(t);
        }
        o.pushKV("transactions", txs);
        return o;
    }

    UniValue tx_detail(const Hash32& txid, const Mine& mine)
    {
        auto filled{node.filled_transaction(txid)};
        if (!filled) throw HostError("No such transaction");
        const Transaction& tx{filled->first.first.transaction};
        const std::vector<Output>& spent{filled->first.second};
        UniValue o{UniValue::VOBJ};
        o.pushKV("txid", bt::hex_encode(txid));
        const bool pending{!filled->second.has_value()};
        o.pushKV("pending", pending);
        if (!pending) {
            const Hash32 block{filled->second->first};
            o.pushKV("block", bt::hex_encode(block));
            if (auto b{block_of(block)}) {
                o.pushKV("height", static_cast<uint64_t>(b->first));
                if (auto tm{main_time(b->second)}) o.pushKV("time", *tm);
                if (auto h{node.height()}) o.pushKV("confirmations", static_cast<uint64_t>(*h >= b->first ? *h - b->first + 1 : 0));
            }
        }
        UniValue ins{UniValue::VARR};
        uint64_t in_total{0}, out_total{0};
        for (size_t i{0}; i < tx.inputs.size(); ++i) {
            UniValue x{i < spent.size() ? output_json(spent[i], mine) : UniValue{UniValue::VOBJ}};
            x.pushKV("outpoint", json::outpoint(tx.inputs[i].first));
            if (i < spent.size()) in_total += spent[i].get_value();
            ins.push_back(x);
        }
        UniValue outs{UniValue::VARR};
        for (const Output& out : tx.outputs) {
            outs.push_back(output_json(out, mine));
            out_total += out.get_value();
        }
        o.pushKV("inputs", ins);
        o.pushKV("outputs", outs);
        if (in_total >= out_total) o.pushKV("fee", n64(in_total - out_total));
        if (tx.data) o.pushKV("data", data_json(tx));
        return o;
    }

    UniValue address_detail(const Address& a, const Mine& mine)
    {
        const std::set<Address> one{a};
        uint64_t balance{0};
        UniValue utxos{UniValue::VARR};
        for (auto& [op, o] : node.utxos_by_addresses(one)) {
            balance += o.get_value();
            if (utxos.size() < 500) {
                UniValue u{output_json(o, mine)};
                u.pushKV("outpoint", json::outpoint(op));
                utxos.push_back(u);
            }
        }
        UniValue stxos{UniValue::VARR};
        for (auto& [op, s] : node.stxos_by_addresses(one)) {
            if (stxos.size() >= 500) break;
            UniValue u{output_json(s.output, mine)};
            u.pushKV("outpoint", json::outpoint(op));
            u.pushKV("spentBy", s.inpoint.kind == InPoint::Withdrawal ? "bundle:" + bt::hex_encode_rev(s.inpoint.id.span()) : bt::hex_encode(s.inpoint.id));
            stxos.push_back(u);
        }
        UniValue o{UniValue::VOBJ};
        o.pushKV("address", a.to_base58());
        if (mine) o.pushKV("mine", mine->contains(a));
        o.pushKV("balance", n64(balance));
        o.pushKV("utxos", utxos);
        o.pushKV("spent", stxos);
        o.pushKV("depositAddress", a.format_for_deposit());
        o.pushKV("votecoin", fnum(state([&](const State& st, const Archive&, const Txn& t) { return st.reputation(t, a); })));
        return o;
    }

    // ---------------------------------------------------------- peg
    UniValue peg()
    {
        const auto mine{addresses()};
        UniValue o{UniValue::VOBJ};
        auto pending{node.pending_withdrawal_bundle()};
        std::set<OutPoint> in_pending;
        if (pending) {
            UniValue p{UniValue::VOBJ};
            p.pushKV("m6id", bt::hex_encode_rev(pending->m6id().span()));
            uint64_t total{0};
            for (auto& [op, out] : pending->spend_utxos) {
                total += out.get_value();
                in_pending.insert(op);
            }
            p.pushKV("withdrawals", static_cast<uint64_t>(pending->spend_utxos.size()));
            p.pushKV("total", n64(total));
            o.pushKV("pendingBundle", p);
        } else {
            o.pushKV("pendingBundle", UniValue{});
        }
        auto failed{node.latest_failed_withdrawal_bundle_height()};
        o.pushKV("lastFailedHeight", failed ? UniValue(uint64_t(*failed)) : UniValue{});
        UniValue w{UniValue::VARR};
        auto withdrawal = [&](const OutPoint& op, const Output& out, const std::string& st, const std::optional<Hash32>& m6id) {
            UniValue r{UniValue::VOBJ};
            r.pushKV("outpoint", json::outpoint(op));
            r.pushKV("txid", bt::hex_encode(op.txid));
            r.pushKV("mainAddress", out.content.main_address.to_string());
            r.pushKV("amount", n64(out.content.value));
            r.pushKV("mainFee", n64(out.content.main_fee));
            r.pushKV("state", st);
            if (m6id) {
                r.pushKV("m6id", bt::hex_encode_rev(m6id->span()));
                if (auto b{node.withdrawal_bundle(*m6id)}) r.pushKV("bundleStatus", to_string(b->second.latest().value));
            }
            w.push_back(r);
        };
        for (auto& [op, out] : node.utxos_by_addresses(mine)) {
            if (out.content.kind != Content::Withdrawal) continue;
            withdrawal(op, out, in_pending.contains(op) ? "in-pending-bundle" : "waiting", pending && in_pending.contains(op) ? std::optional<Hash32>{pending->m6id()} : std::nullopt);
        }
        for (auto& [op, s] : node.stxos_by_addresses(mine)) {
            if (s.output.content.kind != Content::Withdrawal || s.inpoint.kind != InPoint::Withdrawal) continue;
            auto b{node.withdrawal_bundle(s.inpoint.id)};
            std::string st{"submitted"};
            if (b && b->second.latest().value == BundleStatus::Confirmed) st = "paid";
            else if (b && b->second.latest().value == BundleStatus::Failed) st = "failed";
            withdrawal(op, s.output, st, s.inpoint.id);
        }
        o.pushKV("withdrawals", w);
        o.pushKV("wealth", n64(node.sidechain_wealth()));
        o.pushKV("mainchain", link ? link->info() : UniValue{});
        o.pushKV("slot", static_cast<int>(THIS_SIDECHAIN));
        return o;
    }

    // ---------------------------------------------------------- questions of the spends
    Address side_address(const UniValue& v, const std::string& what)
    {
        if (!v.isStr()) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": a Hivemind address");
        auto a{Address::from_base58(v.get_str())};
        if (!a) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": not a Hivemind address (" + v.get_str() + ")");
        return *a;
    }

    bt::btc::Address main_address(const UniValue& v)
    {
        if (!v.isStr()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "mainchain_address: an eCash address");
        auto a{bt::btc::Address::parse(v.get_str())};
        if (!a) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Not an eCash address: " + v.get_str());
        if (!main_address_of_network(*a, network)) throw HostError(bt::appui::HOST_INVALID_REQUEST, "This eCash address is of another network than the one this node follows (" + network + ")");
        return *a;
    }

    void enough(uint64_t need)
    {
        const uint64_t have{balances().available};
        if (need > have) throw HostError(bt::appui::RPC_WALLET_ERROR, "Not enough available: " + coins(have) + " for " + coins(need));
    }

    void need_seed()
    {
        if (!wallet.has_seed()) throw HostError(bt::appui::RPC_WALLET_ERROR, "The wallet has no backup phrase yet: set it up first");
    }

    static UniValue same_numbers(const UniValue& p, size_t n, const std::vector<size_t>& numbers)
    {
        if (!p.isArray() || p.size() < n) throw HostError(bt::appui::HOST_INVALID_REQUEST, "params: " + std::to_string(n) + " parameters");
        UniValue out{UniValue::VARR};
        for (size_t i{0}; i < p.size() && i < n; ++i) {
            if (std::find(numbers.begin(), numbers.end(), i) != numbers.end()) out.push_back(n64(u64_of(p[i], "param " + std::to_string(i))));
            else out.push_back(p[i]);
        }
        return out;
    }

    /** The request object of a LayerTwo method (its one parameter): an object, or refused. */
    static const UniValue& request_of(const UniValue& p)
    {
        if (!p.isArray() || p.size() < 1 || !p[0].isObject()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "params: [request]");
        return p[0];
    }

    static void put_opt_text(UniValue& out, const UniValue& in, const char* key, size_t max, bool multiline = false)
    {
        const UniValue& v{in.find_value(key)};
        if (v.isNull()) return;
        out.pushKV(key, text_of(v, key, max, multiline));
    }

    static void put_opt_texts(UniValue& out, const UniValue& in, const char* key, size_t max_items)
    {
        const UniValue& v{in.find_value(key)};
        if (v.isNull()) return;
        if (!v.isArray() || v.size() > max_items) throw HostError(bt::appui::HOST_INVALID_REQUEST, std::string{key} + ": a list of texts");
        UniValue a{UniValue::VARR};
        for (const UniValue& x : v.getValues()) a.push_back(text_of(x, key, 2000));
        out.pushKV(key, a);
    }

    static void put_opt_f64(UniValue& out, const UniValue& in, const char* key)
    {
        const UniValue& v{in.find_value(key)};
        if (v.isNull()) return;
        out.pushKV(key, UniValue(f64_of(v, key)));
    }

    static void put_opt_u64(UniValue& out, const UniValue& in, const char* key)
    {
        const UniValue& v{in.find_value(key)};
        if (v.isNull()) return;
        out.pushKV(key, n64(u64_of(v, key)));
    }

    /** A decision to claim, in its one form (the fields LayerTwo reads, typed). */
    static UniValue claim_entry(const UniValue& in, bool with_type)
    {
        if (!in.isObject()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "a decision: an object");
        UniValue e{UniValue::VOBJ};
        if (with_type) e.pushKV("type", "new");
        e.pushKV("period_index", n64(u64_of(in.find_value("period_index"), "period_index")));
        if (with_type) e.pushKV("decision_type", text_of(in.find_value("decision_type"), "decision_type", 16));
        e.pushKV("header", text_of(in.find_value("header"), "header", 100));
        put_opt_text(e, in, "description", 2000, true);
        put_opt_text(e, in, "option_0_label", 200);
        put_opt_text(e, in, "option_1_label", 200);
        put_opt_texts(e, in, "option_labels", 64);
        put_opt_texts(e, in, "tags", 32);
        if (with_type) {
            put_opt_f64(e, in, "min");
            put_opt_f64(e, in, "max");
            put_opt_f64(e, in, "increment");
        }
        return e;
    }

    struct Slot {
        uint32_t period{0};
        std::string header;
        uint64_t fee{0};
    };

    /** The slots a claim takes and their listing fees, as decision_claim/market_create allocate them (the cheapest open
     *  slot of the period not taken by the mempool or an earlier entry). Refused when a period has none. */
    std::vector<Slot> preview_slots(const std::vector<std::pair<uint32_t, std::string>>& wanted)
    {
        auto pending{node.pending_decision_claim_ids()};
        return state([&](const State& st, const Archive&, const Txn& t) {
            auto [ts, h, g] = st.period_context(t);
            const uint32_t cur{get_current_period(ts, h, g, st.config())};
            std::map<uint32_t, std::set<uint32_t>> picked;
            std::vector<Slot> out;
            for (auto& [period, header] : wanted) {
                if (period < cur) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Period " + std::to_string(period) + " is over: decisions are claimed for period " + std::to_string(cur) + " or later");
                auto pricing{st.period_pricing(t, period)};
                if (!pricing) throw HostError("No decision slots are open in period " + std::to_string(period) + " yet");
                std::optional<DecisionId> pick;
                for (auto& id : st.available_decisions_in_period(t, period))
                    if (slot_unlocked(id.decision_index(), pricing->mints) && !pending.contains(id.b) && !picked[period].contains(id.decision_index())) {
                        pick = id;
                        break;
                    }
                if (!pick) throw HostError("No free decision slot in period " + std::to_string(period));
                picked[period].insert(pick->decision_index());
                std::string err;
                auto fee{fee_for_index(pricing->p_period, pricing->mints, pick->decision_index(), &err)};
                if (!fee) throw HostError(err);
                out.push_back({period, header, *fee});
            }
            return out;
        });
    }

    std::map<std::string, Confirmed> confirmed()
    {
        std::map<std::string, Confirmed> c;
        c["create_transfer"] = Confirmed{
            [](const UniValue& p) { return same_numbers(p, 3, {1, 2}); },
            [this](const UniValue& p) {
                need_seed();
                const Address to{side_address(p[0], "dest")};
                const uint64_t value{p[1].getInt<uint64_t>()}, fee{p[2].getInt<uint64_t>()};
                if (value == 0) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Nothing to send");
                const uint64_t total{bt::appui::spend_total({value, fee})};
                enough(total);
                Question q;
                q.title = "Send " + coins(value);
                q.message = addresses().contains(to) ? "To an address of this wallet." : "To this address on Hivemind. A payment cannot be taken back.";
                q.accept = "Send";
                q.kind = "send";
                q.rows = {{"To", to.to_base58(), "address"}, {"Amount", coins(value), "amount"}, {"Fee", coins(fee), "amount"}, {"Total", coins(total), "total"}};
                return q;
            },
            [this](const UniValue& p) { return l2l_call("create_transfer", p); }};
        c["create_transfer_many"] = Confirmed{
            [](const UniValue& p) {
                if (!p.isArray() || p.size() < 2 || !p[0].isObject()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "params: {address: satoshis}, fee");
                UniValue dests{UniValue::VOBJ};
                for (size_t i{0}; i < p[0].size(); ++i) dests.pushKV(p[0].getKeys()[i], n64(u64_of(p[0].getValues()[i], "amount")));
                UniValue out{UniValue::VARR};
                out.push_back(dests);
                out.push_back(n64(u64_of(p[1], "fee_sats")));
                return out;
            },
            [this](const UniValue& p) {
                need_seed();
                const UniValue& dests{p[0]};
                if (dests.size() == 0) throw HostError(bt::appui::HOST_INVALID_REQUEST, "No recipient");
                if (dests.size() > 2000) throw HostError(bt::appui::HOST_INVALID_REQUEST, "At most 2000 recipients in one payment");
                const uint64_t fee{p[1].getInt<uint64_t>()};
                uint64_t total{bt::appui::spend_total({fee})};
                Question q;
                q.kind = "send-many";
                q.accept = "Send";
                std::set<Address> seen;
                for (size_t i{0}; i < dests.size(); ++i) {
                    const Address a{side_address(UniValue{dests.getKeys()[i]}, "dest")};
                    if (!seen.insert(a).second) throw HostError(bt::appui::HOST_INVALID_REQUEST, "invalid entry: found duplicate key");
                    const uint64_t v{dests.getValues()[i].getInt<uint64_t>()};
                    total = bt::appui::spend_total({total, v});
                    q.rows.push_back({"To", a.to_base58(), "address"});
                    q.rows.push_back({"Amount", coins(v), "amount"});
                }
                enough(total);
                q.title = "Send " + coins(total - fee) + " to " + std::to_string(dests.size()) + (dests.size() == 1 ? " recipient" : " recipients");
                q.message = "One transaction on Hivemind. A payment cannot be taken back.";
                q.rows.push_back({"Fee", coins(fee), "amount"});
                q.rows.push_back({"Total", coins(total), "total"});
                return q;
            },
            [this](const UniValue& p) { return l2l_call("create_transfer_many", p); }};
        c["create_withdrawal"] = Confirmed{
            [](const UniValue& p) { return same_numbers(p, 4, {1, 2, 3}); },
            [this](const UniValue& p) {
                need_seed();
                const bt::btc::Address to{main_address(p[0])};
                const uint64_t value{p[1].getInt<uint64_t>()}, fee{p[2].getInt<uint64_t>()}, main_fee{p[3].getInt<uint64_t>()};
                if (value == 0) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Nothing to withdraw");
                const uint64_t total{bt::appui::spend_total({value, fee, main_fee})};
                enough(total);
                Question q;
                q.title = "Withdraw " + coins(value) + " to eCash";
                q.message = "Your coins leave Hivemind now. eCash pays them to the address below once a withdrawal bundle with them is "
                            "accepted by eCash's miners, which takes a long time (their votes). Until then they show as being withdrawn.";
                q.accept = "Withdraw";
                q.kind = "withdraw";
                q.rows = {{"To (on eCash)", to.to_string(), "address"}, {"Amount", coins(value), "amount"}, {"Fee on eCash", coins(main_fee), "amount"},
                          {"Fee on Hivemind", coins(fee), "amount"}, {"Total", coins(total), "total"}};
                return q;
            },
            [this](const UniValue& p) { return l2l_call("create_withdrawal", p); }};
        c["create_deposit"] = Confirmed{
            [](const UniValue& p) { return same_numbers(p, 3, {1, 2}); },
            [this](const UniValue& p) {
                const Address to{side_address(p[0], "address")};
                const uint64_t value{p[1].getInt<uint64_t>()}, fee{p[2].getInt<uint64_t>()};
                if (value == 0) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Nothing to deposit");
                const uint64_t total{bt::appui::spend_total({value, fee})};
                const bool ours{addresses().contains(to)};
                Question q;
                q.title = "Deposit " + coins(value) + " into Hivemind";
                q.message = "Paid on eCash by the wallet of the mainchain interface this node uses (" + node.enforcer().url() +
                            "), into Hivemind's slot " + std::to_string(int(THIS_SIDECHAIN)) + ". It is credited on Hivemind in the first block after eCash mines it.";
                if (!ours) {
                    q.message += " The address is NOT one of this wallet: the coins go to whoever holds it.";
                    q.level = "danger";
                }
                q.accept = "Deposit";
                q.kind = "deposit";
                q.rows = {{"Into (Hivemind)", to.to_base58(), "address"}, {"Amount", coins(value), "amount"}, {"Fee on eCash", coins(fee), "amount"},
                          {"Total (paid on eCash)", coins(total), "total"}};
                return q;
            },
            [this](const UniValue& p) {
                UniValue r{l2l_call("create_deposit", p)};
                if (poller) poller->poke();
                return r;
            }};
        c["mine"] = Confirmed{
            [](const UniValue& p) {
                UniValue out{UniValue::VARR};
                if (p.isArray() && p.size() > 0 && !p[0].isNull()) out.push_back(n64(u64_of(p[0], "fee")));
                return out;
            },
            [this](const UniValue& p) {
                const uint64_t bid{bt::appui::spend_total({p.size() > 0 ? p[0].getInt<uint64_t>() : App::EMPTY_BLOCK_BMM_BRIBE})};
                Question q;
                q.title = "Merge-mine a block of Hivemind";
                q.message = "Asks eCash's miners to include the next block of Hivemind (blind merged mining), with a bid paid on eCash by the "
                            "wallet of the mainchain interface (" + node.enforcer().url() + "). The bid is paid whether or not the block is taken.";
                q.accept = "Bid";
                q.kind = "mine";
                q.rows = {{"Bid (at most)", p.size() > 0 ? coins(bid) : "the block's fees, or " + coins(App::EMPTY_BLOCK_BMM_BRIBE), "amount"}};
                return q;
            },
            [this](const UniValue& p) { return l2l_call("mine", p); }};

        // ---- Votecoin (reputation): moved from the wallet's voter address, the fee paid by a coin of that address.
        c["transfer_votecoin"] = Confirmed{
            [](const UniValue& p) {
                if (!p.isArray() || p.size() < 3) throw HostError(bt::appui::HOST_INVALID_REQUEST, "params: [dest, amount, fee_sats]");
                UniValue out{UniValue::VARR};
                out.push_back(p[0]);
                out.push_back(UniValue(f64_of(p[1], "amount")));
                out.push_back(n64(u64_of(p[2], "fee_sats")));
                return out;
            },
            [this](const UniValue& p) {
                need_seed();
                const Address to{side_address(p[0], "dest")};
                const double amount{p[1].get_real()};
                const uint64_t fee{p[2].getInt<uint64_t>()};
                if (!(amount > 0.0)) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Votecoin to move: more than nothing");
                if (amount != math::round_reputation(amount)) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Votecoin has at most 10 decimals");
                const Address voter{wallet.voter_address()};
                if (to == voter) throw HostError(bt::appui::HOST_INVALID_REQUEST, "That is this wallet's voter address itself");
                const double have{state([&](const State& st, const Archive&, const Txn& t) { return st.reputation(t, voter); })};
                if (amount > have) throw HostError(bt::appui::RPC_WALLET_ERROR, "Not enough Votecoin: " + votecoin_text(have) + " for " + votecoin_text(amount));
                // The fee is paid by coins of the voter address (the chain's replay protection of a transfer).
                uint64_t at_voter{0};
                const auto spent{wallet.mempool_spent()};
                for (auto& [op, o] : wallet.utxos())
                    if (o.address == voter && o.content.kind == Content::Value && !spent.contains(op)) at_voter = bt::appui::spend_total({at_voter, o.get_value()});
                if (fee == 0 || at_voter < fee)
                    throw HostError(bt::appui::RPC_WALLET_ERROR, "The fee is paid from the voter address, which holds " + coins(at_voter) +
                                                                    ": send it a little ECX first (" + voter.to_base58() + ")");
                Question q;
                q.title = "Move " + votecoin_text(amount) + " Votecoin";
                q.message = "Votecoin is the reputation that votes on the decisions. Once moved, the votes and the fees it earns are the other address's. It cannot be taken back.";
                q.accept = "Move";
                q.kind = "votecoin";
                q.rows = {{"To", to.to_base58(), "address"}, {"Votecoin", votecoin_text(amount) + " of " + votecoin_text(have), "text"},
                          {"From (your voter address)", voter.to_base58(), "address"}, {"Fee", coins(fee), "amount"}};
                return q;
            },
            [this](const UniValue& p) { return l2l_call("transfer_votecoin", p); }};

        // ---- decisions: a slot of a period claimed (listing fee), its question for the voters of the next period.
        c["decision_claim"] = Confirmed{
            [](const UniValue& p) {
                const UniValue& rq{request_of(p)};
                UniValue r{UniValue::VOBJ};
                r.pushKV("decision_type", text_of(rq.find_value("decision_type"), "decision_type", 16));
                const UniValue& items{rq.find_value("decisions")};
                if (!items.isArray() || items.empty() || items.size() > 64) throw HostError(bt::appui::HOST_INVALID_REQUEST, "decisions: 1 to 64 decisions");
                UniValue ds{UniValue::VARR};
                for (const UniValue& it : items.getValues()) ds.push_back(claim_entry(it, false));
                r.pushKV("decisions", ds);
                put_opt_f64(r, rq, "min");
                put_opt_f64(r, rq, "max");
                put_opt_f64(r, rq, "increment");
                r.pushKV("tx_fee_sats", n64(u64_of(rq.find_value("tx_fee_sats"), "tx_fee_sats")));
                put_opt_u64(r, rq, "max_listing_fee_sats");
                UniValue out{UniValue::VARR};
                out.push_back(r);
                return out;
            },
            [this](const UniValue& p) {
                need_seed();
                const UniValue& rq{p[0]};
                const std::string type{rq.find_value("decision_type").get_str()};
                if (type != "binary" && type != "scaled" && type != "category") throw HostError(bt::appui::HOST_INVALID_REQUEST, "decision_type: binary, scaled or category");
                const UniValue& items{rq.find_value("decisions")};
                if (type == "category" && items.size() != 1) throw HostError(bt::appui::HOST_INVALID_REQUEST, "A category decision is claimed alone");
                std::vector<std::pair<uint32_t, std::string>> wanted;
                for (const UniValue& it : items.getValues()) {
                    const uint64_t period{it.find_value("period_index").getInt<uint64_t>()};
                    if (period > 127) throw HostError(bt::appui::HOST_INVALID_REQUEST, "period_index: periods go to 127");
                    wanted.emplace_back(static_cast<uint32_t>(period), it.find_value("header").get_str());
                }
                std::string kind_words{type == "binary" ? "Yes or no" : type == "category" ? "One of several options" : "A number"};
                if (type == "scaled") {
                    const UniValue& mn{rq.find_value("min")};
                    const UniValue& mx{rq.find_value("max")};
                    if (!mn.isNum() || !mx.isNum()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "A scaled decision needs min and max");
                    if (!(mn.get_real() < mx.get_real())) throw HostError(bt::appui::HOST_INVALID_REQUEST, "max must be above min");
                    kind_words += " from " + num_text(mn.get_real()) + " to " + num_text(mx.get_real());
                }
                if (type == "category") {
                    const UniValue& labels{items[0].find_value("option_labels")};
                    if (!labels.isArray() || labels.size() < 2) throw HostError(bt::appui::HOST_INVALID_REQUEST, "A category decision needs at least 2 options");
                }
                const auto slots{preview_slots(wanted)};
                uint64_t listing{0};
                for (auto& s : slots) listing = bt::appui::spend_total({listing, s.fee});
                const UniValue& cap{rq.find_value("max_listing_fee_sats")};
                if (cap.isNum() && listing > cap.getInt<uint64_t>()) throw HostError("The listing fee is now " + coins(listing) + ", above the " + coins(cap.getInt<uint64_t>()) + " you allowed");
                const uint64_t tx_fee{rq.find_value("tx_fee_sats").getInt<uint64_t>()};
                const uint64_t most{cap.isNum() ? cap.getInt<uint64_t>() : listing};
                const uint64_t total{bt::appui::spend_total({most, tx_fee})};
                enough(total);
                Question q;
                q.title = slots.size() == 1 ? "List a decision" : "List " + std::to_string(slots.size()) + " decisions";
                q.message = "Each decision takes a slot of its period, paid with its listing fee. Its question is voted on by the holders of "
                            "Votecoin in the period after it; markets can be opened on it meanwhile.";
                q.accept = "List";
                q.kind = "decision";
                for (auto& s : slots) {
                    q.rows.push_back({"Question", s.header, "text"});
                    q.rows.push_back({"Period", std::to_string(s.period) + " (" + period_to_name(s.period) + "), voted on in period " + std::to_string(s.period + 1), "text"});
                    q.rows.push_back({"Listing fee", coins(s.fee), "amount"});
                }
                q.rows.push_back({"Answer", kind_words, "text"});
                q.rows.push_back({"Fee", coins(tx_fee), "amount"});
                q.rows.push_back({cap.isNum() && most > listing ? "Total (at most)" : "Total", coins(total), "total"});
                return q;
            },
            [this](const UniValue& p) { return l2l_call("decision_claim", p); }};

        // ---- markets: opened on decisions (claimed or new ones), funded by the wallet (its liquidity and storage fee).
        c["market_create"] = Confirmed{
            [](const UniValue& p) {
                const UniValue& rq{request_of(p)};
                UniValue r{UniValue::VOBJ};
                r.pushKV("title", text_of(rq.find_value("title"), "title", 1000));
                r.pushKV("description", text_of(rq.find_value("description"), "description", 10000, true));
                const UniValue& dims{rq.find_value("dimensions")};
                if (!dims.isArray() || dims.empty() || dims.size() > 16) throw HostError(bt::appui::HOST_INVALID_REQUEST, "dimensions: 1 to 16 decisions");
                UniValue ds{UniValue::VARR};
                for (const UniValue& d : dims.getValues()) {
                    if (!d.isObject()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "a dimension: an object");
                    const std::string tag{text_of(d.find_value("type"), "type", 16)};
                    if (tag == "existing") {
                        UniValue e{UniValue::VOBJ};
                        e.pushKV("type", "existing");
                        e.pushKV("id", text_of(d.find_value("id"), "id", 6));
                        ds.push_back(e);
                    } else if (tag == "new") {
                        ds.push_back(claim_entry(d, true));
                    } else {
                        throw HostError(bt::appui::HOST_INVALID_REQUEST, "a dimension: existing or new");
                    }
                }
                r.pushKV("dimensions", ds);
                put_opt_f64(r, rq, "beta");
                put_opt_f64(r, rq, "trading_fee");
                put_opt_u64(r, rq, "initial_liquidity");
                for (const char* k : {"tx_pow_hash_selector", "tx_pow_ordering", "tx_pow_difficulty"}) {
                    const UniValue& v{rq.find_value(k)};
                    if (v.isNull()) continue;
                    const uint64_t x{u64_of(v, k)};
                    if (x > 255) throw HostError(bt::appui::HOST_INVALID_REQUEST, std::string{k} + ": 0 to 255");
                    r.pushKV(k, n64(x));
                }
                r.pushKV("tx_fee_sats", n64(u64_of(rq.find_value("tx_fee_sats"), "tx_fee_sats")));
                put_opt_u64(r, rq, "max_listing_fee_sats");
                UniValue out{UniValue::VARR};
                out.push_back(r);
                return out;
            },
            [this](const UniValue& p) {
                need_seed();
                const UniValue& rq{p[0]};
                const UniValue& dims{rq.find_value("dimensions")};
                const UniValue& beta_v{rq.find_value("beta")};
                const UniValue& liq_v{rq.find_value("initial_liquidity")};
                if (beta_v.isNum() && liq_v.isNum()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Give either beta or initial_liquidity, not both");
                if (beta_v.isNum() && !(beta_v.get_real() > 0.0)) throw HostError(bt::appui::HOST_INVALID_REQUEST, "beta: above 0");
                const UniValue& fee_v{rq.find_value("trading_fee")};
                if (fee_v.isNum() && !(fee_v.get_real() >= 0.0 && fee_v.get_real() < 1.0)) throw HostError(bt::appui::HOST_INVALID_REQUEST, "trading_fee: a share of each trade, from 0 to under 1");
                size_t outcomes{1};
                std::vector<std::pair<uint32_t, std::string>> wanted;
                std::vector<Row> about;
                for (const UniValue& d : dims.getValues()) {
                    size_t n{2};
                    if (d.find_value("type").get_str() == "existing") {
                        auto id{decision_id_of(d.find_value("id").get_str())};
                        if (!id) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Not a decision id: " + d.find_value("id").get_str());
                        auto e{state([&](const State& st, const Archive&, const Txn& t) { return st.decision_entry(t, *id); })};
                        if (!e || !e->decision) throw HostError("Decision " + id->to_hex() + " is not claimed");
                        if (e->decision->is_categorical()) n = e->decision->decision_type.options.size();
                        about.push_back({"Decision", bt::appui::display_text_shown(e->decision->header), "text"});
                    } else {
                        const std::string dt{d.find_value("decision_type").get_str()};
                        if (dt != "binary" && dt != "scaled" && dt != "category") throw HostError(bt::appui::HOST_INVALID_REQUEST, "decision_type: binary, scaled or category");
                        if (dt == "category") {
                            const UniValue& labels{d.find_value("option_labels")};
                            if (!labels.isArray() || labels.size() < 2) throw HostError(bt::appui::HOST_INVALID_REQUEST, "A category decision needs at least 2 options");
                            n = labels.size();
                        }
                        if (dt == "scaled" && (!d.find_value("min").isNum() || !d.find_value("max").isNum() || !(d.find_value("min").get_real() < d.find_value("max").get_real())))
                            throw HostError(bt::appui::HOST_INVALID_REQUEST, "A scaled decision needs min below max");
                        const uint64_t period{d.find_value("period_index").getInt<uint64_t>()};
                        if (period > 127) throw HostError(bt::appui::HOST_INVALID_REQUEST, "period_index: periods go to 127");
                        wanted.emplace_back(static_cast<uint32_t>(period), d.find_value("header").get_str());
                        about.push_back({"New decision", d.find_value("header").get_str(), "text"});
                    }
                    if (n == 0 || outcomes > 4096 / n) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Too many outcomes for one market");
                    outcomes *= n;
                }
                const auto slots{preview_slots(wanted)};
                uint64_t listing{0};
                for (auto& s : slots) listing = bt::appui::spend_total({listing, s.fee});
                const UniValue& cap{rq.find_value("max_listing_fee_sats")};
                if (cap.isNum() && listing > cap.getInt<uint64_t>()) throw HostError("The listing fee is now " + coins(listing) + ", above the " + coins(cap.getInt<uint64_t>()) + " you allowed");
                uint64_t liquidity{0};
                if (liq_v.isNum()) liquidity = liq_v.getInt<uint64_t>();
                else {
                    std::string err;
                    auto s{math::to_sats(math::lmsr_liquidity(beta_v.isNum() ? beta_v.get_real() : DEFAULT_MARKET_BETA, outcomes), math::Rounding::Up, &err)};
                    if (!s) throw HostError(bt::appui::HOST_INVALID_REQUEST, "amount overflow");
                    liquidity = *s;
                }
                const uint64_t storage{market_storage_fee(outcomes)};
                const uint64_t tx_fee{rq.find_value("tx_fee_sats").getInt<uint64_t>()};
                const uint64_t most{cap.isNum() ? cap.getInt<uint64_t>() : listing};
                const uint64_t total{bt::appui::spend_total({liquidity, storage, most, tx_fee})};
                enough(total);
                Question q;
                q.title = "Open a market";
                q.message = "The market maker is funded by your liquidity: it is what the market pays out at most beyond its trades, and what is "
                            "left of it comes back to you when the market settles. The title and description are yours to choose: what the "
                            "market pays is decided by the decisions below, not by its title.";
                q.accept = "Open the market";
                q.kind = "market";
                q.rows.push_back({"Title", rq.find_value("title").get_str(), "text"});
                for (auto& r : about) q.rows.push_back(r);
                q.rows.push_back({"Outcomes", std::to_string(outcomes), "text"});
                q.rows.push_back({"Trading fee", pct_text(fee_v.isNum() ? fee_v.get_real() : 0.005), "text"});
                q.rows.push_back({"Liquidity", coins(liquidity), "amount"});
                q.rows.push_back({"Storage fee", coins(storage), "amount"});
                if (!slots.empty()) q.rows.push_back({"Listing fees", coins(listing) + (cap.isNum() && most > listing ? " (at most " + coins(most) + ")" : ""), "amount"});
                q.rows.push_back({"Fee", coins(tx_fee), "amount"});
                q.rows.push_back({cap.isNum() && most > listing ? "Total (at most)" : "Total", coins(total), "total"});
                return q;
            },
            [this](const UniValue& p) { return l2l_call("market_create", p); }};

        // ---- trades: shares of an outcome bought from the market maker (at most max_cost) or sold to it (at least min_proceeds).
        c["market_buy"] = Confirmed{
            [](const UniValue& p) {
                const UniValue& rq{request_of(p)};
                const UniValue& dry{rq.find_value("dry_run")};
                if (dry.isTrue()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "A dry run is a quote (hivemind.quote), not a trade");
                UniValue r{UniValue::VOBJ};
                r.pushKV("market_id", text_of(rq.find_value("market_id"), "market_id", 12));
                r.pushKV("outcome_index", n64(u64_of(rq.find_value("outcome_index"), "outcome_index")));
                r.pushKV("shares_amount", UniValue(shares_of(rq.find_value("shares_amount"), "shares_amount")));
                r.pushKV("max_cost", n64(u64_of(rq.find_value("max_cost"), "max_cost")));
                r.pushKV("dry_run", false);
                UniValue out{UniValue::VARR};
                out.push_back(r);
                return out;
            },
            [this](const UniValue& p) {
                need_seed();
                const UniValue& rq{p[0]};
                const Market m{market_or_refuse(rq.find_value("market_id"))};
                if (m.market_state != MarketState::Trading) throw HostError("This market does not trade any more");
                const uint64_t outcome{rq.find_value("outcome_index").getInt<uint64_t>()};
                const int64_t shares{rq.find_value("shares_amount").getInt<int64_t>()};
                const uint64_t max_cost{rq.find_value("max_cost").getInt<uint64_t>()};
                if (outcome >= m.shares.size()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "No such outcome in this market");
                const Quote qt{quote(m, outcome, shares, true)};
                const uint64_t now{bt::appui::spend_total({qt.cost, math::TRADE_MINER_FEE_SATS})};
                if (now > max_cost) throw HostError("The price moved: these shares cost " + coins(now) + " now, above the " + coins(max_cost) + " you allowed");
                // The wallet puts up max_cost; what the trade does not take comes back with the block that takes it.
                enough(max_cost);
                const MarketNames n{names_of(m.id)};
                Question q;
                q.title = "Buy " + count_text(static_cast<uint64_t>(shares)) + " shares";
                q.message = "Each share pays 1 sat (0.00000001 ECX) if this outcome is what the decisions resolve to, nothing otherwise. "
                            "The price is the market maker's when the block takes the trade: it pays at most the amount below; what it does not "
                            "need comes back in that block.";
                q.accept = "Buy";
                q.kind = "trade";
                q.rows = {{"Market", bt::appui::display_text_shown(m.title), "text"},
                          {"Outcome", outcome < n.outcomes.size() ? bt::appui::display_text_shown(n.outcomes[outcome]) : "Outcome " + std::to_string(outcome), "text"},
                          {"Shares", count_text(static_cast<uint64_t>(shares)), "text"},
                          {"Chance now", pct_text(qt.price_before) + " (after the trade " + pct_text(qt.price_after) + ")", "text"},
                          {"Cost now", coins(qt.cost), "amount"},
                          {"Of which trading fee", coins(qt.fee), "amount"},
                          {"Miner fee", coins(math::TRADE_MINER_FEE_SATS), "amount"},
                          {"Pays at most", coins(max_cost), "total"}};
                return q;
            },
            [this](const UniValue& p) { return l2l_call("market_buy", p); }};
        c["market_sell"] = Confirmed{
            [](const UniValue& p) {
                const UniValue& rq{request_of(p)};
                const UniValue& dry{rq.find_value("dry_run")};
                if (dry.isTrue()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "A dry run is a quote (hivemind.quote), not a trade");
                UniValue r{UniValue::VOBJ};
                r.pushKV("market_id", text_of(rq.find_value("market_id"), "market_id", 12));
                r.pushKV("outcome_index", n64(u64_of(rq.find_value("outcome_index"), "outcome_index")));
                r.pushKV("shares_amount", UniValue(shares_of(rq.find_value("shares_amount"), "shares_amount")));
                r.pushKV("seller_address", text_of(rq.find_value("seller_address"), "seller_address", 64));
                r.pushKV("min_proceeds", n64(u64_of(rq.find_value("min_proceeds"), "min_proceeds")));
                r.pushKV("dry_run", false);
                UniValue out{UniValue::VARR};
                out.push_back(r);
                return out;
            },
            [this](const UniValue& p) {
                need_seed();
                const UniValue& rq{p[0]};
                const Market m{market_or_refuse(rq.find_value("market_id"))};
                if (m.market_state != MarketState::Trading) throw HostError("This market does not trade any more");
                const uint64_t outcome{rq.find_value("outcome_index").getInt<uint64_t>()};
                const int64_t shares{rq.find_value("shares_amount").getInt<int64_t>()};
                const uint64_t min_proceeds{rq.find_value("min_proceeds").getInt<uint64_t>()};
                const Address seller{side_address(rq.find_value("seller_address"), "seller_address")};
                if (!addresses().contains(seller)) throw HostError(bt::appui::HOST_INVALID_REQUEST, "The seller address is not one of this wallet");
                if (outcome >= m.shares.size()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "No such outcome in this market");
                const int64_t owned{held(seller, m.id, static_cast<uint32_t>(outcome))};
                if (owned < shares) throw HostError(bt::appui::RPC_WALLET_ERROR, "That address holds " + count_text(static_cast<uint64_t>(std::max<int64_t>(owned, 0))) + " shares of this outcome");
                const Quote qt{quote(m, outcome, shares, false)};
                if (qt.net < min_proceeds) throw HostError("The price moved: these shares bring " + coins(qt.net) + " now, below the " + coins(min_proceeds) + " you asked for");
                enough(math::TRADE_MINER_FEE_SATS);
                const MarketNames n{names_of(m.id)};
                Question q;
                q.title = "Sell " + count_text(static_cast<uint64_t>(shares)) + " shares";
                q.message = "Sold to the market maker at its price when the block takes the trade; it is not done if they would bring less than "
                            "the least below. The proceeds come in that block.";
                q.accept = "Sell";
                q.kind = "trade";
                q.rows = {{"Market", bt::appui::display_text_shown(m.title), "text"},
                          {"Outcome", outcome < n.outcomes.size() ? bt::appui::display_text_shown(n.outcomes[outcome]) : "Outcome " + std::to_string(outcome), "text"},
                          {"Shares", count_text(static_cast<uint64_t>(shares)) + " of " + count_text(static_cast<uint64_t>(owned)), "text"},
                          {"From", seller.to_base58(), "address"},
                          {"Brings now", coins(qt.net), "amount"},
                          {"Of which trading fee", coins(qt.fee), "amount"},
                          {"At least", coins(min_proceeds), "amount"},
                          {"Miner fee", coins(math::TRADE_MINER_FEE_SATS), "total"}};
                return q;
            },
            [this](const UniValue& p) { return l2l_call("market_sell", p); }};
        c["market_amplify_beta"] = Confirmed{
            [](const UniValue& p) {
                const UniValue& rq{request_of(p)};
                UniValue r{UniValue::VOBJ};
                r.pushKV("market_id", text_of(rq.find_value("market_id"), "market_id", 12));
                r.pushKV("amount_sats", n64(u64_of(rq.find_value("amount_sats"), "amount_sats")));
                UniValue out{UniValue::VARR};
                out.push_back(r);
                return out;
            },
            [this](const UniValue& p) {
                need_seed();
                const UniValue& rq{p[0]};
                const Market m{market_or_refuse(rq.find_value("market_id"))};
                if (m.market_state != MarketState::Trading) throw HostError("This market does not trade any more");
                const uint64_t amount{rq.find_value("amount_sats").getInt<uint64_t>()};
                if (amount == 0) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Nothing to add");
                if (!addresses().contains(m.creator_address)) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Only the market's author adds liquidity: its address is not one of this wallet");
                const uint64_t total{bt::appui::spend_total({amount, math::TRADE_MINER_FEE_SATS})};
                enough(total);
                Question q;
                q.title = "Add " + coins(amount) + " of liquidity";
                q.message = "The market maker gets deeper: prices move less with each trade. The liquidity stays in the market until it settles.";
                q.accept = "Add";
                q.kind = "market";
                q.rows = {{"Market", bt::appui::display_text_shown(m.title), "text"}, {"Liquidity added", coins(amount), "amount"}, {"Miner fee", coins(math::TRADE_MINER_FEE_SATS), "amount"},
                          {"Total", coins(total), "total"}};
                return q;
            },
            [this](const UniValue& p) { return l2l_call("market_amplify_beta", p); }};

        // ---- votes: one ballot of the wallet's voter address, for the decisions being voted on now, counted as cast
        // (the chain has no sealed votes: our pages' seal-then-reveal is this one step).
        c["vote_submit"] = Confirmed{
            [](const UniValue& p) {
                if (!p.isArray() || p.size() < 2 || !p[0].isArray()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "params: [[{decision_id, vote_value}], fee_sats]");
                if (p[0].empty() || p[0].size() > 1000) throw HostError(bt::appui::HOST_INVALID_REQUEST, "A ballot of 1 to 1000 votes");
                UniValue votes{UniValue::VARR};
                for (const UniValue& v : p[0].getValues()) {
                    if (!v.isObject()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "a vote: {decision_id, vote_value}");
                    UniValue x{UniValue::VOBJ};
                    x.pushKV("decision_id", text_of(v.find_value("decision_id"), "decision_id", 6));
                    x.pushKV("vote_value", UniValue(f64_of(v.find_value("vote_value"), "vote_value")));
                    votes.push_back(x);
                }
                UniValue out{UniValue::VARR};
                out.push_back(votes);
                out.push_back(n64(u64_of(p[1], "fee_sats")));
                return out;
            },
            [this](const UniValue& p) {
                need_seed();
                const uint64_t fee{p[1].getInt<uint64_t>()};
                const Address voter{wallet.voter_address()};
                Question q;
                std::optional<uint32_t> period;
                std::set<DecisionId> seen;
                std::vector<DecisionId> ids;
                for (const UniValue& v : p[0].getValues()) {
                    const std::string text{v.find_value("decision_id").get_str()};
                    auto id{decision_id_of(text)};
                    if (!id) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Not a decision id: " + text);
                    if (!seen.insert(*id).second) throw HostError(bt::appui::HOST_INVALID_REQUEST, "A decision twice in one ballot");
                    if (!period) period = id->voting_period();
                    else if (*period != id->voting_period()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "A ballot is for the decisions of one period");
                    ids.push_back(*id);
                }
                const double rep{state([&](const State& st, const Archive&, const Txn& t) {
                    // Every check the chain makes of a ballot, before asking: a ballot it would refuse costs its fee for nothing.
                    for (size_t i{0}; i < ids.size(); ++i) {
                        const UniValue& v{p[0][i]};
                        const DecisionId* id{&ids[i]};
                        const std::string ids_text{id->to_hex()};
                        auto e{st.decision_entry(t, *id)};
                        if (!e || !e->decision) throw HostError("Decision " + ids_text + " is not claimed");
                        if (st.decision_state(t, *id) != DecisionState::Voting) throw HostError("Decision " + ids_text + " is not being voted on now");
                        if (st.vote(t, id->voting_period(), voter, *id)) throw HostError("Your voter address voted on this decision already: " + e->decision->header);
                        const Decision& d{*e->decision};
                        const double x{v.find_value("vote_value").get_real()};
                        if (d.is_scaled()) {
                            const double mn{d.decision_type.min}, mx{d.decision_type.max}, inc{d.decision_type.increment};
                            if (x < mn || x > mx) throw HostError(bt::appui::HOST_INVALID_REQUEST, d.header + ": a number from " + num_text(mn) + " to " + num_text(mx));
                            const double delta{x - mn}, k{std::round(delta / inc)}, tol{std::fmax(std::fabs(inc) * 1e-9, 1e-12)};
                            if (!(std::fabs(delta - k * inc) <= tol)) throw HostError(bt::appui::HOST_INVALID_REQUEST, d.header + ": in steps of " + num_text(inc) + " from " + num_text(mn));
                        } else if (d.is_categorical()) {
                            const double n{double(d.decision_type.options.size())};
                            if (!(x >= 0.0 && x <= n && x == std::trunc(x))) throw HostError(bt::appui::HOST_INVALID_REQUEST, d.header + ": one of its options");
                        } else if (!(x >= 0.0 && x <= 1.0)) {
                            throw HostError(bt::appui::HOST_INVALID_REQUEST, d.header + ": from 0 (no) to 1 (yes)");
                        }
                        q.rows.push_back({"Decision", bt::appui::display_text_shown(d.header), "text"});
                        q.rows.push_back({"Your answer", bt::appui::display_text_shown(answer_text(d, x)), "text"});
                    }
                    return st.reputation(t, voter);
                })};
                // Only Votecoin votes: a voter address without any is refused by the chain.
                if (!(rep > 0.0)) throw HostError(bt::appui::RPC_WALLET_ERROR, "Your voter address holds no Votecoin: only Votecoin votes (" + voter.to_base58() + ")");
                enough(fee);
                q.title = seen.size() == 1 ? "Cast your vote" : "Cast your ballot (" + std::to_string(seen.size()) + " votes)";
                q.message = "Hivemind counts a ballot as it is cast: there is no sealed vote to reveal later, and your answers are public once "
                            "the ballot is in a block. Your Votecoin weighs them; voting with the consensus keeps and earns Votecoin.";
                q.accept = "Vote";
                q.kind = "vote";
                q.rows.push_back({"Voting period", std::to_string(period.value_or(0)), "text"});
                q.rows.push_back({"Voter", voter.to_base58(), "address"});
                q.rows.push_back({"Votecoin", votecoin_text(rep), "text"});
                q.rows.push_back({"Fee", coins(fee), "total"});
                return q;
            },
            [this](const UniValue& p) { return l2l_call("vote_submit", p); }};
        // Every spend's balance looked at again at once.
        for (auto& [name, conf] : c) {
            auto run{conf.run};
            conf.run = [this, run](const UniValue& p) {
                UniValue r{run(p)};
                if (poller) poller->poke();
                return r;
            };
        }
        return c;
    }
};

const std::vector<std::string>& AppLayer::l2l_reads()
{
    static const std::vector<std::string> reads{
        "balance", "calculate_initial_liquidity", "decision_get", "decision_list", "decision_listing_fee", "decision_status", "format_deposit_address",
        "get_best_mainchain_block_hash", "get_best_sidechain_block_hash", "get_block", "get_block_hash", "get_bmm_inclusions", "get_stxos", "get_transaction",
        "get_transaction_info", "get_utxos", "get_voter_address", "get_wallet_addresses", "get_wallet_utxos", "get_withdrawal_bundle", "getblockcount",
        "latest_failed_withdrawal_bundle_height", "list_mempool", "list_open_periods_with_pricing", "list_peers", "list_utxos", "mainchain_sync_progress",
        "market_get", "market_list", "market_positions", "market_price_history", "openapi_schema", "pending_withdrawal_bundle", "sidechain_wealth",
        "vote_list", "vote_period", "vote_voter", "vote_voters", "votecoin_balance"};
    return reads;
}

const std::vector<std::string>& AppLayer::confirmed_methods()
{
    static const std::vector<std::string> c{"create_deposit", "create_transfer", "create_transfer_many", "create_withdrawal", "decision_claim", "market_amplify_beta",
                                            "market_buy", "market_create", "market_sell", "mine", "transfer_votecoin", "vote_submit"};
    return c;
}

const std::vector<std::string>& AppLayer::host_methods()
{
    static const std::vector<std::string> h{"dropin.overview", "dropin.history", "dropin.newAddress", "dropin.addresses", "dropin.blocks", "dropin.block",
                                            "dropin.transaction", "dropin.address", "dropin.search", "dropin.checkAddress", "dropin.peg", "dropin.peers",
                                            "dropin.peers.connect", "dropin.peers.forget", "dropin.seed.status", "dropin.seed.prepare", "dropin.seed.create",
                                            "dropin.seed.restore", "dropin.seed.show", "dropin.seed.save", "hivemind.status", "hivemind.markets", "hivemind.market",
                                            "hivemind.decisions", "hivemind.quote", "hivemind.positions", "hivemind.votecoin", "hivemind.ballots", "hivemind.payouts"};
    return h;
}

AppLayer::AppLayer(App& app, std::string network, std::string datadir, bt::appui::AppOptions options)
    : m_(std::make_unique<Impl>(app, std::move(network), std::move(datadir), std::move(options)))
{
}

AppLayer::~AppLayer() { stop(); }

uint16_t AppLayer::port() const { return server_ ? server_->port() : 0; }

UniValue AppLayer::history(size_t limit) { return m_->history(limit); }

bool AppLayer::start(std::string& err)
{
    Impl& m{*m_};
    if (!bt::fsperm::private_dir(m.datadir)) {
        err = "cannot make " + m.datadir;
        return false;
    }
    m.store = std::make_unique<bt::appui::AppStore>(m.datadir + "/backbone-app.sqlite");
    std::string why;
    bt::appui::ServerOptions so{bt::appui::server_options(m.options, m.datadir, why)};
    if (!why.empty()) {
        err = why;
        return false;
    }
    server_ = std::make_unique<bt::appui::RpcServer>(std::move(so));
    m.link = std::make_unique<bt::appui::LinkMonitor>(m.node.enforcer(), THIS_SIDECHAIN, [&m] { return m.followed(); });

    // ---- the app RPC's own methods: the LayerTwo reads, as theirs; the reads of the kit's shell; the setup of the wallet.
    for (const std::string& name : l2l_reads()) {
        server_->add(name, {}, [&m, name](const bt::appui::Call& c) { return m.l2l_call(name, c.params); });
    }
    server_->add("getmainchaininfo", {}, [&m](const bt::appui::Call&) { return m.link->info(); });
    server_->add("getbalances", {}, [&m](const bt::appui::Call&) {
        UniValue mine{UniValue::VOBJ};
        const Impl::Balances b{m.balances()};
        mine.pushKV("trusted", bt::appui::coins_value(b.available));
        mine.pushKV("untrusted_pending", bt::appui::coins_value(b.pending_in));
        mine.pushKV("immature", bt::appui::coins_value(b.withdrawing));
        UniValue out{UniValue::VOBJ};
        out.pushKV("mine", mine);
        return out;
    });
    server_->add("getblockchaininfo", {}, [&m](const bt::appui::Call&) {
        UniValue o{UniValue::VOBJ};
        auto h{m.node.height()};
        auto t{m.node.tip()};
        o.pushKV("chain", m.network);
        o.pushKV("blocks", h ? int64_t(*h) : int64_t{-1});
        o.pushKV("headers", h ? int64_t(*h) : int64_t{-1});
        o.pushKV("bestblockhash", t ? bt::hex_encode(*t) : "");
        o.pushKV("initialblockdownload", false);
        o.pushKV("verificationprogress", 1.0);
        o.pushKV("warnings", UniValue{UniValue::VARR});
        return o;
    });
    server_->add("getnetworkinfo", {}, [&m](const bt::appui::Call&) {
        UniValue o{UniValue::VOBJ};
        size_t n{0};
        for (auto& p : m.node.peers()) n += p.connected ? 1 : 0;
        o.pushKV("version", 2000000);
        o.pushKV("subversion", std::string{"/backbone-truthcoin:"} + VERSION_TEXT + "/");
        o.pushKV("connections", static_cast<uint64_t>(n));
        o.pushKV("networkactive", true);
        o.pushKV("warnings", UniValue{UniValue::VARR});
        return o;
    });
    server_->add("uptime", {}, [&m](const bt::appui::Call&) {
        return UniValue(static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - m.started).count()));
    });
    // The setup of the wallet by the unified wallet (doc/app.md "One backup phrase"): its own user (the cookie) sets
    // the phrase it derived; refused when the wallet has another one. Never offered to the pages.
    server_->add("getwalletphraseinfo", {}, [&m](const bt::appui::Call&) { return m.seed_status(); });
    server_->add("setwalletphrase", {"phrase"}, [&m](const bt::appui::Call& c) {
        if (!c[0].isStr()) throw RpcError(bt::appui::RPC_INVALID_PARAMETER, "phrase: the 12 to 24 words");
        std::string phrase{c[0].get_str()};
        const bt::appui::WipeOnExit wipe_phrase{phrase};
        try {
            UniValue r{m.set_phrase(phrase, "unified")};
            LOG_INFO("app layer: the wallet's phrase was set by the app (fingerprint %s)", r.find_value("fingerprint").get_str().c_str());
            return r;
        } catch (const HostError& e) {
            throw RpcError(e.code, e.what());
        }
    });
    server_->add("stop", {}, [&m](const bt::appui::Call&) {
        if (m.app.on_stop) m.app.on_stop();
        return UniValue("Truthcoin stopping");
    });
    server_->add("getappmanifest", {}, [this](const bt::appui::Call&) { return manifest(); });

    // ---- the host of the pages
    bt::appui::ChainHost chain;
    chain.name = "Hivemind";
    chain.stem = "hivemind";
    chain.network = m.network;
    chain.info = [&m] { return m.info(); };
    chain.wallet_id = [&m] { return m.wallet_id(); };
    chain.page_reads = {"getmainchaininfo", "getbalances", "getblockchaininfo", "getnetworkinfo", "uptime"};
    for (const std::string& r : l2l_reads()) chain.page_reads.insert(r);
    chain.confirmed = m.confirmed();
    auto& H{chain.host};
    auto& P{chain.privileges};
    H["dropin.overview"] = [&m](const UniValue&, HostEnv&) {
        UniValue o{UniValue::VOBJ};
        const bool has{m.wallet.has_seed()};
        const Impl::Balances b{m.balances()};
        UniValue w{UniValue::VOBJ};
        w.pushKV("hasSeed", has);
        w.pushKV("available", n64(b.available));
        w.pushKV("withdrawing", n64(b.withdrawing));
        w.pushKV("pendingIn", n64(b.pending_in));
        w.pushKV("pendingOut", n64(b.pending_out));
        o.pushKV("wallet", w);
        auto h{m.node.height()};
        auto t{m.node.tip()};
        UniValue c{UniValue::VOBJ};
        c.pushKV("height", h ? int64_t(*h) : int64_t{-1});
        c.pushKV("tip", t ? bt::hex_encode(*t) : "");
        size_t peers{0};
        for (auto& p : m.node.peers()) peers += p.connected ? 1 : 0;
        c.pushKV("peers", static_cast<uint64_t>(peers));
        c.pushKV("wealth", n64(m.node.sidechain_wealth()));
        c.pushKV("slot", static_cast<int>(THIS_SIDECHAIN));
        c.pushKV("network", m.network);
        o.pushKV("chain", c);
        o.pushKV("mainchain", m.link->info());
        return o;
    };
    P["dropin.overview"] = {"balance", "getblockcount"};
    H["dropin.history"] = [&m](const UniValue& p, HostEnv&) {
        const UniValue& lim{param(p, "limit")};
        const size_t limit{lim.isNum() ? static_cast<size_t>(std::clamp<int64_t>(lim.getInt<int64_t>(), 1, 5000)) : size_t{500}};
        if (!m.wallet.has_seed()) return UniValue{UniValue::VARR};
        return m.history(limit);
    };
    P["dropin.history"] = {"get_wallet_utxos"};
    H["dropin.newAddress"] = [&m](const UniValue&, HostEnv&) {
        m.need_seed();
        const Address a{m.wallet.get_new_address()};
        UniValue o{UniValue::VOBJ};
        o.pushKV("address", a.to_base58());
        o.pushKV("depositAddress", a.format_for_deposit());
        return o;
    };
    P["dropin.newAddress"] = {"get_new_address"};
    H["dropin.addresses"] = [&m](const UniValue&, HostEnv&) {
        UniValue out{UniValue::VARR};
        if (!m.wallet.has_seed()) return out;
        std::map<Address, std::pair<uint64_t, uint64_t>> got; // balance, received
        const auto mine{m.addresses()};
        for (auto& [op, o] : m.node.utxos_by_addresses(mine)) {
            got[o.address].first += o.get_value();
            got[o.address].second += o.get_value();
        }
        for (auto& [op, s] : m.node.stxos_by_addresses(mine)) got[s.output.address].second += s.output.get_value();
        for (const Address& a : mine) {
            UniValue r{UniValue::VOBJ};
            r.pushKV("address", a.to_base58());
            r.pushKV("depositAddress", a.format_for_deposit());
            r.pushKV("balance", n64(got[a].first));
            r.pushKV("received", n64(got[a].second));
            out.push_back(r);
        }
        return out;
    };
    P["dropin.addresses"] = {"get_wallet_addresses"};
    H["dropin.blocks"] = [&m](const UniValue& p, HostEnv&) {
        const UniValue& cnt{param(p, "count")};
        const UniValue& from{param(p, "from")};
        const int64_t count{cnt.isNum() ? std::clamp<int64_t>(cnt.getInt<int64_t>(), 1, 200) : int64_t{20}};
        UniValue out{UniValue::VARR};
        auto h{m.node.height()};
        if (!h) return out;
        int64_t at{from.isNum() ? std::min<int64_t>(from.getInt<int64_t>(), *h) : int64_t(*h)};
        for (; at >= 0 && static_cast<int64_t>(out.size()) < count; --at) {
            auto hash{m.node.block_hash_at(static_cast<uint32_t>(at))};
            if (!hash) break;
            auto hd{m.node.header(*hash)};
            auto b{m.node.body(*hash)};
            if (!hd || !b) continue;
            out.push_back(m.block_summary(static_cast<uint32_t>(at), *hash, *hd, *b));
        }
        return out;
    };
    P["dropin.blocks"] = {"get_block"};
    H["dropin.block"] = [&m](const UniValue& p, HostEnv& env) {
        const UniValue& height{param(p, "height")};
        if (height.isNum()) {
            auto hash{m.node.block_hash_at(static_cast<uint32_t>(std::clamp<int64_t>(height.getInt<int64_t>(), 0, UINT32_MAX)))};
            if (!hash) throw HostError("No block at this height");
            return m.block_detail(*hash, m.mine_for(env));
        }
        auto hash{m.parse_hash(str_param(p, "hash"))};
        if (!hash) throw HostError(bt::appui::HOST_INVALID_REQUEST, "hash: 64 hex digits");
        return m.block_detail(*hash, m.mine_for(env));
    };
    P["dropin.block"] = {"get_block"};
    H["dropin.transaction"] = [&m](const UniValue& p, HostEnv& env) {
        auto txid{m.parse_hash(str_param(p, "txid"))};
        if (!txid) throw HostError(bt::appui::HOST_INVALID_REQUEST, "txid: 64 hex digits");
        return m.tx_detail(*txid, m.mine_for(env));
    };
    P["dropin.transaction"] = {"get_transaction"};
    H["dropin.address"] = [&m](const UniValue& p, HostEnv& env) {
        auto a{Address::from_base58(str_param(p, "address"))};
        if (!a) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Not a Hivemind address");
        return m.address_detail(*a, m.mine_for(env));
    };
    P["dropin.address"] = {"get_utxos", "get_stxos"};
    H["dropin.search"] = [&m](const UniValue& p, HostEnv&) {
        std::string q{str_param(p, "query")};
        q.erase(0, q.find_first_not_of(" \t"));
        q.erase(q.find_last_not_of(" \t") + 1);
        UniValue o{UniValue::VOBJ};
        if (q.size() > 120) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Too long");
        if (!q.empty() && q.size() <= 10 && q.find_first_not_of("0123456789") == std::string::npos) {
            auto hash{m.node.block_hash_at(static_cast<uint32_t>(std::min<uint64_t>(std::stoull(q), UINT32_MAX)))};
            if (hash) {
                o.pushKV("kind", "block");
                o.pushKV("hash", bt::hex_encode(*hash));
                return o;
            }
        } else if (auto h{m.parse_hash(q)}) {
            if (m.node.header(*h)) {
                o.pushKV("kind", "block");
                o.pushKV("hash", q);
                return o;
            }
            o.pushKV("kind", "transaction");
            o.pushKV("txid", q);
            return o;
        } else if (Address::from_base58(q)) {
            o.pushKV("kind", "address");
            o.pushKV("address", q);
            return o;
        } else if (auto a{Address::from_deposit_address(q)}) {
            // A deposit address: the Hivemind address inside.
            o.pushKV("kind", "address");
            o.pushKV("address", a->to_base58());
            return o;
        }
        o.pushKV("kind", "none");
        return o;
    };
    P["dropin.search"] = {"get_block", "get_transaction"};
    H["dropin.checkAddress"] = [&m](const UniValue& p, HostEnv& env) {
        const std::string text{str_param(p, "address")};
        const std::string side{param(p, "side").isStr() ? param(p, "side").get_str() : "hivemind"};
        UniValue o{UniValue::VOBJ};
        if (side == "mainchain") {
            auto a{bt::btc::Address::parse(text)};
            o.pushKV("valid", a.has_value() && main_address_of_network(*a, m.network));
            if (a && !main_address_of_network(*a, m.network)) o.pushKV("error", "an eCash address of another network");
            else if (!a) o.pushKV("error", Address::from_base58(text) ? "a Hivemind address, not an eCash one" : "not an eCash address");
            return o;
        }
        auto a{Address::from_base58(text)};
        o.pushKV("valid", a.has_value());
        // Whether it is the wallet's: only to a caller that may read the wallet's addresses.
        if (a) {
            if (env.may_call(WALLET_READ)) o.pushKV("mine", m.addresses().contains(*a));
        } else {
            o.pushKV("error", bt::btc::Address::parse(text) ? "an eCash address, not a Hivemind one" : text.rfind("s13_", 0) == 0 ? "a deposit address: use it on eCash" : "not a Hivemind address");
        }
        return o;
    };
    P["dropin.checkAddress"] = {};
    H["dropin.peg"] = [&m](const UniValue&, HostEnv&) { return m.peg(); };
    P["dropin.peg"] = {"pending_withdrawal_bundle", "get_wallet_utxos"};
    H["dropin.peers"] = [&m](const UniValue&, HostEnv&) {
        UniValue out{UniValue::VARR};
        for (auto& pv : m.node.peers()) {
            UniValue o{UniValue::VOBJ};
            o.pushKV("address", pv.address.to_string());
            o.pushKV("connected", pv.connected);
            o.pushKV("inbound", pv.inbound);
            out.push_back(o);
        }
        return out;
    };
    P["dropin.peers"] = {"list_peers"};
    H["dropin.peers.connect"] = [&m](const UniValue& p, HostEnv&) {
        std::string a{str_param(p, "address")};
        if (a.rfind(':') == std::string::npos) a += ":" + std::to_string(DEFAULT_PORT);
        std::string err;
        if (!m.node.connect_peer(a, err)) throw HostError(err);
        return UniValue{true};
    };
    P["dropin.peers.connect"] = {"connect_peer"};
    H["dropin.peers.forget"] = [&m](const UniValue& p, HostEnv&) {
        std::string a{str_param(p, "address")};
        if (a.rfind(':') == std::string::npos) a += ":" + std::to_string(DEFAULT_PORT);
        return UniValue{m.node.forget_peer(a)};
    };
    P["dropin.peers.forget"] = {"forget_peer"};
    // ---- the backup phrase: its words shown only in the app's trusted dialog, never to the pages.
    H["dropin.seed.status"] = [&m](const UniValue&, HostEnv&) { return m.seed_status(); };
    P["dropin.seed.status"] = {"get_wallet_addresses"};
    H["dropin.seed.prepare"] = [&m](const UniValue&, HostEnv&) {
        if (m.wallet.has_seed()) throw HostError(bt::appui::RPC_WALLET_ERROR, "This wallet has a backup phrase already");
        UniValue o{UniValue::VOBJ};
        o.pushKV("id", m.phrases.put(bt::hd::generate_mnemonic(12)));
        o.pushKV("words", 12);
        return o;
    };
    P["dropin.seed.prepare"] = {"set_seed_from_mnemonic"};
    H["dropin.seed.create"] = [&m](const UniValue& p, HostEnv& env) {
        const std::string id{str_param(p, "id")};
        auto phrase{m.phrases.get(id)};
        if (!phrase) throw HostError(bt::appui::HOST_INVALID_REQUEST, "This new phrase is not known any more (5 minutes): make another");
        const bt::appui::WipeOnExit wipe_phrase{*phrase};
        if (m.wallet.has_seed()) throw HostError(bt::appui::RPC_WALLET_ERROR, "This wallet has a backup phrase already");
        Question q;
        const bt::appui::WipeOnExit wipe_words{q.details};
        q.title = "Your backup phrase";
        q.message = "Write these 12 words down, in order, on paper, and keep them safe. They are the only way to get this wallet back, "
                    "its coins and its Votecoin. Anyone who has them can take them. They are not shown to the app's pages.";
        q.details = Impl::words_block(*phrase);
        q.accept = "I wrote them down";
        q.level = "warning";
        q.kind = "phrase.create";
        if (!env.ask(q).accepted) throw HostError(bt::appui::HOST_CANCELLED, "Cancelled");
        UniValue r{m.set_phrase(*phrase, "app")};
        m.phrases.erase(id);
        return r;
    };
    P["dropin.seed.create"] = {"set_seed_from_mnemonic"};
    H["dropin.seed.restore"] = [&m](const UniValue& p, HostEnv& env) {
        std::string phrase{str_param(p, "phrase")};
        const bt::appui::WipeOnExit wipe_typed{phrase};
        auto parsed{bt::hd::parse_mnemonic(phrase)};
        if (!parsed) throw HostError(bt::appui::HOST_INVALID_REQUEST, bt::hd::mnemonic_error(phrase));
        const bt::appui::WipeOnExit wipe_parsed{*parsed};
        if (m.wallet.has_seed()) throw HostError(bt::appui::RPC_WALLET_ERROR, "This wallet has a backup phrase already");
        auto seed{bt::hd::mnemonic_to_seed(*parsed)};
        const std::string fp{bt::appui::seed_fingerprint(seed.span())};
        sodium_memzero(seed.data(), seed.size());
        Question q;
        q.title = "Restore the wallet from this phrase";
        q.message = "The wallet takes the keys of this backup phrase. Its coins, Votecoin and shares show once the node has looked at the chain.";
        q.rows = {{"Words", std::to_string(std::count(parsed->begin(), parsed->end(), ' ') + 1), "text"}, {"Fingerprint", fp, "text"}};
        q.accept = "Restore";
        q.kind = "phrase.restore";
        if (!env.ask(q).accepted) throw HostError(bt::appui::HOST_CANCELLED, "Cancelled");
        return m.set_phrase(*parsed, "restored");
    };
    P["dropin.seed.restore"] = {"set_seed_from_mnemonic"};
    // Shown or saved only when named in the whitelist (no privilege stands for it): the most sensitive read there is.
    H["dropin.seed.show"] = [&m](const UniValue&, HostEnv& env) {
        auto phrase{m.kept_phrase()};
        if (!phrase) throw HostError(bt::appui::RPC_WALLET_ERROR, "This wallet's phrase was not set through the app: keep the backup you made then");
        const bt::appui::WipeOnExit wipe_phrase{*phrase};
        Question q;
        const bt::appui::WipeOnExit wipe_words{q.details};
        q.title = "Your backup phrase";
        q.message = "Make sure nobody can see your screen. Anyone who has these words can take this wallet's coins and Votecoin.";
        q.details = Impl::words_block(*phrase);
        q.accept = "Done";
        q.cancel = "Close";
        q.level = "warning";
        q.kind = "phrase.show";
        env.ask(q);
        return UniValue{true};
    };
    H["dropin.seed.save"] = [&m](const UniValue& p, HostEnv& env) {
        const std::string path{str_param(p, "path")};
        const auto file{env.file(path, true)};
        auto phrase{m.kept_phrase()};
        if (!phrase) throw HostError(bt::appui::RPC_WALLET_ERROR, "This wallet's phrase was not set through the app: keep the backup you made then");
        const bt::appui::WipeOnExit wipe_phrase{*phrase};
        Question q;
        q.title = "Save your backup phrase to a file";
        q.message = "Anyone who can read this file can take this wallet's coins and Votecoin. Keep it somewhere only you can reach, off this computer if you can.";
        q.rows = {{"File", path, "text"}};
        q.accept = "Save";
        q.kind = "phrase.save";
        if (!env.ask(q).accepted) throw HostError(bt::appui::HOST_CANCELLED, "Cancelled");
        std::string content{"Hivemind backup phrase (BIP-39, truthcoin 0.20.0 keys)\n" + *phrase + "\n"};
        const bt::appui::WipeOnExit wipe_content{content};
        try {
            bt::appui::ChosenFiles::write(file, content);
        } catch (const std::runtime_error& e) {
            throw HostError(std::string{"The file chosen cannot be written: "} + e.what());
        }
        return UniValue{true};
    };

    // ---- Hivemind's own: the period clock, the markets, the decisions, a quote; the wallet's positions, Votecoin,
    // ballots and payouts (only to a reader of the wallet's addresses: they say which of the chain's are the wallet's).
    H["hivemind.status"] = [&m](const UniValue&, HostEnv&) {
        return m.state([&](const State& st, const Archive&, const Txn& t) {
            UniValue o{UniValue::VOBJ};
            o.pushKV("period", m.period_json(st, t));
            const auto markets{st.all_markets(t)};
            uint64_t trading{0}, settled{0};
            for (auto& mk : markets) {
                trading += mk.market_state == MarketState::Trading;
                settled += mk.market_state == MarketState::Settled;
            }
            UniValue counts{UniValue::VOBJ};
            counts.pushKV("markets", static_cast<uint64_t>(markets.size()));
            counts.pushKV("trading", trading);
            counts.pushKV("settled", settled);
            uint64_t decisions{0}, voting{0};
            for (auto& [period, es] : st.all_period_decisions(t))
                for (auto& e : es) {
                    if (!e.decision) continue;
                    ++decisions;
                    voting += st.decision_state(t, e.decision_id) == DecisionState::Voting;
                }
            counts.pushKV("decisions", decisions);
            counts.pushKV("voting", voting);
            const auto reps{st.all_reputations(t)};
            double total{0};
            for (auto& [a, r] : reps) total += r;
            counts.pushKV("voters", static_cast<uint64_t>(reps.size()));
            o.pushKV("counts", counts);
            o.pushKV("votecoinTotal", fnum(total));
            o.pushKV("minerFee", n64(math::TRADE_MINER_FEE_SATS));
            return o;
        });
    };
    P["hivemind.status"] = {"decision_status"};
    H["hivemind.markets"] = [&m](const UniValue&, HostEnv& env) {
        const Impl::Mine mine{m.mine_for(env)};
        return m.state([&](const State& st, const Archive&, const Txn& t) {
            UniValue out{UniValue::VARR};
            for (auto& mk : st.all_markets(t)) {
                if (out.size() >= 2000) break;
                out.push_back(m.market_json(st, t, mk, mine));
            }
            return out;
        });
    };
    P["hivemind.markets"] = {"market_list", "market_get"};
    H["hivemind.market"] = [&m](const UniValue& p, HostEnv& env) {
        auto id{market_id_of(str_param(p, "id"))};
        if (!id) throw HostError(bt::appui::HOST_INVALID_REQUEST, "id: the market's id (12 hex digits)");
        const Impl::Mine mine{m.mine_for(env)};
        UniValue o{m.state([&](const State& st, const Archive&, const Txn& t) -> UniValue {
            auto mk{st.market(t, *id)};
            if (!mk) throw HostError("No such market");
            UniValue j{m.market_json(st, t, *mk, mine)};
            // Who holds its shares: the chain's share accounts (whose they are only to a reader of the wallet).
            UniValue holders{UniValue::VARR};
            for (auto& [a, acc] : st.all_share_accounts(t))
                for (auto& [k, shares] : acc.positions) {
                    if (k.first != *id || shares == 0 || holders.size() >= 200) continue;
                    UniValue h{UniValue::VOBJ};
                    h.pushKV("address", a.to_base58());
                    h.pushKV("outcome", static_cast<uint64_t>(k.second));
                    h.pushKV("shares", UniValue(shares));
                    if (mine) h.pushKV("mine", mine->contains(a));
                    holders.push_back(h);
                }
            j.pushKV("holders", holders);
            return j;
        })};
        // Its prices over time, as the chain made them (market_price_history).
        UniValue params{UniValue::VARR};
        params.push_back(id->to_hex());
        try {
            o.pushKV("history", m.l2l_call("market_price_history", params));
        } catch (const RpcError&) {
            o.pushKV("history", UniValue{UniValue::VARR});
        }
        return o;
    };
    P["hivemind.market"] = {"market_get", "market_price_history"};
    H["hivemind.decisions"] = [&m](const UniValue&, HostEnv& env) {
        const Impl::Mine mine{m.mine_for(env)};
        return m.state([&](const State& st, const Archive&, const Txn& t) {
            UniValue out{UniValue::VARR};
            for (auto& [period, es] : st.all_period_decisions(t))
                for (auto& e : es) {
                    if (!e.decision || out.size() >= 5000) continue;
                    out.push_back(m.decision_json(st, t, e, mine));
                }
            return out;
        });
    };
    P["hivemind.decisions"] = {"decision_list", "decision_get"};
    H["hivemind.quote"] = [&m](const UniValue& p, HostEnv&) {
        const Market mk{m.market_or_refuse(param(p, "market"))};
        const uint64_t outcome{u64_of(param(p, "outcome"), "outcome")};
        const int64_t shares{shares_of(param(p, "shares"), "shares")};
        const std::string side{param(p, "side").isStr() ? param(p, "side").get_str() : "buy"};
        if (side != "buy" && side != "sell") throw HostError(bt::appui::HOST_INVALID_REQUEST, "side: buy or sell");
        if (outcome >= mk.shares.size()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "No such outcome in this market");
        const Impl::Quote q{m.quote(mk, outcome, shares, side == "buy")};
        UniValue o{UniValue::VOBJ};
        o.pushKV("side", side);
        o.pushKV("shares", UniValue(shares));
        o.pushKV("cost", n64(q.cost));
        o.pushKV("tradingFee", n64(q.fee));
        if (side == "sell") o.pushKV("net", n64(q.net));
        o.pushKV("minerFee", n64(math::TRADE_MINER_FEE_SATS));
        o.pushKV("priceBefore", fnum(q.price_before));
        o.pushKV("priceAfter", fnum(q.price_after));
        return o;
    };
    P["hivemind.quote"] = {"market_get"};
    H["hivemind.positions"] = [&m](const UniValue&, HostEnv&) {
        const auto mine{m.addresses()};
        return m.state([&](const State& st, const Archive&, const Txn& t) {
            UniValue out{UniValue::VARR};
            std::map<MarketId, std::pair<Market, std::vector<double>>> cache;
            for (const Address& a : mine) {
                auto acc{st.share_account(t, a)};
                if (!acc) continue;
                for (auto& [k, shares] : acc->positions) {
                    if (shares == 0 || out.size() >= 5000) continue;
                    auto it{cache.find(k.first)};
                    if (it == cache.end()) {
                        auto mk{st.market(t, k.first)};
                        if (!mk) continue;
                        auto ms{mk->market_state == MarketState::Trading ? st.mempool_shares(t, k.first) : std::nullopt};
                        it = cache.emplace(k.first, std::make_pair(*mk, Impl::prices_or_uniform(ms ? *ms : mk->shares, Impl::market_beta(*mk)))).first;
                    }
                    const auto& [mk, prices] = it->second;
                    const double price{k.second < prices.size() ? prices[k.second] : 0.0};
                    UniValue o{UniValue::VOBJ};
                    o.pushKV("market", k.first.to_hex());
                    o.pushKV("outcome", static_cast<uint64_t>(k.second));
                    o.pushKV("address", a.to_base58());
                    o.pushKV("shares", UniValue(shares));
                    o.pushKV("price", fnum(price));
                    // What they are worth at the price now (a share pays 1 sat if right), in satoshis.
                    o.pushKV("value", fnum(std::fmax(0.0, std::floor(double(shares) * price))));
                    out.push_back(o);
                }
            }
            return out;
        });
    };
    P["hivemind.positions"] = {"get_wallet_addresses", "market_positions"};
    H["hivemind.votecoin"] = [&m](const UniValue&, HostEnv&) {
        UniValue o{UniValue::VOBJ};
        if (!m.wallet.has_seed()) {
            o.pushKV("voter", UniValue{});
            o.pushKV("votecoin", 0.0);
            o.pushKV("voterCoins", n64(0));
            o.pushKV("addresses", UniValue{UniValue::VARR});
            return o;
        }
        const Address voter{m.wallet.voter_address()};
        const auto mine{m.addresses()};
        uint64_t voter_coins{0};
        const auto spent{m.wallet.mempool_spent()};
        for (auto& [op, out] : m.wallet.utxos())
            if (out.address == voter && out.content.kind == Content::Value && !spent.contains(op)) voter_coins = voter_coins + out.get_value() < voter_coins ? UINT64_MAX : voter_coins + out.get_value();
        return m.state([&](const State& st, const Archive&, const Txn& t) {
            o.pushKV("voter", voter.to_base58());
            o.pushKV("votecoin", fnum(st.reputation(t, voter)));
            o.pushKV("voterCoins", n64(voter_coins));
            UniValue other{UniValue::VARR};
            for (const Address& a : mine) {
                const double r{st.reputation(t, a)};
                if (a == voter || !(r > 0.0)) continue;
                UniValue x{UniValue::VOBJ};
                x.pushKV("address", a.to_base58());
                x.pushKV("votecoin", fnum(r));
                other.push_back(x);
            }
            o.pushKV("addresses", other);
            return o;
        });
    };
    P["hivemind.votecoin"] = {"get_wallet_addresses", "get_voter_address", "votecoin_balance"};
    H["hivemind.ballots"] = [&m](const UniValue&, HostEnv&) {
        UniValue out{UniValue::VARR};
        if (!m.wallet.has_seed()) return out;
        const auto mine{m.addresses()};
        return m.state([&](const State& st, const Archive&, const Txn& t) {
            for (auto& [k, e] : st.all_votes(t)) {
                if (!mine.contains(k.voter) || out.size() >= 2000) continue;
                UniValue o{UniValue::VOBJ};
                o.pushKV("decision", k.decision_id.to_hex());
                o.pushKV("period", static_cast<uint64_t>(k.period_id));
                o.pushKV("voter", k.voter.to_base58());
                o.pushKV("height", static_cast<uint64_t>(e.block_height));
                auto v{e.value.to_float()};
                auto d{st.decision_entry(t, k.decision_id)};
                if (v && d && d->decision) {
                    o.pushKV("value", fnum(d->decision->denormalize_value(*v)));
                    o.pushKV("answer", answer_text(*d->decision, d->decision->denormalize_value(*v)));
                } else {
                    o.pushKV("value", UniValue{});
                    o.pushKV("answer", "Abstain");
                }
                out.push_back(o);
            }
            return out;
        });
    };
    P["hivemind.ballots"] = {"get_wallet_addresses", "vote_list"};
    H["hivemind.payouts"] = [&m](const UniValue&, HostEnv&) {
        const auto mine{m.addresses()};
        return m.state([&](const State& st, const Archive&, const Txn& t) {
            UniValue out{UniValue::VARR};
            for (auto& mk : st.markets_by_state(t, MarketState::Settled)) {
                auto v{t.get(State::T_SETTLEMENT_UNDO, u32_key(mk.last_updated_height))};
                if (!v) continue;
                for (auto& e : dec_settlement_undo(*v)) {
                    const auto& s{e.payout_summary};
                    if (s.market_id != mk.id) continue;
                    UniValue o{UniValue::VOBJ};
                    o.pushKV("market", mk.id.to_hex());
                    o.pushKV("height", static_cast<uint64_t>(s.block_height));
                    o.pushKV("treasuryPaid", n64(s.treasury_distributed));
                    o.pushKV("feesPaid", n64(s.total_fees_distributed));
                    o.pushKV("shareholders", static_cast<uint64_t>(s.shareholder_count));
                    UniValue shares{UniValue::VARR}, fees{UniValue::VARR};
                    uint64_t total{0};
                    for (auto& p : s.payouts) {
                        if (!mine.contains(p.address)) continue;
                        UniValue x{UniValue::VOBJ};
                        x.pushKV("address", p.address.to_base58());
                        x.pushKV("outcome", static_cast<uint64_t>(p.outcome_index));
                        x.pushKV("shares", UniValue(p.shares_redeemed));
                        x.pushKV("finalPrice", fnum(p.final_price));
                        x.pushKV("paid", n64(p.payout_sats));
                        total = bt::appui::spend_total({total, p.payout_sats});
                        shares.push_back(x);
                    }
                    for (auto& f : s.fee_payouts) {
                        if (!mine.contains(f.address)) continue;
                        UniValue x{UniValue::VOBJ};
                        x.pushKV("address", f.address.to_base58());
                        x.pushKV("role", f.fee_role == FeeRole::CorrectVoter ? "voter" : f.fee_role == FeeRole::DecisionAuthor ? "decision-author" : "market-author");
                        x.pushKV("paid", n64(f.amount_sats));
                        total = bt::appui::spend_total({total, f.amount_sats});
                        fees.push_back(x);
                    }
                    o.pushKV("shares", shares);
                    o.pushKV("fees", fees);
                    if (s.creator_refund && mine.contains(s.creator_refund->first)) {
                        o.pushKV("refund", n64(s.creator_refund->second));
                        total = bt::appui::spend_total({total, s.creator_refund->second});
                    }
                    o.pushKV("total", n64(total));
                    out.push_back(o);
                }
            }
            return out;
        });
    };
    P["hivemind.payouts"] = {"get_wallet_addresses", "market_get"};

    chain.snapshot = [&m] { return m.poller ? m.poller->snapshot() : UniValue{UniValue::VOBJ}; };
    host_ = std::make_unique<bt::appui::RemoteHost>(std::move(chain), bt::appui::HostOptions{m.options.chooser, m.options.file_dir}, *server_);
    m.poller = std::make_unique<bt::appui::Poller>(host_->events(), WALLET_NAME, [&m] { return m.look(); });
    if (!server_->listen(err)) return false;
    m.link->start();
    m.poller->start();
    LOG_INFO("app RPC on 127.0.0.1:%u (%zu methods; the LayerTwo RPC is not changed by it)", unsigned(server_->port()), server_->methods().size());
    return true;
}

void AppLayer::stop()
{
    if (!m_) return;
    if (m_->poller) m_->poller->stop();
    if (m_->link) m_->link->stop();
    if (host_) host_->stop();
    if (server_) server_->stop();
}

std::vector<std::string> AppLayer::app_methods() const
{
    std::vector<std::string> out{server_ ? server_->methods() : std::vector<std::string>{}};
    return out;
}

UniValue AppLayer::manifest() const
{
    // What a pack of Hivemind's pages uses of this node (the unified wallet names it in the app's whitelist): the
    // reads of the pages, the confirmed methods, the host methods, and the host's own methods.
    UniValue o{UniValue::VOBJ};
    o.pushKV("stem", "hivemind");
    o.pushKV("hostApi", bt::appui::HOST_API);
    UniValue reads{UniValue::VARR};
    for (const char* r : {"getmainchaininfo", "getbalances", "getblockchaininfo", "getnetworkinfo", "uptime"}) reads.push_back(r);
    for (const std::string& r : l2l_reads()) reads.push_back(r);
    o.pushKV("reads", reads);
    UniValue confirmed{UniValue::VARR};
    for (const std::string& c : confirmed_methods()) confirmed.push_back(c);
    o.pushKV("confirmed", confirmed);
    UniValue host{UniValue::VARR};
    for (const std::string& h : host_methods()) host.push_back(h);
    o.pushKV("host", host);
    UniValue protocol{UniValue::VARR};
    for (const char* p : {"webuihost", "describeconfirmation", "webuievents", "webuichosenfile", "appserverproof"}) protocol.push_back(p);
    o.pushKV("protocol", protocol);
    return o;
}

} // namespace hivemind
