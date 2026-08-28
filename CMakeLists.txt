# SPDX-License-Identifier: Apache-2.0
#
# CMake build for the Godot-independent parts of the client: the protocol,
# transport and core layers, plus the native test suite and the integration
# probe. The Godot GDExtension itself is built with SCons (see SConstruct),
# which reuses the same sources.
cmake_minimum_required(VERSION 3.16)
project(styly_netsync_godot CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

option(STYLY_NETSYNC_BUILD_TESTS "Build the native test suite" ON)
option(STYLY_NETSYNC_BUILD_TRANSPORT "Build the ZeroMQ transport (requires libzmq)" ON)

# --- Protocol + core (no ZeroMQ, no Godot) -----------------------------------

add_library(styly_netsync_protocol STATIC
  src/protocol/binary_reader.cpp
  src/protocol/binary_writer.cpp
  src/protocol/pose_codec.cpp
  src/protocol/protocol_v8.cpp
  src/core/json_util.cpp
  src/godot/coordinate_converter.cpp
)
target_include_directories(styly_netsync_protocol PUBLIC src)

# --- Transport + client core (needs libzmq) ----------------------------------

if(STYLY_NETSYNC_BUILD_TRANSPORT)
  include(cmake/FindOrBuildLibzmq.cmake)

  add_library(styly_netsync_core STATIC
    src/transport/zmq_transport.cpp
    src/transport/server_discovery.cpp
    src/core/device_mapping.cpp
    src/core/rpc_manager.cpp
    src/core/network_variable_manager.cpp
    src/core/object_sync_manager.cpp
    src/core/netsync_client.cpp
    src/core/device_id.cpp
  )
  target_include_directories(styly_netsync_core PUBLIC src)
  target_link_libraries(styly_netsync_core PUBLIC styly_netsync_protocol styly::libzmq)
  find_package(Threads REQUIRED)
  target_link_libraries(styly_netsync_core PUBLIC Threads::Threads)
endif()

# --- Tests -------------------------------------------------------------------

if(STYLY_NETSYNC_BUILD_TESTS)
  enable_testing()

  set(STYLY_GOLDEN_VECTORS "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/vectors.json")

  function(styly_add_test name source)
    add_executable(${name} ${source})
    target_include_directories(${name} PRIVATE src tests)
    target_compile_definitions(${name}
      PRIVATE "STYLY_GOLDEN_VECTORS_PATH=\"${STYLY_GOLDEN_VECTORS}\"")
    target_link_libraries(${name} PRIVATE ${ARGN})
    add_test(NAME ${name} COMMAND ${name})
  endfunction()

  styly_add_test(test_golden_vectors tests/protocol/test_golden_vectors.cpp
                 styly_netsync_protocol)
  styly_add_test(test_binary_io tests/protocol/test_binary_io.cpp styly_netsync_protocol)
  styly_add_test(test_pose_codec tests/protocol/test_pose_codec.cpp styly_netsync_protocol)
  styly_add_test(test_json_util tests/protocol/test_json_util.cpp styly_netsync_protocol)
  styly_add_test(test_coordinates tests/compatibility/test_coordinates.cpp
                 styly_netsync_protocol)

  if(STYLY_NETSYNC_BUILD_TRANSPORT)
    styly_add_test(test_topic_routing tests/transport/test_topic_routing.cpp
                   styly_netsync_core)
    styly_add_test(test_outbound_queue tests/transport/test_outbound_queue.cpp
                   styly_netsync_core)
    styly_add_test(test_discovery_parsing tests/transport/test_discovery_parsing.cpp
                   styly_netsync_core)
    styly_add_test(test_client_state tests/compatibility/test_client_state.cpp
                   styly_netsync_core)

    # Integration probe: a headless client driven from Python against a real
    # STYLY NetSync server. Not registered with CTest — see tests/integration.
    add_executable(netsync_probe tests/integration/netsync_probe.cpp)
    target_include_directories(netsync_probe PRIVATE src)
    target_link_libraries(netsync_probe PRIVATE styly_netsync_core)
  endif()
endif()
