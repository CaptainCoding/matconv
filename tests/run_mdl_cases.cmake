set(cases
    "diffuse_matte|<standard_surface"
    "ggx_reflect|<standard_surface"
    "ggx_anisotropic|<standard_surface"
    "ggx_anisotropic_swapped|<standard_surface"
    "ggx_anisotropic_zero|<standard_surface"
    "ggx_anisotropic_conductor|<standard_surface"
    "ggx_layer_isotropic|<standard_surface"
    "ggx_layer_anisotropic|<standard_surface"
    "colored_ggx_layers|<standard_surface"
    "ggx_transmit|<standard_surface"
    "ggx_transmit_rough|<standard_surface"
    "ggx_glass_smooth|<standard_surface"
    "ggx_glass_rough|<standard_surface"
    "ggx_multiscatter|<standard_surface"
    "conditional_bsdf|<ifequal "
    "specular_coat|<standard_surface"
    "fresnel_glass|<standard_surface"
    "simple_glossy_glass|<standard_surface"
    "simple_glossy_reflect|<standard_surface"
    "separate_tint_glass|<standard_surface"
    "curve_reflection|<standard_surface"
    "curve_film_zero|<standard_surface"
    "weighted_layer|<standard_surface"
    "weighted_thin_film|<standard_surface"
    "weighted_conductor|<standard_surface"
    "weighted_nested|<standard_surface"
    "parameterized_scatter_mode|<standard_surface"
    "parameterized_scatter_both|<standard_surface"
    "directional_glass|<standard_surface"
    "measured_curve_glass|<standard_surface"
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
    "perlin_texture|<noise3d "
    "perlin_unsupported|<standard_surface"
    "radians_vector|<multiply name=\"radians_"
    "sheen_fabric|<standard_surface"
    "unsupported_color_layer|<standard_surface"
    "uv_transform|<combine3 name=\"transformed_uv_"
    "uv_function|<combine3 name=\"transformed_uv_"
    "uv_function_unsupported|<image "
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
    if(material MATCHES "^ggx_anisotropic")
        foreach(fragment IN ITEMS
                "interfacename=\"roughness_u\""
                "interfacename=\"roughness_v\""
                "<input name=\"specular_anisotropy\" type=\"float\" nodename=\"anisotropy_nonzero_"
                "<input name=\"specular_rotation\" type=\"float\" nodename=\"anisotropy_rotation_"
                "<input name=\"tangent\" type=\"vector3\" nodename=\"texture_tangent_"
                "<divide name=\"anisotropy_ratio_"
                "<ifgreater name=\"anisotropy_safe_major_")
            string(FIND "${generated}" "${fragment}" found)
            if(found EQUAL -1)
                message(FATAL_ERROR "${material} does not preserve '${fragment}'.")
            endif()
        endforeach()
    endif()
    if(material STREQUAL "colored_ggx_layers" OR material STREQUAL "ggx_layer_isotropic")
        foreach(fragment IN ITEMS
                "<input name=\"specular_anisotropy\" type=\"float\" nodename=\"ggx_layer_anisotropy_"
                "<input name=\"coat\" type=\"float\""
                "<input name=\"coat_roughness\" type=\"float\" nodename=\"roughness_sqrt_"
                "<input name=\"coat_anisotropy\" type=\"float\" value=\"0.0\""
                "<mix name=\"ggx_layer_roughness_"
                "<mix name=\"ggx_layer_anisotropy_")
            string(FIND "${generated}" "${fragment}" found)
            if(found EQUAL -1)
                message(FATAL_ERROR "${material} does not preserve '${fragment}'.")
            endif()
        endforeach()
    endif()
    if(material STREQUAL "colored_ggx_layers")
        foreach(fragment IN ITEMS
                "<input name=\"base_color\" type=\"color3\" nodename=\"ggx_layer_color_"
                "<multiply name=\"metal_curve_weight_"
                "interfacename=\"reflectance\""
                "interfacename=\"curve_weight\"")
            string(FIND "${generated}" "${fragment}" found)
            if(found EQUAL -1)
                message(FATAL_ERROR "${material} does not preserve '${fragment}'.")
            endif()
        endforeach()
    endif()
    if(material STREQUAL "ggx_layer_anisotropic")
        foreach(fragment IN ITEMS
                "<input name=\"tangent\" type=\"vector3\" nodename=\"ggx_layer_tangent_"
                "<input name=\"specular_rotation\" type=\"float\" nodename=\"ggx_layer_rotation_"
                "<ifgreater name=\"ggx_layer_upper_tangent_"
                "<ifgreater name=\"ggx_layer_upper_rotation_"
                "interfacename=\"weight\"")
            string(FIND "${generated}" "${fragment}" found)
            if(found EQUAL -1)
                message(FATAL_ERROR "${material} does not preserve '${fragment}'.")
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
    if(material STREQUAL "unsupported_uv" OR material STREQUAL "uv_function_unsupported")
        if(NOT "${stdout}${stderr}" MATCHES "MaterialX's default UV0 is used")
            message(FATAL_ERROR "${material} UV fallback was not reported.")
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
        if(material STREQUAL "uv_function")
            foreach(fragment IN ITEMS "interfacename=\"scale_u\""
                    "interfacename=\"offset_v\"" "nodename=\"transformed_uv_"
                    "nodename=\"uv_vector2_\"" "nodename=\"uv_tangent_"
                    "nodedef=\"ND_normalmap_vector2\"")
                string(FIND "${generated}" "${fragment}" found)
                if(found EQUAL -1)
                    message(FATAL_ERROR "uv_function is missing '${fragment}'.")
                endif()
            endforeach()
            if("${stdout}${stderr}" MATCHES
               "MaterialX's default UV0 is used|normal map tangent could not be represented")
                message(FATAL_ERROR "uv_function lost coordinates or tangents.")
            endif()
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
                "<add name=\"color_layer_add_" "<multiply name=\"color_layer_multiply_"
                "<overlay name=\"color_layer_overlay_"
                "<input name=\"fg\" type=\"color3\" nodename=\"color_layer_multiply_"
                "<subtract name=\"fmod_"
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
                "unsupported blend_color_layers mode color_layer_hue")
            message(FATAL_ERROR "Unsupported color-layer mode was not reported.")
        endif()
        if("${generated}" MATCHES
                "<input name=\"base_color\" type=\"color3\" nodename=\"power_")
            message(FATAL_ERROR "Unsupported layer was replaced by a power-node default.")
        endif()
    endif()
    if(material STREQUAL "perlin_texture")
        foreach(fragment IN ITEMS
                "<position name=\"texture_position_"
                "value=\"object\""
                "<noise3d name=\"perlin_noise_"
                "<absval name=\"noise_abs_"
                "<mix name=\"perlin_color_"
                "interfacename=\"color1\""
                "interfacename=\"color2\""
                "interfacename=\"low\""
                "interfacename=\"high\"")
            string(FIND "${generated}" "${fragment}" found)
            if(found EQUAL -1)
                message(FATAL_ERROR "${material} does not preserve '${fragment}'.")
            endif()
        endforeach()
        if(NOT "${stdout}${stderr}" MATCHES "pattern differs from MDL's 4D noise")
            message(FATAL_ERROR "${material} did not report the noise approximation.")
        endif()
    endif()
    if(material STREQUAL "perlin_unsupported")
        if(NOT "${stdout}${stderr}" MATCHES "unsupported dynamic or modified noise parameters")
            message(FATAL_ERROR "${material} did not report unsupported noise phase.")
        endif()
        if("${generated}" MATCHES "<noise3d ")
            message(FATAL_ERROR "${material} silently ignored unsupported noise phase.")
        endif()
    endif()
    if(material STREQUAL "radians_vector")
        foreach(fragment IN ITEMS "interfacename=\"angle\"" "0.017453292519943295")
            string(FIND "${generated}" "${fragment}" found)
            if(found EQUAL -1)
                message(FATAL_ERROR "${material} does not preserve '${fragment}'.")
            endif()
        endforeach()
    endif()
    if(material STREQUAL "sheen_fabric")
        foreach(fragment IN ITEMS
                "<input name=\"sheen\" type=\"float\" value=\"1.0\""
                "<input name=\"sheen_color\" type=\"color3\" interfacename=\"sheen_color\""
                "<input name=\"sheen_roughness\" type=\"float\" interfacename=\"roughness\""
                "<input name=\"base_color\" type=\"color3\" interfacename=\"base_color\"")
            string(FIND "${generated}" "${fragment}" found)
            if(found EQUAL -1)
                message(FATAL_ERROR "${material} does not preserve '${fragment}'.")
            endif()
        endforeach()
        if("${stdout}${stderr}" MATCHES "unsupported BSDF")
            message(FATAL_ERROR "${material} fell back to diffuse.")
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
    if(material STREQUAL "simple_glossy_glass")
        foreach(fragment IN ITEMS
                "<input name=\"transmission\" type=\"float\" value=\"1.0\""
                "<input name=\"transmission_color\" type=\"color3\" interfacename=\"tint\""
                "<input name=\"transmission_extra_roughness\" type=\"float\" nodename=\"transmission_roughness_offset_"
                "<subtract name=\"transmission_roughness_offset_"
                "interfacename=\"reflection_roughness\""
                "interfacename=\"transmission_roughness\"")
            string(FIND "${generated}" "${fragment}" found)
            if(found EQUAL -1)
                message(FATAL_ERROR "${material} does not preserve '${fragment}'.")
            endif()
        endforeach()
        if(NOT "${stdout}${stderr}" MATCHES "simple_glossy was approximated.*Phong")
            message(FATAL_ERROR "${material} did not report the Phong-to-GGX approximation.")
        endif()
        if("${stdout}${stderr}" MATCHES "unsupported BSDF|surface.scattering could not be translated")
            message(FATAL_ERROR "${material} fell back to diffuse.")
        endif()
    endif()
    if(material STREQUAL "simple_glossy_reflect")
        foreach(fragment IN ITEMS
                "<input name=\"metalness\" type=\"float\" value=\"1.0\""
                "<input name=\"specular_anisotropy\" type=\"float\" nodename=\"anisotropy_nonzero_"
                "<input name=\"tangent\" type=\"vector3\" nodename=\"texture_tangent_"
                "interfacename=\"roughness_u\""
                "interfacename=\"roughness_v\"")
            string(FIND "${generated}" "${fragment}" found)
            if(found EQUAL -1)
                message(FATAL_ERROR "${material} does not preserve '${fragment}'.")
            endif()
        endforeach()
    endif()
    if(material STREQUAL "separate_tint_glass")
        foreach(fragment IN ITEMS
                "<input name=\"transmission\" type=\"float\" value=\"1.0\""
                "<input name=\"specular_color\" type=\"color3\" nodename=\"microfacet_tint_"
                "<input name=\"transmission_color\" type=\"color3\" nodename=\"microfacet_transmission_tint_"
                "<multiply name=\"microfacet_tint_"
                "<multiply name=\"microfacet_transmission_tint_"
                "interfacename=\"reflection\""
                "interfacename=\"transmission\"")
            string(FIND "${generated}" "${fragment}" found)
            if(found EQUAL -1)
                message(FATAL_ERROR "${material} does not preserve '${fragment}'.")
            endif()
        endforeach()
        if("${stdout}${stderr}" MATCHES "surface.scattering could not be translated")
            message(FATAL_ERROR "${material} fell back to diffuse.")
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
            if(material MATCHES "^weighted_(thin_film|conductor|nested)$")
                foreach(fragment IN ITEMS
                        "<input name=\"base_color\" type=\"color3\" nodename=\"weighted_subtree_"
                        "<input name=\"mix\" type=\"float\" interfacename=\"weight\""
                        "weighted BSDF subtree was approximated")
                    string(FIND "${generated}${stdout}${stderr}" "${fragment}" found)
                    if(found EQUAL -1)
                        message(FATAL_ERROR "${material} does not preserve '${fragment}'.")
                    endif()
                endforeach()
            endif()
            if(material STREQUAL "weighted_thin_film")
                string(REGEX MATCH
                    "<input name=\"base_color\" type=\"color3\" nodename=\"([^\"]+)\""
                    color_input "${generated}")
                set(color_mix "${CMAKE_MATCH_1}")
                string(REGEX MATCH
                    "<mix name=\"${color_mix}\" type=\"color3\"[^>]*>[^<]*<input name=\"fg\" type=\"color3\" nodename=\"([^\"]+)\"[^>]*/>[^<]*<input name=\"bg\" type=\"color3\" nodename=\"([^\"]+)\"[^>]*/>[^<]*<input name=\"mix\" type=\"float\" interfacename=\"weight\""
                    color_graph "${generated}")
                set(foreground "${CMAKE_MATCH_1}")
                set(background "${CMAKE_MATCH_2}")
                string(FIND "${generated}"
                    "<constant name=\"${foreground}\" type=\"color3\">\n      <input name=\"value\" type=\"color3\" value=\"0, 0.800000012, 0\""
                    green_found)
                string(FIND "${generated}"
                    "<constant name=\"${background}\" type=\"color3\">\n      <input name=\"value\" type=\"color3\" value=\"0.800000012, 0, 0\""
                    red_found)
                if(NOT color_input OR NOT color_graph OR green_found EQUAL -1 OR red_found EQUAL -1)
                    message(FATAL_ERROR "weighted_thin_film does not preserve the weight 0 and 1 colors.")
                endif()
            endif()
            if(material MATCHES "^parameterized_scatter_(mode|both)$")
                if(NOT "${generated}" MATCHES "<input name=\"transmission\" type=\"float\" value=\"1.0\"")
                    message(FATAL_ERROR "${material} lost the default transmission mode.")
                endif()
                if(material STREQUAL "parameterized_scatter_both" AND
                   NOT "${generated}" MATCHES "<input name=\"specular\" type=\"float\" value=\"1.0\"")
                    message(FATAL_ERROR "${material} lost the default reflection mode.")
                endif()
                if(NOT "${stdout}${stderr}" MATCHES "enum parameter mode is fixed to its default")
                    message(FATAL_ERROR "${material} did not report the fixed enum parameter.")
                endif()
            endif()
            if(material STREQUAL "directional_glass" OR material STREQUAL "measured_curve_glass")
                if(NOT "${generated}" MATCHES "<input name=\"transmission\" type=\"float\" value=\"1.0\"")
                    message(FATAL_ERROR "${material} lost its underlying transmissive BSDF.")
                endif()
                if(NOT "${stdout}${stderr}" MATCHES "underlying BSDF is approximated")
                    message(FATAL_ERROR "${material} did not report its omitted factor.")
                endif()
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
       material STREQUAL "uv_function" OR
       material STREQUAL "normal_layer" OR
       material STREQUAL "normal_layer_uv" OR
       material STREQUAL "normal_geometry" OR
       material STREQUAL "colored_ggx_layers" OR
       material STREQUAL "ggx_layer_anisotropic" OR
       material STREQUAL "ggx_layer_isotropic" OR
       material MATCHES "^weighted_(thin_film|conductor|nested)$" OR
       material STREQUAL "parameterized_scatter_mode" OR
       material STREQUAL "parameterized_scatter_both" OR
       material STREQUAL "directional_glass" OR
       material STREQUAL "measured_curve_glass" OR
       material MATCHES "^ggx_anisotropic")
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

