# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.2)
project(openr)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Disable all warnings. Warnings can vary across toolchains. We rely on
# internaly configured CI (lint and compile) to raise the warnings we find
# useful
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w")

# Optionally build with LTO
option(BUILD_WITH_LTO "BUILD_WITH_LTO" OFF)
if (BUILD_WITH_LTO)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto")
endif()

include_directories(${CMAKE_SOURCE_DIR})
include_directories(${CMAKE_BINARY_DIR})

set(
  CMAKE_MODULE_PATH
  "${CMAKE_SOURCE_DIR}/build/fbcode_builder/CMake"
  "${CMAKE_SOURCE_DIR}"
  ${CMAKE_MODULE_PATH}
)
include(FBThriftCppLibrary)
include(FBGenCMakeBuildInfo)

set(REQ_BOOST_COMPONENTS ${REQ_BOOST_COMPONENTS} system thread context filesystem program_options regex)

find_library(ASYNC async PATHS)
find_package(Boost REQUIRED COMPONENTS ${REQ_BOOST_COMPONENTS})
find_library(CONCURRENCY concurrency PATHS)
find_library(DOUBLE-CONVERSION double-conversion)
find_package(folly REQUIRED)
find_package(fb303 CONFIG REQUIRED)
find_package(fizz REQUIRED)
find_package(fmt REQUIRED)
find_package(Gflags REQUIRED)
find_package(Glog REQUIRED)
find_library(RE2 re2)
find_library(SODIUM sodium)
find_package(FBThrift REQUIRED)
find_package(wangle REQUIRED)
find_package(Threads REQUIRED)
find_library(ZSTD zstd)
find_library(BENCHMARK follybenchmark PATHS)
#find_package(range-v3 REQUIRED)

find_path(RE2_INCLUDE_DIR re2/re2.h)

set(FOLLY_EXCEPTION_TRACER)
if (TARGET Folly::folly_exception_tracer)
  set(FOLLY_EXCEPTION_TRACER Folly::folly_exception_tracer)
endif()


add_compile_definitions(NO_FOLLY_EXCEPTION_TRACER)

include_directories(
  ${Boost_INCLUDE_DIR}
  ${FB303_INCLUDE_DIR}
  ${FBTHRIFT_INCLUDE_DIR}
  ${FOLLY_INCLUDE_DIR}
  ${RE2_INCLUDE_DIR}
)

#
# Build thrift libs
#

SET(OPENR_THRIFT_LIBS)

add_fbthrift_cpp_library(
  bgp_config_cpp2
  openr/if/BgpConfig.thrift
  OPTIONS
    json
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} bgp_config_cpp2)

add_fbthrift_cpp_library(
  routing_policy_cpp2
  configerator/structs/neteng/config/routing_policy.thrift
  OPTIONS
    json
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} routing_policy_cpp2)

add_fbthrift_cpp_library(
  vip_service_config_cpp2
  configerator/structs/neteng/config/vip_service_config.thrift
  OPTIONS
    json
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} vip_service_config_cpp2)

add_fbthrift_cpp_library(
  openr_config_cpp2
  openr/if/OpenrConfig.thrift
  OPTIONS
    json
  DEPENDS
    bgp_config_cpp2
    routing_policy_cpp2
    vip_service_config_cpp2
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} openr_config_cpp2)

add_fbthrift_cpp_library(
  network_cpp2
  openr/if/Network.thrift
  OPTIONS
    json
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} network_cpp2)

add_fbthrift_cpp_library(
  platform_cpp2
  openr/if/Platform.thrift
  SERVICES
    FibService
    NeighborListenerClientForFibagent
  OPTIONS
    json
  DEPENDS
    fb303::fb303_thrift_cpp
    network_cpp2
    types_cpp2
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} platform_cpp2)

add_fbthrift_cpp_library(
  kv_store_cpp2
  openr/if/KvStore.thrift
  OPTIONS
    json
  DEPENDS
    fb303::fb303_thrift_cpp
  SERVICES
    KvStoreService
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} kv_store_cpp2)

