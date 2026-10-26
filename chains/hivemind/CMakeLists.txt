# Truthcoin / Hivemind (eCash slot 13) rule module and binaries.
add_library(hivemind_core STATIC
  src/types.cpp
  src/fmt.cpp
  src/math.cpp
  src/state.cpp
  src/block.cpp
  src/twpd.cpp
  src/validation.cpp
  src/decisions.cpp
  src/voting.cpp
  src/market_block.cpp
  src/settlement.cpp
  src/undo.cpp
  src/chain.cpp
  src/mempool.cpp
  src/json.cpp
  src/json_parse.cpp)
target_include_directories(hivemind_core PUBLIC include)
target_link_libraries(hivemind_core PUBLIC bt_core hivemind_math_ffi)

# Public betanet data through our state machine (fixture building, per-height digests).
add_executable(truthcoin-replay tools/replay.cpp)
target_link_libraries(truthcoin-replay PRIVATE hivemind_core)

if(BT_WITH_NET)
  # Fetch full blocks from public betanet peers over their QUIC protocol (client only).
  add_executable(truthcoin-fetch-blocks tools/fetch_blocks.cpp)
  target_link_libraries(truthcoin-fetch-blocks PRIVATE hivemind_core bt_net)
endif()

if(BT_WITH_NET AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
  add_library(hivemind_node STATIC src/node.cpp src/side_tips.cpp src/wallet.cpp src/app.cpp src/rpc_node.cpp src/rpc_markets.cpp)
  target_link_libraries(hivemind_node PUBLIC hivemind_core bt_net)
  # drop-in names: truthcoin-dc v0.20.0 ships `truthcoin` and `truthcoin-cli`
  add_executable(truthcoin src/main.cpp)
  target_link_libraries(truthcoin PRIVATE hivemind_node)
  add_executable(truthcoin-cli src/cli.cpp)
  target_link_libraries(truthcoin-cli PRIVATE hivemind_core)
endif()
