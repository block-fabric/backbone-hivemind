// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// truthcoin-replay: public betanet data through our state machine, side by side with theirs.
//
//   truthcoin-replay fixture <blocks.bin> <twpd13.json>  > fixture.jsonl
//       blocks.bin from truthcoin-fetch-blocks, twpd13.json = the public enforcer's
//       GetTwoWayPegData (slot 13, up to the tip). Checks every block's bincode re-encoding
//       and its BMM (a main block commits to the header hash and is a child of prev_main),
//       then writes one line per block: {height, block, main_ts, twpd} where twpd is the
//       2WPD window their reorg_to_tip passes (main ancestors in (prev_main(N-1),
//       prev_main(N)], all ancestors for block 0) and main_ts the timestamp of the BMM block
//       (connect_tip_: best main verification).
//   truthcoin-replay run <fixture.jsonl> [--testing N]
//       prevalidate + connect + 2WPD for each line with every consensus check, printing the
//       same per-height digest as `hm_oracle replay` (tests/oracle) does from their State.
#include <hivemind/chain.h>
#include <hivemind/state.h>

#include <bt/kv.h>
#include <bt/parse.h>

#include <univalue.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

#include <unistd.h>

using namespace hivemind;

static std::string slurp(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}

static Bytes body_bytes(const Block& b)
{
    Writer w;
    bincode(w, b.header);
    bincode(w, b.body);
    return w.buf;
}

static int fixture(const std::string& blocks_path, const std::string& twpd_path)
{
    std::string all = slurp(blocks_path);
    UniValue tw;
    if (!tw.read(slurp(twpd_path))) {
        std::fprintf(stderr, "bad %s\n", twpd_path.c_str());
        return 1;
    }
    struct Main {
        bt::mainchain::BlockHeaderInfo hi;
        bt::mainchain::BlockInfo bi;
    };
    std::map<uint32_t, std::vector<Main>> by_height;
    std::map<Hash32, Main> by_hash;
    for (auto& item : tw["blocks"].getValues()) {
        auto hi = bt::mainchain::header_info_from_json(item["blockHeaderInfo"]);
        auto bi = bt::mainchain::block_info_from_json(item["blockInfo"]);
        if (!hi || !bi) {
            std::fprintf(stderr, "bad twpd item\n");
            return 1;
        }
        by_height[hi->height].push_back({*hi, *bi});
        by_hash[hi->block_hash] = {*hi, *bi};
    }
    size_t p = 0;
    uint32_t want = 0, reencoded = 0, bmm = 0;
    std::map<int, uint32_t> kinds;
    size_t ntx = 0;
    std::optional<uint32_t> prev_main_height;
    while (p + 8 <= all.size()) {
        uint32_t h, n;
        std::memcpy(&h, all.data() + p, 4);
        std::memcpy(&n, all.data() + p + 4, 4);
        p += 8;
        if (h != want || p + n > all.size()) {
            std::fprintf(stderr, "blocks.bin: bad record at %u\n", h);
            return 1;
        }
        ByteSpan raw((const uint8_t*)all.data() + p, n);
        p += n;
        want++;
        auto resp = P2P::decode_response(raw);
        if (resp.kind != P2P::Response::Block) return 1;
        Block b{resp.header, resp.body};
        Bytes enc = body_bytes(b);
        Bytes again(raw.begin() + 4, raw.end());
        if (enc != again) {
            std::fprintf(stderr, "height %u: re-encoding differs\n", h);
            return 1;
        }
        reencoded++;
        for (auto& tx : b.body.transactions) kinds[tx.data ? int(tx.data->index()) + 1 : 0]++;
        ntx += b.body.transactions.size();
        Hash32 side = b.header.hash();
        // BMM: a main block whose commitment is the header hash and whose parent is prev_main
        const Main* bmm_block = nullptr;
        for (auto& [mh, m] : by_hash)
            if (m.bi.bmm_commitment == side && m.hi.prev_block_hash == b.header.prev_main_hash) {
                if (!bmm_block || U256::from_le(m.hi.work) > U256::from_le(bmm_block->hi.work)) bmm_block = &m;
            }
        if (!bmm_block) {
            std::fprintf(stderr, "height %u: no BMM verification for %s\n", h, bt::hex_encode(side).c_str());
            return 1;
        }
        bmm++;
        uint32_t pm_height = bmm_block->hi.height - 1;
        UniValue window(UniValue::VARR);
        for (auto& [mh_height, ms] : by_height) {
            if (mh_height > pm_height) break;
            if (prev_main_height && mh_height <= *prev_main_height) continue;
            for (auto& m : ms) {
                if (m.bi.events.empty()) continue;
                UniValue w(UniValue::VOBJ);
                w.pushKV("main", bt::hex_encode_rev(m.hi.block_hash.span()));
                w.pushKV("bmm", m.bi.bmm_commitment ? UniValue(bt::hex_encode(*m.bi.bmm_commitment)) : UniValue());
                UniValue evs(UniValue::VARR);
                for (auto& e : m.bi.events) {
                    UniValue ev(UniValue::VOBJ);
                    if (auto* d = std::get_if<bt::mainchain::Deposit>(&e)) {
                        UniValue x(UniValue::VOBJ);
                        x.pushKV("tx_index", d->sequence_number);
                        x.pushKV("txid", bt::hex_encode_rev(d->txid.span()));
                        x.pushKV("vout", uint64_t(d->vout));
                        x.pushKV("address_hex", bt::hex_encode(d->address));
                        x.pushKV("value", d->value_sats);
                        ev.pushKV("deposit", x);
                    } else {
                        auto& wb = std::get<bt::mainchain::WithdrawalBundleEvent>(e);
                        UniValue x(UniValue::VOBJ);
                        x.pushKV("m6id", bt::hex_encode_rev(wb.m6id.span()));
                        x.pushKV("status", wb.status == bt::mainchain::BundleStatus::Submitted   ? "submitted"
                                           : wb.status == bt::mainchain::BundleStatus::Confirmed ? "confirmed"
                                                                                                 : "failed");
                        ev.pushKV("bundle", x);
                    }
                    evs.push_back(ev);
                }
                w.pushKV("events", evs);
                window.push_back(w);
            }
        }
        prev_main_height = pm_height;
        UniValue line(UniValue::VOBJ);
        line.pushKV("height", uint64_t(h));
        line.pushKV("block", bt::hex_encode(enc));
        line.pushKV("main_ts", bmm_block->hi.timestamp);
        line.pushKV("twpd", window);
        std::cout << line.write() << "\n";
    }
    std::fprintf(stderr, "%u blocks: %u re-encoded byte-identical, %u BMM-verified; %zu transactions:", want, reencoded, bmm, ntx);
    static const char* names[] = {"plain", "ClaimDecision", "CreateMarket", "Trade", "SubmitVote", "SubmitBallot", "TransferReputation", "AmplifyBeta"};
    for (auto& [k, n] : kinds) std::fprintf(stderr, " %s=%u", names[k], n);
    std::fprintf(stderr, "\n");
    return 0;
}

