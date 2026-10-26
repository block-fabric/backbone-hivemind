// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Hivemind's app layer (doc/app.md): what our app of Hivemind (Truthcoin) and the unified wallet need of this node, on
// the app RPC (bt/appui), beside the LayerTwo JSON-RPC, which it does not touch. It reads and acts through the same
// Node, Wallet and App the LayerTwo methods use: the LayerTwo reads are served as they are (the same handlers, the same
// JSON), every spend is a confirmed method (asked in the app's trusted dialog, bound to a one-time token: payments,
// decisions claimed, markets opened, trades, ballots, Votecoin moved), and the wallet's history, the explorer, the peg,
// the markets, decisions, votes, positions and payouts as the pages show them, and the backup phrase are host methods.
#pragma once

#include <hivemind/app.h>

#include <bt/appui/dropin.h>
#include <bt/appui/host.h>
#include <bt/appui/rpcserver.h>
#include <bt/jsonrpc.h>

#include <memory>
#include <string>

namespace hivemind {

class AppLayer {
public:
    /** `network`: the LayerTwo name of the network (betanet, regtest...). */
    AppLayer(App& app, std::string network, std::string datadir, bt::appui::AppOptions options);
    ~AppLayer();
    AppLayer(const AppLayer&) = delete;
    AppLayer& operator=(const AppLayer&) = delete;

    bool start(std::string& err);
    void stop();
    uint16_t port() const;

    /** The LayerTwo methods the pages read (the same handlers and JSON as the LayerTwo server's). */
    static const std::vector<std::string>& l2l_reads();
    /** The spends of the pages (rpc.confirmed), LayerTwo's names. */
    static const std::vector<std::string>& confirmed_methods();
    /** The host methods of the pages. */
    static const std::vector<std::string>& host_methods();
    /** Every method of the app RPC: the pack's manifest is checked against it. */
    std::vector<std::string> app_methods() const;
    UniValue manifest() const;

    /** The host, for tests (webuihost without HTTP). */
    bt::appui::RemoteHost& host() { return *host_; }
    bt::appui::RpcServer& server() { return *server_; }

    /** The wallet's history (dropin.history): one row a transaction touching the wallet, newest first. */
    UniValue history(size_t limit);

private:
    struct Impl;
    std::unique_ptr<Impl> m_;
    std::unique_ptr<bt::appui::RpcServer> server_;
    std::unique_ptr<bt::appui::RemoteHost> host_;
};

} // namespace hivemind
