// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <hivemind/wallet.h>

#include <hivemind/json.h>
#include <hivemind/math.h>

#include <bt/crypto.h>
#include <bt/ecies.h>

#include <sodium.h>

#include <algorithm>
#include <cstring>

namespace hivemind {

using bt::kv::Txn;

enum : uint8_t { W_SEED = 1, W_INDEX_TO_ADDR, W_ADDR_TO_INDEX, W_INDEX_TO_EPK, W_EPK_TO_INDEX, W_INDEX_TO_VK, W_VK_TO_INDEX };
enum KeyPurpose : uint32_t { TX_SIGNING = 0, ENCRYPTION = 1, MESSAGE_SIGNING = 2 };

static Bytes be32(uint32_t v)
{
    Writer w;
    w.u32be(v);
    return w.buf;
}
static uint32_t from_be32(ByteSpan b) { return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3]; }

// ------------------------------------------------------------------ parse_dimensions
static std::optional<DecisionId> parse_single(const std::string& s)
{
    auto b = bt::hex_decode(s);
    if (!b || b->size() != 3) return std::nullopt;
    DecisionId d;
    std::memcpy(d.b.data(), b->data(), 3);
    return d;
}

static std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::optional<std::vector<DimensionSpec>> parse_dimensions(const std::string& in)
{
    std::string s = trim(in);
    if (s.empty() || s.front() != '[' || s.back() != ']' || s.size() < 2) return std::nullopt;
    std::string inner = s.substr(1, s.size() - 2);
    std::vector<DimensionSpec> dims;
    size_t i = 0;
    while (i < inner.size()) {
        while (i < inner.size() && (std::isspace((unsigned char)inner[i]) || inner[i] == ',')) i++;
        if (i >= inner.size()) break;
        if (inner[i] == '[') {
            size_t start = i + 1;
            int depth = 1;
            i++;
            while (i < inner.size() && depth > 0) {
                if (inner[i] == '[') depth++;
                else if (inner[i] == ']') depth--;
                i++;
            }
            if (depth != 0) return std::nullopt;
            auto d = parse_single(trim(inner.substr(start, i - 1 - start)));
            if (!d) return std::nullopt;
            dims.push_back(DimensionSpec{DimensionSpec::Categorical, *d});
        } else {
            size_t start = i;
            while (i < inner.size() && inner[i] != ',' && inner[i] != '[') i++;
            auto d = parse_single(trim(inner.substr(start, i - start)));
            if (!d) return std::nullopt;
            dims.push_back(DimensionSpec{DimensionSpec::Single, *d});
        }
    }
    return dims;
}

uint64_t market_storage_fee(size_t n) { return 1000 + uint64_t(n) * uint64_t(n); }

// ------------------------------------------------------------------ keys
Wallet::Wallet(const std::string& path, Node& node, bool szcc) : store_(bt::kv::open_sqlite(path)), node_(node), spend_zero_conf_change_(szcc) {}

bt::hd::Xpriv Wallet::xpriv(const bt::ByteArray<64>& seed, uint32_t purpose, uint32_t index)
{
    // m/43'/1899'/<purpose>'/<SIDECHAIN_NUMBER>'/0'/index (wallet/mod.rs:1076-1106)
    auto v = bt::hd::Variant::V0_18_1;
    auto x = bt::hd::Xpriv::master(seed, v);
    return x.derive_hardened(43, v).derive_hardened(1899, v).derive_hardened(purpose, v).derive_hardened(THIS_SIDECHAIN, v).derive_hardened(0, v).derive(index, v);
}

std::optional<bt::ByteArray<64>> Wallet::seed()
{
    Txn t(*store_);
    auto v = t.get(W_SEED, Bytes{0});
    if (!v || v->size() != 64) return std::nullopt;
    return bt::ByteArray<64>::from_span(*v);
}

bt::ByteArray<64> Wallet::need_seed()
{
    auto s = seed();
    if (!s) throw WalletError("no seed has been set");
    return *s;
}

std::optional<Hash32> Wallet::seed_digest(std::string_view label)
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto s = seed();
    if (!s) return std::nullopt;
    const uint8_t zero = 0;
    Hash32 h = bt::sha256_parts({bt::ByteSpan(reinterpret_cast<const uint8_t*>(label.data()), label.size()), bt::ByteSpan(&zero, 1), s->span()});
    sodium_memzero(s->data(), s->size());
    return h;
}