static bt::mainchain::TwoWayPegData twpd_from_json(const UniValue& v)
{
    bt::mainchain::TwoWayPegData d;
    for (auto& b : v.getValues()) {
        auto main = bt::hash_from_rev_hex(b["main"].get_str());
        bt::mainchain::BlockInfo bi;
        if (b["bmm"].isStr()) bi.bmm_commitment = *bt::hex_to_array<32>(b["bmm"].get_str());
        for (auto& e : b["events"].getValues()) {
            if (e.exists("deposit")) {
                auto& x = e["deposit"];
                bt::mainchain::Deposit dep;
                dep.sequence_number = x["tx_index"].getInt<uint64_t>();
                dep.txid = *bt::hash_from_rev_hex(x["txid"].get_str());
                dep.vout = x["vout"].getInt<uint32_t>();
                dep.address = *bt::hex_decode(x["address_hex"].get_str());
                dep.value_sats = x["value"].getInt<uint64_t>();
                bi.events.emplace_back(dep);
            } else {
                auto& x = e["bundle"];
                bt::mainchain::WithdrawalBundleEvent w;
                w.m6id = *bt::hash_from_rev_hex(x["m6id"].get_str());
                auto s = x["status"].get_str();
                w.status = s == "submitted" ? bt::mainchain::BundleStatus::Submitted
                           : s == "confirmed" ? bt::mainchain::BundleStatus::Confirmed
                                              : bt::mainchain::BundleStatus::Failed;
                bi.events.emplace_back(w);
            }
        }
        d.replace(*main, bi);
    }
    return d;
}