set(root_output "${OUTPUT_DIR}/root-image-output")
file(MAKE_DIRECTORY "${root_output}")
execute_process(
    COMMAND "${MATCONV_EXECUTABLE}" mtlx-to-mdl
        --input "${MDL_CASES_DIR}/../root_image.mtlx"
        --output "${root_output}/root_image.mdl"
        --material root_material
    RESULT_VARIABLE root_result
    OUTPUT_VARIABLE root_stdout
    ERROR_VARIABLE root_stderr
)
if(NOT root_result EQUAL 0)
    message(FATAL_ERROR "Root-level texture conversion failed:\n${root_stdout}${root_stderr}")
endif()
file(READ "${root_output}/root_image.mdl" root_mdl)
if(NOT root_mdl MATCHES "\\./textures/checker\\.png" OR
   NOT EXISTS "${root_output}/textures/checker.png")
    message(FATAL_ERROR "Root-level image was not packaged and referenced.")
endif()
file(SHA256 "${MDL_CASES_DIR}/textures/checker.png" source_hash)
file(SHA256 "${root_output}/textures/checker.png" packaged_hash)
if(NOT source_hash STREQUAL packaged_hash)
    message(FATAL_ERROR "Packaged root-level texture differs from its source.")
endif()
file(READ "${MDL_CASES_DIR}/../root_image.mtlx" missing_document)
string(REPLACE "mdl/textures/checker.png" "missing-texture.png"
    missing_document "${missing_document}")
