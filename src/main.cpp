#include "command_line.h"

#include <exception>
#include <filesystem>
#include <iostream>

int main(int argc, char *argv[]) {
  const matconv::CommandLineResult command_line =
      matconv::parse_command_line(argc, argv);

  switch (command_line.action) {
  case matconv::CommandLineAction::Help:
    matconv::print_help();
    return 0;
  case matconv::CommandLineAction::Version:
    std::cout << MATCONV_VERSION << '\n';
    return 0;
  case matconv::CommandLineAction::Error:
    std::cerr << command_line.error << '\n';
    return 2;
  case matconv::CommandLineAction::Convert:
    break;
  }

  if (!std::filesystem::is_regular_file(command_line.options.input)) {
    std::cerr << "Input file does not exist or is not a regular file: "
              << command_line.options.input << '\n';
    return 2;
  }

  try {
    return matconv::convert(command_line.options);
  } catch (const std::exception &error) {
    std::cerr << "Conversion failed: " << error.what() << '\n';
    return 1;
  }
}
