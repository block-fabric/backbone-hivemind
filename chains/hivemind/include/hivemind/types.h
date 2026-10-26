// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Truthcoin / Hivemind (eCash betanet slot 13) consensus types, byte-exact with
// truthcoin-dc v0.20.0 (`types/`, MIT, aa8b3ea). The core is thunder-rust's (block,
// coinbase, CBMT merkle roots, Utreexo, FROST ristretto255); Truthcoin adds the
// transaction data (decisions, markets, trades, votes, reputation), two outpoint kinds
// (MarketFunds, Payout), MarketFunds outputs, actor proofs, and a domain byte in the
// signed message. Every hash is BLAKE3(borsh(x)); the P2P wire uses bincode 1.3 over serde.
// Citations are truthcoin-dc paths at v0.20.0.
#pragma once

#include <bt/bitcoin.h>
#include <bt/bytes.h>
#include <bt/codec.h>
#include <bt/crypto.h>
#include <bt/frost.h>
#include <bt/utreexo.h>

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace hivemind {

using bt::Bytes;
using bt::append;
using bt::ByteSpan;
using bt::Hash32;
using bt::Reader;
using bt::Writer;

constexpr uint8_t THIS_SIDECHAIN = 13; // types/lib.rs:51

/** types/address.rs: 20 raw bytes, text = plain base58, no version, no checksum. */
struct Address {
    bt::Hash20 b;
    static const Address ALL_ZEROS;
    std::string to_base58() const;
    static std::optional<Address> from_base58(std::string_view s);
    /** `s13_<b58>_<hex(sha256(prefix))[..6]>` (address.rs:30-36). */
    std::string format_for_deposit() const;
    /** Address::from_deposit_address (address.rs:38-53): nullopt with their error text in *err. */
    static std::optional<Address> from_deposit_address(std::string_view s, std::string* err = nullptr);
    friend auto operator<=>(const Address&, const Address&) = default;
    friend bool operator==(const Address&, const Address&) = default;
};

/** authorization.rs get_address: first 20 bytes of BLAKE3-XOF(compressed VK). */
Address address_from_vk(const bt::frost::Point& vk);

/** types/market.rs: 6-byte market id; Ord by bytes. */
struct MarketId {
    std::array<uint8_t, 6> b{};
    std::string to_hex() const;
    friend auto operator<=>(const MarketId&, const MarketId&) = default;
    friend bool operator==(const MarketId&, const MarketId&) = default;
};

/** types/decision.rs: standard bit 23, 7-bit period, 16-bit index; Ord by bytes. */
struct DecisionId {
    std::array<uint8_t, 3> b{};
    uint32_t as_u32() const { return uint32_t(b[0]) << 16 | uint32_t(b[1]) << 8 | b[2]; }
    bool is_standard() const { return (as_u32() >> 23) & 1; }
    uint32_t period_index() const { return (as_u32() >> 16) & 0x7F; }
    uint32_t decision_index() const { return as_u32() & 0xFFFF; }
    uint32_t voting_period() const { return period_index() + 1; }
    std::string to_hex() const;
    /** DecisionId::new: nullopt if period > 127 or index > 65535 (with their reason). */
    static std::optional<DecisionId> make(bool is_standard, uint32_t period, uint32_t index, std::string* err = nullptr);
    /** from_bytes always succeeds for 3 bytes (both fields are masked); from_hex checks length/hex. */
    static std::optional<DecisionId> from_hex(std::string_view s, std::string* err = nullptr);
    friend auto operator<=>(const DecisionId&, const DecisionId&) = default;
    friend bool operator==(const DecisionId&, const DecisionId&) = default;
};

/** decision.rs DecisionType: Ord compares Scaled fields with f64::total_cmp. */
struct DecisionType {
    enum Kind : uint8_t { Binary = 0, Scaled = 1, Category = 2 };
    Kind kind = Binary;
    double min = 0, max = 0, increment = 0; // Scaled
    std::vector<std::string> options;       // Category
    bool operator==(const DecisionType& o) const;
};

/** types/market.rs DimensionSpec */
struct DimensionSpec {
    enum Kind : uint8_t { Single = 0, Categorical = 1 };
    Kind kind = Single;
    DecisionId id;
    bool operator==(const DimensionSpec&) const = default;
};

/** transaction/mod.rs:76-92 */
struct DecisionClaimEntry {
    std::array<uint8_t, 3> decision_id_bytes{};
    std::string header, description;
    std::optional<std::string> option_0_label, option_1_label;
    std::optional<std::vector<std::string>> option_labels, tags;
};

struct ClaimDecisionPayload {
    DecisionType decision_type;
    std::vector<DecisionClaimEntry> decisions;
};

struct BallotItem {
    std::array<uint8_t, 3> decision_id_bytes{};
    double vote_value = 0;
};

// transaction/mod.rs:96-157 TransactionData variants, in declaration (tag) order.
struct TxCreateMarket {
    std::string title, description;
    std::vector<DimensionSpec> dimension_specs;
    std::vector<ClaimDecisionPayload> new_claims;
    std::optional<double> trading_fee;
    std::optional<uint8_t> tx_pow_hash_selector, tx_pow_ordering, tx_pow_difficulty;
};
struct TxTrade {
    MarketId market_id;
    uint32_t outcome_index = 0;
    int64_t shares = 0;
    Address trader;
    uint64_t limit_sats = 0;
    std::optional<uint64_t> tx_pow_nonce;
    Hash32 prev_block_hash;
    bool is_buy() const { return shares > 0; }
    uint64_t shares_abs() const { return shares < 0 ? uint64_t(0) - uint64_t(shares) : uint64_t(shares); }
};
struct TxSubmitVote {
    Address voter;
    std::array<uint8_t, 3> decision_id_bytes{};
    double vote_value = 0;
    uint32_t voting_period = 0;
};
struct TxSubmitBallot {
    Address voter;
    std::vector<BallotItem> votes;
    uint32_t voting_period = 0;
};
struct TxTransferReputation {
    Address sender, dest;
    double amount = 0;
};
struct TxAmplifyBeta {
    MarketId market_id;
    uint64_t amount = 0;
    Address market_author;
};
using TxData = std::variant<ClaimDecisionPayload, TxCreateMarket, TxTrade, TxSubmitVote, TxSubmitBallot, TxTransferReputation, TxAmplifyBeta>;
enum TxDataKind : uint8_t { CLAIM_DECISION = 0, CREATE_MARKET, TRADE, SUBMIT_VOTE, SUBMIT_BALLOT, TRANSFER_REPUTATION, AMPLIFY_BETA };
const char* tx_data_name(size_t index);

/** output/content.rs */
struct Content {
    enum Kind : uint8_t { Value = 0, Withdrawal = 1, MarketFunds = 2 };
    Kind kind = Value;
    uint64_t value = 0;             // Value; Withdrawal value; MarketFunds amount
    uint64_t main_fee = 0;          // Withdrawal
    bt::btc::Address main_address;  // Withdrawal
    MarketId market_id;             // MarketFunds
    bool is_fee = false;            // MarketFunds
    /** Withdrawal = value + main_fee, saturating at u64::MAX (content.rs:96-104). */
    uint64_t get_value() const;
    bool is_market_treasury() const { return kind == MarketFunds && !is_fee; }
    bool operator==(const Content& o) const;
};

struct Output {
    Address address;
    Content content;
    uint64_t get_value() const { return content.get_value(); }
    bool operator==(const Output&) const = default;
};

/** transaction/outpoint.rs. Ord = variant index, then the variant's fields in order. */
struct OutPoint {
    enum Kind : uint8_t { Regular = 0, Coinbase = 1, Deposit = 2, MarketFunds = 3, Payout = 4 };
    Kind kind = Regular;
    Hash32 txid;          // Txid / CoinbaseTxid / bitcoin Txid (internal order) / Payout hash
    uint32_t vout = 0;    // Regular, Coinbase, Deposit, Payout
    MarketId market_id;   // MarketFunds
    uint32_t block_height = 0;
    bool is_fee = false;
    friend std::strong_ordering operator<=>(const OutPoint& a, const OutPoint& b);
    friend bool operator==(const OutPoint& a, const OutPoint& b) { return (a <=> b) == 0; }
    /** OutPointKey: borsh form zero-padded to 37 bytes, the key of utxos/stxos. */
    std::array<uint8_t, 37> key() const;
    std::string to_string() const; // Display (outpoint.rs:62-84)
    static OutPoint regular(const Hash32& txid, uint32_t vout);
    static OutPoint coinbase(const Hash32& txid, uint32_t vout);
    static OutPoint market_funds(const MarketId& m, uint32_t h, bool fee);
    static OutPoint payout(const Hash32& h, uint32_t vout);
};

/** Reference to a spend (transaction/mod.rs:44-58). */
struct InPoint {
    enum Kind : uint8_t { Regular = 0, Withdrawal = 1, Redistribution = 2 };
    Kind kind = Regular;
    Hash32 id; // txid, or m6id (internal byte order)
    uint32_t vin = 0;
    bool operator==(const InPoint&) const = default;
};

struct SpentOutput {
    Output output;
    InPoint inpoint;
};

struct Transaction {
    std::vector<std::pair<OutPoint, Hash32>> inputs; // (outpoint, utxo_hash)
    bt::utreexo::Proof proof;                        // #[borsh(skip)], serde only
    std::vector<Output> outputs;
    std::optional<TxData> data;

    /** borsh; throws bt::DecodeError("NaN") where borsh refuses to serialize a NaN f64
     *  (their `hash()` panics there). */
    Bytes canonical_bytes() const;
    uint64_t canonical_size() const { return canonical_bytes().size(); }
    Hash32 txid() const;
    bool has_nan() const;
};

struct Authorization {
    bt::frost::Point verifying_key;
    bt::frost::Signature signature;
};

struct AuthorizedTransaction {
    Transaction transaction;
    std::vector<Authorization> authorizations;
    std::optional<Authorization> actor_proof;
};

struct Coinbase {
    Bytes memo;
    std::vector<Output> outputs;
};

struct Body {
    Coinbase coinbase;
    std::vector<Transaction> transactions;
    std::vector<Authorization> authorizations;
    std::vector<std::optional<Authorization>> actor_proofs; // one per tx (not committed)
    size_t inputs_len() const;
    std::vector<AuthorizedTransaction> authorized_transactions() const;
    static Body from_authorized(std::vector<AuthorizedTransaction> txs, Coinbase cb);
};

/** block/mod.rs:16-39 */
struct Header {
    Hash32 merkle_root;
    std::optional<Hash32> prev_side_hash;
    Hash32 prev_main_hash; // bitcoin block hash, internal byte order
    std::vector<bt::utreexo::NodeHash> roots;

    Hash32 hash() const;
    Hash32 coinbase_txid() const;
    bool operator==(const Header& o) const
    {
        return merkle_root == o.merkle_root && prev_side_hash == o.prev_side_hash && prev_main_hash == o.prev_main_hash &&
               roots == o.roots;
    }
};

struct Block {
    Header header;
    Body body;
};

/** A transaction together with the outputs it spends (FilledTransaction). */
struct FilledTransaction {
    const Transaction* tx = nullptr;
    std::vector<Output> spent_utxos;
    std::optional<Address> actor_address; // address of the actor proof, if any
    /** get_fee(): value_in - value_out with checked sums; error -> nullopt. */
    std::optional<uint64_t> fee() const;
    std::optional<uint64_t> value_in() const;
    const TxData* data() const { return tx->data ? &*tx->data : nullptr; }
    template <class T>
    const T* get() const { return tx->data ? std::get_if<T>(&*tx->data) : nullptr; }
};

// ---------------------------------------------------------------- borsh
void borsh(Writer& w, const Address& a);
void borsh(Writer& w, const Content& c);
void borsh(Writer& w, const Output& o);
void borsh(Writer& w, const OutPoint& o);
void borsh(Writer& w, const DecisionType& t);
void borsh(Writer& w, const DimensionSpec& d);
void borsh(Writer& w, const ClaimDecisionPayload& p);
void borsh(Writer& w, const TxData& d);
void borsh(Writer& w, const Transaction& t);
void borsh(Writer& w, const Authorization& a);
void borsh(Writer& w, const Coinbase& c);
void borsh(Writer& w, const Body& b);
void borsh(Writer& w, const Header& h);
/** borsh f64: NaN is refused (throws bt::DecodeError). */
void borsh_f64(Writer& w, double v);
template <class T>
Bytes borsh_bytes(const T& x)
{
    Writer w;
    borsh(w, x);
    return w.buf;
}
template <class T>
Hash32 hash(const T& x)
{
    return bt::blake3(borsh_bytes(x));
}

/** utxo_hash / Utreexo leaf = blake3(borsh{outpoint, output}) (output/mod.rs). */
Hash32 pointed_output_hash(const OutPoint& op, const Output& out);

// ---------------------------------------------------------------- bincode (serde)
void bincode(Writer& w, const Address& a);
void bincode(Writer& w, const Content& c);
void bincode(Writer& w, const Output& o);
void bincode(Writer& w, const OutPoint& o);
void bincode(Writer& w, const InPoint& o);
void bincode(Writer& w, const DecisionType& t);
void bincode(Writer& w, const ClaimDecisionPayload& p);
void bincode(Writer& w, const TxData& d);
void bincode(Writer& w, const bt::utreexo::NodeHash& h);
void bincode(Writer& w, const bt::utreexo::Proof& p);
void bincode(Writer& w, const Transaction& t);
void bincode(Writer& w, const Authorization& a);
void bincode(Writer& w, const AuthorizedTransaction& t);
void bincode(Writer& w, const Coinbase& c);
void bincode(Writer& w, const Body& b);
void bincode(Writer& w, const Header& h);
void bincode(Writer& w, const Block& b);
void bincode_f64(Writer& w, double v);

Address de_address(Reader& r);
Content de_content(Reader& r);
Output de_output(Reader& r);
OutPoint de_outpoint(Reader& r);
InPoint de_inpoint(Reader& r);
DecisionType de_decision_type(Reader& r);
ClaimDecisionPayload de_claim_payload(Reader& r);
TxData de_tx_data(Reader& r);
bt::utreexo::NodeHash de_nodehash(Reader& r);
bt::utreexo::Proof de_proof(Reader& r);
Transaction de_transaction(Reader& r);
Authorization de_authorization(Reader& r);
AuthorizedTransaction de_authorized_transaction(Reader& r);
Coinbase de_coinbase(Reader& r);
Body de_body(Reader& r);
Header de_header(Reader& r);
double de_f64(Reader& r);

/** borsh decoders (the RPC's raw tx hex and get_block_index are borsh). */
Output borsh_de_output(Reader& r);
OutPoint borsh_de_outpoint(Reader& r);
TxData borsh_de_tx_data(Reader& r);
Transaction borsh_de_transaction(Reader& r);

// ---------------------------------------------------------------- merkle
/** block/body.rs compute_merkle_root. nullopt where theirs returns an error (block invalid). */
std::optional<Hash32> compute_merkle_root(const Coinbase& cb, const std::vector<FilledTransaction>& txs);
std::optional<Hash32> coinbase_merkle_root(const Coinbase& cb);
std::optional<Hash32> outputs_merkle_root(const std::vector<Output>& outs);
Hash32 inputs_merkle_root(const std::vector<std::pair<OutPoint, Hash32>>& ins);
/** Transaction::compute_merkle_root: {inputs root, outputs root, hash(borsh(data))}. */
std::optional<Hash32> tx_merkle_root(const Transaction& t);

/** coinbase.rs compute_txid */
Hash32 compute_coinbase_txid(const Hash32& merkle_root, const Hash32& prev_main_hash, const std::optional<Hash32>& prev_side);

// ---------------------------------------------------------------- limits
/** lib/state/mod.rs:707-738 (f64 powi + floor, cap START*111 from month 120). */
uint64_t body_sigops_limit(uint32_t height);
uint64_t body_size_limit(uint32_t height);
/** compiler-rt __powidf2: what Rust's f64::powi lowers to for a runtime exponent. */
double powi(double a, int b);

// ---------------------------------------------------------------- authorizations
enum class Dst : uint8_t { Transaction = 0, Arbitrary = 0xFF };
/** tx_msg_canonical: [Dst::Transaction] ++ borsh(tx). */
Bytes tx_msg_canonical(const Transaction& tx);
/** verify_body: verify_authorizations (exact count; every signature over its tx's message),
 *  then every actor proof of authorized_transactions(). */
bool verify_body(const Body& body);
/** verify_transaction: verify_authorized_transaction (count == inputs, non-empty batch) and
 *  the actor proof. */
bool verify_transaction(const AuthorizedTransaction& tx);
/** sign/verify with a domain byte (authorization.rs sign/verify). */
bool verify_dst(const bt::frost::Signature& sig, const bt::frost::Point& vk, Dst dst, ByteSpan msg);

/** tx_pow.rs serialize_trade_for_pow: borsh{market_id, outcome_index, shares, trader, limit_sats, prev_block_hash}. */
Bytes serialize_trade_for_pow(const MarketId& m, uint32_t outcome_index, int64_t shares, const Address& trader, uint64_t limit_sats,
                              const Hash32& prev_block_hash);

} // namespace hivemind