bool Wallet::has_seed()
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    return seed().has_value();
}

void Wallet::set_seed(const bt::ByteArray<64>& s)
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto cur = seed();
    if (cur) {
        if (*cur == s) return;
        throw WalletError("seed has already been set");
    }
    Txn t(*store_);
    t.put(W_SEED, Bytes{0}, s.span());
    for (uint8_t tb : {W_INDEX_TO_ADDR, W_ADDR_TO_INDEX, W_INDEX_TO_EPK, W_EPK_TO_INDEX, W_INDEX_TO_VK, W_VK_TO_INDEX}) t.clear(tb);
    t.commit();
}

void Wallet::set_seed_from_mnemonic(const std::string& mnemonic)
{
    auto p = bt::hd::parse_mnemonic(mnemonic);
    if (!p) throw WalletError(bt::hd::mnemonic_error(mnemonic));
    set_seed(bt::hd::mnemonic_to_seed(*p));
}

bt::frost::SigningKey Wallet::signing_key(uint32_t index)
{
    auto x = xpriv(need_seed(), TX_SIGNING, index);
    if (x.secret.is_zero()) throw WalletError("expected secret scalar to be non-zero");
    return bt::frost::SigningKey{x.secret};
}

bt::frost::SigningKey Wallet::signing_key_for(const Address& a)
{
    Txn t(*store_);
    auto v = t.get(W_ADDR_TO_INDEX, a.b.span());
    if (!v) throw WalletError("address " + a.to_base58() + " does not exist");
    return signing_key(from_be32(*v));
}

Address Wallet::new_index_address(uint32_t index)
{
    Address a = address_from_vk(signing_key(index).verifying_key());
    Txn t(*store_);
    t.put(W_INDEX_TO_ADDR, be32(index), a.b.span());
    t.put(W_ADDR_TO_INDEX, a.b.span(), be32(index));
    t.commit();
    return a;
}

// get_new_address: the index after the last one; index 0 (the voter address) first
Address Wallet::get_new_address()
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    uint32_t index = 0;
    {
        Txn t(*store_);
        if (auto lv = t.last(W_INDEX_TO_ADDR)) index = from_be32(lv->first) + 1;
    }
    return new_index_address(index);
}

Address Wallet::voter_address()
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    {
        Txn t(*store_);
        if (auto v = t.get(W_INDEX_TO_ADDR, be32(0))) {
            Address a;
            std::memcpy(a.b.data(), v->data(), 20);
            return a;
        }
    }
    return new_index_address(0);
}

std::optional<Address> last_address(bt::kv::Store& s)
{
    Txn t(s);
    auto lv = t.last(W_INDEX_TO_ADDR);
    if (!lv) return std::nullopt;
    Address a;
    std::memcpy(a.b.data(), lv->second.data(), 20);
    return a;
}

Address Wallet::get_or_generate_last_address()
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (auto a = last_address(*store_)) return *a;
    return get_new_address();
}

// get_receive_address: the last address until an output the wallet holds or held pays it
Address Wallet::get_receive_address()
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (auto a = last_address(*store_)) {
        bool received = false;
        for (auto& [op, o] : utxos())
            if (o.address == *a) received = true;
        if (!received)
            for (auto& [op, s] : stxos())
                if (s.output.address == *a) received = true;
        if (!received) return *a;
    }
    return get_new_address();
}

std::set<Address> Wallet::addresses()
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    Txn t(*store_);
    std::set<Address> r;
    t.scan(W_INDEX_TO_ADDR, [&](ByteSpan, ByteSpan v) {
        Address a;
        std::memcpy(a.b.data(), v.data(), 20);
        r.insert(a);
        return true;
    });
    return r;
}

bt::ByteArray<32> Wallet::get_new_encryption_key()
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    Txn t(*store_);
    uint32_t index = 0;
    if (auto lv = t.last(W_INDEX_TO_EPK)) index = from_be32(lv->first) + 1;
    auto x = xpriv(need_seed(), ENCRYPTION, index);
    bt::ByteArray<32> secret = bt::ByteArray<32>::from_span(x.secret.span()); // StaticSecret::from(scalar bytes)
    auto epk = bt::ecies::x25519_public(secret);
    t.put(W_INDEX_TO_EPK, be32(index), epk.span());
    t.put(W_EPK_TO_INDEX, epk.span(), be32(index));
    t.commit();
    return epk;
}

