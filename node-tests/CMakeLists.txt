cmake_minimum_required(VERSION 3.14)
project(MembershipAllocationClientModuleTests LANGUAGES CXX)

include(LogosTest)

find_library(LIBP2P_PATH
    NAMES liblibp2p.so liblibp2p.dylib
    PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../lib
    NO_DEFAULT_PATH)

if(NOT LIBP2P_PATH)
    message(FATAL_ERROR "the libp2p library is not staged in ../lib")
endif()

if(NOT DEFINED LIBP2P_MODULE_SRC)
    message(FATAL_ERROR "LIBP2P_MODULE_SRC must name the libp2p_module source the peers in these tests are built from")
endif()

logos_test(
    NAME membership_allocation_client_module_tests
    MODULE_SOURCES
        ../src/own_node.cpp
        ${LIBP2P_MODULE_SRC}/src/utils.cpp
        ${LIBP2P_MODULE_SRC}/src/topic_queues.cpp
        ${LIBP2P_MODULE_SRC}/src/stream_queues.cpp
        ${LIBP2P_MODULE_SRC}/src/plugin.cpp
        ${LIBP2P_MODULE_SRC}/src/callbacks.cpp
        ${LIBP2P_MODULE_SRC}/src/kademlia.cpp
        ${LIBP2P_MODULE_SRC}/src/stream.cpp
        ${LIBP2P_MODULE_SRC}/src/protocol_bridge.cpp
        ${LIBP2P_MODULE_SRC}/src/gossipsub.cpp
        ${LIBP2P_MODULE_SRC}/src/service_discovery.cpp
        ${LIBP2P_MODULE_SRC}/src/custom_handlers.cpp
        ${LIBP2P_MODULE_SRC}/src/peerstore.cpp
        ${LIBP2P_MODULE_SRC}/src/metrics.cpp
    TEST_SOURCES
        main.cpp
        own_node.cpp
    EXTRA_INCLUDES
        ../lib
        ../src
        ${LIBP2P_MODULE_SRC}/src
    EXTRA_LINK_LIBS
        ${LIBP2P_PATH}
        tinycbor
)

get_filename_component(LIBP2P_DIR "${LIBP2P_PATH}" DIRECTORY)
set_target_properties(membership_allocation_client_module_tests PROPERTIES
    BUILD_RPATH "${LIBP2P_DIR}"
)
