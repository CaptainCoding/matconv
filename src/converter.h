#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace matconv {

enum class Direction { MdlToMaterialX, MaterialXToMdl };

struct Options {
  Direction direction = Direction::MdlToMaterialX;
  std::filesystem::path input;
  std::filesystem::path output;
  std::string material;
  std::vector<std::filesystem::path> mdl_paths;
  bool strict = false;
};

int convert(const Options &options);

} // namespace matconv
