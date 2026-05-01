if (NOT DEFINED MWB_CLIENT)
    message(FATAL_ERROR "MWB_CLIENT is required")
endif()
if (NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "TEST_DIR is required")
endif()

file(MAKE_DIRECTORY "${TEST_DIR}")
set(helper "${TEST_DIR}/export-windows-pair-helper.ps1")
file(REMOVE "${helper}")

execute_process(
    COMMAND "${MWB_CLIENT}" export-windows-pair
        --output "${helper}"
        --force
        --linux-ip 192.0.2.10
        --name LinuxPeer
        --key 1234567890123456
        --position top-right
    RESULT_VARIABLE export_result
    OUTPUT_VARIABLE export_stdout
    ERROR_VARIABLE export_stderr
)
if (NOT export_result EQUAL 0)
    message(FATAL_ERROR "export-windows-pair failed: ${export_stderr}")
endif()
if (NOT EXISTS "${helper}")
    message(FATAL_ERROR "export-windows-pair did not write helper")
endif()

file(READ "${helper}" script)

foreach (required
    "[switch]$DryRun"
    "[switch]$Check"
    "Assert-SettingsSchema"
    "Check passed: settings path, schema, version, and requested peer placement are compatible."
    "Dry run: no changes written."
    "Copy-Item -LiteralPath $settingsPath -Destination $backupPath -Force"
    "Backup written:"
    "Restore command:"
    "No changes written."
)
    string(FIND "${script}" "${required}" required_pos)
    if (required_pos EQUAL -1)
        message(FATAL_ERROR "helper is missing required text: ${required}")
    endif()
endforeach()

foreach (forbidden
    "taskkill"
    "Stop-Service"
    "Stop-Process"
    "Stop-PowerToysProcesses"
    "sc.exe"
    "net stop"
)
    string(FIND "${script}" "${forbidden}" forbidden_pos)
    if (NOT forbidden_pos EQUAL -1)
        message(FATAL_ERROR "helper contains destructive process/service management text: ${forbidden}")
    endif()
endforeach()

string(FIND "${script}" "Copy-Item -LiteralPath $settingsPath -Destination $backupPath -Force" backup_pos)
string(FIND "${script}" "Set-Content -LiteralPath $settingsPath" write_pos)
if (backup_pos EQUAL -1 OR write_pos EQUAL -1 OR NOT backup_pos LESS write_pos)
    message(FATAL_ERROR "helper must back up settings before writing them")
endif()

set(dry_helper "${TEST_DIR}/export-windows-pair-dry-run.ps1")
file(REMOVE "${dry_helper}")
execute_process(
    COMMAND "${MWB_CLIENT}" export-windows-pair
        --output "${dry_helper}"
        --dry-run
        --linux-ip 192.0.2.10
        --name LinuxPeer
        --key 1234567890123456
    RESULT_VARIABLE dry_result
    OUTPUT_VARIABLE dry_stdout
    ERROR_VARIABLE dry_stderr
)
if (NOT dry_result EQUAL 0)
    message(FATAL_ERROR "export-windows-pair --dry-run failed: ${dry_stderr}")
endif()
if (EXISTS "${dry_helper}")
    message(FATAL_ERROR "export-windows-pair --dry-run wrote an output file")
endif()
string(FIND "${dry_stdout}" "[switch]$DryRun" dry_pos)
if (dry_pos EQUAL -1)
    message(FATAL_ERROR "--dry-run output did not include generated helper text")
endif()

set(check_helper "${TEST_DIR}/export-windows-pair-check.ps1")
file(REMOVE "${check_helper}")
execute_process(
    COMMAND "${MWB_CLIENT}" export-windows-pair
        --output "${check_helper}"
        --check
        --linux-ip 192.0.2.10
        --name LinuxPeer
        --key 1234567890123456
    RESULT_VARIABLE check_result
    OUTPUT_VARIABLE check_stdout
    ERROR_VARIABLE check_stderr
)
if (NOT check_result EQUAL 0)
    message(FATAL_ERROR "export-windows-pair --check failed: ${check_stderr}")
endif()
if (EXISTS "${check_helper}")
    message(FATAL_ERROR "export-windows-pair --check wrote an output file")
endif()
string(FIND "${check_stdout}" "[switch]$Check" check_pos)
if (check_pos EQUAL -1)
    message(FATAL_ERROR "--check output did not include generated helper text")
endif()
string(FIND "${check_stderr}" "run it with -Check" check_hint_pos)
if (check_hint_pos EQUAL -1)
    message(FATAL_ERROR "--check did not report how to validate Windows settings without writing")
endif()
