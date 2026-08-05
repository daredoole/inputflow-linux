cmake_minimum_required(VERSION 3.10)
project(mwb_client VERSION 0.2.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MWB_ENABLE_SANITIZERS "Build with compiler sanitizers for debug hardening" OFF)
set(MWB_SANITIZERS "address,undefined" CACHE STRING "Comma-separated sanitizer list used when MWB_ENABLE_SANITIZERS=ON")
option(MWB_ENABLE_FUZZING "Build coverage-guided security fuzz targets with Clang" OFF)

if (MWB_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=${MWB_SANITIZERS}")
    check_cxx_source_compiles("int main() { return 0; }" MWB_SANITIZER_RUNTIME_AVAILABLE)
    unset(CMAKE_REQUIRED_FLAGS)
    if (NOT MWB_SANITIZER_RUNTIME_AVAILABLE)
        message(FATAL_ERROR "MWB_ENABLE_SANITIZERS=ON, but the requested sanitizer runtime is not linkable by this compiler")
    endif()
endif()

if (MWB_ENABLE_FUZZING AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "MWB_ENABLE_FUZZING requires Clang and libFuzzer")
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
find_package(X11 REQUIRED)

add_executable(mwb_client
    src/AndroidRelay.cpp
    src/AppConfig.cpp
    src/AppState.cpp
    src/ClientRuntime.cpp
    src/Discovery.cpp
    src/InputDispatcher.cpp
    src/LibeiInputCaptureBridge.cpp
    src/LocalAndroidInputBridge.cpp
    src/main.cpp
    src/PeerRecovery.cpp
    src/SecretStore.cpp
    src/SecureFile.cpp
    src/TopologyModel.cpp
    src/ClipboardManager.cpp
    src/CryptoHelper.cpp
    src/InputManager.cpp
    src/MediaKeyBridge.cpp
    src/NetworkManager.cpp
)

target_include_directories(mwb_client PRIVATE src)
target_include_directories(mwb_client PRIVATE ${OPENSSL_INCLUDE_DIR})
target_include_directories(mwb_client PRIVATE ${X11_INCLUDE_DIR})

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
    ${X11_LIBRARIES}
    pthread
)

include(CTest)

find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
    pkg_check_modules(LIBEI_INPUT_CAPTURE QUIET libei-1.0 gio-unix-2.0 gio-2.0 glib-2.0)
    pkg_check_modules(LIBINPUT_GESTURES QUIET libinput)
endif()

if (LIBEI_INPUT_CAPTURE_FOUND)
    target_compile_definitions(mwb_client PRIVATE MWB_HAVE_LIBEI_INPUT_CAPTURE=1)
    target_include_directories(mwb_client PRIVATE ${LIBEI_INPUT_CAPTURE_INCLUDE_DIRS})
    target_link_libraries(mwb_client PRIVATE ${LIBEI_INPUT_CAPTURE_LDFLAGS})
endif()

if (LIBINPUT_GESTURES_FOUND)
    message(STATUS "Native libinput gesture monitor enabled")
    target_compile_definitions(mwb_client PRIVATE MWB_HAVE_LIBINPUT_GESTURES=1)
    target_include_directories(mwb_client PRIVATE ${LIBINPUT_GESTURES_INCLUDE_DIRS})
    target_link_libraries(mwb_client PRIVATE ${LIBINPUT_GESTURES_LDFLAGS})
else()
    message(STATUS "Native libinput gesture monitor disabled; install libinput-devel to enable it")
endif()

