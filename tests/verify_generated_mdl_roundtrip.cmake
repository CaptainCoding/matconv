execute_process(
    COMMAND "${MATCONV_EXECUTABLE}" mtlx-to-mdl
        --input "${MDL_INPUT}"
        --output "${MDL_OUTPUT}"
        --material "${MATERIAL}"
    RESULT_VARIABLE mdl_result
    OUTPUT_VARIABLE mdl_stdout
    ERROR_VARIABLE mdl_stderr
)
if(NOT mdl_result EQUAL 0)
    message(FATAL_ERROR "MaterialX-to-MDL conversion failed:\n${mdl_stdout}${mdl_stderr}")
endif()

execute_process(
    COMMAND "${MATCONV_EXECUTABLE}" mdl-to-mtlx
        --input "${MDL_OUTPUT}"
        --output "${MTLX_OUTPUT}"
        --material "${MATERIAL}"
    RESULT_VARIABLE mtlx_result
    OUTPUT_VARIABLE mtlx_stdout
    ERROR_VARIABLE mtlx_stderr
)
if(NOT mtlx_result EQUAL 0)
    message(FATAL_ERROR "Generated MDL could not be reloaded without manual support paths:\n${mtlx_stdout}${mtlx_stderr}")
endif()

if(NOT EXISTS "${MTLX_OUTPUT}")
    message(FATAL_ERROR "Generated MaterialX output was not written.")
endif()