bt::frost::Point Wallet::get_new_verifying_key()
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    Txn t(*store_);
    uint32_t index = 0;
    if (auto lv = t.last(W_INDEX_TO_VK)) index = from_be32(lv->first) + 1;
    auto x = xpriv(need_seed(), MESSAGE_SIGNING, index);
    auto vk = bt::frost::SigningKey{x.secret}.verifying_key();
    const auto& enc = vk;
    t.put(W_INDEX_TO_VK, be32(index), enc.span());
    t.put(W_VK_TO_INDEX, enc.span(), be32(index));
    t.commit();
    return vk;
}

bt::frost::Signature Wallet::sign_arbitrary_msg(const bt::frost::Point& vk, const std::string& msg)
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    uint32_t index;
    {
        Txn t(*store_);
        auto v = t.get(W_VK_TO_INDEX, vk.span());
        if (!v) throw WalletError("Message signature verification key " + json::verifying_key_str(vk) + " does not exist");
        index = from_be32(*v);
    }
    auto x = xpriv(need_seed(), MESSAGE_SIGNING, index);
    Bytes m{uint8_t(Dst::Arbitrary)};
    m.insert(m.end(), msg.begin(), msg.end());
    return bt::frost::SigningKey{x.secret}.sign(m);
}

Authorization Wallet::sign_arbitrary_msg_as_addr(const Address& a, const std::string& msg)
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto sk = signing_key_for(a);
    Bytes m{uint8_t(Dst::Arbitrary)};
    m.insert(m.end(), msg.begin(), msg.end());
    return Authorization{sk.verifying_key(), sk.sign(m)};
}

Bytes Wallet::decrypt_msg(const bt::ByteArray<32>& epk, ByteSpan ciphertext)
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    uint32_t index;
    {
        Txn t(*store_);
        auto v = t.get(W_EPK_TO_INDEX, epk.span());
        if (!v) throw WalletError("Encryption pubkey " + json::encryption_pubkey_str(epk) + " does not exist");
        index = from_be32(*v);
    }
    auto x = xpriv(need_seed(), ENCRYPTION, index);
    auto r = bt::ecies::decrypt(bt::ByteArray<32>::from_span(x.secret.span()), ciphertext);
    if (!r) throw WalletError("ECIES error: DecryptionFailed");
    return *r;
}

// ------------------------------------------------------------------ coins (app.rs update_wallet)
std::map<OutPoint, Output> Wallet::utxos()
{
    std::map<OutPoint, Output> r;
    for (auto& [op, o] : node_.utxos_by_addresses(addresses())) r[op] = o;
    return r;
}

std::map<OutPoint, Output> Wallet::unconfirmed_utxos()
{
    std::set<OutPoint> confirmed;
    for (auto& [op, _] : utxos()) confirmed.insert(op);
    return node_.own_unconfirmed_utxos(addresses(), confirmed);
}

std::set<OutPoint> Wallet::mempool_spent()
{
    std::vector<OutPoint> ops;
    for (auto& [op, _] : utxos()) ops.push_back(op);
    std::set<OutPoint> r;
    for (auto& [op, _] : node_.unconfirmed_spent(ops)) r.insert(op);
    return r;
}

std::map<OutPoint, SpentOutput> Wallet::stxos()
{
    std::map<OutPoint, SpentOutput> r;
    for (auto& [op, s] : node_.stxos_by_addresses(addresses())) r[op] = s;
    return r;
}

Balance Wallet::balance()
{
    Balance b;
    auto spent = mempool_spent();
    for (auto& [op, o] : utxos()) {
        if (spent.count(op)) continue;
        b.total += o.get_value();
        if (o.content.kind != Content::Withdrawal) b.available += o.get_value();
    }
    for (auto& [op, o] : unconfirmed_utxos()) {
        b.total += o.get_value();
        b.unconfirmed += o.get_value();
        if (spend_zero_conf_change_ && o.content.kind != Content::Withdrawal) b.available += o.get_value();
    }
    return b;
}

