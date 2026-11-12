#pragma once

#include "converter.h"

#include <mi/neuraylib/ineuray.h>

#include <memory>

namespace matconv::detail {

class MdlRuntime {
public:
  explicit MdlRuntime(const Options &options);
  MdlRuntime(const MdlRuntime &) = delete;
  MdlRuntime &operator=(const MdlRuntime &) = delete;
  ~MdlRuntime();

  mi::neuraylib::INeuray *get() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace matconv::detail
