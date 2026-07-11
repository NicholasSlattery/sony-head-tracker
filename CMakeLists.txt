cmake_minimum_required(VERSION 3.25)
project(sony_head_tracker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(sony_head_tracker_core STATIC
    src/math.cpp
    src/orientation.cpp
    src/protocol.cpp
    src/hid_descriptor.cpp
    src/app_config.cpp
    src/diagnostics.cpp
)
target_include_directories(sony_head_tracker_core PUBLIC include)
target_compile_options(sony_head_tracker_core PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang>:-Wall;-Wextra;-Wpedantic>
)

enable_testing()
add_executable(sony_head_tracker_tests
    tests/test_main.cpp
    tests/math_tests.cpp
    tests/macos_support_tests.cpp
    tests/orientation_tests.cpp
    tests/protocol_tests.cpp
    tests/descriptor_tests.cpp
    tests/config_tests.cpp
    tests/diagnostics_tests.cpp
)
target_link_libraries(sony_head_tracker_tests PRIVATE sony_head_tracker_core)
add_test(NAME core COMMAND sony_head_tracker_tests)

if(APPLE)
    enable_language(OBJCXX)
    add_library(sony_head_tracker_macos_platform STATIC
        src/macos/app_config_store_macos.cpp
        src/macos/audio_wake_macos.cpp
        src/macos/bluetooth_recovery_macos.mm
        src/macos/hid_backend_macos.cpp
        src/macos/logger_macos.cpp
        src/macos/output_udp_posix.cpp
    )
    target_link_libraries(sony_head_tracker_macos_platform PUBLIC
        sony_head_tracker_core
        "-framework AudioToolbox"
        "-framework AudioUnit"
        "-framework CoreAudio"
        "-framework CoreFoundation"
        "-framework Foundation"
        "-framework IOBluetooth"
        "-framework IOKit"
    )
    target_compile_options(sony_head_tracker_macos_platform PRIVATE
        -Wall -Wextra -Wpedantic
    )

    add_library(sony_head_tracker_macos_bridge STATIC
        macos/Bridge/sony_head_tracker_c.mm
    )
    target_include_directories(sony_head_tracker_macos_bridge PUBLIC macos/Bridge)
    target_link_libraries(sony_head_tracker_macos_bridge PUBLIC
        sony_head_tracker_macos_platform
    )
    target_compile_options(sony_head_tracker_macos_bridge PRIVATE
        -Wall -Wextra -Wpedantic
    )

    add_executable(sony-head-tracker-macos
        src/macos/main_macos.cpp
    )
    target_link_libraries(sony-head-tracker-macos PRIVATE
        sony_head_tracker_macos_platform
    )
    target_compile_options(sony-head-tracker-macos PRIVATE
        -Wall -Wextra -Wpedantic
    )

    add_executable(sony_head_tracker_c_api_tests
        tests/test_main.cpp
        tests/c_api_tests.cpp
    )
    target_link_libraries(sony_head_tracker_c_api_tests PRIVATE
        sony_head_tracker_macos_bridge
    )
    add_test(NAME c-api COMMAND sony_head_tracker_c_api_tests)
endif()