// wallet/mod.rs select_coins: confirmed coins by value, then own unconfirmed change
Wallet::Selected Wallet::select_coins(uint64_t value)
{
    auto spent = mempool_spent();
    std::vector<std::pair<OutPoint, Output>> conf, unconf;
    for (auto& [op, o] : utxos())
        if (!spent.count(op)) conf.emplace_back(op, o);
    if (spend_zero_conf_change_)
        for (auto& [op, o] : unconfirmed_utxos()) unconf.emplace_back(op, o);
    auto by_value = [](auto& a, auto& b) { return a.second.get_value() < b.second.get_value(); };
    std::stable_sort(conf.begin(), conf.end(), by_value);
    std::stable_sort(unconf.begin(), unconf.end(), by_value);
    size_t nconf = conf.size();
    conf.insert(conf.end(), unconf.begin(), unconf.end());
    Selected s;
    for (size_t i = 0; i < conf.size(); i++) {
        auto& [op, o] = conf[i];
        if (o.content.kind == Content::Withdrawal || o.content.kind == Content::MarketFunds) continue;
        if (s.total > value) break;
        if (s.total + o.get_value() < s.total) throw WalletError("amount overflow");
        s.total += o.get_value();
        s.coins[op] = o;
        if (i >= nconf) s.unconfirmed.insert(op);
    }
    if (s.total < value) throw WalletError("not enough funds");
    return s;
}

// wallet/mod.rs select_bitcoins_from_address: largest first, value outputs of one address
std::pair<uint64_t, std::map<OutPoint, Output>> Wallet::select_from_address(uint64_t value, const Address& a)
{
    auto spent = mempool_spent();
    std::vector<std::pair<OutPoint, Output>> c, u;
    for (auto& [op, o] : utxos())
        if (o.address == a && o.content.kind == Content::Value && o.get_value() > 0 && !spent.count(op)) c.emplace_back(op, o);
    auto desc = [](auto& x, auto& y) { return x.second.get_value() > y.second.get_value(); };
    std::stable_sort(c.begin(), c.end(), desc);
    if (spend_zero_conf_change_) {
        for (auto& [op, o] : unconfirmed_utxos())
            if (o.address == a && o.content.kind == Content::Value && o.get_value() > 0) u.emplace_back(op, o);
        std::stable_sort(u.begin(), u.end(), desc);
        c.insert(c.end(), u.begin(), u.end());
    }
    uint64_t total = 0;
    std::map<OutPoint, Output> sel;
    for (auto& [op, o] : c) {
        if (total + o.get_value() < total) throw WalletError("amount overflow");
        total += o.get_value();
        sel[op] = o;
        if (total >= value) return {total, sel};
    }
    throw WalletError("not enough funds");
}

static std::vector<std::pair<OutPoint, Hash32>> spend_inputs(const std::map<OutPoint, Output>& coins)
{
    std::vector<std::pair<OutPoint, Hash32>> ins;
    for (auto& [op, o] : coins) ins.emplace_back(op, pointed_output_hash(op, o));
    return ins;
}

void Wallet::push_change(std::vector<Output>& outs, uint64_t change)
{
    if (change > 0) {
        Output o;
        o.address = get_new_address();
        o.content.kind = Content::Value;
        o.content.value = change;
        outs.push_back(o);
    }
}

static Output value_output(const Address& a, uint64_t v)
{
    Output o;
    o.address = a;
    o.content.kind = Content::Value;
    o.content.value = v;
    return o;
}

Transaction Wallet::create_transaction_many(const std::map<Address, uint64_t>& dests, uint64_t fee)
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (dests.empty()) throw WalletError("no transfer destination");
    uint64_t value = 0;
    for (auto& [a, v] : dests) {
        if (value + v < value) throw WalletError("amount overflow");
        value += v;
    }
    if (value + fee < value) throw WalletError("amount overflow");
    auto sel = select_coins(value + fee);
    Transaction tx;
    tx.inputs = spend_inputs(sel.coins);
    std::vector<bt::utreexo::NodeHash> targets;
    for (auto& [op, h] : tx.inputs)
        if (!sel.unconfirmed.count(op)) targets.push_back(bt::utreexo::NodeHash::some(h));
    auto p = node_.tip_accumulator().prove(targets);
    if (!p) throw WalletError("utreexo error");
    tx.proof = *p;
    for (auto& [a, v] : dests) tx.outputs.push_back(value_output(a, v));
    tx.outputs.push_back(value_output(get_new_address(), sel.total - value - fee));
    return tx;
}