static int run(const std::string& path, std::optional<uint32_t> testing)
{
    auto dir = std::filesystem::temp_directory_path() / ("truthcoin-replay-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    int rc = 0;
    {
        auto store = bt::kv::open_sqlite((dir / "replay.sqlite").string());
        DecisionConfig cfg;
        if (testing) {
            cfg.is_blocks = true;
            cfg.quantity = *testing;
        }
        State st(cfg);
        Archive archive;
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line)) {
            UniValue v;
            if (!v.read(line)) return 1;
            auto raw = bt::hex_decode(v["block"].get_str());
            Reader r(*raw);
            Block b;
            b.header = de_header(r);
            b.body = de_body(r);
            uint64_t main_ts = v["main_ts"].getInt<uint64_t>();
            auto d = twpd_from_json(v["twpd"]);
            bt::kv::Txn t(*store);
            try {
                archive.put_header(t, b.header);
                auto pre = st.prevalidate(t, b.header, b.body, archive);
                st.connect_prevalidated(t, b.header, b.body, std::move(pre), main_ts);
                st.connect_2wpd(t, d);
                archive.put_body(t, b.header.hash(), b.body);
                t.commit();
                std::cout << st.digest(t).write() << "\n";
            } catch (const std::exception& e) {
                UniValue o(UniValue::VOBJ);
                o.pushKV("height", v["height"]);
                o.pushKV("error", e.what());
                std::cout << o.write() << "\n";
                rc = 1;
                break;
            }
        }
    }
    std::filesystem::remove_all(dir);
    return rc;
}

