cmake_minimum_required(VERSION 3.10)

# Project name
project(WWATP)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Enable verbose output
set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_BUILD_TYPE Debug)

# Find Boost library
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()
find_package(Boost REQUIRED COMPONENTS system)
find_package(yaml-cpp REQUIRED)

# Checks for header files.
include(CheckIncludeFile)
check_include_file("arpa/inet.h"   HAVE_ARPA_INET_H)
check_include_file("netinet/in.h"  HAVE_NETINET_IN_H)
check_include_file("netinet/ip.h"  HAVE_NETINET_IP_H)
check_include_file("unistd.h"      HAVE_UNISTD_H)
check_include_file("sys/endian.h"  HAVE_SYS_ENDIAN_H)
check_include_file("endian.h"      HAVE_ENDIAN_H)
check_include_file("byteswap.h"    HAVE_BYTESWAP_H)
check_include_file("asm/types.h"   HAVE_ASM_TYPES_H)
check_include_file("linux/netlink.h"   HAVE_LINUX_NETLINK_H)
check_include_file("linux/rtnetlink.h" HAVE_LINUX_RTNETLINK_H)

# Print the results of the checks
message(STATUS "HAVE_ARPA_INET_H: ${HAVE_ARPA_INET_H}")
message(STATUS "HAVE_NETINET_IN_H: ${HAVE_NETINET_IN_H}")
message(STATUS "HAVE_NETINET_IP_H: ${HAVE_NETINET_IP_H}")
message(STATUS "HAVE_UNISTD_H: ${HAVE_UNISTD_H}")
message(STATUS "HAVE_SYS_ENDIAN_H: ${HAVE_SYS_ENDIAN_H}")
message(STATUS "HAVE_ENDIAN_H: ${HAVE_ENDIAN_H}")
message(STATUS "HAVE_BYTESWAP_H: ${HAVE_BYTESWAP_H}")
message(STATUS "HAVE_ASM_TYPES_H: ${HAVE_ASM_TYPES_H}")
message(STATUS "HAVE_LINUX_NETLINK_H: ${HAVE_LINUX_NETLINK_H}")
message(STATUS "HAVE_LINUX_RTNETLINK_H: ${HAVE_LINUX_RTNETLINK_H}")

# pthread_rwlock_t on Linux requires a feature flag. We limit this to Linux
# because, on Apple platforms, it instead disables APIs we use. See compat(5)
# and sys/cdefs.h. Reportedly, FreeBSD also breaks when this is set. See
# https://crbug.com/boringssl/471.
#if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
#  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_XOPEN_SOURCE=700")
#endif()

# cd libraries
# git clone https://github.com/Dobiasd/FunctionalPlus
# cmake -S FunctionalPlus -B FunctionalPlus/build
# cmake --build FunctionalPlus/build
# sudo cmake --install FunctionalPlus/build
find_package(FunctionalPlus REQUIRED)

# Include directories
include_directories(
    ${Boost_INCLUDE_DIRS}
    ${PROJECT_SOURCE_DIR}/common/backend_cpp/interface
    ${PROJECT_SOURCE_DIR}/common/backend_cpp/memory/include
    ${PROJECT_SOURCE_DIR}/common/transport/include 
    ${PROJECT_SOURCE_DIR}/common/backend_cpp/structure
    ${PROJECT_SOURCE_DIR}/common/backend_cpp/http3_client
    ${PROJECT_SOURCE_DIR}/common/backend_cpp/file
    ${PROJECT_SOURCE_DIR}/common/frontend/http3_server_cpp
    ${PROJECT_SOURCE_DIR}/common/frontend/backend_tester_cpp
    ${PROJECT_SOURCE_DIR}/common/frontend/mediator_cpp
    ${PROJECT_SOURCE_DIR}/common/frontend/wwatp_service_cpp
    ${PROJECT_SOURCE_DIR}/common/frontend
    ${PROJECT_SOURCE_DIR}/libraries/boringssl/include
    ${PROJECT_SOURCE_DIR}/test_instances/catch2_unit_tests
)

