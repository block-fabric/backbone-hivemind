// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Truthcoin node: state + archive + mempool on one store, the mainchain link (enforcer
// gRPC), block connection with two-way-peg windows and reorgs (port of truthcoin-dc v0.20.0
// lib/node/{mod.rs, net_task.rs, mainchain_task.rs}), the market-aware mempool
// (lib/mempool.rs) and the QUIC P2P layer.
#pragma once

#include <hivemind/chain.h>
#include <hivemind/mempool.h>
#include <hivemind/side_tips.h>
#include <hivemind/state.h>

#include <bt/enforcer.h>
#include <bt/kv.h>
#include <bt/quic.h>
#include <bt/sidechain/node.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace hivemind {

struct NodeConfig {
    std::string datadir;
    Network network = Network::Betanet;
    std::optional<std::array<uint8_t, 4>> magic_override;
    bt::quic::SockAddr net_addr{"0.0.0.0", DEFAULT_PORT};
    std::string mainchain_grpc_url = "http://127.0.0.1:50051";
    std::vector<std::string> add_peers; // host:port
    std::vector<std::string> server_names;
    bool use_seeds = true;
    bool p2p = true;
    bool rebuild_side_tips = false;             // --bt-rebuild-side-tips
    std::optional<uint32_t> decision_config_testing; // --decision-config-testing N (blocks per period)
};

struct PeerView {
    bt::quic::SockAddr address;
    bool connected = false;
    bool inbound = false;
    std::string name; // bt::quic::Endpoint::peer_name: "<Name> <version>", "unknown (upstream)", "unknown"; empty until connected
};

/** types/lib.rs MainchainSyncProgress */
struct SyncProgress {
    enum Phase : uint8_t { Idle, Headers, Writing, StateSync } phase = Idle;
    uint32_t done = 0, total = 0, tip_height = 0;
};

class Node {
public:
    explicit Node(NodeConfig cfg);
    ~Node();
    bool start(std::string& err);
    void stop();

    // ---- queries (all take the node lock)
    std::optional<Hash32> tip();
    std::optional<uint32_t> height();
    std::optional<Hash32> block_hash_at(uint32_t height);
    std::optional<Header> header(const Hash32& h);
    std::optional<Body> body(const Hash32& h);
    std::optional<uint32_t> block_height(const Hash32& h);
    bool is_descendant(const Hash32& anc, const Hash32& desc);
    std::map<Hash32, uint32_t> tx_inclusions(const Hash32& txid);
    std::optional<Hash32> best_main_verification(const Hash32& side);
    std::vector<Hash32> bmm_inclusions(const Hash32& side);
    std::vector<std::pair<OutPoint, Output>> utxos();
    std::vector<std::pair<OutPoint, SpentOutput>> stxos();
    std::vector<std::pair<OutPoint, Output>> utxos_by_addresses(const std::set<Address>& a);
    std::vector<std::pair<OutPoint, SpentOutput>> stxos_by_addresses(const std::set<Address>& a);
    std::optional<WithdrawalBundle> pending_withdrawal_bundle();
    std::optional<uint32_t> latest_failed_withdrawal_bundle_height();
    std::optional<std::pair<WithdrawalBundleInfo, RollBack<BundleStatus>>> withdrawal_bundle(const Hash32& m6id);
    uint64_t sidechain_wealth();
    bt::utreexo::MemForest tip_accumulator();
    /** try_get_transaction: first inclusion on the chain (by block hash order), else mempool. */
    std::optional<std::pair<Transaction, std::optional<Hash32>>> transaction(const Hash32& txid);
    /** mempool first, then the first inclusion */
    std::optional<AuthorizedTransaction> authorized_transaction(const Hash32& txid);
    /** try_get_filled_transaction (node/mod.rs:755-822) */
    std::optional<std::pair<std::pair<AuthorizedTransaction, std::vector<Output>>, std::optional<std::pair<Hash32, uint32_t>>>>
    filled_transaction(const Hash32& txid);
    std::vector<AuthorizedTransaction> mempool();
    std::optional<AuthorizedTransaction> mempool_tx(const Hash32& txid);
    /** get_unconfirmed_utxos_by_addresses / own_unconfirmed_utxos / spent-by-mempool */
    std::map<OutPoint, Output> unconfirmed_utxos_by_addresses(const std::set<Address>& a);
    std::map<OutPoint, Output> own_unconfirmed_utxos(const std::set<Address>& a, const std::set<OutPoint>& confirmed);
    std::vector<std::pair<OutPoint, Hash32>> unconfirmed_spent(const std::vector<OutPoint>& ops);
    std::set<std::array<uint8_t, 3>> pending_decision_claim_ids();
    /** run f(state, txn) under the node lock (read-only queries of the RPC layer) */
    template <class F>
    auto with_state(F f)
    {
        std::lock_guard<std::recursive_mutex> l(mu_);
        bt::kv::Txn t(*store_);
        return f(state_, archive_, t);
    }
    std::vector<PeerView> peers();
    SyncProgress sync_progress();

