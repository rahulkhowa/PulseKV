cmake_minimum_required(VERSION 3.20)

project(PulseKV
    VERSION 0.1.0
    DESCRIPTION "Concurrent In-Memory Key-Value Store"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# C++20 required
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Default to Release build if not specified
# ---------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wno-unused-parameter
    )
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O2)
    else()
        add_compile_options(-g -O0)
    endif()
    # Static linking: avoids distributing MinGW runtime DLLs
    add_link_options(-static)
endif()

# ---------------------------------------------------------------------------
# Windows: Winsock (needed from Phase 3 onwards)
# ---------------------------------------------------------------------------
if(WIN32)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
endif()

# ---------------------------------------------------------------------------
# Include paths
# ---------------------------------------------------------------------------
include_directories(${CMAKE_SOURCE_DIR}/include)

# ---------------------------------------------------------------------------
# Storage library (Phase 1)
# ---------------------------------------------------------------------------
add_library(pulsekv_storage STATIC
    src/storage/store.cpp
)
target_include_directories(pulsekv_storage PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# ---------------------------------------------------------------------------
# Protocol library (Phase 2)
# ---------------------------------------------------------------------------
add_library(pulsekv_protocol STATIC
    src/protocol/parser.cpp
)
target_include_directories(pulsekv_protocol PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# ---------------------------------------------------------------------------
# Concurrency library (Phase 7)
# ---------------------------------------------------------------------------
add_library(pulsekv_concurrency STATIC
    src/concurrency/thread_pool.cpp
)
target_include_directories(pulsekv_concurrency PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# ---------------------------------------------------------------------------
# Persistence library (Phase 12)
# ---------------------------------------------------------------------------
add_library(pulsekv_persistence STATIC
    src/persistence/wal.cpp
)
target_link_libraries(pulsekv_persistence PUBLIC pulsekv_storage)
target_include_directories(pulsekv_persistence PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# ---------------------------------------------------------------------------
# Server library (Phase 3 & 7)
# ---------------------------------------------------------------------------
add_library(pulsekv_server STATIC
    src/server/server.cpp
)
target_link_libraries(pulsekv_server PUBLIC
    pulsekv_storage
    pulsekv_protocol
    pulsekv_concurrency
    pulsekv_persistence
    ws2_32
)
target_include_directories(pulsekv_server PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# ---------------------------------------------------------------------------
# Server executable (Phase 3)
# ---------------------------------------------------------------------------
add_executable(pulsekv-server
    src/server/main.cpp
)
target_link_libraries(pulsekv-server PRIVATE
    pulsekv_server
)

# ---------------------------------------------------------------------------
# Client executable (Phase 4)
# ---------------------------------------------------------------------------
add_executable(pulsekv-client
    client/main.cpp
)
target_link_libraries(pulsekv-client PRIVATE
    ws2_32
)

# ---------------------------------------------------------------------------
# Tests (Phase 1, 2, 3, 4 & 7)
# ---------------------------------------------------------------------------
enable_testing()

add_executable(test_store
    tests/test_store.cpp
)
target_link_libraries(test_store PRIVATE pulsekv_storage)
add_test(NAME StoreTests COMMAND test_store)

add_executable(test_parser
    tests/test_parser.cpp
)
target_link_libraries(test_parser PRIVATE pulsekv_protocol)
add_test(NAME ParserTests COMMAND test_parser)

add_executable(test_thread_pool
    tests/test_thread_pool.cpp
)
target_link_libraries(test_thread_pool PRIVATE pulsekv_concurrency)
add_test(NAME ThreadPoolTests COMMAND test_thread_pool)

add_executable(test_server
    tests/test_server.cpp
)
target_link_libraries(test_server PRIVATE
    pulsekv_server
    ws2_32
)
add_test(NAME ServerTests COMMAND test_server)

add_executable(test_wal
    tests/test_wal.cpp
)
target_link_libraries(test_wal PRIVATE
    pulsekv_persistence
    pulsekv_storage
)
add_test(NAME WALTests COMMAND test_wal)

add_executable(test_recovery
    tests/test_recovery.cpp
)
target_link_libraries(test_recovery PRIVATE
    pulsekv_persistence
    pulsekv_storage
)
add_test(NAME RecoveryTests COMMAND test_recovery)

# ---------------------------------------------------------------------------
# Benchmark executable (Phase 5, 9, 14)
# ---------------------------------------------------------------------------
add_executable(pulsekv-benchmark
    benchmark/main.cpp
)
target_link_libraries(pulsekv-benchmark PRIVATE
    ws2_32
    psapi
)

# ---------------------------------------------------------------------------
# Future targets (commented out until their phases are implemented)
# ---------------------------------------------------------------------------