add_fbthrift_cpp_library(
  types_cpp2
  openr/if/Types.thrift
  OPTIONS
    json
  DEPENDS
    network_cpp2
    openr_config_cpp2
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} types_cpp2)

add_build_info(build_info)

install(TARGETS
  ${OPENR_THRIFT_LIBS}
  DESTINATION lib
)

add_library(openrnetlinklib
  openr/nl/NetlinkAddrMessage.cpp
  openr/nl/NetlinkLinkMessage.cpp
  openr/nl/NetlinkNeighborMessage.cpp
  openr/nl/NetlinkRouteMessage.cpp
  openr/nl/NetlinkRuleMessage.cpp
  openr/nl/NetlinkMessageBase.cpp
  openr/nl/NetlinkProtocolSocket.cpp
  openr/nl/NetlinkTypes.cpp
)

if (BUILD_SHARED_LIBS)
  set_target_properties(openrnetlinklib PROPERTIES VERSION 1.0.0 SOVERSION 1)
endif()

target_link_libraries(openrnetlinklib
  build_info
  ${BENCHMARK}
  ${OPENR_THRIFT_LIBS}
  ${DOUBLE-CONVERSION}
  glog::glog
  gflags
  FBThrift::thriftcpp2
  fb303::fb303
  ${ASYNC}
  ${CONCURRENCY}
  ${ZSTD}
  Folly::folly
  ${FOLLY_EXCEPTION_TRACER}
  ${SODIUM}
  ${RE2}
  ${Boost_LIBRARIES}
  -lpthread
  -lcrypto
  stdc++fs
)

install(TARGETS
  openrnetlinklib
  DESTINATION lib
)

option(BUILD_TOOLS "BUILD_TOOLS" ON)


#
# Tests
#

option(BUILD_TESTS "BUILD_TESTS" ON)
option(ADD_ROOT_TESTS "ADD_ROOT_TESTS" ON)

if(BUILD_TESTS)

  enable_testing()

  find_package(GMock MODULE REQUIRED)
  find_package(GTest MODULE REQUIRED)

  include_directories(
    ${GTEST_INCLUDE_DIRS}
    ${GMOCK_INCLUDE_DIRS}
  )

  # function that adds tests, typical usage example:
  # add_openr_test(TestName test_bin_name
  #   SOURCES Feature1Test.cpp Feature2Test.cpp
  #   LIBRARIES additional_library1 -lcrypto
  #   DESTINATION folder/where/to/place/binary
  # )
  function(add_openr_test TEST_NAME BIN_NAME)
    set(one_value_args DESTINATION)
    set(multi_value_args SOURCES LIBRARIES)
    fb_cmake_parse_args(
      ARG
      ""
      "${one_value_args}"
      "${multi_value_args}"
      "${ARGN}"
    )

    add_executable(
      ${BIN_NAME}
      ${ARG_SOURCES}
    )
    target_link_libraries(${BIN_NAME}
      openrnetlinklib
      ${OPENR_THRIFT_LIBS}
      ${GTEST_BOTH_LIBRARIES}
      ${LIBGMOCK_LIBRARIES}
      ${ARG_LIBRARIES}
    )
    add_test(${TEST_NAME} ${BIN_NAME})
    if(NOT "${ARG_DESTINATION}" STREQUAL "")
      install(TARGETS
        ${BIN_NAME}
        DESTINATION ${ARG_DESTINATION}
      )
    endif()
  endfunction()

  add_openr_test(NetlinkTypesTest netlink_types_test
    SOURCES
      openr/nl/tests/NetlinkTypesTest.cpp
    DESTINATION sbin/tests/openr/nl
  )
  if(ADD_ROOT_TESTS)
  # these tests must be run by root user
  add_openr_test(NetlinkProtocolSocketTest netlink_message_test
    SOURCES
      openr/nl/tests/NetlinkProtocolSocketTest.cpp
    DESTINATION sbin/tests/openr/nl
  )
endif()
endif()