    // ---- actions
    /** Node::submit_block. Returns accepted-as-new-tip. */
    bool submit_block(const Hash32& main_block_hash, const Header& h, const Body& b);
    /** Node::submit_transaction: regenerate the proof, validate (unconfirmed parents from the
     *  mempool), mempool put + mempool share update, relay. Throws on refusal. */
    void submit_transaction(AuthorizedTransaction& tx);
    void remove_from_mempool(const Hash32& txid);
    void invalidate_block(const Hash32& h);
    bool connect_peer(const std::string& hostport, std::string& err);
    bool forget_peer(const std::string& hostport);
    bool request_mainchain_ancestor_infos(const Hash32& main);
    /** Move the side tips to main_tip (mainchain_task.rs sync_side_tips_to_tip) and, when
     *  that (dis)connected mainchain blocks, run net_task.rs handle_mainchain_block_event.
     *  False when the side tips cannot follow (their mainchain task fails the same way and
     *  retries); the mainchain headers must be stored. */
    bool sync_to_mainchain_tip(const Hash32& main_tip);
    /** node/mod.rs sync_to_tip (RPC): reorg to this tip if it is better. */
    bool sync_to_tip(const TipRef& t) { return reorg_to_tip(t); }
    /** Tests: run the chain logic on a store without starting the node's threads. */
    void attach_store(std::unique_ptr<bt::kv::Store> s) { store_ = std::move(s); }
    /** Store a header and body as a peer's GetBlock answer does, without reorging. */
    void receive_block(const Header& h, const Body& b);
    /** The checks a peer's GetBlock body passes before it is stored; empty = accepted. */
    std::string check_peer_body(const Hash32& block, const Body& b);
    /** node/mod.rs get_transactions(n): topological non-trades then trades in insertion
     *  order, chained outputs, revalidated, slippage skips, conflicts dropped. */
    std::pair<std::vector<std::pair<AuthorizedTransaction, std::vector<Output>>>, uint64_t> mempool_transactions(size_t n, int exclude = 0);
    /** node/mod.rs regenerate_proof (wallet sends) */
    void regenerate_proof(Transaction& tx);

    bt::enforcer::Client& enforcer() { return *enforcer_; }
    Archive& archive() { return archive_; }
    State& state() { return state_; }
    SideTips& side_tips() { return side_tips_; }
    std::recursive_mutex& lock() { return mu_; }
    bt::kv::Store& store() { return *store_; }
    const NodeConfig& config() const { return cfg_; }
    /** Called after the tip changes (wallet refresh). */
    std::function<void()> on_tip_changed;
    std::string status();

private:
    NodeConfig cfg_;
    std::unique_ptr<bt::kv::Store> store_;
    State state_;
    Archive archive_;
    SideTips side_tips_;
    std::unique_ptr<bt::enforcer::Client> enforcer_;
    std::recursive_mutex mu_;
    Mempool mempool_;
    std::atomic<bool> running_{false};
    std::thread main_thread_, net_thread_, worker_thread_, serve_thread_;
    std::mutex main_mu_;
    std::mutex sync_mu_;
    SyncProgress sync_;

