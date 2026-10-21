cmake_minimum_required(VERSION 3.24)

project(WorkersTrackingNative VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(cmake/Warnings.cmake)

add_library(worker_common)
target_sources(worker_common
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS src
        FILES
            src/common/protocol.h
            src/common/stream_decoder.h
    PRIVATE
        src/common/protocol.cpp
        src/common/stream_decoder.cpp
)
target_include_directories(worker_common PUBLIC src)
enable_strict_warnings(worker_common)

add_executable(WorkerAgent WIN32 src/worker_agent/main.cpp)
target_link_libraries(WorkerAgent PRIVATE worker_common)
enable_strict_warnings(WorkerAgent)

add_executable(Observer WIN32 src/observer/main.cpp)
target_link_libraries(Observer PRIVATE worker_common)
enable_strict_warnings(Observer)

include(CTest)
if(BUILD_TESTING)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(common_tests
        tests/protocol_tests.cpp
        tests/stream_decoder_tests.cpp
    )
    target_link_libraries(common_tests PRIVATE worker_common GTest::gtest_main)
    enable_strict_warnings(common_tests)

    include(GoogleTest)
    gtest_discover_tests(common_tests)
endif()
