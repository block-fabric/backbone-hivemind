// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Truthcoin wallet, key-compatible with truthcoin-dc v0.20.0 lib/wallet:
//   seed (BIP-39) -> bip32ish over ristretto255 (thunder-rust's derivation),
//   keys at m/43'/1899'/<purpose>'/13'/0'/index, purpose 0 = tx signing (addresses),
//   1 = encryption (x25519 secret = scalar bytes), 2 = message signing.
// Address index 0 is the voter address (reputation, votes). The builders are theirs:
// coin selection by value (confirmed first, then own unconfirmed change when allowed),
// MarketFunds/Withdrawal outputs skipped, change to a new address, and actor proofs for
// sells, reputation transfers, votes and ballots whose actor has no input.
// Their LMDB wallet is not read: import by mnemonic restores the same keys and addresses.
#pragma once

#include <hivemind/node.h>

#include <bt/hd.h>
#include <bt/kv.h>

#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace hivemind {

struct WalletError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/** types/wallet.rs Balance */
struct Balance {
    uint64_t total = 0, available = 0, unconfirmed = 0;
};

struct CreateMarketInput {
    std::string title, description, dimensions;
    std::optional<double> beta, trading_fee;
    std::optional<uint64_t> initial_liquidity;
    std::optional<std::vector<size_t>> category_option_counts;
    std::optional<uint8_t> tx_pow_hash_selector, tx_pow_ordering, tx_pow_difficulty;
    std::vector<ClaimDecisionPayload> new_claims;
};

/** state/markets/types.rs parse_dimensions; nullopt = MarketError::InvalidDimensions */
std::optional<std::vector<DimensionSpec>> parse_dimensions(const std::string& s);
/** math/markets.rs market_storage_fee */
uint64_t market_storage_fee(size_t tradeable_outcomes);
constexpr double DEFAULT_MARKET_BETA = 7.0;

class Wallet {
public:
    Wallet(const std::string& path, Node& node, bool spend_zero_conf_change = true);
    bool has_seed();
    void set_seed(const bt::ByteArray<64>& seed);
    void set_seed_from_mnemonic(const std::string& mnemonic);
    bool spend_zero_conf_change() const { return spend_zero_conf_change_; }

    Address get_new_address();
    Address get_receive_address();
    Address get_or_generate_last_address();
    Address voter_address();
    std::set<Address> addresses();
    bt::ByteArray<32> get_new_encryption_key();
    bt::frost::Point get_new_verifying_key();

    /** confirmed wallet outputs (node state by address) */
    std::map<OutPoint, Output> utxos();
    /** own unconfirmed outputs (mempool.rs own_unconfirmed_utxos) */
    std::map<OutPoint, Output> unconfirmed_utxos();
    std::set<OutPoint> mempool_spent();
    std::map<OutPoint, SpentOutput> stxos();
    Balance balance();

    Transaction create_transaction_many(const std::map<Address, uint64_t>& dests, uint64_t fee);
    Transaction create_withdrawal(const bt::btc::Address& main_address, uint64_t value, uint64_t main_fee, uint64_t fee);
    Transaction claim_decision(const ClaimDecisionPayload& p, uint64_t fee);
    std::pair<Transaction, MarketId> create_market(const CreateMarketInput& in, uint64_t fee);
    Transaction trade(const MarketId& m, uint32_t outcome, int64_t shares, const Address& trader, uint64_t limit_sats,
                      std::optional<std::array<uint8_t, 3>> tx_pow, const Hash32& prev_block_hash);
    Transaction amplify_beta(const MarketId& m, uint64_t amount, const Address& author);
    Transaction submit_ballot(const std::vector<BallotItem>& votes, uint32_t voting_period, uint64_t fee);
    Transaction transfer_reputation(const Address& dest, double amount, uint64_t fee);
    AuthorizedTransaction authorize(const Transaction& tx);

    bt::frost::Signature sign_arbitrary_msg(const bt::frost::Point& vk, const std::string& msg);
    Authorization sign_arbitrary_msg_as_addr(const Address& a, const std::string& msg);
    Bytes decrypt_msg(const bt::ByteArray<32>& epk, ByteSpan ciphertext);

    static bt::hd::Xpriv xpriv(const bt::ByteArray<64>& seed, uint32_t purpose, uint32_t index);

private:
    struct Selected {
        uint64_t total = 0;
        std::map<OutPoint, Output> coins;
        std::set<OutPoint> unconfirmed;
    };
    std::unique_ptr<bt::kv::Store> store_;
    Node& node_;
    bool spend_zero_conf_change_;
    std::recursive_mutex mu_;
    std::optional<bt::ByteArray<64>> seed();
    bt::ByteArray<64> need_seed();
    bt::frost::SigningKey signing_key(uint32_t index);
    bt::frost::SigningKey signing_key_for(const Address& a);
    Address new_index_address(uint32_t index);
    Selected select_coins(uint64_t value);
    std::pair<uint64_t, std::map<OutPoint, Output>> select_from_address(uint64_t value, const Address& a);
    void push_change(std::vector<Output>& outs, uint64_t change);
};

} // namespace hivemind
