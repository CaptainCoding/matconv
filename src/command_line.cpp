#include "command_line.h"

#include <iostream>
#include <string_view>
#include <utility>

namespace matconv {
namespace {

constexpr std::string_view version = MATCONV_VERSION;

bool has_help_argument(int argc, char *argv[]) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--help" || argument == "-h")
      return true;
  }
  return false;
}

bool take_value(int &index, int argc, char *argv[], std::string &value,
                std::string &error) {
  if (index + 1 >= argc) {
    error = "Missing value after ";
    error += argv[index];
    return false;
  }

  value = argv[++index];
  return true;
}

CommandLineResult error_result(std::string message) {
  CommandLineResult result;
  result.action = CommandLineAction::Error;
  result.error = std::move(message);
  return result;
}

bool validate_options(const Options &options, bool has_input, bool has_output,
                      std::string &error) {
  if (!has_input || !has_output) {
    error = "Both --input and --output are required.";
    return false;
  }

  const bool mdl_input = options.direction == Direction::MdlToMaterialX;
  const auto expected_input_extension = mdl_input ? ".mdl" : ".mtlx";
  const auto expected_output_extension = mdl_input ? ".mtlx" : ".mdl";
  if (options.input.extension() != expected_input_extension ||
      options.output.extension() != expected_output_extension) {
    error = "File extensions do not match the selected conversion direction.";
    return false;
  }

  if (!mdl_input && !options.mdl_paths.empty()) {
    error = "--mdl-path is only valid for mdl-to-mtlx.";
    return false;
  }
  if (!mdl_input && options.strict) {
    error = "--strict is only valid for mdl-to-mtlx.";
    return false;
  }
  return true;
}

} // namespace

void print_help() {
  std::cout
      << "matconv " << version << "\n"
      << "Convert materials between NVIDIA MDL and MaterialX.\n\n"
      << "Usage:\n"
      << "  matconv mdl-to-mtlx --input FILE.mdl --output FILE.mtlx "
         "[options]\n"
      << "  matconv mtlx-to-mdl --input FILE.mtlx --output FILE.mdl "
         "[options]\n\n"
      << "Options:\n"
      << "  --material NAME       Select an exported MDL material or "
         "MaterialX renderable.\n"
      << "  --mdl-path DIR        Add an MDL module/resource search path; "
         "repeatable.\n"
      << "  --strict              Fail on conversion warnings (mdl-to-mtlx).\n"
      << "  -h, --help            Show this help.\n"
      << "  --version             Show the version.\n\n"
      << "Dependencies are fetched and built with:\n"
      << "  cmake -S . -B build -DMATCONV_FETCH_DEPENDENCIES=ON\n";
}

CommandLineResult parse_command_line(int argc, char *argv[]) {
  if (argc < 2 || has_help_argument(argc, argv)) {
    CommandLineResult result;
    result.action = CommandLineAction::Help;
    return result;
  }

  if (argc == 2 && std::string_view(argv[1]) == "--version") {
    CommandLineResult result;
    result.action = CommandLineAction::Version;
    return result;
  }

  Options options{};
  const std::string_view command = argv[1];
  if (command == "mdl-to-mtlx")
    options.direction = Direction::MdlToMaterialX;
  else if (command == "mtlx-to-mdl")
    options.direction = Direction::MaterialXToMdl;
  else
    return error_result("Unknown command: " + std::string(command));

  bool has_input = false;
  bool has_output = false;
  for (int index = 2; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--strict") {
      options.strict = true;
      continue;
    }
    if (argument != "--input" && argument != "--output" &&
        argument != "--material" && argument != "--mdl-path") {
      return error_result("Unknown option: " + std::string(argument));
    }

    std::string value;
    std::string error;
    if (!take_value(index, argc, argv, value, error))
      return error_result(std::move(error));

    if (argument == "--input") {
      options.input = value;
      has_input = true;
    } else if (argument == "--output") {
      options.output = value;
      has_output = true;
    } else if (argument == "--material") {
      options.material = value;
    } else {
      options.mdl_paths.emplace_back(value);
    }
  }

  std::string error;
  if (!validate_options(options, has_input, has_output, error))
    return error_result(std::move(error));

  CommandLineResult result;
  result.action = CommandLineAction::Convert;
  result.options = std::move(options);
  return result;
}

} // namespace matconv