file(WRITE "${root_output}/missing.mtlx" "${missing_document}")
execute_process(
    COMMAND "${MATCONV_EXECUTABLE}" mtlx-to-mdl
        --input "${root_output}/missing.mtlx"
        --output "${root_output}/missing.mdl"
        --material root_material
    RESULT_VARIABLE missing_result
    OUTPUT_VARIABLE missing_stdout
    ERROR_VARIABLE missing_stderr
)
if(missing_result EQUAL 0 OR
   NOT "${missing_stdout}${missing_stderr}" MATCHES "Could not resolve MaterialX texture resource")
    message(FATAL_ERROR "Missing root-level texture was not reported.")
endif()

set(strict_output "${OUTPUT_DIR}/strict-rejected.mtlx")
file(REMOVE "${strict_output}")
execute_process(
    COMMAND "${MATCONV_EXECUTABLE}" mdl-to-mtlx
        --input "${MDL_CASES_DIR}/parameterized_scatter_mode.mdl"
        --output "${strict_output}"
        --material parameterized_scatter_mode
        --mdl-path "${MDL_PATH}"
        --strict
    RESULT_VARIABLE strict_result
    OUTPUT_VARIABLE strict_stdout
    ERROR_VARIABLE strict_stderr
)
if(strict_result EQUAL 0 OR EXISTS "${strict_output}" OR
   NOT "${strict_stdout}${strict_stderr}" MATCHES "Strict conversion rejected" OR
   NOT "${strict_stdout}${strict_stderr}" MATCHES "enum parameter mode is fixed")
    message(FATAL_ERROR "Strict mode did not reject and report the lossy conversion.")
endif()
