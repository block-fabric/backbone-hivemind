// Codec oracle: every line of tests/data/oracle_vectors.jsonl was written by THEIR crates
// (tests/oracle: `hm_oracle vectors 7 200`, truthcoin-dc v0.20.0) for a random
// transaction with every TransactionData variant, outpoint and content kind. We decode their
// bincode, then re-encode borsh/bincode/serde_json, recompute the txid, verify the
// signatures and actor proof, and recompute block merkle roots. HM_ORACLE_VECTORS=<file>
// runs a larger set (e.g. `hm_oracle vectors 1 4000`).
#include "test.h"

#include <hivemind/json.h>
#include <hivemind/types.h>

#include <univalue.h>

#include <cstdlib>
#include <fstream>

using namespace hivemind;

TEST(hm_oracle_codec_vectors)
{
    std::string path = bttest::data_dir() + "/oracle_vectors.jsonl";
    if (const char* e = std::getenv("HM_ORACLE_VECTORS")) path = e;
    std::ifstream f(path);
    REQUIRE(f.good());
    std::string line;
    size_t txs = 0, bodies = 0;
    while (std::getline(f, line)) {
        UniValue v;
        REQUIRE(v.read(line));
        if (v.exists("i")) {
            auto bc = bt::hex_decode(v["bincode"].get_str());
            REQUIRE(bc.has_value());
            Reader r(*bc);
            Transaction t = de_transaction(r);
            CHECK(r.empty());
            Writer w;
            bincode(w, t);
            CHECK_EQ(bt::hex_encode(w.buf), v["bincode"].get_str());
            CHECK_EQ(bt::hex_encode(t.canonical_bytes()), v["borsh"].get_str());
            CHECK_EQ(bt::hex_encode(t.txid()), v["txid"].get_str());
            CHECK_EQ(json::transaction(t).write(), v["json"].get_str());
            auto abc = bt::hex_decode(v["atx_bincode"].get_str());
            Reader ar(*abc);
            AuthorizedTransaction at = de_authorized_transaction(ar);
            CHECK(ar.empty());
            Writer aw;
            bincode(aw, at);
            CHECK_EQ(bt::hex_encode(aw.buf), v["atx_bincode"].get_str());
            CHECK_EQ(json::authorized_transaction(at).write(), v["atx_json"].get_str());
            CHECK_EQ(verify_transaction(at), v["verify"].get_bool());
            txs++;
        } else {
            auto bb = bt::hex_decode(v["body_bincode"].get_str());
            Reader r(*bb);
            Body b = de_body(r);
            CHECK(r.empty());
            std::vector<FilledTransaction> filled;
            const UniValue& spent = v["spent"];
            for (size_t i = 0; i < b.transactions.size(); i++) {
                FilledTransaction ft{&b.transactions[i], {}, std::nullopt};
                for (auto& s : spent[i].getValues()) {
                    auto ob = bt::hex_decode(s.get_str());
                    Reader orr(*ob);
                    ft.spent_utxos.push_back(de_output(orr));
                }
                filled.push_back(std::move(ft));
            }
            auto root = compute_merkle_root(b.coinbase, filled);
            if (v["merkle_root"].isNull()) CHECK(!root.has_value());
            else {
                REQUIRE(root.has_value());
                CHECK_EQ(bt::hex_encode(*root), v["merkle_root"].get_str());
            }
            CHECK_EQ(json::body(b).write(), v["body_json"].get_str());
            bodies++;
        }
    }
    CHECK(txs > 0);
    CHECK(bodies > 0);
    std::fprintf(stderr, "  %zu transactions, %zu bodies\n", txs, bodies);
}
