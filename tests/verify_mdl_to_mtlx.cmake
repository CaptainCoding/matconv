execute_process(
    COMMAND "${MATCONV_EXECUTABLE}" mdl-to-mtlx
        --input "${MDL_INPUT}"
        --output "${OUTPUT}"
        --material "${MATERIAL}"
        --mdl-path "${MDL_PATH}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "MDL conversion failed:\n${stdout}${stderr}")
endif()

file(READ "${OUTPUT}" generated)
foreach(expected IN ITEMS
    "<nodegraph name=\"slots_graph\""
    "<input name=\"opacity\" type=\"float\""
    "<input name=\"thin\" type=\"boolean\""
    "<uniform_edf"
    "<multiply name=\"surface_emission_intensity\""
    "<clamp"
    "<combine3"
    "<input name=\"displacement\" type=\"vector3\""
    "output=\"backface_output\""
    "output=\"displacement_output\""
)
    string(FIND "${generated}" "${expected}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Generated MaterialX is missing: ${expected}")
    endif()
endforeach()