if (BUILD_TESTING)
    find_program(PYTHON3_EXECUTABLE python3)

    add_executable(mwb_client_unit_tests
        tests/test_main.cpp
        src/AndroidRelay.cpp
        src/AppConfig.cpp
        src/AppState.cpp
        src/Discovery.cpp
        src/PeerRecovery.cpp
        src/SecureFile.cpp
    )
    target_include_directories(mwb_client_unit_tests PRIVATE src)
    target_compile_definitions(mwb_client_unit_tests PRIVATE MWB_TESTING=1)
    target_compile_options(mwb_client_unit_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(mwb_client_unit_tests PRIVATE OpenSSL::Crypto pthread)
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

    add_executable(mwb_crypto_helper_tests
        tests/test_crypto_helper.cpp
        src/CryptoHelper.cpp
    )
    target_include_directories(mwb_crypto_helper_tests PRIVATE src)
    target_compile_options(mwb_crypto_helper_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(mwb_crypto_helper_tests PRIVATE OpenSSL::Crypto)
    mwb_apply_sanitizers(mwb_crypto_helper_tests)

    add_executable(mwb_clipboard_fuzz_smoke
        tests/fuzz_clipboard_payload.cpp
        tests/fuzz_smoke_driver.cpp
        src/ClipboardManager.cpp
    )
    target_include_directories(mwb_clipboard_fuzz_smoke PRIVATE src)
    target_compile_options(mwb_clipboard_fuzz_smoke PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(mwb_clipboard_fuzz_smoke PRIVATE ZLIB::ZLIB)
    mwb_apply_sanitizers(mwb_clipboard_fuzz_smoke)

    add_test(NAME mwb_client_unit_tests COMMAND mwb_client_unit_tests)
    add_test(NAME mwb_input_mapping_tests COMMAND mwb_input_mapping_tests)
    add_test(NAME mwb_inject_mouse_abs_tests COMMAND mwb_inject_mouse_abs_tests)
    add_test(NAME mwb_input_device_capability_tests COMMAND mwb_input_device_capability_tests)
    add_test(NAME mwb_input_latency_tests COMMAND mwb_input_latency_tests)
    add_test(NAME mwb_topology_model_tests COMMAND mwb_topology_model_tests)
    if (PYTHON3_EXECUTABLE)
        add_test(NAME mwb_topology_config_docs
            COMMAND "${PYTHON3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/topology_config_docs_test.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/docs/topology.md"
        )
        add_test(NAME inputflow_systemd_hardening
            COMMAND "${PYTHON3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_systemd_hardening.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/packaging/usr/lib/systemd/user/mwb-client.service"
        )
        add_test(NAME inputflow_ci_security
            COMMAND "${PYTHON3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ci_security.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/.github/workflows"
        )
    endif()
    add_test(NAME mwb_mouse_trace_tests COMMAND mwb_mouse_trace_tests)
    add_test(NAME mwb_media_key_bridge_tests COMMAND mwb_media_key_bridge_tests)
    add_test(NAME mwb_protocol_security_tests COMMAND mwb_protocol_security_tests)
    add_test(NAME mwb_clipboard_socket_security_tests COMMAND mwb_clipboard_socket_security_tests)
    add_test(NAME mwb_crypto_helper_tests COMMAND mwb_crypto_helper_tests)
    add_test(NAME mwb_clipboard_fuzz_smoke COMMAND mwb_clipboard_fuzz_smoke)
    set_tests_properties(mwb_clipboard_fuzz_smoke PROPERTIES TIMEOUT 30)
    add_test(NAME mwb_client_help COMMAND mwb_client --help)
    add_test(NAME mwb_client_doctor COMMAND mwb_client doctor --config "${CMAKE_CURRENT_BINARY_DIR}/missing-doctor-config.ini")
    add_test(NAME mwb_client_topology_explain
        COMMAND mwb_client topology explain "${CMAKE_CURRENT_SOURCE_DIR}/tests/simple.topology"
    )
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
    add_test(
        NAME inputflow_diagnostics_privacy
        COMMAND bash
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_diagnostics_privacy.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/inputflow-diagnostics-bundle.sh"
    )
    set_tests_properties(inputflow_diagnostics_privacy PROPERTIES TIMEOUT 30)
endif()

if (MWB_ENABLE_FUZZING)
    add_executable(mwb_clipboard_fuzzer
        tests/fuzz_clipboard_payload.cpp
        src/ClipboardManager.cpp
    )
    target_include_directories(mwb_clipboard_fuzzer PRIVATE src)
    target_compile_options(mwb_clipboard_fuzzer PRIVATE
        -O1
        -g
        -fno-omit-frame-pointer
        -fsanitize=fuzzer,address,undefined
    )
    set_property(TARGET mwb_clipboard_fuzzer APPEND_STRING PROPERTY
        LINK_FLAGS " -fsanitize=fuzzer,address,undefined")
    target_link_libraries(mwb_clipboard_fuzzer PRIVATE ZLIB::ZLIB)
endif()

if (PkgConfig_FOUND)
    pkg_check_modules(MWB_TRAY_DEPS QUIET IMPORTED_TARGET gtk+-3.0 ayatana-appindicator3-0.1)
    if (MWB_TRAY_DEPS_FOUND)
        # Embed tray + GUI window into the main mwb_client binary
        target_sources(mwb_client PRIVATE
            src/TrayController.cpp
            src/GuiMainWindow.cpp
            src/MonitorLayoutWidget.cpp
        )
        target_compile_definitions(mwb_client PRIVATE MWB_HAVE_GTK_GUI=1)
        target_link_libraries(mwb_client PRIVATE PkgConfig::MWB_TRAY_DEPS)

        # Keep standalone mwb_tray as a fallback (no embedded runtime)
        add_executable(mwb_tray src/TrayController.cpp)
        target_link_libraries(mwb_tray PRIVATE PkgConfig::MWB_TRAY_DEPS)
        mwb_apply_sanitizers(mwb_tray)
    endif()
endif()

install(TARGETS mwb_client RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
if (TARGET mwb_tray)
    install(TARGETS mwb_tray RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
install(
    PROGRAMS packaging/portable/inputflow-controller
    DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(
    PROGRAMS mwb-desktop-ui.sh
    DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/inputflow"
)
install(
    PROGRAMS scripts/inputflow-diagnostics-bundle.sh
    DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/inputflow/scripts"
)
install(
    FILES packaging/usr/lib/systemd/user/mwb-client.service
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/user"
)
install(
    FILES
        packaging/usr/lib/modules-load.d/mwb-client-uinput.conf
        packaging/usr/lib/udev/rules.d/70-mwb-client-uinput.rules
        packaging/usr/lib/sysusers.d/mwb-client.conf
    DESTINATION "${CMAKE_INSTALL_DATADIR}/inputflow/system-integration"
)
install(
    FILES packaging/usr/share/applications/inputflow.desktop
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
)
install(DIRECTORY assets/hicolor/ DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor")
install(
    FILES
        README.md
        SECURITY.md
        LICENSE
        docs/compatibility.md
        docs/security-privacy.md
        docs/public-repository-audit.md
        docs/release-checklist.md
        docs/release-readiness-report.md
        packaging/portable/README.md
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
)

set(CPACK_GENERATOR "TGZ")
set(CPACK_PACKAGE_NAME "inputflow")
set(CPACK_PACKAGE_VENDOR "InputFlow")
set(CPACK_PACKAGE_CONTACT "InputFlow maintainers")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Trusted-LAN Linux client for PowerToys Mouse Without Borders")
set(CPACK_PACKAGE_FILE_NAME
    "inputflow-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}"
)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_PACKAGE_CHECKSUM "SHA256")
include(CPack)
