// The public betanet chain through our state machine: after every block, the state digest
// equals the one THEIR State printed for the same block (tests/data/betanet).
#include "test.h"

#include <hivemind/chain.h>
#include <hivemind/state.h>

#include <bt/kv.h>

#include <univalue.h>

#include <filesystem>
#include <fstream>

#include <unistd.h>

using namespace hivemind;

static bt::mainchain::TwoWayPegData twpd(const UniValue& v)
{
    bt::mainchain::TwoWayPegData d;
    for (auto& b : v.getValues()) {
        bt::mainchain::BlockInfo bi;
        for (auto& e : b["events"].getValues()) {
            auto& x = e["deposit"];
            if (!x.isObject()) throw std::runtime_error("betanet has deposits only");
            bt::mainchain::Deposit dep;
            dep.sequence_number = x["tx_index"].getInt<uint64_t>();
            dep.txid = *bt::hash_from_rev_hex(x["txid"].get_str());
            dep.vout = x["vout"].getInt<uint32_t>();
            dep.address = *bt::hex_decode(x["address_hex"].get_str());
            dep.value_sats = x["value"].getInt<uint64_t>();
            bi.events.emplace_back(dep);
        }
        d.replace(*bt::hash_from_rev_hex(b["main"].get_str()), bi);
    }
    return d;
}

TEST(hm_betanet_replay_matches_their_state)
{
    std::ifstream f(bttest::data_dir() + "/betanet/fixture.jsonl"), g(bttest::data_dir() + "/betanet/oracle_digests.jsonl");
    REQUIRE(f.good() && g.good());
    auto dir = std::filesystem::temp_directory_path() / ("hm-replay-test-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    size_t n = 0;
    {
        auto store = bt::kv::open_sqlite((dir / "s.sqlite").string());
        State st;
        Archive archive;
        std::string line, expect;
        while (std::getline(f, line) && std::getline(g, expect)) {
            UniValue v, e;
            REQUIRE(v.read(line) && e.read(expect));
            auto raw = bt::hex_decode(v["block"].get_str());
            Reader r(*raw);
            Header h = de_header(r);
            Body b = de_body(r);
            bt::kv::Txn t(*store);
            archive.put_header(t, h);
            auto pre = st.prevalidate(t, h, b, archive);
            st.connect_prevalidated(t, h, b, std::move(pre), v["main_ts"].getInt<uint64_t>());
            st.connect_2wpd(t, twpd(v["twpd"]));
            archive.put_body(t, h.hash(), b);
            t.commit();
            UniValue ours = st.digest(t);
            for (auto& k : e.getKeys()) CHECK_EQ(ours[k].write(), e[k].write());
            n++;
        }
    }
    std::filesystem::remove_all(dir);
    CHECK_EQ(n, size_t(651));
}
