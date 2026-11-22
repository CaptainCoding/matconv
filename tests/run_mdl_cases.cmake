set(cases
    "diffuse_matte|<standard_surface"
    "ggx_reflect|<standard_surface"
    "ggx_transmit|<standard_surface"
    "ggx_transmit_rough|<standard_surface"
    "ggx_glass_smooth|<standard_surface"
    "ggx_glass_rough|<standard_surface"
    "ggx_multiscatter|<standard_surface"
    "conditional_bsdf|<ifequal "
    "specular_coat|<standard_surface"
    "fresnel_glass|<standard_surface"
    "curve_reflection|<standard_surface"
    "curve_film_zero|<standard_surface"
    "weighted_layer|<standard_surface"
    "custom_curve_layer|<standard_surface"
    "thin_film|<standard_surface"
    "emission|<standard_surface"
    "color_lerp|color_lerp_delta_"
    "textured_diffuse|<image "
    "textured_roughness|<image "
    "base_file_texture|<image "
    "base_file_texture_alpha|<standard_surface"
    "base_file_texture_transformed|<image "
    "texture_channels|<extract "
    "metallic_diffuse_layer|<mix name=\"metal_diffuse_mix_"
    "color_layers|<mix name=\"color_layer_"
    "unsupported_color_layer|<standard_surface"
    "uv_transform|<combine3 name=\"transformed_uv_"
    "normal_layer|<normalmap "
    "normal_layer_uv|<normalmap "
    "normal_geometry|<normalmap "
    "unsupported_uv|<image "
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
                "colorspace=\"g22_rec709\""
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
    if(material STREQUAL "base_file_texture")
        foreach(fragment IN ITEMS
                "<input name=\"base_color\" type=\"color3\" nodename=\"file_texture_offset_"
                "<input name=\"diffuse_roughness\" type=\"float\" nodename=\"file_texture_average_"
                "value=\"textures/checker.png\" colorspace=\"g22_rec709\""
                "<multiply name=\"file_texture_scale_"
                "<add name=\"file_texture_offset_"
                "<dotproduct name=\"file_texture_average_"
                "interfacename=\"brightness\""
                "value=\"0.029999"
                "value=\"0.333333333, 0.333333333, 0.333333333\"")
            string(FIND "${generated}" "${fragment}" found)
            if(found EQUAL -1)
                message(FATAL_ERROR
                    "base_file_texture output does not preserve '${fragment}'.")
            endif()
        endforeach()
        if("${stdout}${stderr}" MATCHES "unsupported ::base::texture_return")
            message(FATAL_ERROR "base_file_texture fields were not translated.")
        endif()
    endif()
    if(material STREQUAL "base_file_texture_alpha")
        if(NOT "${stdout}${stderr}" MATCHES
                "base::file_texture mono supports only mono_average")
            message(FATAL_ERROR "Unsupported alpha mono mode was not reported.")
        endif()
    endif()
    if(material STREQUAL "base_file_texture_transformed")
        if("${stdout}${stderr}" MATCHES "MaterialX's default UV0 is used")
            message(FATAL_ERROR "Identity UV transform unexpectedly fell back.")
        endif()
        string(REGEX MATCH
            "<input name=\"texcoord\" type=\"vector2\" nodename=\"uv_vector2_[^\"]+\""
            uv_connection "${generated}")
        if(NOT uv_connection)
            message(FATAL_ERROR
                "Transformed texture did not preserve its UV connection.")
        endif()
    endif()
    if(material STREQUAL "unsupported_uv")
        if(NOT "${stdout}${stderr}" MATCHES "MaterialX's default UV0 is used")
            message(FATAL_ERROR "Unsupported object-space UV was not reported.")
        endif()
    endif()
    if(material STREQUAL "uv_transform")
        foreach(fragment IN ITEMS "interfacename=\"scale_u\""
                "interfacename=\"scale_v\"" "interfacename=\"offset_u\""
                "interfacename=\"offset_v\"" "interfacename=\"angle\""
                "matrix_product_" "nodename=\"transformed_uv_"
                "nodename=\"uv_vector2_")
            string(FIND "${generated}" "${fragment}" found)
            if(found EQUAL -1)
                message(FATAL_ERROR "uv_transform is missing '${fragment}'.")
            endif()
        endforeach()
        if("${stdout}${stderr}" MATCHES "MaterialX's default UV0 is used")
            message(FATAL_ERROR "uv_transform unexpectedly fell back to UV0.")
        endif()
    endif()
    if(material STREQUAL "normal_layer" OR material STREQUAL "normal_layer_uv" OR
       material STREQUAL "normal_geometry")
        foreach(fragment IN ITEMS "nodedef=\"ND_normalmap_vector2\""
                "nodename=\"normal_map_" "nodedef=\"ND_normal_vector3\""
                "nodedef=\"ND_tangent_vector3\""
                "nodedef=\"ND_bitangent_vector3\"" "interfacename=\"strength\"")
            string(FIND "${generated}" "${fragment}" found)
            if(found EQUAL -1)
                message(FATAL_ERROR "${material} is missing '${fragment}'.")
            endif()
        endforeach()
        if("${stdout}${stderr}" MATCHES "normal map tangent could not be represented")
            message(FATAL_ERROR "${material} lost its tangent frame.")
        endif()
    endif()
    if(material STREQUAL "texture_channels")
        foreach(fragment IN ITEMS "ND_extract_vector3" "ND_ifequal_floatB"
                "ND_convert_color3_vector3" "<subtract name=\"negative_")
            string(FIND "${generated}" "${fragment}" found)
            if(found EQUAL -1)
                message(FATAL_ERROR "texture_channels is missing '${fragment}'.")
            endif()
        endforeach()
    endif()
    if(material STREQUAL "metallic_diffuse_layer")
        foreach(fragment IN ITEMS "metal_diffuse_mix_" "metal_weight_"
                "<input name=\"metalness\" type=\"float\" nodename=")
            string(FIND "${generated}" "${fragment}" found)
            if(found EQUAL -1)
                message(FATAL_ERROR "metallic_diffuse_layer is missing '${fragment}'.")
            endif()
        endforeach()
    endif()
    if(material STREQUAL "color_layers")
        foreach(fragment IN ITEMS "<mix name=\"color_layer_"
                "<add name=\"color_layer_add_" "<subtract name=\"fmod_"
                "ND_logical_or" "ND_logical_and" "ND_logical_not")
            string(FIND "${generated}" "${fragment}" found)
            if(found EQUAL -1)
                message(FATAL_ERROR "color_layers is missing '${fragment}'.")
            endif()
        endforeach()
        if("${stdout}${stderr}" MATCHES "unsupported ::base::texture_return|unsupported MDL call")
            message(FATAL_ERROR "color_layers has untranslated expressions:\n${stdout}${stderr}")
        endif()
    endif()
    if(material STREQUAL "unsupported_color_layer")
        if(NOT "${stdout}${stderr}" MATCHES
                "unsupported blend_color_layers mode color_layer_multiply")
            message(FATAL_ERROR "Unsupported color-layer mode was not reported.")
        endif()
        if("${generated}" MATCHES
                "<input name=\"base_color\" type=\"color3\" nodename=\"power_")
            message(FATAL_ERROR "Unsupported layer was replaced by a power-node default.")
        endif()
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
    if(material STREQUAL "conditional_bsdf")
        foreach(fragment IN ITEMS
                "nodedef=\"ND_ifequal_floatB\""
                "nodedef=\"ND_ifequal_color3B\""
                "nodedef=\"ND_ifgreater_boolean\""
                "interfacename=\"glass\""
                "interfacename=\"amount\""
                "interfacename=\"ior\""
                "value=\"1, 0, 0\""
                "value=\"0, 0, 1\"")
            string(FIND "${generated}" "${fragment}" found)
            if(found EQUAL -1)
                message(FATAL_ERROR
                    "conditional_bsdf does not preserve '${fragment}'.")
            endif()
        endforeach()
        if("${stdout}${stderr}" MATCHES "unsupported BSDF|unsupported MDL call.*abbe_number_ior")
            message(FATAL_ERROR
                "conditional_bsdf fell back instead of translating its branches or IOR.")
        endif()
    endif()
    if(material STREQUAL "specular_coat")
        foreach(fragment IN ITEMS
                "<input name=\"transmission\" type=\"float\" value=\"1.0\""
                "<input name=\"specular\" type=\"float\" value=\"1.0\""
                "<input name=\"coat\" type=\"float\" interfacename=\"coating\""
                "<input name=\"coat_color\" type=\"color3\" interfacename=\"tint\""
                "<input name=\"coat_roughness\" type=\"float\" value=\"0.0\"")
            string(FIND "${generated}" "${fragment}" found)
            if(found EQUAL -1)
                message(FATAL_ERROR
                    "specular_coat does not preserve '${fragment}'.")
            endif()
        endforeach()
        if("${stdout}${stderr}" MATCHES "unsupported BSDF")
            message(FATAL_ERROR "specular_coat fell back to diffuse.")
        endif()
    endif()
    if(material STREQUAL "fresnel_glass")
        foreach(fragment IN ITEMS
                "<input name=\"transmission\" type=\"float\" value=\"1.0\""
                "<input name=\"specular\" type=\"float\" nodename="
                "<input name=\"transmission_color\" type=\"color3\" interfacename=\"tint\""
                "<input name=\"thin_walled\" type=\"boolean\" nodename=")
            string(FIND "${generated}" "${fragment}" found)
            if(found EQUAL -1)
                message(FATAL_ERROR
                    "fresnel_glass does not preserve '${fragment}'.")
            endif()
        endforeach()
        if("${stdout}${stderr}" MATCHES "unsupported BSDF")
            message(FATAL_ERROR "fresnel_glass fell back to diffuse.")
        endif()
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
    if(material STREQUAL "fresnel_glass")
        string(FIND "${generated_mdl}" "mxp_thin_walled: thin_walled" found)
        string(FIND "${generated_mdl}" "uniform bool thin_walled" uniform_found)
        if(found EQUAL -1 OR uniform_found EQUAL -1)
            message(FATAL_ERROR
                "fresnel_glass reverse shader does not forward a uniform "
                "thin_walled value.")
        endif()
    endif()
    if(material STREQUAL "fresnel_glass" OR
       material STREQUAL "texture_channels" OR
       material STREQUAL "metallic_diffuse_layer" OR
       material STREQUAL "color_layers" OR
       material STREQUAL "uv_transform" OR
       material STREQUAL "normal_layer" OR
       material STREQUAL "normal_layer_uv" OR
       material STREQUAL "normal_geometry")
        execute_process(
            COMMAND "${MATCONV_EXECUTABLE}" mdl-to-mtlx
                --input "${roundtrip_output}"
                --output "${OUTPUT_DIR}/${material}_recompiled.mtlx"
                --material "${material}"
                --mdl-path "${roundtrip_directory}"
            RESULT_VARIABLE recompile_result
            OUTPUT_VARIABLE recompile_stdout
            ERROR_VARIABLE recompile_stderr
        )
        if(NOT recompile_result EQUAL 0)
            message(FATAL_ERROR
                "${material} reverse MDL failed to compile:\n"
                "${recompile_stdout}${recompile_stderr}")
        endif()
    endif()
    if(material STREQUAL "textured_diffuse" OR
       material STREQUAL "textured_roughness" OR
       material STREQUAL "base_file_texture")
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
    if(material STREQUAL "textured_diffuse" OR
       material STREQUAL "base_file_texture")
        string(FIND "${generated_mdl}"
            "NG_g22_rec709_to_lin_rec709_color3" gamma_found)
        if(gamma_found EQUAL -1)
            message(FATAL_ERROR
                "${material} reverse shader omits the texture gamma transform.")
        endif()
    endif()
endforeach()