Transaction Wallet::create_withdrawal(const bt::btc::Address& main_address, uint64_t value, uint64_t main_fee, uint64_t fee)
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (value + fee < value || value + fee + main_fee < value + fee) throw WalletError("amount overflow");
    auto sel = select_coins(value + fee + main_fee);
    Transaction tx;
    tx.inputs = spend_inputs(sel.coins);
    std::vector<bt::utreexo::NodeHash> targets;
    for (auto& [op, h] : tx.inputs)
        if (!sel.unconfirmed.count(op)) targets.push_back(bt::utreexo::NodeHash::some(h));
    auto p = node_.tip_accumulator().prove(targets);
    if (!p) throw WalletError("utreexo error");
    tx.proof = *p;
    Output w;
    w.address = get_new_address();
    w.content.kind = Content::Withdrawal;
    w.content.value = value;
    w.content.main_fee = main_fee;
    w.content.main_address = main_address;
    tx.outputs.push_back(w);
    tx.outputs.push_back(value_output(get_new_address(), sel.total - value - fee - main_fee));
    return tx;
}

Transaction Wallet::claim_decision(const ClaimDecisionPayload& p, uint64_t fee)
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto sel = select_coins(fee);
    Transaction tx;
    tx.inputs = spend_inputs(sel.coins);
    push_change(tx.outputs, sel.total - fee);
    tx.data = TxData(p);
    return tx;
}

std::pair<Transaction, MarketId> Wallet::create_market(const CreateMarketInput& in, uint64_t fee)
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto dims = parse_dimensions(in.dimensions);
    if (!dims) throw WalletError("Invalid decision ID: Failed to parse dimension specification");
    size_t num_outcomes = 1, cat_idx = 0;
    for (auto& d : *dims) {
        if (d.kind == DimensionSpec::Single) num_outcomes *= 2;
        else {
            size_t n = 2;
            if (in.category_option_counts && cat_idx < in.category_option_counts->size()) n = (*in.category_option_counts)[cat_idx];
            cat_idx++;
            num_outcomes *= n;
        }
    }
    uint64_t storage_fee = market_storage_fee(num_outcomes);
    uint64_t treasury = 0;
    if (in.beta && in.initial_liquidity) throw WalletError("Invalid decision ID: Specify either beta or initial_liquidity, not both");
    if (in.initial_liquidity) treasury = *in.initial_liquidity;
    else {
        double liq = math::lmsr_liquidity(in.beta ? *in.beta : DEFAULT_MARKET_BETA, num_outcomes);
        auto s = math::to_sats(liq, math::Rounding::Up, nullptr);
        if (!s) throw WalletError("amount overflow");
        treasury = *s;
    }
    uint64_t total_cost = fee + storage_fee;
    if (total_cost < fee) throw WalletError("amount overflow");
    if (treasury > 0) {
        if (total_cost + treasury < total_cost) throw WalletError("amount overflow");
        total_cost += treasury;
    }
    auto sel = select_coins(total_cost);
    Transaction tx;
    tx.inputs = spend_inputs(sel.coins);
    if (tx.inputs.empty()) throw WalletError("not enough funds");
    // inputs are in outpoint order (their HashMap -> Vec order is not ours, the creator is the
    // first input's address in both)
    Address creator = sel.coins.at(tx.inputs.front().first).address;
    MarketId mid = compute_market_id(in.title, in.description, creator, *dims);
    if (treasury > 0) {
        Output t;
        t.address = market_treasury_address(mid);
        t.content.kind = Content::MarketFunds;
        t.content.market_id = mid;
        t.content.value = treasury;
        t.content.is_fee = false;
        tx.outputs.push_back(t);
    }
    push_change(tx.outputs, sel.total - total_cost);
    TxCreateMarket cm;
    cm.title = in.title;
    cm.description = in.description;
    cm.dimension_specs = *dims;
    cm.new_claims = in.new_claims;
    cm.trading_fee = in.trading_fee;
    cm.tx_pow_hash_selector = in.tx_pow_hash_selector;
    cm.tx_pow_ordering = in.tx_pow_ordering;
    cm.tx_pow_difficulty = in.tx_pow_difficulty;
    tx.data = TxData(cm);
    return {tx, mid};
}

