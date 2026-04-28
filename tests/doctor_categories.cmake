if (NOT DEFINED MWB_CLIENT)
    message(FATAL_ERROR "MWB_CLIENT is required")
endif()

if (NOT DEFINED CONFIG_PATH)
    message(FATAL_ERROR "CONFIG_PATH is required")
endif()

execute_process(
    COMMAND "${MWB_CLIENT}" doctor --config "${CONFIG_PATH}"
    RESULT_VARIABLE doctor_result
    OUTPUT_VARIABLE doctor_output
    ERROR_VARIABLE doctor_error
)

if (NOT doctor_result EQUAL 0)
    message(FATAL_ERROR
        "doctor command failed with exit code ${doctor_result}\n"
        "stdout:\n${doctor_output}\n"
        "stderr:\n${doctor_error}"
    )
endif()

set(required_labels
    "config"
    "uinput module"
    "uinput"
    "session"
    "clipboard helpers"
    "session bus"
    "xdg portal"
    "user service"
    "service state"
    "packaged sysusers"
    "packaged modules-load"
    "packaged udev rule"
)

foreach (label IN LISTS required_labels)
    if (NOT doctor_output MATCHES "\\[[^]]+\\] ${label}:")
        message(FATAL_ERROR
            "doctor output is missing category label '${label}'\n"
            "stdout:\n${doctor_output}\n"
            "stderr:\n${doctor_error}"
        )
    endif()
endforeach()
