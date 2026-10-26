// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <hivemind/app.h>

#include <bt/log.h>

#include <atomic>

namespace hivemind {

using bt::jsonrpc::app_error;
using bt::kv::Txn;

// app.rs build_block_template: on a mainchain reorg below the tip's prev_main, an empty
// block on the last ancestor whose prev_main is still an ancestor.
// Policy (kept fix, consensus-neutral): ours dry-runs the template (prevalidate + connect in
// a transaction that is dropped) and, when it fails, mines without the offending kinds, then
// without transactions.
BlockTemplate App::build_block_template(std::optional<uint64_t> fee)
{
    auto tpl = build_block_template_(fee, 0);
    for (int round = 1; round <= 2; round++) {
        if (tpl.header.prev_side_hash != node.tip() || tpl.body.transactions.empty()) break;
        std::string err;
        node.with_state([&](const State& st, const Archive& a, const Txn& t) {
            bt::kv::Txn& w = const_cast<bt::kv::Txn&>(t); // never committed
            try {
                uint64_t ts = 0;
                if (auto m = a.main_header(w, tpl.header.prev_main_hash)) ts = m->timestamp;
                auto pre = st.prevalidate(w, tpl.header, tpl.body, a);
                st.connect_prevalidated(w, tpl.header, tpl.body, std::move(pre), ts);
            } catch (const std::exception& e) {
                err = e.what();
            }
            return 0;
        });
        if (err.empty()) break;
        LOG_WARN("block template would be invalid (%s); rebuilding %s", err.c_str(), round == 1 ? "without claims, markets and reputation transfers" : "empty");
        tpl = build_block_template_(fee, round);
    }
    return tpl;
}

BlockTemplate App::build_block_template_(std::optional<uint64_t> fee, int exclude)
{
    auto tipr = node.enforcer().get_chain_tip();
    if (!bt::enforcer::ok(tipr)) throw app_error(bt::enforcer::err(tipr).what());
    Hash32 prev_main = bt::enforcer::val(tipr).block_hash;
    auto tip_hash = node.tip();
    std::optional<Hash32> prev_side;
    if (tip_hash) {
        if (!node.request_mainchain_ancestor_infos(prev_main))
            throw app_error("Failed to request mainchain ancestor info for " + bt::hex_encode_rev(prev_main.span()));
        node.with_state([&](const State&, const Archive& a, const Txn& t) {
            Header th = a.need_header(t, *tip_hash);
            Hash32 lcma = a.last_common_main_ancestor(t, prev_main, th.prev_main_hash);
            if (lcma == th.prev_main_hash) prev_side = tip_hash;
            else {
                std::optional<Hash32> cur = tip_hash;
                while (cur) {
                    Header h = a.need_header(t, *cur);
                    if (h.prev_main_hash != lcma && a.is_main_descendant(t, h.prev_main_hash, lcma)) {
                        prev_side = cur;
                        break;
                    }
                    cur = h.prev_side_hash;
                }
            }
            return 0;
        });
    }
    BlockTemplate tpl;
    std::vector<std::pair<AuthorizedTransaction, std::vector<Output>>> txs;
    if (prev_side == tip_hash) {
        // Policy (kept): never more than 999 transactions, so our blocks stay on the CBMT
        // side of the 1000-tx merkle switch (theirs takes up to 1000).
        auto r = node.mempool_transactions(999, exclude);
        txs = std::move(r.first);
        tpl.fees = r.second;
        bool genesis = !tip_hash;
        if (genesis) {
            // the first coinbase output of the genesis block takes the initial reputation
            Output o;
            o.address = wallet.voter_address();
            o.content.value = tpl.fees;
            tpl.body.coinbase.outputs.push_back(o);
        } else if (tpl.fees) {
            Output o;
            o.address = wallet.get_or_generate_last_address();
            o.content.value = tpl.fees;
            tpl.body.coinbase.outputs.push_back(o);
        }
        std::vector<AuthorizedTransaction> ats;
        for (auto& [tx, _] : txs) ats.push_back(tx);
        Coinbase cb = tpl.body.coinbase;
        tpl.body = Body::from_authorized(ats, cb);
        tpl.bribe = fee ? *fee : (tpl.fees ? tpl.fees : EMPTY_BLOCK_BMM_BRIBE);
    } else {
        tpl.bribe = EMPTY_BLOCK_BMM_BRIBE;
    }
    std::vector<FilledTransaction> filled;
    for (size_t i = 0; i < txs.size(); i++) filled.push_back(FilledTransaction{&tpl.body.transactions[i], txs[i].second, std::nullopt});
    auto mr = compute_merkle_root(tpl.body.coinbase, filled);
    if (!mr) throw app_error("failed to compute merkle root");
    tpl.header.merkle_root = *mr;
    tpl.header.prev_side_hash = prev_side;
    tpl.header.prev_main_hash = prev_main;
    // roots: the accumulator of prev_side, modified by this body (body.rs modify_memforest)
    bt::utreexo::MemForest acc;
    if (prev_side)
        node.with_state([&](const State&, const Archive& a, const Txn& t) {
            auto v = a.aux(t, *prev_side);
            if (!v) throw app_error("no accumulator for " + bt::hex_encode(*prev_side));
            acc = *bt::utreexo::MemForest::deserialize(*v);
            return 0;
        });
    AccumulatorDiff diff;
    Hash32 cb_txid = tpl.header.coinbase_txid();
    for (uint32_t v = 0; v < tpl.body.coinbase.outputs.size(); v++)
        diff.insert(bt::utreexo::NodeHash::some(pointed_output_hash(OutPoint::coinbase(cb_txid, v), tpl.body.coinbase.outputs[v])));
    for (auto& tx : tpl.body.transactions) {
        Hash32 txid = tx.txid();
        for (auto& [op, uh] : tx.inputs) diff.remove(bt::utreexo::NodeHash::some(uh));
        for (uint32_t v = 0; v < tx.outputs.size(); v++) diff.insert(bt::utreexo::NodeHash::some(pointed_output_hash(OutPoint::regular(txid, v), tx.outputs[v])));
    }
    std::vector<bt::utreexo::NodeHash> ins, dels;
    diff.split(ins, dels);
    if (auto e = acc.modify(ins, dels)) throw app_error("utreexo error: " + *e);
    tpl.header.roots = acc.get_roots();
    return tpl;
}

// app.rs mine + miner.rs attempt_bmm/confirm_bmm
void App::mine(std::optional<uint64_t> fee)
{
    auto tpl = build_block_template(fee);
    Hash32 h = tpl.header.hash();
    auto r = node.enforcer().create_bmm_critical_data_tx(tpl.bribe, 0, h, tpl.header.prev_main_hash);
    if (!bt::enforcer::ok(r)) throw app_error(bt::enforcer::err(r).what());
    LOG_INFO("created BMM tx %s", bt::hex_encode_rev(bt::enforcer::val(r).span()).c_str());
    std::optional<Hash32> main_hash;
    std::atomic<bool> cancel{false};
    node.enforcer().subscribe_events(
        [&](const bt::enforcer::SubscribeEvent& ev) {
            if (ev.kind == bt::enforcer::SubscribeEvent::Connect && ev.info.bmm_commitment == h) main_hash = ev.header.block_hash;
            return false; // only the first event of the subscription is considered
        },
        &cancel);
    if (!main_hash) return;
    if (!node.submit_block(*main_hash, tpl.header, tpl.body)) LOG_WARN("mine: BMM not accepted as new tip");
}

bool App::connect_block(const Header& h, const Body& b, const Hash32& main_block_hash) { return node.submit_block(main_block_hash, h, b); }

Hash32 App::deposit(const Address& a, uint64_t value, uint64_t fee)
{
    auto r = node.enforcer().create_deposit_tx(a.to_base58(), value, fee);
    if (!bt::enforcer::ok(r)) throw app_error(bt::enforcer::err(r).what());
    return bt::enforcer::val(r);
}

Hash32 App::sign_and_send(const Transaction& tx)
{
    AuthorizedTransaction at = wallet.authorize(tx);
    node.submit_transaction(at);
    return at.transaction.txid();
}

void register_rpc(bt::jsonrpc::Server& s, App& app, bool public_api, bool private_api)
{
    register_rpc_node(s, app, public_api, private_api);
    register_rpc_markets(s, app, public_api, private_api);
}

} // namespace hivemind
