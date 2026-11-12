#pragma once

#include "converter.h"

namespace matconv::detail {

int mdl_to_materialx(const Options &options);
int materialx_to_mdl(const Options &options);

} // namespace matconv::detail
