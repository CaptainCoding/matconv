#pragma once

#include "converter.h"

#include <string>

namespace matconv {

enum class CommandLineAction { Convert, Help, Version, Error };

struct CommandLineResult {
  CommandLineAction action = CommandLineAction::Error;
  Options options{};
  std::string error;
};

CommandLineResult parse_command_line(int argc, char *argv[]);
void print_help();

} // namespace matconv
