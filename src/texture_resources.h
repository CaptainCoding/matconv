#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace matconv::detail {

class TextureResourcePackager {
public:
  explicit TextureResourcePackager(std::filesystem::path output_directory)
      : output_directory_(std::move(output_directory)) {}

  std::string package(const std::filesystem::path &source_path) {
    std::error_code error;
    std::filesystem::path source =
        std::filesystem::absolute(source_path, error).lexically_normal();
    if (error)
      throw std::runtime_error("Could not resolve texture path: " +
                               source_path.string() + ": " + error.message());
    if (!std::filesystem::is_regular_file(source, error) || error)
      throw std::runtime_error("Texture is not a readable file: " +
                               source.string());

    if (const auto found = packaged_paths_.find(source);
        found != packaged_paths_.end())
      return found->second;

    const std::filesystem::path texture_directory =
        output_directory_ / "textures";
    std::filesystem::create_directories(texture_directory, error);
    if (error)
      throw std::runtime_error("Could not create texture directory: " +
                               texture_directory.string() + ": " +
                               error.message());

    const std::filesystem::path source_filename = source.filename();
    const std::filesystem::path stem = source.stem();
    const std::filesystem::path extension = source.extension();
    std::filesystem::path destination;
    for (std::size_t suffix = 0;; ++suffix) {
      const std::string filename =
          suffix == 0 ? source_filename.string()
                      : stem.string() + "_" + std::to_string(suffix) +
                            extension.string();
      destination = texture_directory / filename;

      const bool exists = std::filesystem::exists(destination, error);
      if (error)
        throw std::runtime_error("Could not inspect texture destination: " +
                                 destination.string() + ": " + error.message());
      if (!exists)
        break;

      if (std::filesystem::is_regular_file(destination, error) && !error &&
          files_are_equal(source, destination)) {
        const std::string packaged_path =
            (std::filesystem::path("textures") / destination.filename())
                .generic_string();
        packaged_paths_.emplace(source, packaged_path);
        return packaged_path;
      }

      if (suffix == std::numeric_limits<std::size_t>::max())
        throw std::runtime_error("Could not find a unique name for texture: " +
                                 source.string());
    }

    std::filesystem::copy_file(source, destination,
                               std::filesystem::copy_options::none, error);
    if (error)
      throw std::runtime_error("Could not copy texture to " +
                               destination.string() + ": " + error.message());

    const std::string packaged_path =
        (std::filesystem::path("textures") / destination.filename())
            .generic_string();
    packaged_paths_.emplace(source, packaged_path);
    return packaged_path;
  }

private:
  static bool files_are_equal(const std::filesystem::path &left,
                              const std::filesystem::path &right) {
    std::error_code left_error;
    std::error_code right_error;
    if (std::filesystem::file_size(left, left_error) !=
            std::filesystem::file_size(right, right_error) ||
        left_error || right_error)
      return false;

    std::ifstream left_file(left, std::ios::binary);
    std::ifstream right_file(right, std::ios::binary);
    if (!left_file || !right_file)
      return false;
    return std::equal(std::istreambuf_iterator<char>(left_file),
                      std::istreambuf_iterator<char>(),
                      std::istreambuf_iterator<char>(right_file));
  }

  std::filesystem::path output_directory_;
  std::map<std::filesystem::path, std::string> packaged_paths_;
};

} // namespace matconv::detail