Transaction Wallet::trade(const MarketId& m, uint32_t outcome, int64_t shares, const Address& trader, uint64_t limit_sats,
                          std::optional<std::array<uint8_t, 3>> pow, const Hash32& prev_block_hash)
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    bool buy = shares > 0;
    auto sel = select_coins(buy ? limit_sats : math::TRADE_MINER_FEE_SATS);
    Transaction tx;
    tx.inputs = spend_inputs(sel.coins);
    TxTrade tr;
    tr.market_id = m;
    tr.outcome_index = outcome;
    tr.shares = shares;
    tr.trader = trader;
    tr.limit_sats = limit_sats;
    tr.prev_block_hash = prev_block_hash;
    if (pow && (*pow)[2] > 0) {
        Bytes data = serialize_trade_for_pow(m, outcome, shares, trader, limit_sats, prev_block_hash);
        auto n = math::txpow_mine((*pow)[0], (*pow)[1], (*pow)[2], data, 0, UINT64_MAX);
        if (!n) throw WalletError("TX-PoW mine: u64 space exhausted without finding nonce");
        tr.tx_pow_nonce = *n;
    }
    tx.data = TxData(tr);
    return tx;
}

Transaction Wallet::amplify_beta(const MarketId& m, uint64_t amount, const Address& author)
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (amount == 0) throw WalletError("Invalid decision ID: AmplifyBeta amount must be positive");
    if (amount + math::TRADE_MINER_FEE_SATS < amount) throw WalletError("amount overflow");
    auto sel = select_coins(amount + math::TRADE_MINER_FEE_SATS);
    bool has_author = false;
    for (auto& [op, o] : sel.coins)
        if (o.address == author) has_author = true;
    if (!has_author) throw WalletError("not enough funds");
    Transaction tx;
    tx.inputs = spend_inputs(sel.coins);
    tx.data = TxData(TxAmplifyBeta{m, amount, author});
    return tx;
}

Transaction Wallet::submit_ballot(const std::vector<BallotItem>& votes, uint32_t voting_period, uint64_t fee)
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    Address voter = voter_address();
    auto sel = select_coins(fee);
    Transaction tx;
    tx.inputs = spend_inputs(sel.coins);
    push_change(tx.outputs, sel.total - fee);
    tx.data = TxData(TxSubmitBallot{voter, votes, voting_period});
    return tx;
}

Transaction Wallet::transfer_reputation(const Address& dest, double amount, uint64_t fee)
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    Address voter = voter_address();
    auto [total, coins] = select_from_address(fee, voter);
    Transaction tx;
    tx.inputs = spend_inputs(coins);
    if (total - fee > 0) tx.outputs.push_back(value_output(voter, total - fee));
    tx.data = TxData(TxTransferReputation{voter, dest, amount});
    return tx;
}

// wallet/mod.rs authorize + build_actor_proof
AuthorizedTransaction Wallet::authorize(const Transaction& tx)
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    AuthorizedTransaction at;
    at.transaction = tx;
    Bytes msg = tx_msg_canonical(tx);
    auto conf = utxos();
    auto unconf = unconfirmed_utxos();
    std::set<Address> input_addresses;
    for (auto& [op, _] : tx.inputs) {
        const Output* o = nullptr;
        if (auto it = conf.find(op); it != conf.end()) o = &it->second;
        else if (auto it2 = unconf.find(op); it2 != unconf.end()) o = &it2->second;
        if (!o) throw WalletError("utxo does not exist");
        input_addresses.insert(o->address);
        Txn t(*store_);
        auto v = t.get(W_ADDR_TO_INDEX, o->address.b.span());
        if (!v) throw WalletError("no index for address " + o->address.to_base58());
        auto sk = signing_key(from_be32(*v));
        at.authorizations.push_back(Authorization{sk.verifying_key(), sk.sign(msg)});
    }
    std::optional<Address> actor;
    if (tx.data) {
        if (auto* tr = std::get_if<TxTrade>(&*tx.data); tr && tr->shares < 0 && !input_addresses.count(tr->trader)) actor = tr->trader;
        if (auto* r = std::get_if<TxTransferReputation>(&*tx.data); r && !input_addresses.count(r->sender)) actor = r->sender;
        if (auto* v = std::get_if<TxSubmitVote>(&*tx.data); v && !input_addresses.count(v->voter)) actor = v->voter;
        if (auto* b = std::get_if<TxSubmitBallot>(&*tx.data); b && !input_addresses.count(b->voter)) actor = b->voter;
    }
    if (actor) {
        auto sk = signing_key_for(*actor);
        at.actor_proof = Authorization{sk.verifying_key(), sk.sign(msg)};
    }
    return at;
}

} // namespace hivemind
