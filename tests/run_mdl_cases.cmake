set(cases
    "diffuse_matte|<oren_nayar_diffuse_bsdf"
    "ggx_reflect|<dielectric_bsdf"
    "ggx_transmit|value=\"T\""
    "weighted_layer|<layer "
    "custom_curve_layer|<layer "
    "thin_film|name=\"thinfilm_thickness\""
    "emission|<uniform_edf"
    "color_lerp|color_lerp_delta_"
    "textured_diffuse|<image "
)

file(MAKE_DIRECTORY "${OUTPUT_DIR}")

foreach(case IN LISTS cases)
    string(REPLACE "|" ";" parts "${case}")
    list(GET parts 0 material)
    list(GET parts 1 expected)
    set(input "${MDL_CASES_DIR}/${material}.mdl")
    set(output "${OUTPUT_DIR}/${material}.mtlx")

    execute_process(
        COMMAND "${MATCONV_EXECUTABLE}" mdl-to-mtlx
            --input "${input}"
            --output "${output}"
            --material "${material}"
            --mdl-path "${MDL_PATH}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${material} failed:\n${stdout}${stderr}")
    endif()

    file(READ "${output}" generated)
    string(FIND "${generated}" "${expected}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR
            "${material} output does not contain expected fragment '${expected}'.\n"
            "Converter output:\n${stdout}${stderr}")
    endif()
    if(material STREQUAL "textured_diffuse")
        foreach(fragment IN ITEMS
                "checker.png"
                "colorspace=\"srgb_texture\""
                "<input name=\"index\" type=\"integer\" value=\"0\" />")
            string(FIND "${generated}" "${fragment}" found)
            if(found EQUAL -1)
                message(FATAL_ERROR
                    "textured_diffuse output does not contain '${fragment}'.")
            endif()
        endforeach()
    endif()
    string(FIND "${generated}" "unsupported_surface_bsdf" fallback)
    if(NOT fallback EQUAL -1)
        message(FATAL_ERROR
            "${material} output contains an explicit black fallback.")
    endif()
endforeach()
