cmake_minimum_required(VERSION 3.20)

project(matconv VERSION 0.1.0 LANGUAGES CXX)

option(MATCONV_FETCH_DEPENDENCIES "Fetch the MDL SDK and MaterialX from GitHub" OFF)
option(MATCONV_DIAGNOSTIC_RENDERER "Build the adapted MDL Core diagnostic renderer" OFF)

if(MATCONV_DIAGNOSTIC_RENDERER AND NOT MATCONV_FETCH_DEPENDENCIES)
    message(FATAL_ERROR "The diagnostic renderer requires MATCONV_FETCH_DEPENDENCIES=ON.")
endif()

if(MATCONV_FETCH_DEPENDENCIES)
    if(CMAKE_VERSION VERSION_LESS 3.26)
        message(FATAL_ERROR "MATCONV_FETCH_DEPENDENCIES requires CMake 3.26 or newer")
    endif()

    include(FetchContent)

    if(VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
        set(Boost_ROOT "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}"
            CACHE PATH "vcpkg Boost headers for the MDL SDK" FORCE)
    endif()
    set(MDL_BUILD_SDK ON CACHE BOOL "Build the MDL SDK" FORCE)
    set(MDL_BUILD_SDK_EXAMPLES OFF CACHE BOOL "Build MDL SDK examples")
    set(MDL_BUILD_CORE_EXAMPLES OFF CACHE BOOL "Build MDL Core examples")
    set(MDL_BUILD_ARNOLD_PLUGIN OFF CACHE BOOL "Build the MDL Arnold plugin")
    set(MDL_BUILD_DDS_PLUGIN OFF CACHE BOOL "Build the MDL DDS image plugin")
    set(MDL_BUILD_OPENIMAGEIO_PLUGIN ON CACHE BOOL "Build the MDL OpenImageIO image plugin" FORCE)
    set(MDL_BUILD_DOCUMENTATION OFF CACHE BOOL "Build MDL SDK documentation")
    set(MDL_ENABLE_UNIT_TESTS OFF CACHE BOOL "Build MDL SDK tests")

    set(MATERIALX_BUILD_GEN_GLSL OFF CACHE BOOL "Build the MaterialX GLSL generator")
    set(MATERIALX_BUILD_GEN_OSL OFF CACHE BOOL "Build the MaterialX OSL generator")
    set(MATERIALX_BUILD_GEN_MDL ON CACHE BOOL "Build the MaterialX MDL generator")
    set(MATERIALX_BUILD_GEN_MSL OFF CACHE BOOL "Build the MaterialX MSL generator")
    set(MATERIALX_BUILD_GEN_SLANG OFF CACHE BOOL "Build the MaterialX Slang generator")
    set(MATERIALX_BUILD_RENDER OFF CACHE BOOL "Build MaterialX render modules")
    set(MATERIALX_BUILD_RENDER_PLATFORMS OFF CACHE BOOL "Build MaterialX platform render modules")
    set(MATERIALX_BUILD_TESTS OFF CACHE BOOL "Build MaterialX tests")
    set(MATERIALX_BUILD_DATA_LIBRARY OFF CACHE BOOL "Build the MaterialX data library")

    set(_mdl_sdk_source_dir "${CMAKE_BINARY_DIR}/_deps/matconv_mdl_sdk-src")

    find_package(OpenImageIO CONFIG QUIET)
    if(NOT OpenImageIO_FOUND)
        message(FATAL_ERROR
            "The opt-in MDL build requires OpenImageIO. Configure with the "
            "'mdl-with-image-plugin' preset and VCPKG_ROOT set.")
    elseif(OpenImageIO_VERSION VERSION_LESS 2.4
            OR NOT OpenImageIO_VERSION VERSION_LESS 4.0)
        message(FATAL_ERROR
            "OpenImageIO ${OpenImageIO_VERSION} is unsupported; MDL SDK 2026 "
            "requires OpenImageIO >= 2.4 and < 4.0.")
    endif()

    find_package(OpenEXR CONFIG QUIET)
    if(NOT OpenEXR_FOUND)
        message(FATAL_ERROR
            "The opt-in MDL build requires OpenEXR. Configure with the "
            "'mdl-with-image-plugin' preset and VCPKG_ROOT set.")
    endif()

    FetchContent_Declare(
        matconv_mdl_sdk
        GIT_REPOSITORY https://github.com/NVIDIA/MDL-SDK.git
        GIT_TAG 2026.0.2
        GIT_SHALLOW TRUE
        SOURCE_DIR "${_mdl_sdk_source_dir}"
    )
    set(MDL_BASE_FOLDER "${_mdl_sdk_source_dir}" CACHE PATH "MDL SDK source directory" FORCE)
    set(MDL_INCLUDE_FOLDER "${_mdl_sdk_source_dir}/include" CACHE PATH "MDL SDK public headers" FORCE)
    set(MDL_SRC_FOLDER "${_mdl_sdk_source_dir}/src" CACHE PATH "MDL SDK source files" FORCE)
    set(MDL_EXAMPLES_FOLDER "${_mdl_sdk_source_dir}/examples" CACHE PATH "MDL SDK examples" FORCE)
    set(MDL_DOC_FOLDER "${_mdl_sdk_source_dir}/doc" CACHE PATH "MDL SDK documentation" FORCE)
    FetchContent_MakeAvailable(matconv_mdl_sdk)

    if(MATCONV_DIAGNOSTIC_RENDERER)
        if(NOT TARGET examples-mdl_core-df_native)
            message(FATAL_ERROR
                "The diagnostic renderer requires MDL_BUILD_CORE_EXAMPLES=ON.")
        endif()
        execute_process(COMMAND "${clang_PATH}" --version
            RESULT_VARIABLE _clang_result OUTPUT_VARIABLE _clang_version)
        if(NOT _clang_result EQUAL 0 OR
           NOT _clang_version MATCHES "clang version 12[.]")
            message(FATAL_ERROR
                "The diagnostic renderer requires Clang 12 for the MDL SDK's "
                "LLVM 12 bitcode. Set clang_PATH to a Clang 12 executable and "
                "use a fresh build directory.")
        endif()
        set(_diagnostic_dir "${CMAKE_CURRENT_BINARY_DIR}/diagnostic-renderer")
        file(MAKE_DIRECTORY "${_diagnostic_dir}")
        set(_diagnostic_header "${_diagnostic_dir}/texture_support_native.h")
        set(_diagnostic_source "${_diagnostic_dir}/diagnostic_example_df_native.cpp")
        set(_sdk_texture_header "${_mdl_sdk_source_dir}/examples/mdl_core/shared/texture_support_native.h")
        set(_sdk_renderer_source "${_mdl_sdk_source_dir}/examples/mdl_core/df_native/example_df_native.cpp")
        add_custom_command(
            OUTPUT "${_diagnostic_header}" "${_diagnostic_source}"
            COMMAND "${CMAKE_COMMAND}"
                "-DSDK_HEADER=${_sdk_texture_header}"
                "-DSDK_SOURCE=${_sdk_renderer_source}"
                "-DOUTPUT_HEADER=${_diagnostic_header}"
                "-DOUTPUT_SOURCE=${_diagnostic_source}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/df_native/prepare_texture_header.cmake"
            DEPENDS "${_sdk_texture_header}" "${_sdk_renderer_source}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/df_native/prepare_texture_header.cmake"
            VERBATIM
        )
        add_custom_target(matconv_diagnostic_sources
            DEPENDS "${_diagnostic_header}" "${_diagnostic_source}")
        add_dependencies(examples-mdl_core-df_native matconv_diagnostic_sources)
        set_property(TARGET examples-mdl_core-df_native PROPERTY SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/df_native/renderer.cpp")
        target_include_directories(examples-mdl_core-df_native BEFORE PRIVATE
            "${_diagnostic_dir}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/df_native")
        set_property(TARGET examples-mdl_core-df_native APPEND PROPERTY BUILD_RPATH
            "$<TARGET_FILE_DIR:prod-lib-mdl_core>")
    endif()

    FetchContent_Declare(
        matconv_materialx
        GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/MaterialX.git
        GIT_TAG v1.39.5
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(matconv_materialx)

    set(MATCONV_WITH_SDK ON)
endif()

add_executable(matconv
    src/command_line.cpp
    src/main.cpp
    src/converter.cpp
)

target_compile_features(matconv PRIVATE cxx_std_17)
target_compile_definitions(matconv PRIVATE MATCONV_VERSION="${PROJECT_VERSION}")

if(MATCONV_WITH_SDK)
    target_sources(matconv PRIVATE
        src/mdl_runtime.cpp
        src/mdl_to_materialx.cpp
        src/mdl_expression_translator.cpp
        src/mdl_bsdf_translation.cpp
        src/mdl_native_bsdf_translation.cpp
        src/mdl_texture_translation.cpp
        src/mdl_uv_inline.cpp
        src/mdl_translation_utils.cpp
        src/materialx_to_mdl.cpp
    )
    target_compile_definitions(matconv PRIVATE
        MATCONV_WITH_SDK=1
        MATCONV_MDL_SDK_MODULE="$<TARGET_FILE:mdl::mdl_sdk>"
        MATCONV_MDL_OPENIMAGEIO_PLUGIN="$<TARGET_FILE:shaders-plugin-openimageio>"
        MATCONV_MDL_STANDARD_PATH="${_mdl_sdk_source_dir}/src/shaders/mdl"
        MATCONV_MATERIALX_MDL_PATH="${matconv_materialx_SOURCE_DIR}/source/MaterialXGenMdl/mdl"
        MATCONV_MATERIALX_SOURCE_DIR="${matconv_materialx_SOURCE_DIR}"
    )
    target_include_directories(matconv PRIVATE "${MDL_INCLUDE_FOLDER}")
    target_link_libraries(matconv PRIVATE
        MaterialXCore
        MaterialXFormat
        MaterialXGenShader
        MaterialXGenMdl
        ${CMAKE_DL_LIBS}
    )
    add_dependencies(matconv mdl::mdl_sdk shaders-plugin-openimageio)
else()
    target_compile_definitions(matconv PRIVATE MATCONV_WITH_SDK=0)
endif()

if(MSVC)
    target_compile_options(matconv PRIVATE /W4 /permissive-)
else()
    target_compile_options(matconv PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    add_test(NAME matconv_help COMMAND matconv --help)
    add_test(NAME matconv_version COMMAND matconv --version)
    add_test(NAME matconv_subcommand_help COMMAND matconv mdl-to-mtlx --help)
    add_test(NAME matconv_rejects_unknown_argument COMMAND matconv --not-a-command)
    set_tests_properties(matconv_rejects_unknown_argument PROPERTIES WILL_FAIL TRUE)
    add_test(NAME matconv_rejects_mismatched_extensions
        COMMAND matconv mdl-to-mtlx --input input.mtlx --output output.mtlx
    )
    set_tests_properties(matconv_rejects_mismatched_extensions PROPERTIES WILL_FAIL TRUE)
    add_test(NAME matconv_rejects_missing_option_value
        COMMAND matconv mdl-to-mtlx --input
    )
    set_tests_properties(matconv_rejects_missing_option_value PROPERTIES WILL_FAIL TRUE)
    add_test(NAME matconv_requires_input_and_output
        COMMAND matconv mdl-to-mtlx --input input.mdl
    )
    set_tests_properties(matconv_requires_input_and_output PROPERTIES WILL_FAIL TRUE)
    add_test(NAME matconv_rejects_mdl_path_for_mtlx_input
        COMMAND matconv mtlx-to-mdl
            --input input.mtlx --output output.mdl --mdl-path modules
    )
    set_tests_properties(matconv_rejects_mdl_path_for_mtlx_input PROPERTIES WILL_FAIL TRUE)
    add_test(NAME matconv_rejects_strict_for_mtlx_input
        COMMAND matconv mtlx-to-mdl
            --input input.mtlx --output output.mdl --strict
    )
    set_tests_properties(matconv_rejects_strict_for_mtlx_input PROPERTIES WILL_FAIL TRUE)
    if(MATCONV_WITH_SDK)
        add_test(NAME matconv_mdl_cases
            COMMAND "${CMAKE_COMMAND}"
                "-DMATCONV_EXECUTABLE=$<TARGET_FILE:matconv>"
                "-DMDL_CASES_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/data/mdl"
                "-DMDL_PATH=${CMAKE_CURRENT_SOURCE_DIR}/tests/data/mdl"
                "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/mdl-cases"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_mdl_cases.cmake"
        )
    endif()
    if(MATCONV_DIAGNOSTIC_RENDERER)
        add_executable(matconv_diagnostic_texture_test
            tests/diagnostic_texture_test.cpp)
        target_compile_features(matconv_diagnostic_texture_test PRIVATE cxx_std_17)
        target_include_directories(matconv_diagnostic_texture_test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/df_native")
        target_link_libraries(matconv_diagnostic_texture_test PRIVATE OpenImageIO::OpenImageIO)
        add_test(NAME matconv_diagnostic_texture
            COMMAND matconv_diagnostic_texture_test
                    "${CMAKE_CURRENT_BINARY_DIR}/diagnostic-texture-test")
    endif()
endif()
