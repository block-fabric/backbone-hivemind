// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Truthcoin application (truthcoin-dc v0.20.0 app/app.rs): node + wallet, block templates,
// BMM mining through the enforcer, deposits, and the JSON-RPC surface of rpc-api/lib.rs and
// app/rpc_server.rs (81 methods, serde_json shapes):
//   rpc_node.cpp     node, mempool, wallet, keys and messages
//   rpc_markets.cpp  decisions, markets, trades, votes, reputation
#pragma once

#include <hivemind/node.h>
#include <hivemind/wallet.h>

#include <bt/jsonrpc.h>

namespace hivemind {

struct BlockTemplate {
    uint64_t bribe = 0;
    Header header;
    Body body;
    uint64_t fees = 0;
};

class App {
public:
    App(Node& node, Wallet& wallet) : node(node), wallet(wallet) {}
    Node& node;
    Wallet& wallet;

    static constexpr uint64_t EMPTY_BLOCK_BMM_BRIBE = 1000;
    BlockTemplate build_block_template(std::optional<uint64_t> fee);
    /** exclude: 0 = all, 1 = without ClaimDecision/CreateMarket/TransferReputation, 2 = none. */
    BlockTemplate build_block_template_(std::optional<uint64_t> fee, int exclude);
    void mine(std::optional<uint64_t> fee);
    bool connect_block(const Header& h, const Body& b, const Hash32& main_block_hash);
    Hash32 deposit(const Address& a, uint64_t value, uint64_t fee);
    /** app.rs sign_and_send: authorize, regenerate the proof, submit; returns the txid. */
    Hash32 sign_and_send(const Transaction& tx);
    std::function<void()> on_stop;
};

/** Register the RPC methods: the public server gets the node API, the private one the
 *  private node API and the wallet; one server gets all (default: same address). */
void register_rpc(bt::jsonrpc::Server& s, App& app, bool public_api, bool private_api);
void register_rpc_node(bt::jsonrpc::Server& s, App& app, bool public_api, bool private_api);
void register_rpc_markets(bt::jsonrpc::Server& s, App& app, bool public_api, bool private_api);

} // namespace hivemind