// Ops written by `hm_oracle simulate` (THEIR State, Archive, Wallet): repeat each on ours
// and compare. Returns the number of mismatches.
static int sim(const std::string& path, uint32_t testing, bool verbose)
{
    auto dir = std::filesystem::temp_directory_path() / ("truthcoin-sim-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    int bad = 0;
    std::map<std::string, std::pair<uint32_t, uint32_t>> counts; // op -> (n, mismatches)
    {
        auto store = bt::kv::open_sqlite((dir / "sim.sqlite").string());
        DecisionConfig cfg;
        cfg.is_blocks = true;
        cfg.quantity = testing;
        State st(cfg);
        Archive archive;
        std::ifstream f(path);
        std::string line;
        size_t ln = 0;
        auto mismatch = [&](const std::string& op, const std::string& what) {
            bad++;
            counts[op].second++;
            if (bad <= 20) std::fprintf(stderr, "line %zu %s: %s\n", ln, op.c_str(), what.c_str());
        };
        auto compare_digest = [&](const std::string& op, const bt::kv::Txn& t, const UniValue& want) {
            UniValue ours = st.digest(t);
            for (auto& k : want.getKeys())
                if (ours[k].write() != want[k].write()) {
                    mismatch(op, k + ": ours " + ours[k].write() + " theirs " + want[k].write());
                    break;
                }
        };
        while (std::getline(f, line)) {
            ln++;
            UniValue v;
            if (!v.read(line)) return 1;
            std::string op = v["op"].get_str();
            counts[op].first++;
            if (op == "check") {
                auto raw = bt::hex_decode(v["atx"].get_str());
                Reader r(*raw);
                AuthorizedTransaction atx = de_authorized_transaction(r);
                bt::kv::Txn t(*store);
                std::string err;
                bool ok = true;
                try {
                    st.validate_transaction(t, {}, atx, archive);
                } catch (const std::exception& e) {
                    ok = false;
                    err = e.what();
                }
                if (ok != v["ok"].get_bool()) mismatch(op, std::string("ours ") + (ok ? "accepts" : "rejects (" + err + ")") + ", theirs " + v["error"].get_str());
                else if (!ok) {
                    counts["check-reject-text"].first++;
                    if (err != v["error"].get_str()) {
                        counts["check-reject-text"].second++; // reported, not a consensus mismatch
                        if (verbose) std::fprintf(stderr, "  text: ours [%s] theirs [%s]\n", err.c_str(), v["error"].get_str().c_str());
                    }
                }
                else if (verbose && !ok) std::fprintf(stderr, "  reject: ours [%s] theirs [%s]\n", err.c_str(), v["error"].get_str().c_str());
            } else if (op == "connect" || op == "badblock" || op == "badconnect") {
                auto raw = bt::hex_decode(v["block"].get_str());
                Reader r(*raw);
                Block b;
                b.header = de_header(r);
                b.body = de_body(r);
                uint64_t main_ts = v["main_ts"].getInt<uint64_t>();
                bt::kv::Txn t(*store);
                std::string err;
                try {
                    archive.put_header(t, b.header);
                    auto pre = st.prevalidate(t, b.header, b.body, archive);
                    if (op != "badblock") {
                        st.connect_prevalidated(t, b.header, b.body, std::move(pre), main_ts);
                        st.connect_2wpd(t, twpd_from_json(v["twpd"]));
                        archive.put_body(t, b.header.hash(), b.body);
                        archive.put_aux(t, b.header.hash(), st.accumulator(t).serialize());
                    }
                } catch (const std::exception& e) {
                    err = e.what();
                    if (err.empty()) err = "error";
                }
                if (op == "connect") {
                    if (!err.empty()) {
                        mismatch(op, "ours fails: " + err);
                        break;
                    }
                    t.commit();
                    compare_digest(op, t, v["digest"]);
                } else if (op == "badconnect") {
                    if (err.empty()) mismatch(op, "ours connects, theirs: " + v["error"].get_str());
                    else if (verbose) std::fprintf(stderr, "  bad connect: ours [%s] theirs [%s]\n", err.c_str(), v["error"].get_str().c_str());
                } else {
                    bool theirs_err = !v["error"].isNull();
                    if (theirs_err != !err.empty())
                        mismatch(op, "ours " + (err.empty() ? std::string("accepts") : "rejects (" + err + ")") + ", theirs " + (theirs_err ? v["error"].get_str() : "accept"));
                    else if (verbose && theirs_err) std::fprintf(stderr, "  bad block: ours [%s] theirs [%s]\n", err.c_str(), v["error"].get_str().c_str());
                    // not committed
                }
            } else if (op == "disconnect") {
                bt::kv::Txn t(*store);
                try {
                    auto tip = *st.tip(t);
                    Header h = *archive.header(t, tip);
                    Body b = *archive.body(t, tip);
                    st.disconnect_2wpd(t, twpd_from_json(v["twpd"]));
                    st.disconnect_tip(t, h, b);
                    // net_task.rs disconnect_tip_: the accumulator of the new tip from the archive
                    if (auto nt = st.tip(t)) st.put_accumulator(t, *bt::utreexo::MemForest::deserialize(*archive.aux(t, *nt)));
                    else st.put_accumulator(t, {});
                } catch (const std::exception& e) {
                    mismatch(op, std::string("ours fails: ") + e.what());
                    break;
                }
                t.commit();
                compare_digest(op, t, v["digest"]);
            }
        }
    }
    std::filesystem::remove_all(dir);
    for (auto& [op, c] : counts) std::fprintf(stderr, "%s: %u, %u mismatches\n", op.c_str(), c.first, c.second);
    return bad ? 1 : 0;
}

int main(int argc, char** argv)
{
    std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "fixture" && argc == 4) return fixture(argv[2], argv[3]);
    if (cmd == "run" && argc >= 3) {
        std::optional<uint32_t> testing;
        for (int i = 3; i + 1 < argc; i++)
            if (std::string(argv[i]) == "--testing") testing = uint32_t(std::stoul(argv[i + 1]));
        return run(argv[2], testing);
    }
    if (cmd == "sim" && argc >= 3) {
        uint32_t testing = 4;
        bool verbose = false;
        for (int i = 3; i < argc; i++) {
            if (std::string(argv[i]) == "--testing" && i + 1 < argc) testing = uint32_t(std::stoul(argv[i + 1]));
            if (std::string(argv[i]) == "-v") verbose = true;
        }
        return sim(argv[2], testing, verbose);
    }
    std::fprintf(stderr, "usage: truthcoin-replay sim <ops.jsonl> [--testing N] [-v]\n");
    std::fprintf(stderr, "usage: truthcoin-replay fixture <blocks.bin> <twpd13.json> | run <fixture.jsonl> [--testing N]\n");
    return 2;
}