# Add common source files
set(COMMON_SOURCES
    ${PROJECT_SOURCE_DIR}/common/transport/src/memory_pool.cpp
    ${PROJECT_SOURCE_DIR}/common/transport/src/shared_chunk.cpp
    ${PROJECT_SOURCE_DIR}/common/transport/src/tcp_communication.cpp
    ${PROJECT_SOURCE_DIR}/common/transport/src/quic_listener.cpp
    ${PROJECT_SOURCE_DIR}/common/transport/src/quic_connector.cpp
    ${PROJECT_SOURCE_DIR}/common/transport/src/server_base.cc
    ${PROJECT_SOURCE_DIR}/common/transport/src/client_base.cc
    ${PROJECT_SOURCE_DIR}/common/transport/src/config_base.cc
    ${PROJECT_SOURCE_DIR}/common/transport/src/tls_server_session_boringssl.cc
    ${PROJECT_SOURCE_DIR}/common/transport/src/tls_server_context_boringssl.cc
    ${PROJECT_SOURCE_DIR}/common/transport/src/tls_client_session_boringssl.cc
    ${PROJECT_SOURCE_DIR}/common/transport/src/tls_client_context_boringssl.cc
    ${PROJECT_SOURCE_DIR}/common/transport/src/util.cc
    ${PROJECT_SOURCE_DIR}/common/transport/src/http.cc
    ${PROJECT_SOURCE_DIR}/common/transport/src/util_openssl.cc
    ${PROJECT_SOURCE_DIR}/common/transport/src/tls_session_base_quictls.cc
    ${PROJECT_SOURCE_DIR}/common/transport/src/tls_shared_boringssl.cc
    ${PROJECT_SOURCE_DIR}/common/transport/src/urlparse.c
    ${PROJECT_SOURCE_DIR}/common/transport/src/debug.cc
    ${PROJECT_SOURCE_DIR}/common/transport/src/shared.cc
    ${PROJECT_SOURCE_DIR}/common/transport/src/siphash.cc
    ${PROJECT_SOURCE_DIR}/common/backend_cpp/interface/tree_node.cpp
    ${PROJECT_SOURCE_DIR}/common/backend_cpp/memory/src/memory_tree.cpp
    ${PROJECT_SOURCE_DIR}/common/backend_cpp/memory/src/simple_backend.cpp
    ${PROJECT_SOURCE_DIR}/common/backend_cpp/memory/src/transactional_backend.cpp
    ${PROJECT_SOURCE_DIR}/common/backend_cpp/memory/src/threadsafe_backend.cpp
    ${PROJECT_SOURCE_DIR}/common/backend_cpp/memory/src/transactional_backend.cpp
    ${PROJECT_SOURCE_DIR}/common/backend_cpp/structure/composite_backend.cpp
    ${PROJECT_SOURCE_DIR}/common/backend_cpp/structure/redirected_backend.cpp
    ${PROJECT_SOURCE_DIR}/common/backend_cpp/http3_client/http3_client_backend.cpp
    ${PROJECT_SOURCE_DIR}/common/backend_cpp/interface/http3_tree_message.cpp
    ${PROJECT_SOURCE_DIR}/common/backend_cpp/interface/http3_tree_message_helpers.cpp
    ${PROJECT_SOURCE_DIR}/common/backend_cpp/file/file_backend.cpp
    ${PROJECT_SOURCE_DIR}/common/frontend/http3_server_cpp/http3_server.cpp
    ${PROJECT_SOURCE_DIR}/common/frontend/mediator_cpp/cloning_mediator.cpp
    ${PROJECT_SOURCE_DIR}/common/frontend/mediator_cpp/yaml_mediator.cpp
    ${PROJECT_SOURCE_DIR}/common/frontend/wwatp_service_cpp/wwatp_service.cpp
)

# Add server executable
add_executable(wwatp_server ${COMMON_SOURCES} server/main.cpp)

# Add test executables
add_executable(library_tester ${COMMON_SOURCES} test_instances/library_tester/library_tester.cpp)
add_executable(memory_pool_tester ${COMMON_SOURCES} test_instances/memory_pool_tester/memory_pool_tester.cpp)
add_executable(http3_client_backend_tester
    ${COMMON_SOURCES}
    test_instances/http3_client_backend_tester/http3_client_backend_tester.cpp
    test_instances/catch2_unit_tests/backend_testbed.cpp
)
add_executable(catch2_unit_tests
    ${COMMON_SOURCES}
    test_instances/catch2_unit_tests/http3_message_tests.cpp
    test_instances/catch2_unit_tests/shared_chunk_tests.cpp
    test_instances/catch2_unit_tests/backend_cpp_tests.cpp
    test_instances/catch2_unit_tests/backend_testbed.cpp
    test_instances/catch2_unit_tests/http3_message_helper_tests.cpp
    test_instances/catch2_unit_tests/frontend_cpp_tests.cpp
    test_instances/catch2_unit_tests/yaml_tests.cpp
    test_instances/catch2_unit_tests/tree_node_tests.cpp
)

# Define preprocessor macros
#add_definitions(-DENABLE_EXAMPLE_BORINGSSL -DWITH_EXAMPLE_BORINGSSL -DHAVE_ARPA_INET_H)

# Include ExternalProject module
include(ExternalProject)

