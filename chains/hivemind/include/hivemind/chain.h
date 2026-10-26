// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Truthcoin's traits for the core's archive and P2P templates, and its network constants.
// truthcoin-dc v0.20.0 lib/archive and lib/net/peer/message.rs are thunder-rust v0.18.2's
// (same tables, messages and "THNDR" magic), lib/net/mod.rs:169-195 the seeds.
#pragma once

#include <hivemind/types.h>

#include <bt/sidechain/archive.h>
#include <bt/sidechain/p2p.h>

namespace hivemind {

struct Chain {
    using Header = hivemind::Header;
    using Body = hivemind::Body;
    using AuthorizedTransaction = hivemind::AuthorizedTransaction;
    static constexpr size_t MIN_HEADER_BYTES = 32 + 1 + 32 + 8;
    static Hash32 hash(const Header& h) { return h.hash(); }
    static void encode_header(Writer& w, const Header& h) { bincode(w, h); }
    static Header decode_header(Reader& r) { return de_header(r); }
    static void encode_body(Writer& w, const Body& b) { bincode(w, b); }
    static Body decode_body(Reader& r) { return de_body(r); }
    static void encode_authorized_tx(Writer& w, const AuthorizedTransaction& t) { bincode(w, t); }
    static AuthorizedTransaction decode_authorized_tx(Reader& r) { return de_authorized_transaction(r); }
    static std::vector<Hash32> txids(const Body& b)
    {
        std::vector<Hash32> r;
        for (auto& t : b.transactions) r.push_back(t.txid());
        return r;
    }
};

using Archive = bt::sidechain::Archive<Chain>;
using P2P = bt::sidechain::P2P<Chain>;
using bt::sidechain::BmmResult;
using bt::sidechain::Network;
using bt::sidechain::TipRef;
using bt::sidechain::U256;

/** "THNDR" Baudot prefix (shared with Thunder): Betanet 85189504, Regtest ..00, Signet 01, Forknet 02. */
inline std::array<uint8_t, 4> magic_bytes(Network n) { return bt::sidechain::magic_bytes({0x85, 0x18, 0x95, 0x00}, n); }
constexpr uint16_t DEFAULT_PORT = 4000 + THIS_SIDECHAIN;     // 4013
constexpr uint16_t DEFAULT_RPC_PORT = 6000 + THIS_SIDECHAIN; // 6013
std::vector<std::string> seed_peers(Network n);
/** CARGO_PKG_VERSION, sent in every heartbeat. */
inline const char* VERSION = "0.20.0";
inline bt::sidechain::Version peer_version() { return {0, 20, 0}; }

} // namespace hivemind
