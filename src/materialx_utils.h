#pragma once

#include <MaterialXCore/Document.h>
#include <MaterialXFormat/Util.h>

#include <cctype>
#include <string>
#include <string_view>

namespace matconv::detail {

namespace mx = MaterialX;

inline mx::FileSearchPath materialx_search_path() {
  mx::FileSearchPath search_path = mx::getDefaultDataSearchPath();
  search_path.append(mx::FilePath(MATCONV_MATERIALX_SOURCE_DIR));
  return search_path;
}

inline void load_materialx_libraries(const mx::DocumentPtr &document,
                                     const mx::FileSearchPath &search_path) {
  mx::loadLibraries({"libraries"}, search_path, document);
}

inline std::string safe_material_name(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const unsigned char character : value) {
    result.push_back((std::isalnum(character) || character == '_')
                         ? static_cast<char>(character)
                         : '_');
  }
  if (result.empty() ||
      std::isdigit(static_cast<unsigned char>(result.front()))) {
    result.insert(result.begin(), '_');
  }
  return result;
}

} // namespace matconv::detail