# Clone and build BoringSSL
# git clone https://boringssl.googlesource.com/boringssl
# cd boringssl
# git checkout 294ab9730c570213b496cfc2fc14b3c0bfcd4bcc
# cmake -B build -DCMAKE_POSITION_INDEPENDENT_CODE=ON
# make -j$(nproc) -C build
ExternalProject_Add(
    boringssl
    PREFIX ${PROJECT_SOURCE_DIR}/libraries/boringssl
    SOURCE_DIR ${PROJECT_SOURCE_DIR}/libraries/boringssl
    BINARY_DIR ${PROJECT_SOURCE_DIR}/libraries/boringssl
    CONFIGURE_COMMAND cmake -B build -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    BUILD_COMMAND make -j$(nproc) -C build
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${PROJECT_SOURCE_DIR}/libraries/boringssl/build/libssl.a ${PROJECT_SOURCE_DIR}/libraries/boringssl/build/libcrypto.a
)
# Create an imported target for the BoringSSL library
add_library(boringssl_lib STATIC IMPORTED)
set_target_properties(boringssl_lib PROPERTIES
    IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/libraries/boringssl/build/libssl.a
)
add_library(boringcrypto_lib STATIC IMPORTED)
set_target_properties(boringcrypto_lib PROPERTIES
    IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/libraries/boringssl/build/libcrypto.a
)


# nghttp3 library
# cd libraries
# git clone https://github.com/ngtcp2/nghttp3
# cd nghttp3
# git submodule update --init
# autoreconf -i
# ./configure
# make
# sudo make install
ExternalProject_Add(
    nghttp3
    PREFIX ${PROJECT_SOURCE_DIR}/libraries/nghttp3
    SOURCE_DIR ${PROJECT_SOURCE_DIR}/libraries/nghttp3
    BINARY_DIR ${PROJECT_SOURCE_DIR}/libraries/nghttp3
    CONFIGURE_COMMAND ""
    BUILD_COMMAND make
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${PROJECT_SOURCE_DIR}/libraries/nghttp3/lib/.libs/libnghttp3.a
)
# Create an imported target for the nghttp3 library
add_library(nghttp3_lib STATIC IMPORTED)
set_target_properties(nghttp3_lib PROPERTIES
    IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/libraries/nghttp3/lib/.libs/libnghttp3.a
)

# Clone and build ngtcp2
# git clone --recursive  https://github.com/ngtcp2/ngtcp2
# cd ngtcp2
# autoreconf -i
# ./configure PKG_CONFIG_PATH=$PWD/../nghttp3/lib \
#     BORINGSSL_LIBS="-L$PWD/../boringssl/build/ssl -lssl -L$PWD/../boringssl/build/crypto -lcrypto" \
#     BORINGSSL_CFLAGS="-I$PWD/../boringssl/include"\
#     --with-boringssl
# make
# And for the shared library libngtcp2_crypto_boringssl.so:
# from repo root (or cd libraries/ngtcp2/crypto/boringssl first)
# mkdir -p libraries/ngtcp2/crypto/boringssl/build
# cd libraries/ngtcp2/crypto/boringssl/build

# cmake .. \
#   -DENABLE_SHARED_LIB=ON \
#   -DENABLE_STATIC_LIB=OFF \
#   -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
#   -DBORINGSSL_INCLUDE_DIRS=/mnt/d/code/wwatp/libraries/boringssl/include \
#   -DBORINGSSL_LIBRARIES="/mnt/d/code/wwatp/libraries/boringssl/build/libssl.a;/mnt/d/code/wwatp/libraries/boringssl/build/libcrypto.a"

# # build the shared target (name is ngtcp2_crypto_boringssl)
# make -j ngtcp2_crypto_boringssl

# # optional: see the output
# ls -l libngtcp2_crypto_boringssl.so* || ls -l .
ExternalProject_Add(
    ngtcp2
    PREFIX ${PROJECT_SOURCE_DIR}/libraries/ngtcp2
    SOURCE_DIR ${PROJECT_SOURCE_DIR}/libraries/ngtcp2
    BINARY_DIR ${PROJECT_SOURCE_DIR}/libraries/ngtcp2
    CONFIGURE_COMMAND ""
    BUILD_COMMAND make
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${PROJECT_SOURCE_DIR}/libraries/ngtcp2/lib/.libs/libngtcp2.a
)
# Create an imported target for the ngtcp2 library
add_library(ngtcp2_lib STATIC IMPORTED)
set_target_properties(ngtcp2_lib PROPERTIES
    IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/libraries/ngtcp2/lib/.libs/libngtcp2.a
)
# Create an imported target for the ngtcp2 library
add_library(ngtcp2_crypto_lib STATIC IMPORTED)
set_target_properties(ngtcp2_crypto_lib PROPERTIES
    IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/libraries/ngtcp2/crypto/boringssl/libngtcp2_crypto_boringssl.a
)

