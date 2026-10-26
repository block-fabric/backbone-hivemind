// Mempool: a refused transaction leaves nothing behind (their insert is one LMDB write
// transaction, dropped on error), so it cannot block later claims of the same decisions.
#include "test.h"

#include <hivemind/mempool.h>

using namespace hivemind;

static AuthorizedTransaction claim_tx(uint8_t input_seed, uint8_t decision, uint8_t salt)
{
    AuthorizedTransaction a;
    Hash32 prev{};
    prev[0] = input_seed;
    a.transaction.inputs.push_back({OutPoint::regular(prev, 0), Hash32{}});
    ClaimDecisionPayload p;
    DecisionClaimEntry e;
    e.decision_id_bytes = {0, 0, decision};
    e.header = std::string(1, char('a' + salt));
    p.decisions.push_back(e);
    a.transaction.data = TxData(p);
    return a;
}

TEST(hm_mempool_refused_tx_leaves_no_claim)
{
    Mempool mp;
    auto first = claim_tx(1, 7, 0);
    mp.put(first);
    // spends the same input and claims a different decision: refused as a double spend
    auto ds = claim_tx(1, 9, 1);
    bool refused = false;
    try {
        mp.put(ds);
    } catch (const MempoolError& e) {
        refused = e.kind == MempoolError::DoubleSpent;
    }
    CHECK(refused);
    CHECK(!mp.pending_decision_claim_ids().count({0, 0, 9}));
    CHECK_EQ(mp.size(), size_t(1));
    // a later valid claim of decision 9 is accepted
    auto ok = claim_tx(2, 9, 2);
    mp.put(ok);
    CHECK(mp.pending_decision_claim_ids().count({0, 0, 9}));
    // an input spent twice inside one transaction is refused without side effects
    auto twice = claim_tx(3, 11, 3);
    twice.transaction.inputs.push_back(twice.transaction.inputs[0]);
    refused = false;
    try {
        mp.put(twice);
    } catch (const MempoolError&) {
        refused = true;
    }
    CHECK(refused);
    CHECK(!mp.pending_decision_claim_ids().count({0, 0, 11}));
    CHECK(!mp.spender(twice.transaction.inputs[0].first).has_value());
    // removing the claimant frees its decision
    mp.remove(ok.transaction.txid());
    CHECK(!mp.pending_decision_claim_ids().count({0, 0, 9}));
}
