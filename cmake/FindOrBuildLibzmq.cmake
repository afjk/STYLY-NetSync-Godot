# SPDX-License-Identifier: Apache-2.0
#
# Provides the imported target `styly::libzmq`.
#
# By default the vendored submodule at third_party/libzmq is built as a static
# library, so a checkout builds reproducibly with no system packages. Set
# STYLY_NETSYNC_SYSTEM_LIBZMQ=ON to link a system libzmq instead (useful on CI
# images that already ship one, and for distro packaging).

option(STYLY_NETSYNC_SYSTEM_LIBZMQ "Link the system libzmq instead of the vendored submodule" OFF)

if(TARGET styly::libzmq)
  return()
endif()

if(STYLY_NETSYNC_SYSTEM_LIBZMQ)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SYSTEM_ZMQ QUIET libzmq)
  endif()

  if(SYSTEM_ZMQ_FOUND)
    add_library(styly::libzmq INTERFACE IMPORTED)
    target_include_directories(styly::libzmq INTERFACE ${SYSTEM_ZMQ_INCLUDE_DIRS})
    target_link_libraries(styly::libzmq INTERFACE ${SYSTEM_ZMQ_LINK_LIBRARIES})
    message(STATUS "styly-netsync: using system libzmq ${SYSTEM_ZMQ_VERSION}")
    return()
  endif()

  find_path(ZMQ_INCLUDE_DIR zmq.h)
  find_library(ZMQ_LIBRARY NAMES zmq libzmq)
  if(ZMQ_INCLUDE_DIR AND ZMQ_LIBRARY)
    add_library(styly::libzmq INTERFACE IMPORTED)
    target_include_directories(styly::libzmq INTERFACE ${ZMQ_INCLUDE_DIR})
    target_link_libraries(styly::libzmq INTERFACE ${ZMQ_LIBRARY})
    message(STATUS "styly-netsync: using system libzmq at ${ZMQ_LIBRARY}")
    return()
  endif()

  message(FATAL_ERROR
    "STYLY_NETSYNC_SYSTEM_LIBZMQ=ON but no system libzmq was found. "
    "Install libzmq3-dev (or equivalent), or set STYLY_NETSYNC_SYSTEM_LIBZMQ=OFF "
    "to build the vendored submodule.")
endif()

set(STYLY_LIBZMQ_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/libzmq")
if(NOT EXISTS "${STYLY_LIBZMQ_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "third_party/libzmq is empty. Run:\n"
    "    git submodule update --init --recursive")
endif()

# libzmq's own options. Draft APIs stay off: the client uses only the stable API.
set(BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC ON CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_PERF_TOOL OFF CACHE BOOL "" FORCE)
set(WITH_DOC OFF CACHE BOOL "" FORCE)
set(ENABLE_CPACK OFF CACHE BOOL "" FORCE)
set(ENABLE_DRAFTS OFF CACHE BOOL "" FORCE)
set(ENABLE_CURVE OFF CACHE BOOL "" FORCE)
set(WITH_TLS OFF CACHE BOOL "" FORCE)
set(WITH_LIBSODIUM OFF CACHE BOOL "" FORCE)
set(ZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)

# libzmq switches its ipc:// transport on for any Windows build that can see
# <afunix.h>, but src/ipc_address.hpp only includes that header under _MSC_VER
# and otherwise falls through to <sys/socket.h>, which does not exist on
# Windows. A MinGW build — which is what CMake picks up on the GitHub Windows
# image — therefore fails to compile libzmq. The client speaks tcp:// and UDP
# only and never opens an ipc:// endpoint, so turning the transport off costs
# nothing. Pre-seeding the cache variable also skips libzmq's own
# check_include_files, which is what would otherwise switch it back on.
if(WIN32)
  set(ZMQ_HAVE_IPC OFF CACHE BOOL "" FORCE)
endif()

# libzmq 4.3.5 opens with `cmake_minimum_required(VERSION 2.8.12)`, and CMake 4
# removed compatibility with anything below 3.5 — so on a runner with CMake 4
# (macOS and Windows images already ship it) configuring the submodule fails
# before a single source is read. This is the escape hatch CMake documents for
# exactly this case: it applies to the pinned submodule for the duration of the
# add_subdirectory below and nothing else. Drop it when libzmq is bumped to a
# revision that raises its own minimum.
set(STYLY_SAVED_POLICY_VERSION_MINIMUM "${CMAKE_POLICY_VERSION_MINIMUM}")
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

add_subdirectory("${STYLY_LIBZMQ_DIR}" "${CMAKE_BINARY_DIR}/libzmq" EXCLUDE_FROM_ALL)

set(CMAKE_POLICY_VERSION_MINIMUM "${STYLY_SAVED_POLICY_VERSION_MINIMUM}")

if(TARGET libzmq-static)
  set(STYLY_LIBZMQ_TARGET libzmq-static)
elseif(TARGET libzmq)
  set(STYLY_LIBZMQ_TARGET libzmq)
else()
  message(FATAL_ERROR "libzmq did not define a usable target")
endif()

add_library(styly::libzmq INTERFACE IMPORTED)
target_link_libraries(styly::libzmq INTERFACE ${STYLY_LIBZMQ_TARGET})
target_include_directories(styly::libzmq INTERFACE "${STYLY_LIBZMQ_DIR}/include")
# The client uses only the stable API; make that explicit so a draft-enabled
# libzmq cannot silently change behaviour.
target_compile_definitions(styly::libzmq INTERFACE ZMQ_BUILD_DRAFT_API=0)
message(STATUS "styly-netsync: building vendored libzmq (${STYLY_LIBZMQ_TARGET})")
