cmake_minimum_required(VERSION 3.22)
project(backbone_hivemind VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# The shared engine (git submodule ../backbone-core at core/): BT_* options, dependencies
# (core/depends/built/<host> when present), bt_core, bt_net, bt_test_harness,
# bt_core_test_cases. -ffp-contract=off is propagated by bt_core.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/core/CMakeLists.txt")
  message(FATAL_ERROR "core/ is empty: run `git submodule update --init`")
endif()
add_subdirectory(core)

# Their consensus math, compiled from their own sources (third_party/truthcoin-dc, MIT)
# behind a C ABI: rust/hivemind_math_ffi. Built with their toolchain (rust-toolchain.toml
# there: 1.97) and their Cargo.lock versions.
# Offline, from the vendored crates (rust/fetch-vendor.sh fills rust/vendor and checks it
# against rust/VENDOR.sha256; rust/.cargo/config.toml points cargo at it).
find_package(Threads REQUIRED)
find_program(CARGO cargo HINTS "$ENV{HOME}/.cargo/bin" REQUIRED)
set(HM_RUST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/rust/hivemind_math_ffi")
set(HM_RUST_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/rust-target")
set(HM_RUST_TRIPLE "" CACHE STRING "cargo --target triple (empty = host)")
if(HM_RUST_TRIPLE)
  set(HM_RUST_LIB "${HM_RUST_TARGET_DIR}/${HM_RUST_TRIPLE}/release/libhivemind_math_ffi.a")
  set(HM_RUST_TARGET_ARG --target ${HM_RUST_TRIPLE})
else()
  set(HM_RUST_LIB "${HM_RUST_TARGET_DIR}/release/libhivemind_math_ffi.a")
  set(HM_RUST_TARGET_ARG "")
endif()
file(GLOB_RECURSE HM_THIRD_PARTY_RS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/truthcoin-dc/*.rs")
add_custom_command(
  OUTPUT "${HM_RUST_LIB}"
  COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${HM_RUST_TARGET_DIR}
          ${CARGO} build --release --locked --offline -j4 ${HM_RUST_TARGET_ARG} --manifest-path "${HM_RUST_DIR}/Cargo.toml"
  WORKING_DIRECTORY "${HM_RUST_DIR}"
  DEPENDS "${HM_RUST_DIR}/src/lib.rs" "${HM_RUST_DIR}/Cargo.toml" "${HM_RUST_DIR}/Cargo.lock" ${HM_THIRD_PARTY_RS}
  COMMENT "cargo: truthcoin-dc v0.20.0 lib/math (hivemind_math_ffi)"
  VERBATIM)
add_custom_target(hivemind_math_ffi_build DEPENDS "${HM_RUST_LIB}")
add_library(hivemind_math_ffi STATIC IMPORTED GLOBAL)
set_target_properties(hivemind_math_ffi PROPERTIES IMPORTED_LOCATION "${HM_RUST_LIB}")
if(WIN32)
  set_target_properties(hivemind_math_ffi PROPERTIES INTERFACE_LINK_LIBRARIES "ws2_32;userenv;ntdll;bcrypt")
else()
  set_target_properties(hivemind_math_ffi PROPERTIES INTERFACE_LINK_LIBRARIES "Threads::Threads;${CMAKE_DL_LIBS};m")
endif()
add_dependencies(hivemind_math_ffi hivemind_math_ffi_build)

add_subdirectory(chains/hivemind)

# Our app of Hivemind (doc/app.md): hivemind-app, the node (truthcoin) with our interface; and its pages as a pack for the
# unified wallet (target hivemind-app-pack). Needs WebKitGTK (Linux) or WebView2 (Windows), npm, and the sidechain
# template's repository (BT_KIT_DIR) at the commit of app/kit.commit.
option(BT_BUILD_APP "Build hivemind-app" OFF)
if(BT_BUILD_APP)
  include(core/cmake/BtDesktopApp.cmake)
  bt_desktop_app(hivemind-app STEM hivemind NAME Hivemind VERSION ${PROJECT_VERSION}
                 CHAIN ${CMAKE_SOURCE_DIR}/app/chain MANIFEST ${CMAKE_SOURCE_DIR}/app/manifest.json
                 KIT_COMMIT_FILE ${CMAKE_SOURCE_DIR}/app/kit.commit MAIN ${CMAKE_SOURCE_DIR}/chains/hivemind/app/main.cpp NODE_TARGET truthcoin)
endif()

if(BT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
