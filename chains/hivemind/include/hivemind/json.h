// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// JSON forms of Truthcoin types exactly as serde_json renders them in truthcoin-dc's
// JSON-RPC (human-readable serde): hex hashes, base58 addresses, externally tagged enums,
// [u8; N] as arrays of numbers, f64 as ryu writes it, verifying keys in bech32m.
#pragma once

#include <hivemind/state.h>

#include <bt/jsonrpc.h>
#include <univalue.h>

namespace hivemind::json {

UniValue f64(double x);                    // serde_json number (ryu); non-finite -> null
UniValue hash(const Hash32& h);            // hex (BlockHash, Txid, MerkleRoot, ...)
UniValue btc_hash(const Hash32& h);        // display order (bitcoin::BlockHash, Txid, M6id)
UniValue bytes_array(ByteSpan b);          // serde of [u8; N] / Vec<u8>: array of numbers
UniValue address(const Address& a);
UniValue market_id(const MarketId& m);     // [u8; 6] array
UniValue decision_id(const DecisionId& d); // [u8; 3] array
UniValue outpoint(const OutPoint& o);
UniValue content(const Content& c);
UniValue output(const Output& o);
UniValue pointed_output(const OutPoint& op, const Output& o);
UniValue inpoint(const InPoint& i);
UniValue spent_output(const SpentOutput& s);
UniValue node_hash(const bt::utreexo::NodeHash& n);
UniValue proof(const bt::utreexo::Proof& p);
UniValue decision_type(const DecisionType& t);
UniValue claim_payload(const ClaimDecisionPayload& p);
UniValue tx_data(const TxData& d);
UniValue transaction(const Transaction& t);
/** verifying key Display: bech32m "ba-svk1..." */
std::string verifying_key_str(const bt::frost::Point& vk);
/** EncryptionPubKey Display: bech32m "ba-enc1..." */
std::string encryption_pubkey_str(const bt::ByteArray<32>& epk);
std::string signature_str(const bt::frost::Signature& s);
UniValue authorization(const Authorization& a);
UniValue authorized_transaction(const AuthorizedTransaction& t);
UniValue coinbase(const Coinbase& c);
UniValue body(const Body& b);
UniValue header(const Header& h);
UniValue block(const Block& b);
UniValue bitcoin_tx(const std::vector<bt::btc::TxOut>& outs);
UniValue withdrawal_bundle(const WithdrawalBundle& w);
UniValue withdrawal_bundle_info(const WithdrawalBundleInfo& i);

// ---- parsing (json_parse.cpp): throws ParseError (the RPC layer answers -32602)
struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
uint64_t parse_u64(const UniValue& v);
uint32_t parse_u32(const UniValue& v);
int64_t parse_i64(const UniValue& v);
double parse_f64(const UniValue& v);
bool parse_bool(const UniValue& v);
std::string parse_str(const UniValue& v);
Hash32 parse_hash(const UniValue& v);
Hash32 parse_btc_hash(const UniValue& v);
Address parse_address(const UniValue& v);
MarketId parse_market_id(const UniValue& v);
OutPoint parse_outpoint(const UniValue& v);
Content parse_content(const UniValue& v);
Output parse_output(const UniValue& v);
ClaimDecisionPayload parse_claim_payload(const UniValue& v);
TxData parse_tx_data(const UniValue& v);
Transaction parse_transaction(const UniValue& v);
bt::frost::Point parse_verifying_key(const UniValue& v);
bt::frost::Signature parse_signature(const UniValue& v);
Authorization parse_authorization(const UniValue& v);
AuthorizedTransaction parse_authorized_transaction(const UniValue& v);
Header parse_header(const UniValue& v);
Body parse_body(const UniValue& v);

} // namespace hivemind::json
