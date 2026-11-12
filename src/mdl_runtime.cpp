#include "mdl_runtime.h"

#include <mi/base/handle.h>
#include <mi/neuraylib/factory.h>
#include <mi/neuraylib/imdl_configuration.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace matconv::detail {
namespace {

class MdlSdkModule {
public:
  using Factory = mi::base::IInterface *(*)(const mi::base::Uuid &);

  explicit MdlSdkModule(const std::filesystem::path &path) {
#if defined(_WIN32)
    handle_ = LoadLibraryExW(path.wstring().c_str(), nullptr,
                             LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!handle_) {
      throw std::runtime_error(
          "Could not load MDL SDK module: " + path.string() +
          " (Windows error " + std::to_string(GetLastError()) + ")");
    }

    const auto symbol =
        GetProcAddress(static_cast<HMODULE>(handle_), "mi_factory");
    if (!symbol) {
      const DWORD error = GetLastError();
      FreeLibrary(static_cast<HMODULE>(handle_));
      handle_ = nullptr;
      throw std::runtime_error(
          "MDL SDK module does not export mi_factory (Windows error " +
          std::to_string(error) + ")");
    }
#else
    handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle_) {
      const char *error = dlerror();
      throw std::runtime_error(
          "Could not load MDL SDK module: " + path.string() + " (" +
          (error ? error : "unknown loader error") + ")");
    }

    dlerror();
    void *symbol = dlsym(handle_, "mi_factory");
    const char *error = dlerror();
    if (error || !symbol) {
      const std::string detail = error ? error : "symbol not found";
      dlclose(handle_);
      handle_ = nullptr;
      throw std::runtime_error("MDL SDK module does not export mi_factory: " +
                               detail);
    }
#endif
    factory_ = reinterpret_cast<Factory>(symbol);
  }

  MdlSdkModule(const MdlSdkModule &) = delete;
  MdlSdkModule &operator=(const MdlSdkModule &) = delete;

  ~MdlSdkModule() {
#if defined(_WIN32)
    if (handle_)
      FreeLibrary(static_cast<HMODULE>(handle_));
#else
    if (handle_)
      dlclose(handle_);
#endif
  }

  mi::base::IInterface *create_runtime() const {
    return factory_(mi::neuraylib::INeuray::IID());
  }

private:
#if defined(_WIN32)
  HMODULE handle_ = nullptr;
#else
  void *handle_ = nullptr;
#endif
  Factory factory_ = nullptr;
};

mi::base::Handle<mi::neuraylib::INeuray>
create_neuray(const MdlSdkModule &module) {
  mi::base::Handle<mi::base::IInterface> root(module.create_runtime());
  if (!root)
    throw std::runtime_error("Could not create an MDL SDK runtime instance.");

  mi::base::Handle<mi::neuraylib::INeuray> neuray(
      root->get_interface<mi::neuraylib::INeuray>());
  if (!neuray) {
    throw std::runtime_error(
        "MDL SDK factory did not provide the INeuray interface.");
  }
  return neuray;
}

void add_search_path(mi::neuraylib::IMdl_configuration *configuration,
                     const std::filesystem::path &path) {
  const std::string path_string = path.string();
  if (configuration->add_mdl_path(path_string.c_str()) != 0)
    throw std::runtime_error("Could not add MDL search path: " + path_string);
  if (configuration->add_resource_path(path_string.c_str()) != 0)
    throw std::runtime_error("Could not add MDL resource path: " + path_string);
}

} // namespace

class MdlRuntime::Impl {
public:
  explicit Impl(const Options &options)
      : module_(MATCONV_MDL_SDK_MODULE), neuray_(create_neuray(module_)) {
    configure_paths(options);
    if (neuray_->start() != 0)
      throw std::runtime_error("Could not start the MDL SDK runtime.");
  }

  ~Impl() {
    if (neuray_ && neuray_->shutdown() != 0)
      std::cerr << "Warning: MDL SDK shutdown reported a failure.\n";
  }

  mi::neuraylib::INeuray *get() const { return neuray_.get(); }

private:
  void configure_paths(const Options &options) {
    mi::base::Handle<mi::neuraylib::IMdl_configuration> configuration(
        neuray_->get_api_component<mi::neuraylib::IMdl_configuration>());
    if (!configuration) {
      throw std::runtime_error(
          "The MDL SDK runtime has no configuration component.");
    }

    for (const auto &path : options.mdl_paths) {
      if (!std::filesystem::is_directory(path)) {
        throw std::runtime_error("MDL search path is not a directory: " +
                                 path.string());
      }
      add_search_path(configuration.get(), path);
    }

    const std::filesystem::path input =
        std::filesystem::absolute(options.input);
    add_search_path(configuration.get(), input.parent_path());
    if (configuration->add_mdl_path(MATCONV_MDL_STANDARD_PATH) != 0)
      throw std::runtime_error(
          "Could not add the MDL SDK standard module path.");
    configuration->add_mdl_system_paths();
  }

  MdlSdkModule module_;
  mi::base::Handle<mi::neuraylib::INeuray> neuray_;
};

MdlRuntime::MdlRuntime(const Options &options)
    : impl_(std::make_unique<Impl>(options)) {}

MdlRuntime::~MdlRuntime() = default;

mi::neuraylib::INeuray *MdlRuntime::get() const { return impl_->get(); }

} // namespace matconv::detail
