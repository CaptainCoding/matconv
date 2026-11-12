#include "converter.h"

#include <iostream>

#if MATCONV_WITH_SDK
#include "conversion.h"
#endif

namespace matconv {

int convert(const Options &options) {
#if MATCONV_WITH_SDK
  if (options.direction == Direction::MdlToMaterialX)
    return detail::mdl_to_materialx(options);
  return detail::materialx_to_mdl(options);
#else
  (void)options;
  std::cerr << "Conversion requires the MDL SDK and MaterialX MDL generator. "
               "Reconfigure with -DMATCONV_FETCH_DEPENDENCIES=ON.\n";
  return 1;
#endif
}

} // namespace matconv
