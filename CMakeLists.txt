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
            src/common/winsock_socket.h
    PRIVATE
        src/common/protocol.cpp
        src/common/stream_decoder.cpp
        src/common/winsock_socket.cpp
)
target_include_directories(worker_common PUBLIC src)
target_link_libraries(worker_common PUBLIC ws2_32)
enable_strict_warnings(worker_common)

add_library(worker_capture)
target_sources(worker_capture
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS src
        FILES
            src/worker_agent/capture_pixel_copy.h
            src/worker_agent/capture_coordinator.h
            src/worker_agent/capture_profiles.h
            src/worker_agent/change_detector.h
            src/worker_agent/dxgi_screen_capture.h
            src/worker_agent/frame.h
            src/worker_agent/gdi_screen_capture.h
            src/worker_agent/image_processor.h
            src/worker_agent/latest_frame_queue.h
            src/worker_agent/screen_capture.h
            src/worker_agent/windows_capture_factory.h
    PRIVATE
        src/worker_agent/capture_pixel_copy.cpp
        src/worker_agent/capture_coordinator.cpp
        src/worker_agent/capture_profiles.cpp
        src/worker_agent/change_detector.cpp
        src/worker_agent/dxgi_screen_capture.cpp
        src/worker_agent/frame.cpp
        src/worker_agent/gdi_screen_capture.cpp
        src/worker_agent/image_processor.cpp
        src/worker_agent/windows_capture_factory.cpp
)
target_include_directories(worker_capture PUBLIC src)
target_link_libraries(worker_capture PRIVATE d3d11 dxgi windowscodecs gdi32 ole32)
enable_strict_warnings(worker_capture)

add_library(worker_transport)
target_sources(worker_transport
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS src
        FILES
            src/worker_agent/adaptive_rate_controller.h
            src/worker_agent/agent_orchestrator.h
            src/worker_agent/agent_session.h
            src/worker_agent/transport_schedulers.h
    PRIVATE
        src/worker_agent/adaptive_rate_controller.cpp
        src/worker_agent/agent_orchestrator.cpp
        src/worker_agent/agent_session.cpp
        src/worker_agent/transport_schedulers.cpp
)
target_include_directories(worker_transport PUBLIC src)
target_link_libraries(worker_transport PUBLIC worker_common worker_capture)
enable_strict_warnings(worker_transport)

add_executable(WorkerAgent WIN32 src/worker_agent/main.cpp)
target_link_libraries(WorkerAgent PRIVATE worker_common worker_capture worker_transport)
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
        tests/agent_control_tests.cpp
        tests/capture_coordinator_tests.cpp
        tests/capture_core_tests.cpp
        tests/image_processor_tests.cpp
        tests/protocol_tests.cpp
        tests/stream_decoder_tests.cpp
        tests/winsock_integration_tests.cpp
        tests/windows_capture_tests.cpp
    )
    target_link_libraries(common_tests PRIVATE worker_common worker_capture worker_transport GTest::gtest_main)
    enable_strict_warnings(common_tests)

    include(GoogleTest)
    gtest_discover_tests(common_tests DISCOVERY_TIMEOUT 30)
endif()