# Shared variant of ngtcp2 crypto glue (built in subdir with CMake)
add_library(ngtcp2_crypto_boringssl_so SHARED IMPORTED)
set_target_properties(ngtcp2_crypto_boringssl_so PROPERTIES
    IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/libraries/ngtcp2/crypto/boringssl/.build/libngtcp2_crypto_boringssl.so
)

include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.5.4 # or latest stable
)
FetchContent_MakeAvailable(Catch2)

# Make Catch2's CTest integration module available
list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)

# Ensure executables depend on external libraries
add_dependencies(wwatp_server ngtcp2 nghttp3 boringssl)
add_dependencies(library_tester ngtcp2 nghttp3 boringssl)
add_dependencies(memory_pool_tester ngtcp2 nghttp3 boringssl)
add_dependencies(http3_client_backend_tester ngtcp2 nghttp3 boringssl Catch2)
add_dependencies(catch2_unit_tests ngtcp2 nghttp3 boringssl Catch2)

# Link libraries (if any)
target_link_libraries(wwatp_server ngtcp2_lib nghttp3_lib boringssl_lib boringcrypto_lib ngtcp2_crypto_lib ${Boost_LIBRARIES} pthread dl m ev FunctionalPlus::fplus yaml-cpp)
target_link_libraries(library_tester ngtcp2_lib nghttp3_lib boringssl_lib boringcrypto_lib ngtcp2_crypto_lib ${Boost_LIBRARIES} pthread dl m ev FunctionalPlus::fplus yaml-cpp)
target_link_libraries(memory_pool_tester ngtcp2_lib nghttp3_lib boringssl_lib boringcrypto_lib ngtcp2_crypto_lib ${Boost_LIBRARIES} pthread dl m ev FunctionalPlus::fplus yaml-cpp)
target_link_libraries(http3_client_backend_tester PRIVATE ngtcp2_lib nghttp3_lib boringssl_lib boringcrypto_lib ngtcp2_crypto_lib ${Boost_LIBRARIES} pthread dl m ev FunctionalPlus::fplus Catch2::Catch2 yaml-cpp)
target_link_libraries(catch2_unit_tests PRIVATE ngtcp2_lib nghttp3_lib boringssl_lib boringcrypto_lib ngtcp2_crypto_lib ${Boost_LIBRARIES} pthread dl m ev FunctionalPlus::fplus Catch2::Catch2WithMain yaml-cpp)

target_compile_options(http3_client_backend_tester PRIVATE -g -O0 -fsanitize=address,undefined -Wall -Wextra -Wpedantic)
target_link_options(http3_client_backend_tester PRIVATE -fsanitize=address,undefined)
# Enable verbose linker output
# set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--verbose")

# Enable testing and have CTest discover Catch2 test cases from our executable
enable_testing()
catch_discover_tests(catch2_unit_tests)

# Optional: Build C-compatible QUIC client facade for Node bindings (FFI/N-API)
option(BUILD_WWATP_QUIC_C "Build C facade shared lib for QuicConnector" ON)
if(BUILD_WWATP_QUIC_C)
    # Build C facade shared library, linking to transport/client sources
    add_library(wwatp_quic_c SHARED ${COMMON_SOURCES}
        ${PROJECT_SOURCE_DIR}/common/transport/src/quic_connector_c.cc
    )
    target_include_directories(wwatp_quic_c PUBLIC ${PROJECT_SOURCE_DIR}/common/transport/include ${Boost_INCLUDE_DIRS})
    add_dependencies(wwatp_quic_c ngtcp2 nghttp3 boringssl)
    # Prefer shared objects for ngtcp2/nghttp3 to satisfy -fPIC in shared lib
    set(NGTCP2_SO ${PROJECT_SOURCE_DIR}/libraries/ngtcp2/lib/.libs/libngtcp2.so)
    set(NGHTTP3_SO ${PROJECT_SOURCE_DIR}/libraries/nghttp3/lib/.libs/libnghttp3.so)
    target_link_libraries(wwatp_quic_c PRIVATE ${NGTCP2_SO} ${NGHTTP3_SO} ngtcp2_crypto_boringssl_so boringssl_lib boringcrypto_lib ${Boost_LIBRARIES} pthread dl m ev FunctionalPlus::fplus yaml-cpp)
    # Ensure PIC for any objects we compile into this shared library
    set_target_properties(wwatp_quic_c PROPERTIES POSITION_INDEPENDENT_CODE ON)
    # Help the runtime linker find dependent .so during development
    set_target_properties(wwatp_quic_c PROPERTIES BUILD_RPATH "${PROJECT_SOURCE_DIR}/libraries/ngtcp2/lib/.libs;${PROJECT_SOURCE_DIR}/libraries/nghttp3/lib/.libs;${PROJECT_SOURCE_DIR}/libraries/ngtcp2/crypto/boringssl/.build")
    set_target_properties(wwatp_quic_c PROPERTIES OUTPUT_NAME "wwatp_quic")
endif()