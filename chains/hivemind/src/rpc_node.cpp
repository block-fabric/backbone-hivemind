// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// JSON-RPC: node, mempool, wallet, keys and messages (rpc-api/lib.rs node/wallet traits,
// app/rpc_server.rs). Errors as theirs: custom_err = code -1 with the error chain text.
#include <hivemind/app.h>
#include <hivemind/json.h>

#include <bt/bech32.h>
#include <bt/ecies.h>
#include <bt/parse.h>
#include <bt/hd.h>
#include <bt/log.h>

#include <chrono>
#include <thread>

namespace hivemind {

using bt::jsonrpc::app_error;
using bt::jsonrpc::Params;
using bt::kv::Txn;

namespace {

// A parameter that does not deserialize: jsonrpsee's -32602 with serde's text as data.
template <class F>
bt::jsonrpc::Handler P(F f)
{
    return [f](const Params& p) -> UniValue {
        try {
            return f(p);
        } catch (const json::ParseError& e) {
            throw bt::jsonrpc::invalid_params(e.what());
        } catch (const bt::jsonrpc::RpcError&) {
            throw;
        } catch (const std::exception& e) {
            throw app_error(e.what());
        }
    };
}

UniValue arr() { return UniValue(UniValue::VARR); }

std::set<Address> parse_addresses(const UniValue& v)
{
    if (!v.isArray()) throw json::ParseError("invalid type: expected a sequence");
    std::set<Address> s;
    for (auto& a : v.getValues()) s.insert(json::parse_address(a));
    return s;
}

UniValue pointed_list(const std::vector<std::pair<OutPoint, Output>>& v)
{
    UniValue a = arr();
    for (auto& [op, o] : v) a.push_back(json::pointed_output(op, o));
    return a;
}
UniValue pointed_map(const std::map<OutPoint, Output>& v)
{
    UniValue a = arr();
    for (auto& [op, o] : v) a.push_back(json::pointed_output(op, o));
    return a;
}

UniValue two_way_peg_event(const TwoWayPegEvent& e)
{
    UniValue r(UniValue::VOBJ), i(UniValue::VOBJ);
    i.pushKV("outpoint", json::outpoint(e.outpoint));
    if (e.kind == TwoWayPegEvent::Deposit) {
        i.pushKV("output", json::output(e.output));
        r.pushKV("Deposit", i);
    } else if (e.kind == TwoWayPegEvent::BundleSpend) {
        i.pushKV("m6id", json::btc_hash(e.m6id));
        r.pushKV("BundleSpend", i);
    } else {
        i.pushKV("output", json::output(e.output));
        i.pushKV("m6id", json::btc_hash(e.m6id));
        r.pushKV("BundleReturn", i);
    }
    return r;
}

UniValue tx_verbose(const Transaction& t)
{
    UniValue r = json::transaction(t);
    r.pushKV("canonical_bytes", bt::hex_encode(t.canonical_bytes()));
    return r;
}

std::string peer_addr(const std::string& s)
{
    auto hp = bt::split_hostport(s, DEFAULT_PORT);
    if (!hp) throw json::ParseError("invalid value: string \"" + s + "\", expected a peer address");
    return s;
}

} // namespace

void register_rpc_node(bt::jsonrpc::Server& s, App& app, bool public_api, bool private_api)
{
    Node& n = app.node;
    Wallet& w = app.wallet;
    if (public_api) {
        s.add("connect_block", {"block", "main_block_hash"}, 2, P([&app](const Params& p) {
                  Header h = json::parse_header(p[0].find_value("header"));
                  Body b = json::parse_body(p[0].find_value("body"));
                  return UniValue(app.connect_block(h, b, json::parse_btc_hash(p[1])));
              }));
        s.add("get_block", {"block_hash", "verbose"}, 1, P([&n](const Params& p) -> UniValue {
                  Hash32 h = json::parse_hash(p[0]);
                  bool verbose = !p[1].isNull() && json::parse_bool(p[1]);
                  auto hd = n.header(h);
                  if (!hd) return UniValue();
                  auto b = n.body(h);
                  if (!b) throw app_error("archive error: no block body with hash " + bt::hex_encode(h));
                  if (!verbose) return json::block(Block{*hd, *b});
                  UniValue r(UniValue::VOBJ), body = json::body(*b), txs = arr();
                  for (auto& t : b->transactions) txs.push_back(tx_verbose(t));
                  body.pushKV("transactions", txs);
                  r.pushKV("header", json::header(*hd));
                  r.pushKV("body", body);
                  return r;
              }));
        s.add("get_block_hash", {"height"}, 1, P([&n](const Params& p) {
                  auto r = n.block_hash_at(json::parse_u32(p[0]));
                  return r ? json::hash(*r) : UniValue();
              }));
        s.add("get_block_index", {"block_hash"}, 1, P([&n](const Params& p) {
                  Hash32 h = json::parse_hash(p[0]);
                  auto b = n.body(h);
                  if (!b) throw app_error("archive error: no block body with hash " + bt::hex_encode(h));
                  UniValue r(UniValue::VOBJ), txs = arr(), deps = arr(), spends = arr();
                  for (auto& t : b->transactions) {
                      UniValue x(UniValue::VOBJ);
                      Bytes raw = t.canonical_bytes();
                      x.pushKV("txid", json::hash(t.txid()));
                      x.pushKV("size", UniValue(uint64_t(raw.size())));
                      x.pushKV("raw", bt::hex_encode(raw));
                      txs.push_back(x);
                  }
                  auto ht = n.block_height(h);
                  if (!ht || n.block_hash_at(*ht) != h) throw app_error("block " + bt::hex_encode(h) + " is not in the active chain");
                  auto evs = n.with_state([&](const State& st, const Archive&, const Txn& t) { return st.two_way_peg_events(t, *ht); });
                  for (auto& e : evs) {
                      UniValue x(UniValue::VOBJ);
                      x.pushKV("outpoint", json::outpoint(e.outpoint));
                      if (e.kind == TwoWayPegEvent::Deposit) {
                          x.pushKV("output", json::output(e.output));
                          deps.push_back(x);
                      } else if (e.kind == TwoWayPegEvent::BundleSpend) {
                          x.pushKV("m6id", json::btc_hash(e.m6id));
                          spends.push_back(x);
                      }
                  }
                  r.pushKV("txs", txs);
                  r.pushKV("deposits", deps);
                  r.pushKV("bundle_spends", spends);
                  return r;
              }));
        s.add("get_bmm_inclusions", {"block_hash"}, 1, P([&n](const Params& p) {
                  UniValue a = arr();
                  for (auto& m : n.bmm_inclusions(json::parse_hash(p[0]))) a.push_back(json::btc_hash(m));
                  return a;
              }));
        s.add("get_best_mainchain_block_hash", {}, 0, P([&n](const Params&) -> UniValue {
                  auto t = n.tip();
                  if (!t) return UniValue();
                  auto m = n.best_main_verification(*t);
                  if (!m) throw app_error("archive error: no BMM verification for " + bt::hex_encode(*t));
                  return json::btc_hash(*m);
              }));
        s.add("get_best_sidechain_block_hash", {}, 0, P([&n](const Params&) {
                  auto t = n.tip();
                  return t ? json::hash(*t) : UniValue();
              }));
        s.add("get_stxos", {"addresses"}, 1, P([&n](const Params& p) {
                  UniValue a = arr();
                  for (auto& [op, so] : n.stxos_by_addresses(parse_addresses(p[0]))) {
                      UniValue x(UniValue::VOBJ);
                      x.pushKV("outpoint", json::outpoint(op));
                      x.pushKV("output", json::spent_output(so));
                      a.push_back(x);
                  }
                  return a;
              }));
        s.add("get_transaction", {"txid"}, 1, P([&n](const Params& p) -> UniValue {
                  auto t = n.transaction(json::parse_hash(p[0]));
                  if (!t) return UniValue();
                  UniValue r(UniValue::VOBJ);
                  r.pushKV("tx", json::transaction(t->first));
                  r.pushKV("block_hash", t->second ? json::hash(*t->second) : UniValue());
                  return r;
              }));
        s.add("get_two_way_peg_events", {"block_hash"}, 1, P([&n](const Params& p) {
                  Hash32 h = json::parse_hash(p[0]);
                  auto ht = n.block_height(h);
                  if (!ht) throw app_error("archive error: no height info for block hash " + bt::hex_encode(h));
                  if (n.block_hash_at(*ht) != h) throw app_error("block " + bt::hex_encode(h) + " is not in the active chain");
                  auto evs = n.with_state([&](const State& st, const Archive&, const Txn& t) { return st.two_way_peg_events(t, *ht); });
                  UniValue a = arr();
                  for (auto& e : evs) a.push_back(two_way_peg_event(e));
                  return a;
              }));
        s.add("get_utxos", {"addresses"}, 1, P([&n](const Params& p) { return pointed_list(n.utxos_by_addresses(parse_addresses(p[0]))); }));
        s.add("get_withdrawal_bundle", {"m6id"}, 1, P([&n](const Params& p) -> UniValue {
                  auto b = n.withdrawal_bundle(json::parse_btc_hash(p[0]));
                  if (!b) return UniValue();
                  UniValue r(UniValue::VOBJ);
                  r.pushKV("info", json::withdrawal_bundle_info(b->first));
                  r.pushKV("status", to_string(b->second.latest().value));
                  return r;
              }));
        s.add("getblockcount", {}, 0, P([&n](const Params&) {
                  auto h = n.height();
                  return UniValue(uint64_t(h ? *h + 1 : 0));
              }));
        s.add("latest_failed_withdrawal_bundle_height", {}, 0, P([&n](const Params&) {
                  auto h = n.latest_failed_withdrawal_bundle_height();
                  return h ? UniValue(uint64_t(*h)) : UniValue();
              }));
        s.add("list_mempool", {}, 0, P([&n](const Params&) {
                  UniValue a = arr();
                  for (auto& at : n.mempool()) {
                      UniValue x(UniValue::VOBJ);
                      Bytes raw = at.transaction.canonical_bytes();
                      x.pushKV("txid", json::hash(at.transaction.txid()));
                      x.pushKV("size", UniValue(uint64_t(raw.size())));
                      x.pushKV("raw", bt::hex_encode(raw));
                      x.pushKV("tx", json::transaction(at.transaction));
                      a.push_back(x);
                  }
                  return a;
              }));
        s.add("list_peers", {}, 0, P([&n](const Params&) {
                  UniValue a = arr();
                  for (auto& pv : n.peers()) {
                      UniValue o(UniValue::VOBJ);
                      o.pushKV("address", pv.address.to_string());
                      o.pushKV("status", pv.connected ? "Connected" : "Connecting");
                      a.push_back(o);
                  }
                  return a;
              }));
        s.add("list_utxos", {}, 0, P([&n](const Params&) { return pointed_list(n.utxos()); }));
        s.add("mainchain_sync_progress", {}, 0, P([&n](const Params&) {
                  auto sp = n.sync_progress();
                  static const char* PH[] = {"idle", "headers", "writing", "state"};
                  UniValue o(UniValue::VOBJ);
                  o.pushKV("phase", PH[sp.phase]);
                  o.pushKV("done", UniValue(uint64_t(sp.done)));
                  o.pushKV("total", UniValue(uint64_t(sp.total)));
                  o.pushKV("tip_height", UniValue(uint64_t(sp.tip_height)));
                  return o;
              }));
        s.add("pending_withdrawal_bundle", {}, 0, P([&n](const Params&) {
                  auto b = n.pending_withdrawal_bundle();
                  return b ? json::withdrawal_bundle(*b) : UniValue();
              }));
        s.add("sidechain_wealth", {}, 0, P([&n](const Params&) { return UniValue(n.sidechain_wealth()); }));
        s.add("submit_transaction", {"transaction"}, 1, P([&n](const Params& p) {
                  auto tx = json::parse_authorized_transaction(p[0]);
                  n.submit_transaction(tx);
                  return json::hash(tx.transaction.txid());
              }));
        s.add("await_block_height", {"target_height", "timeout_ms"}, 1, P([&n](const Params& p) {
                  uint32_t target = json::parse_u32(p[0]);
                  uint64_t timeout = p[1].isNull() ? 10000 : json::parse_u64(p[1]);
                  auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
                  for (;;) {
                      uint32_t cur = n.height().value_or(0);
                      if (cur >= target) return UniValue(uint64_t(cur));
                      if (std::chrono::steady_clock::now() >= until)
                          throw app_error("Timeout waiting for block height " + std::to_string(target) + ". Current height: " + std::to_string(cur));
                      std::this_thread::sleep_for(std::chrono::milliseconds(100));
                  }
              }));
        s.add("get_transaction_info", {"txid"}, 1, P([&n](const Params& p) -> UniValue {
                  auto r = n.filled_transaction(json::parse_hash(p[0]));
                  if (!r) return UniValue();
                  UniValue o(UniValue::VOBJ), conf, txin;
                  if (r->second) {
                      uint32_t tip_height = *n.height();
                      uint32_t h = *n.block_height(r->second->first);
                      conf = UniValue(uint64_t(tip_height - h));
                      txin = UniValue(UniValue::VOBJ);
                      txin.pushKV("block_hash", json::hash(r->second->first));
                      txin.pushKV("idx", UniValue(uint64_t(r->second->second)));
                  }
                  FilledTransaction ft{&r->first.first.transaction, r->first.second, std::nullopt};
                  auto fee = ft.fee();
                  if (!fee) throw app_error("value out exceeds value in");
                  o.pushKV("confirmations", conf);
                  o.pushKV("fee_sats", UniValue(*fee));
                  o.pushKV("txin", txin);
                  return o;
              }));
        s.add("push_tx", {"tx_hex"}, 1, P([&n](const Params& p) {
                  std::string hx = json::parse_str(p[0]);
                  auto b = bt::hex_decode(hx);
                  if (!b) throw app_error("invalid hex: Invalid character");
                  AuthorizedTransaction tx;
                  try {
                      Reader r(*b);
                      tx = de_authorized_transaction(r);
                  } catch (const std::exception& e) {
                      throw app_error(std::string("failed to deserialize AuthorizedTransaction: ") + e.what());
                  }
                  Hash32 txid = tx.transaction.txid();
                  n.submit_transaction(tx);
                  return json::hash(txid);
              }));
        s.add("votecoin_balance", {"address"}, 1, P([&n](const Params& p) {
                  Address a = json::parse_address(p[0]);
                  return json::f64(n.with_state([&](const State& st, const Archive&, const Txn& t) { return st.reputation(t, a); }));
              }));
    }
    if (private_api) {
        s.add("connect_peer", {"addr"}, 1, P([&n](const Params& p) {
                  std::string a = peer_addr(json::parse_str(p[0])), err;
                  if (!n.connect_peer(a, err)) throw app_error(err);
                  return UniValue();
              }));
        s.add("forget_peer", {"addr"}, 1, P([&n](const Params& p) {
                  std::string a = peer_addr(json::parse_str(p[0]));
                  if (a.rfind(':') == std::string::npos) a += ":" + std::to_string(DEFAULT_PORT);
                  n.forget_peer(a);
                  return UniValue();
              }));
        s.add("invalidate_block", {"block_hash"}, 1, P([&n](const Params& p) {
                  n.invalidate_block(json::parse_hash(p[0]));
                  return UniValue();
              }));
        s.add("remove_from_mempool", {"txid"}, 1, P([&n](const Params& p) {
                  n.remove_from_mempool(json::parse_hash(p[0]));
                  return UniValue();
              }));
        s.add("stop", {}, 0, [&app](const Params&) {
            if (app.on_stop) app.on_stop();
            return UniValue();
        });
        s.add("sync_to_tip", {"block_hash"}, 1, P([&n](const Params& p) {
                  Hash32 h = json::parse_hash(p[0]);
                  auto m = n.best_main_verification(h);
                  if (!m) throw app_error("archive error: no BMM verification for " + bt::hex_encode(h));
                  return UniValue(n.sync_to_tip(TipRef{h, *m}));
              }));
        // ---- wallet
        s.add("balance", {}, 0, P([&w](const Params&) {
                  auto b = w.balance();
                  UniValue o(UniValue::VOBJ);
                  o.pushKV("total_sats", UniValue(b.total));
                  o.pushKV("available_sats", UniValue(b.available));
                  o.pushKV("unconfirmed_sats", UniValue(b.unconfirmed));
                  return o;
              }));
        s.add("create_deposit", {"address", "value_sats", "fee_sats"}, 3, P([&app](const Params& p) {
                  return json::btc_hash(app.deposit(json::parse_address(p[0]), json::parse_u64(p[1]), json::parse_u64(p[2])));
              }));
        s.add("create_transfer", {"dest", "value_sats", "fee_sats"}, 3, P([&app, &w](const Params& p) {
                  std::map<Address, uint64_t> d{{json::parse_address(p[0]), json::parse_u64(p[1])}};
                  return json::hash(app.sign_and_send(w.create_transaction_many(d, json::parse_u64(p[2]))));
              }));
        s.add("create_transfer_many", {"dests", "fee_sats"}, 2, P([&app, &w](const Params& p) {
                  if (!p[0].isObject()) throw json::ParseError("invalid type: expected a map");
                  std::map<Address, uint64_t> d;
                  for (size_t i = 0; i < p[0].size(); i++) {
                      Address a = json::parse_address(UniValue(p[0].getKeys()[i]));
                      if (d.count(a)) throw json::ParseError("invalid entry: found duplicate key"); // MapPreventDuplicates
                      d[a] = json::parse_u64(p[0].getValues()[i]);
                  }
                  return json::hash(app.sign_and_send(w.create_transaction_many(d, json::parse_u64(p[1]))));
              }));
        s.add("create_withdrawal", {"mainchain_address", "amount_sats", "fee_sats", "mainchain_fee_sats"}, 4, P([&app, &w](const Params& p) {
                  auto ma = bt::btc::Address::parse(json::parse_str(p[0]));
                  if (!ma) throw json::ParseError("invalid value: expected a bitcoin address");
                  return json::hash(app.sign_and_send(w.create_withdrawal(*ma, json::parse_u64(p[1]), json::parse_u64(p[3]), json::parse_u64(p[2]))));
              }));
        s.add("decrypt_msg", {"encryption_pubkey", "ciphertext"}, 2, P([&w](const Params& p) {
                  std::string es = json::parse_str(p[0]);
                  auto eb = bt::bech32::decode_bech32m_canonical(es, "ba-enc");
                  if (!eb || eb->size() != 32) throw json::ParseError("invalid value: expected an encryption pubkey");
                  std::string c = json::parse_str(p[1]);
                  auto ct = bt::hex_decode(c);
                  if (!ct) throw bt::jsonrpc::RpcError(-32602, "Invalid hex string", "Invalid character");
                  return UniValue(bt::hex_encode(w.decrypt_msg(bt::ByteArray<32>::from_span(*eb), *ct)));
              }));
        s.add("encrypt_msg", {"encryption_pubkey", "msg"}, 2, P([](const Params&) -> UniValue {
                  throw bt::jsonrpc::RpcError(-32601, "Encryption not implemented", "Use external encryption tools");
              }));
        s.add("format_deposit_address", {"address"}, 1, P([](const Params& p) { return UniValue(json::parse_address(p[0]).format_for_deposit()); }));
        s.add("generate_mnemonic", {}, 0, P([](const Params&) { return UniValue(bt::hd::generate_mnemonic(12)); }));
        s.add("get_block_template", {}, 0, P([&app](const Params&) {
                  auto t = app.build_block_template(std::nullopt);
                  UniValue o(UniValue::VOBJ);
                  o.pushKV("critical_hash", json::hash(t.header.hash()));
                  o.pushKV("block", json::block(Block{t.header, t.body}));
                  o.pushKV("fees_sats", UniValue(t.fees));
                  return o;
              }));
        s.add("get_new_address", {}, 0, P([&w](const Params&) { return json::address(w.get_new_address()); }));
        s.add("get_wallet_addresses", {}, 0, P([&w](const Params&) {
                  auto as = w.addresses();
                  std::vector<std::string> v;
                  for (auto& a : as) v.push_back(a.to_base58());
                  std::sort(v.begin(), v.end());
                  UniValue a = arr();
                  for (auto& x : v) a.push_back(x);
                  return a;
              }));
        s.add("get_wallet_utxos", {}, 0, P([&w](const Params&) { return pointed_map(w.utxos()); }));
        s.add("get_unconfirmed_wallet_utxos", {}, 0, P([&w](const Params&) { return pointed_map(w.unconfirmed_utxos()); }));
        s.add("mine", {"fee"}, 0, P([&app](const Params& p) {
                  std::optional<uint64_t> fee;
                  if (!p[0].isNull()) fee = json::parse_u64(p[0]);
                  app.mine(fee);
                  return UniValue();
              }));
        s.add("set_seed_from_mnemonic", {"mnemonic"}, 1, P([&w](const Params& p) {
                  w.set_seed_from_mnemonic(json::parse_str(p[0]));
                  return UniValue();
              }));
        s.add("sign_transaction", {"transaction", "broadcast"}, 1, P([&n, &w](const Params& p) {
                  auto at = w.authorize(json::parse_transaction(p[0]));
                  bool bc = !p[1].isNull() && json::parse_bool(p[1]);
                  if (bc) n.submit_transaction(at);
                  return json::authorized_transaction(at);
              }));
        s.add("get_new_encryption_key", {}, 0, P([&w](const Params&) { return UniValue(json::encryption_pubkey_str(w.get_new_encryption_key())); }));
        s.add("get_new_verifying_key", {}, 0, P([&w](const Params&) { return UniValue(json::verifying_key_str(w.get_new_verifying_key())); }));
        s.add("my_unconfirmed_utxos", {}, 0, P([&n, &w](const Params&) { return pointed_map(n.unconfirmed_utxos_by_addresses(w.addresses())); }));
        s.add("refresh_wallet", {}, 0, P([](const Params&) { return UniValue(); })); // ours reads the node state directly
        s.add("sign_arbitrary_msg", {"verifying_key", "msg"}, 2, P([&w](const Params& p) {
                  return UniValue(json::signature_str(w.sign_arbitrary_msg(json::parse_verifying_key(p[0]), json::parse_str(p[1]))));
              }));
        s.add("sign_arbitrary_msg_as_addr", {"address", "msg"}, 2, P([&w](const Params& p) {
                  return json::authorization(w.sign_arbitrary_msg_as_addr(json::parse_address(p[0]), json::parse_str(p[1])));
              }));
        s.add("verify_signature", {"signature", "verifying_key", "dst", "msg"}, 4, P([](const Params& p) {
                  auto sig = json::parse_signature(p[0]);
                  auto vk = json::parse_verifying_key(p[1]);
                  std::string dst = json::parse_str(p[2]), m = json::parse_str(p[3]);
                  Dst d;
                  if (dst == "transaction") d = Dst::Transaction;
                  else if (dst == "arbitrary") d = Dst::Arbitrary;
                  else throw json::ParseError("unknown variant `" + dst + "`, expected `transaction` or `arbitrary`");
                  return UniValue(verify_dst(sig, vk, d, ByteSpan((const uint8_t*)m.data(), m.size())));
              }));
        s.add("get_voter_address", {}, 0, P([&w](const Params&) { return json::address(w.voter_address()); }));
        s.add("transfer_votecoin", {"dest", "amount", "fee_sats"}, 3, P([&app, &w](const Params& p) {
                  return json::hash(app.sign_and_send(w.transfer_reputation(json::parse_address(p[0]), json::parse_f64(p[1]), json::parse_u64(p[2]))));
              }));
    }
}

} // namespace hivemind
