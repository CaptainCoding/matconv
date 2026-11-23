#include "diagnostic_texture.h"

#include <OpenImageIO/imageio.h>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}

void test_image(const std::filesystem::path &directory, int channels,
                const std::vector<std::string> &names,
                const std::vector<float> &source,
                const std::vector<float> &expected) {
  const auto path = directory / ("channels_" + std::to_string(channels) + ".exr");
  OIIO::ImageSpec spec(2, 2, channels, OIIO::TypeDesc::FLOAT);
  spec.channelnames = names;
  spec.alpha_channel = (channels == 2 ? 1 : channels >= 4 ? 3 : -1);
  auto output = OIIO::ImageOutput::create(path.string());
  check(output && output->open(path.string(), spec), "Could not create " + path.string());
  check(output->write_image(OIIO::TypeDesc::FLOAT, source.data()),
        "Could not write " + path.string());
  check(output->close(), "Could not close " + path.string());
  auto input = OIIO::ImageInput::open(path.string());
  check(static_cast<bool>(input), "Could not open " + path.string());
  const auto actual = matconv::diagnostic::read_texture(*input, true);
  check(actual.size() == expected.size(), "Unexpected RGBA output size.");
  for (std::size_t i = 0; i < actual.size(); ++i)
    check(std::abs(actual[i] - expected[i]) < 0.0001f,
          "Unexpected channel value for " + path.string() + " at " + std::to_string(i));
  const auto gamma = matconv::diagnostic::read_texture(*input, false);
  for (std::size_t i = 0; i < gamma.size(); ++i) {
    const float value = i % 4 == 3 ? expected[i] : std::pow(expected[i], 2.2f);
    check(std::abs(gamma[i] - value) < 0.0001f,
          "Unexpected gamma or alpha value for " + path.string());
  }
  check(input->close(), "Could not close " + path.string());
  bool rejected = false;
  try {
    matconv::diagnostic::read_texture(*input, true);
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  check(rejected, "Reading a closed image did not fail.");
}

void test_unsupported_image(const std::filesystem::path &directory) {
  const auto path = directory / "unsupported_channels.exr";
  OIIO::ImageSpec spec(1, 1, 5, OIIO::TypeDesc::FLOAT);
  spec.channelnames = {"R", "G", "B", "A", "Z"};
  spec.alpha_channel = 3;
  const float pixel[] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f};
  auto output = OIIO::ImageOutput::create(path.string());
  check(output && output->open(path.string(), spec) &&
            output->write_image(OIIO::TypeDesc::FLOAT, pixel) && output->close(),
        "Could not create unsupported-channel test image.");
  auto input = OIIO::ImageInput::open(path.string());
  check(static_cast<bool>(input), "Could not open unsupported-channel test image.");
  bool rejected = false;
  try {
    matconv::diagnostic::read_texture(*input, true);
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  check(rejected, "Unsupported image channels were not rejected.");
}

} // namespace

int main(int argc, char **argv) {
  try {
    check(argc == 2, "Expected a test output directory.");
    const std::filesystem::path directory(argv[1]);
    std::filesystem::create_directories(directory);
    test_image(directory, 1, {"Y"}, {0.1f, 0.2f, 0.3f, 0.4f},
               {0.3f, 0.3f, 0.3f, 1, 0.4f, 0.4f, 0.4f, 1,
                0.1f, 0.1f, 0.1f, 1, 0.2f, 0.2f, 0.2f, 1});
    test_image(directory, 2, {"Y", "A"},
               {0.1f, 0.9f, 0.2f, 0.8f, 0.3f, 0.7f, 0.4f, 0.6f},
               {0.3f, 0.3f, 0.3f, 0.7f, 0.4f, 0.4f, 0.4f, 0.6f,
                0.1f, 0.1f, 0.1f, 0.9f, 0.2f, 0.2f, 0.2f, 0.8f});
    test_image(directory, 3, {"R", "G", "B"},
               {0.1f, 0.2f, 0.3f, 0.2f, 0.3f, 0.4f,
                0.3f, 0.4f, 0.5f, 0.4f, 0.5f, 0.6f},
               {0.3f, 0.4f, 0.5f, 1, 0.4f, 0.5f, 0.6f, 1,
                0.1f, 0.2f, 0.3f, 1, 0.2f, 0.3f, 0.4f, 1});
    test_image(directory, 4, {"R", "G", "B", "A"},
               {0.1f, 0.2f, 0.3f, 0.9f, 0.2f, 0.3f, 0.4f, 0.8f,
                0.3f, 0.4f, 0.5f, 0.7f, 0.4f, 0.5f, 0.6f, 0.6f},
               {0.3f, 0.4f, 0.5f, 0.7f, 0.4f, 0.5f, 0.6f, 0.6f,
                0.1f, 0.2f, 0.3f, 0.9f, 0.2f, 0.3f, 0.4f, 0.8f});
    test_image(directory, 3, {"R", "G", "B"},
               {0.1f, 0.1f, 0.1f, 0.2f, 0.2f, 0.2f,
                0.3f, 0.3f, 0.3f, 0.4f, 0.4f, 0.4f},
               {0.3f, 0.3f, 0.3f, 1, 0.4f, 0.4f, 0.4f, 1,
                0.1f, 0.1f, 0.1f, 1, 0.2f, 0.2f, 0.2f, 1});
    test_unsupported_image(directory);
    return 0;
  } catch (const std::runtime_error &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
