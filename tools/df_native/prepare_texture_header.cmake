file(READ "${SDK_HEADER}" source)
set(start_marker "        const OIIO::ImageSpec& spec = image->spec();")
set(end_marker "    // Loading texture from BSDF meta data")
string(FIND "${source}" "${start_marker}" start)
string(FIND "${source}" "${end_marker}" end)
if(start LESS 0 OR end LESS_EQUAL start)
    message(FATAL_ERROR "Unsupported MDL SDK texture helper; cannot apply diagnostic image adapter.")
endif()
string(SUBSTRING "${source}" 0 ${start} prefix)
string(SUBSTRING "${source}" ${end} -1 suffix)
set(replacement [=[
        if (!image)
            throw std::runtime_error("Diagnostic renderer: missing image.");
        const OIIO::ImageSpec& spec = image->spec();
        size.x = spec.width;
        size.y = spec.height;
        size.z = spec.depth;
        image_data = matconv::diagnostic::read_texture(
            *image, gamma_mode == mi::mdl::IValue_texture::gamma_linear);
        data = image_data.data();
    }

]=])
file(WRITE "${OUTPUT_HEADER}" "#include \"diagnostic_texture.h\"\n${prefix}${replacement}${suffix}")

file(READ "${SDK_SOURCE}" example)
set(entry "int MAIN_UTF8(int argc, char* argv[])")
set(windows_entry "COMMANDLINE_TO_UTF8")
string(FIND "${example}" "${entry}" entry_position)
string(FIND "${example}" "${windows_entry}" windows_position)
if(entry_position LESS 0 OR windows_position LESS entry_position)
    message(FATAL_ERROR "Unsupported MDL SDK renderer entry point.")
endif()
string(REPLACE "${entry}" "static int sdk_df_native_main(int argc, char* argv[])" example "${example}")
string(REPLACE "${windows_entry}" "" example "${example}")
file(WRITE "${OUTPUT_SOURCE}" "${example}")