    void connect_tip_(bt::kv::Txn& t, const Header& h, const Body& b, const bt::mainchain::TwoWayPegData& d);
    void disconnect_tip_(bt::kv::Txn& t);
    /** invalid_body: set when a block on the way failed to connect (its body was deleted). */
    bool reorg_to_tip(const TipRef& new_tip, bool* invalid_body = nullptr);
    std::vector<Hash32> main_ancestors(const bt::kv::Txn& t, const Hash32& from, const std::optional<Hash32>& stop);
    void update_mempool_market(bt::kv::Txn& t, const Transaction& tx);
    void connect_mempool_cleanup(bt::kv::Txn& t, const Body& b);
    bool confirmed_on_active_chain(const bt::kv::Txn& t, const Hash32& txid);
    void put_body_(bt::kv::Txn& t, const Hash32& h, const Body& b);
    void delete_body_(bt::kv::Txn& t, const Hash32& h);
    void handle_mainchain_block_event();
    std::string check_peer_body_(const bt::kv::Txn& t, const Hash32& block, const Body& b);
    bool catch_up_mainchain();
    void mainchain_loop();

    // P2P
    struct Peer {
        bt::quic::ConnId conn = 0;
        bt::quic::SockAddr addr;
        std::string dial;
        bool connected = false, inbound = false;
        std::string name; // bt::quic::Endpoint::peer_name
        std::optional<bt::sidechain::PeerState> state;
        int64_t last_sync_try = 0;
        bool bodies_chain = false; // a request_bodies chain is running for this peer
    };
    std::unique_ptr<bt::quic::Endpoint> ep_;
    std::mutex peers_mu_;
    std::map<bt::quic::ConnId, Peer> peers_;
    std::map<std::string, int64_t> dial_targets_;
    std::map<std::string, int64_t> banned_; // rate_key(ip) -> until
    bt::sidechain::HeadersBudgets headers_budgets_; // under peers_mu_
    std::array<uint8_t, 4> magic_{};
    // The P2P loop reads only these (never the node lock or the store), so a long store
    // write cannot delay heartbeats past their 5 s timeout (which gets us banned).
    std::mutex cache_mu_;
    std::optional<bt::sidechain::TipInfo> cached_tip_info_;
    std::optional<uint32_t> cached_height_;
    std::atomic<size_t> mempool_size_{0};
    void refresh_cache();
    std::mutex jobs_mu_;
    std::condition_variable jobs_cv_;
    std::deque<std::function<void()>> jobs_;
    void post_job(std::function<void()> f);
    void net_loop();
    void worker_loop();
    void serve_loop();
    void post_serve(std::function<void()> f);
    std::mutex serve_mu_;
    std::condition_variable serve_cv_;
    std::deque<std::function<void()>> serve_;
    void on_request(bt::quic::ConnId c, int64_t stream, Bytes payload);
    void send_request(bt::quic::ConnId c, const P2P::Request& q, size_t limit, std::function<void(std::optional<P2P::Response>)> fn);
    void handle_peer_state(bt::quic::ConnId c, const bt::sidechain::PeerState& ps);
    void on_headers(bt::quic::ConnId c, const bt::sidechain::TipInfo& target, const std::vector<Header>& hs);
    void request_bodies(bt::quic::ConnId c, const bt::sidechain::TipInfo& target);
    void start_body_chain(bt::quic::ConnId c, const bt::sidechain::TipInfo& target);
    void end_body_chain(bt::quic::ConnId c);
    void ban(bt::quic::ConnId c, const std::string& why);
    std::optional<bt::sidechain::TipInfo> our_tip_info();
    size_t relay_tx(const AuthorizedTransaction& tx, std::optional<bt::quic::ConnId> except);
};

} // namespace hivemind
