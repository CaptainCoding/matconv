set(cases
    "diffuse_matte|<standard_surface"
    "ggx_reflect|<standard_surface"
    "ggx_transmit|<standard_surface"
    "ggx_transmit_rough|<standard_surface"
    "ggx_glass_smooth|<standard_surface"
    "ggx_glass_rough|<standard_surface"
    "ggx_multiscatter|<standard_surface"
    "curve_reflection|<standard_surface"
    "curve_film_zero|<standard_surface"
    "weighted_layer|<standard_surface"
    "custom_curve_layer|<standard_surface"
    "thin_film|<standard_surface"
    "emission|<standard_surface"
    "color_lerp|color_lerp_delta_"
    "textured_diffuse|<image "
    "textured_roughness|<image "
)

file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(roundtrip_directory "${OUTPUT_DIR}/roundtrip")
file(MAKE_DIRECTORY "${roundtrip_directory}")

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
                "<standard_surface"
                "checker.png"
                "value=\"textures/checker.png\""
                "colorspace=\"srgb_texture\""
                "<input name=\"index\" type=\"integer\" value=\"0\" />")
            string(FIND "${generated}" "${fragment}" found)
            if(found EQUAL -1)
                message(FATAL_ERROR
                    "textured_diffuse output does not contain '${fragment}'.")
            endif()
        endforeach()
    endif()
    if(material STREQUAL "textured_roughness")
        foreach(fragment IN ITEMS
                "value=\"textures/checker.png\""
                "<input name=\"specular_roughness\" type=\"float\" nodename=\"roughness_sqrt_"
                "<sqrt name=\"roughness_sqrt_")
            string(FIND "${generated}" "${fragment}" found)
            if(found EQUAL -1)
                message(FATAL_ERROR
                    "textured_roughness output does not contain '${fragment}'.")
            endif()
        endforeach()
    endif()
    if(material STREQUAL "ggx_transmit")
        foreach(fragment IN ITEMS
                "<input name=\"transmission\" type=\"float\" value=\"1.0\" />"
                "<input name=\"specular_IOR\" type=\"float\" nodename="
                "<dotproduct "
                "1.60000002")
            string(FIND "${generated}" "${fragment}" found)
            if(found EQUAL -1)
                message(FATAL_ERROR
                    "ggx_transmit output does not preserve its transmission "
                    "mapping or material IOR through "
                    "'${fragment}'.")
            endif()
        endforeach()
    endif()
    if(material STREQUAL "ggx_reflect" OR
       material STREQUAL "ggx_transmit" OR
       material STREQUAL "ggx_glass_smooth" OR
       material STREQUAL "ggx_transmit_rough" OR
       material STREQUAL "ggx_glass_rough")
        string(REGEX MATCH
            "<input name=\"specular_roughness\" type=\"float\" nodename=\"([^\"]+)\""
            roughness_input "${generated}")
        set(sqrt_node "${CMAKE_MATCH_1}")
        string(REGEX MATCH
            "<sqrt name=\"${sqrt_node}\" type=\"float\">\n      <input name=\"in\" type=\"float\" nodename=\"([^\"]+)\""
            sqrt_input "${generated}")
        set(max_node "${CMAKE_MATCH_1}")
        string(FIND "${generated}" "<max name=\"${max_node}\" type=\"float\">"
            max_found)
        string(REGEX MATCH
            "<max name=\"${max_node}\" type=\"float\">\n      <input name=\"in1\"[^\n]*\n      <input name=\"in2\" type=\"float\" nodename=\"([^\"]+)\""
            max_zero_input "${generated}")
        set(zero_node "${CMAKE_MATCH_1}")
        string(FIND "${generated}"
            "<constant name=\"${zero_node}\" type=\"float\">\n      <input name=\"value\" type=\"float\" value=\"0.0\""
            zero_found)
        if(NOT roughness_input OR NOT sqrt_input OR max_found EQUAL -1 OR
           NOT max_zero_input OR zero_found EQUAL -1)
            message(FATAL_ERROR
                "${material} does not convert MDL GGX roughness to the "
                "standard_surface roughness convention.")
        endif()
        if(NOT material STREQUAL "ggx_reflect")
            string(FIND "${generated}"
                "<input name=\"transmission_extra_roughness\" type=\"float\" value=\"0.0\""
                zero_extra)
            if(zero_extra EQUAL -1)
                message(FATAL_ERROR
                    "${material} adds unwanted transmission roughness.")
            endif()
        endif()
    endif()
    if(material STREQUAL "ggx_reflect" OR
       material STREQUAL "ggx_transmit_rough" OR
       material STREQUAL "ggx_glass_rough")
        string(FIND "${generated}"
            "<max name=\"${max_node}\" type=\"float\">\n      <input name=\"in1\" type=\"float\" interfacename=\"roughness\""
            found)
        if(found EQUAL -1)
            message(FATAL_ERROR
                "${material} does not preserve the roughness parameter.")
        endif()
    endif()
    if(material STREQUAL "ggx_transmit" OR
       material STREQUAL "ggx_glass_smooth")
        string(REGEX MATCH
            "<max name=\"${max_node}\" type=\"float\">\n      <input name=\"in1\" type=\"float\" nodename=\"([^\"]+)\""
            max_input "${generated}")
        set(roughness_node "${CMAKE_MATCH_1}")
        string(FIND "${generated}"
            "<constant name=\"${roughness_node}\" type=\"float\">\n      <input name=\"value\" type=\"float\" value=\"0\""
            zero_roughness)
        if(NOT max_input OR zero_roughness EQUAL -1)
            message(FATAL_ERROR
                "${material} does not preserve smooth transmission roughness.")
        endif()
    endif()
    if(material STREQUAL "ggx_reflect")
        foreach(fragment IN ITEMS
                "<input name=\"base\" type=\"float\" value=\"1.0\" />"
                "<input name=\"metalness\" type=\"float\" value=\"1.0\" />"
                "<input name=\"base_color\" type=\"color3\" interfacename=\"reflectance\"")
            string(FIND "${generated}" "${fragment}" found)
            if(found EQUAL -1)
                message(FATAL_ERROR
                    "ggx_reflect output does not preserve its specular mapping "
                    "through '${fragment}'.")
            endif()
        endforeach()
    endif()
    if(material STREQUAL "curve_reflection" OR
       material STREQUAL "curve_film_zero")
        string(FIND "${generated}"
            "<input name=\"base\" type=\"float\" value=\"0.0\" />" found)
        if(found EQUAL -1)
            message(FATAL_ERROR
                "${material} introduces a diffuse base for an empty BSDF.")
        endif()
    endif()
    if(material STREQUAL "thin_film")
        foreach(fragment IN ITEMS
                "<input name=\"thin_film_thickness\" type=\"float\" nodename="
                "value=\"420\""
                "<input name=\"thin_film_IOR\" type=\"float\" nodename=")
            string(FIND "${generated}" "${fragment}" found)
            if(found EQUAL -1)
                message(FATAL_ERROR
                    "thin_film output does not preserve '${fragment}'.")
            endif()
        endforeach()
    endif()
    if(material STREQUAL "emission")
        foreach(fragment IN ITEMS
                "<input name=\"emission\" type=\"float\" value=\"1.0\" />"
                "<input name=\"emission_color\" type=\"color3\" nodename=")
            string(FIND "${generated}" "${fragment}" found)
            if(found EQUAL -1)
                message(FATAL_ERROR
                    "emission output does not preserve '${fragment}'.")
            endif()
        endforeach()
    endif()
    string(FIND "${generated}" "unsupported_surface_bsdf" fallback)
    if(NOT fallback EQUAL -1)
        message(FATAL_ERROR
            "${material} output contains an explicit black fallback.")
    endif()

    set(roundtrip_output
        "${roundtrip_directory}/${material}_roundtrip.mdl")
    execute_process(
        COMMAND "${MATCONV_EXECUTABLE}" mtlx-to-mdl
            --input "${output}"
            --output "${roundtrip_output}"
            --material "${material}"
        RESULT_VARIABLE reverse_result
        OUTPUT_VARIABLE reverse_stdout
        ERROR_VARIABLE reverse_stderr
    )
    if(NOT reverse_result EQUAL 0)
        message(FATAL_ERROR
            "${material} reverse conversion failed:\n"
            "${reverse_stdout}${reverse_stderr}")
    endif()

    file(READ "${roundtrip_output}" generated_mdl)
    string(FIND "${generated_mdl}" "export material ${material}" exported)
    if(exported EQUAL -1)
        message(FATAL_ERROR
            "${material} reverse conversion did not export the expected MDL "
            "material.\n${reverse_stdout}${reverse_stderr}")
    endif()
    if(material STREQUAL "textured_diffuse" OR
       material STREQUAL "textured_roughness")
        string(FIND "${generated_mdl}" "./textures/checker.png" found)
        if(found EQUAL -1)
            message(FATAL_ERROR
                "${material} reverse conversion does not reference its "
                "packaged texture.")
        endif()
        if(NOT EXISTS "${roundtrip_directory}/textures/checker.png")
            message(FATAL_ERROR
                "${material} reverse conversion did not package its "
                "texture next to the generated MDL.")
        endif()
    endif()
endforeach()
