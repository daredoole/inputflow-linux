cmake_minimum_required(VERSION 3.10)
project(mwb_client CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MWB_ENABLE_SANITIZERS "Build with compiler sanitizers for debug hardening" OFF)
set(MWB_SANITIZERS "address,undefined" CACHE STRING "Comma-separated sanitizer list used when MWB_ENABLE_SANITIZERS=ON")

if (MWB_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=${MWB_SANITIZERS}")
    check_cxx_source_compiles("int main() { return 0; }" MWB_SANITIZER_RUNTIME_AVAILABLE)
    unset(CMAKE_REQUIRED_FLAGS)
    if (NOT MWB_SANITIZER_RUNTIME_AVAILABLE)
        message(FATAL_ERROR "MWB_ENABLE_SANITIZERS=ON, but the requested sanitizer runtime is not linkable by this compiler")
    endif()
endif()

function(mwb_apply_sanitizers target)
    if (MWB_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -O1
            -g
            -fno-omit-frame-pointer
            -fsanitize=${MWB_SANITIZERS}
        )
        set_property(TARGET ${target} APPEND_STRING PROPERTY LINK_FLAGS " -fsanitize=${MWB_SANITIZERS}")
    endif()
endfunction()

# Find OpenSSL for AES decryption/encryption matching PowerToys MWB
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

add_executable(mwb_client
    src/AppConfig.cpp
    src/AppState.cpp
    src/ClientRuntime.cpp
    src/Discovery.cpp
    src/InputDispatcher.cpp
    src/main.cpp
    src/PeerRecovery.cpp
    src/SecretStore.cpp
    src/ClipboardManager.cpp
    src/CryptoHelper.cpp
    src/InputManager.cpp
    src/MediaKeyBridge.cpp
    src/NetworkManager.cpp
)

target_include_directories(mwb_client PRIVATE src)
target_include_directories(mwb_client PRIVATE ${OPENSSL_INCLUDE_DIR})

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mwb_client PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wformat=2
        -Werror=format-security
        -fstack-protector-strong
    )
    target_compile_definitions(mwb_client PRIVATE
        $<$<AND:$<NOT:$<CONFIG:Debug>>,$<NOT:$<BOOL:${MWB_ENABLE_SANITIZERS}>>>:_FORTIFY_SOURCE=3>
        _GLIBCXX_ASSERTIONS
    )
    set_property(TARGET mwb_client APPEND_STRING PROPERTY LINK_FLAGS " -Wl,-z,relro,-z,now")
endif()
mwb_apply_sanitizers(mwb_client)

target_link_libraries(mwb_client PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    pthread
)

include(CTest)

find_package(PkgConfig QUIET)

