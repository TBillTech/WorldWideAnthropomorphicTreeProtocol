cmake_minimum_required(VERSION 3.10)

# Project name
project(WWATP)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Enable verbose output
set(CMAKE_VERBOSE_MAKEFILE ON)

# Find Boost library
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()
find_package(Boost REQUIRED COMPONENTS system)

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
    ${PROJECT_SOURCE_DIR}/common/frontend/http3_server_cpp
    ${PROJECT_SOURCE_DIR}/libraries/boringssl/include
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
    ${PROJECT_SOURCE_DIR}/common/backend_cpp/http3_client/http3_client_backend.cpp
    ${PROJECT_SOURCE_DIR}/common/frontend/http3_server_cpp/http3_server.cpp
)

# Add test executables
add_executable(library_tester ${COMMON_SOURCES} test_instances/library_tester/library_tester.cpp)
add_executable(shared_chunk_tester ${COMMON_SOURCES} test_instances/shared_chunk_tester/shared_chunk_tester.cpp)
add_executable(memory_pool_tester ${COMMON_SOURCES} test_instances/memory_pool_tester/memory_pool_tester.cpp)
add_executable(backend_cpp_tester ${COMMON_SOURCES} test_instances/backend_cpp_tester/backend_cpp_tester.cpp)

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
#     BORINGSSL_CFLAGS="-I$PWD/../boringssl/include" \
#     --with-boringssl
# make
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

# Ensure library_tester depends on quiche
add_dependencies(library_tester ngtcp2 nghttp3 boringssl)
add_dependencies(shared_chunk_tester ngtcp2 nghttp3 boringssl)
add_dependencies(memory_pool_tester ngtcp2 nghttp3 boringssl)
add_dependencies(backend_cpp_tester ngtcp2 nghttp3 boringssl)

# Link libraries (if any)
target_link_libraries(library_tester ngtcp2_lib nghttp3_lib boringssl_lib boringcrypto_lib ngtcp2_crypto_lib ${Boost_LIBRARIES} pthread dl m ev FunctionalPlus::fplus)
target_link_libraries(shared_chunk_tester ngtcp2_lib nghttp3_lib boringssl_lib boringcrypto_lib ngtcp2_crypto_lib ${Boost_LIBRARIES} pthread dl m ev FunctionalPlus::fplus)
target_link_libraries(memory_pool_tester ngtcp2_lib nghttp3_lib boringssl_lib boringcrypto_lib ngtcp2_crypto_lib ${Boost_LIBRARIES} pthread dl m ev FunctionalPlus::fplus)
target_link_libraries(backend_cpp_tester ngtcp2_lib nghttp3_lib boringssl_lib boringcrypto_lib ngtcp2_crypto_lib ${Boost_LIBRARIES} pthread dl m ev FunctionalPlus::fplus)

# Enable verbose linker output
# set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--verbose")

# Enable testing
enable_testing()

# Add tests
add_test(NAME TreeBusTest COMMAND library_tester)