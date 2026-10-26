// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <hivemind/chain.h>

namespace hivemind {

// lib/net/mod.rs:169-195
std::vector<std::string> seed_peers(Network n)
{
    auto p = ":" + std::to_string(DEFAULT_PORT);
    switch (n) {
    case Network::Betanet: return {"seed.beta.ecash.eu.com" + p};
    case Network::Forknet: return {"explorer.bip300.xyz" + p, "157.180.8.224" + p};
    case Network::Signet: return {"172.105.148.135" + p, "truthcoin.bip300.xyz" + p};
    case Network::Regtest: return {};
    }
    return {};
}

} // namespace hivemind