if (BUILD_TESTING)
    add_executable(mwb_client_unit_tests
        tests/test_main.cpp
        src/AppConfig.cpp
        src/AppState.cpp
        src/Discovery.cpp
        src/PeerRecovery.cpp
    )
    target_include_directories(mwb_client_unit_tests PRIVATE src)
    target_compile_options(mwb_client_unit_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(mwb_client_unit_tests PRIVATE pthread)
    mwb_apply_sanitizers(mwb_client_unit_tests)

    add_executable(mwb_input_mapping_tests
        tests/test_input_mapping.cpp
    )
    target_include_directories(mwb_input_mapping_tests PRIVATE src)
    target_compile_options(mwb_input_mapping_tests PRIVATE -Wall -Wextra -Wpedantic)
    mwb_apply_sanitizers(mwb_input_mapping_tests)

    add_executable(mwb_inject_mouse_abs_tests
        tests/test_inject_mouse_abs.cpp
    )
    target_include_directories(mwb_inject_mouse_abs_tests PRIVATE src)
    target_compile_options(mwb_inject_mouse_abs_tests PRIVATE -Wall -Wextra -Wpedantic)
    mwb_apply_sanitizers(mwb_inject_mouse_abs_tests)

    add_executable(mwb_input_device_capability_tests
        tests/test_input_device_capabilities.cpp
    )
    target_include_directories(mwb_input_device_capability_tests PRIVATE src)
    target_compile_options(mwb_input_device_capability_tests PRIVATE -Wall -Wextra -Wpedantic)
    mwb_apply_sanitizers(mwb_input_device_capability_tests)

    add_executable(mwb_input_latency_tests
        tests/test_input_latency.cpp
    )
    target_include_directories(mwb_input_latency_tests PRIVATE src)
    target_compile_options(mwb_input_latency_tests PRIVATE -Wall -Wextra -Wpedantic)
    mwb_apply_sanitizers(mwb_input_latency_tests)

    add_executable(mwb_topology_model_tests
        tests/test_topology_model.cpp
        src/TopologyModel.cpp
    )
    target_include_directories(mwb_topology_model_tests PRIVATE src)
    target_compile_options(mwb_topology_model_tests PRIVATE -Wall -Wextra -Wpedantic)
    mwb_apply_sanitizers(mwb_topology_model_tests)

    add_executable(mwb_mouse_trace_tests
        tests/test_mouse_trace.cpp
        src/InputManager.cpp
        src/MediaKeyBridge.cpp
    )
    target_include_directories(mwb_mouse_trace_tests PRIVATE src)
    target_compile_options(mwb_mouse_trace_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(mwb_mouse_trace_tests PRIVATE pthread)
    mwb_apply_sanitizers(mwb_mouse_trace_tests)

    add_executable(mwb_media_key_bridge_tests
        tests/test_media_key_bridge.cpp
        src/MediaKeyBridge.cpp
    )
    target_include_directories(mwb_media_key_bridge_tests PRIVATE src)
    target_compile_options(mwb_media_key_bridge_tests PRIVATE -Wall -Wextra -Wpedantic)
    mwb_apply_sanitizers(mwb_media_key_bridge_tests)

    add_executable(mwb_protocol_security_tests
        tests/test_protocol_security.cpp
        src/ClipboardManager.cpp
    )
    target_include_directories(mwb_protocol_security_tests PRIVATE src)
    target_compile_options(mwb_protocol_security_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(mwb_protocol_security_tests PRIVATE
        ZLIB::ZLIB
    )
    mwb_apply_sanitizers(mwb_protocol_security_tests)

    add_executable(mwb_clipboard_socket_security_tests
        tests/test_clipboard_socket_security.cpp
        src/ClipboardManager.cpp
        src/CryptoHelper.cpp
        src/NetworkManager.cpp
    )
    target_include_directories(mwb_clipboard_socket_security_tests PRIVATE src)
    target_compile_options(mwb_clipboard_socket_security_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(mwb_clipboard_socket_security_tests PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        ZLIB::ZLIB
        pthread
    )
    mwb_apply_sanitizers(mwb_clipboard_socket_security_tests)

    add_test(NAME mwb_client_unit_tests COMMAND mwb_client_unit_tests)
    add_test(NAME mwb_input_mapping_tests COMMAND mwb_input_mapping_tests)
    add_test(NAME mwb_inject_mouse_abs_tests COMMAND mwb_inject_mouse_abs_tests)
    add_test(NAME mwb_input_device_capability_tests COMMAND mwb_input_device_capability_tests)
    add_test(NAME mwb_input_latency_tests COMMAND mwb_input_latency_tests)
    add_test(NAME mwb_topology_model_tests COMMAND mwb_topology_model_tests)
    add_test(NAME mwb_mouse_trace_tests COMMAND mwb_mouse_trace_tests)
    add_test(NAME mwb_media_key_bridge_tests COMMAND mwb_media_key_bridge_tests)
    add_test(NAME mwb_protocol_security_tests COMMAND mwb_protocol_security_tests)
    add_test(NAME mwb_clipboard_socket_security_tests COMMAND mwb_clipboard_socket_security_tests)
    add_test(NAME mwb_client_help COMMAND mwb_client --help)
    add_test(NAME mwb_client_doctor COMMAND mwb_client doctor --config "${CMAKE_CURRENT_BINARY_DIR}/missing-doctor-config.ini")
    add_test(NAME mwb_client_doctor_categories
        COMMAND ${CMAKE_COMMAND}
            "-DMWB_CLIENT=$<TARGET_FILE:mwb_client>"
            "-DCONFIG_PATH=${CMAKE_CURRENT_BINARY_DIR}/missing-doctor-config.ini"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/doctor_categories.cmake"
    )
    add_test(NAME mwb_export_windows_pair_helper
        COMMAND ${CMAKE_COMMAND}
            "-DMWB_CLIENT=$<TARGET_FILE:mwb_client>"
            "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/export_windows_pair"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/export_windows_pair_helper.cmake"
    )
    add_test(NAME mwb_client_doctor_invalid_config COMMAND mwb_client doctor --config "${CMAKE_CURRENT_SOURCE_DIR}/tests/invalid_config.ini")
    set_tests_properties(mwb_client_doctor_invalid_config PROPERTIES WILL_FAIL TRUE)
endif()

if (PkgConfig_FOUND)
    pkg_check_modules(MWB_TRAY_DEPS QUIET IMPORTED_TARGET gtk+-3.0 ayatana-appindicator3-0.1)
    if (MWB_TRAY_DEPS_FOUND)
        add_executable(mwb_tray src/TrayController.cpp)
        target_link_libraries(mwb_tray PRIVATE PkgConfig::MWB_TRAY_DEPS)
        mwb_apply_sanitizers(mwb_tray)
    endif()
endif()
