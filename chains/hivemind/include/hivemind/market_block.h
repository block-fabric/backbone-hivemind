// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#pragma once

#include <hivemind/chain.h>
#include <hivemind/state.h>

#include <memory>

namespace hivemind {

/** markets/block.rs TradeSimulation: trades and AmplifyBeta of a block under construction,
 *  applied in block order with the rules of block connect (templates and the mempool). */
class TradeSimulation {
public:
    TradeSimulation();
    ~TradeSimulation();
    /** true = Applied, false = Skipped (slippage; reason in *skip_reason). Throws StateError. */
    bool apply(const State& s, const Archive& a, const bt::kv::Txn& t, const FilledTransaction& ft, std::string* skip_reason = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hivemind
