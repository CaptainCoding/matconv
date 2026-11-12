#include "conversion.h"
#include "materialx_utils.h"
#include "mdl_runtime.h"

#include <MaterialXCore/Document.h>
#include <MaterialXCore/Node.h>
#include <MaterialXFormat/XmlIo.h>

#include <mi/base/handle.h>
#include <mi/neuraylib/idatabase.h>
#include <mi/neuraylib/iexpression.h>
#include <mi/neuraylib/ifunction_definition.h>
#include <mi/neuraylib/imdl_factory.h>
#include <mi/neuraylib/imdl_impexp_api.h>
#include <mi/neuraylib/imodule.h>
#include <mi/neuraylib/iscope.h>
#include <mi/neuraylib/istring.h>
#include <mi/neuraylib/itransaction.h>
#include <mi/neuraylib/ivalue.h>

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace matconv::detail {
namespace {

namespace mx = MaterialX;

struct ParameterTransferStats {
  std::size_t transferred = 0;
  std::size_t skipped = 0;
};

const char *materialx_input_for(const std::string &parameter_name) {
  if (parameter_name == "base_color")
    return "base_color";
  if (parameter_name == "roughness")
    return "specular_roughness";
  if (parameter_name == "metallic")
    return "metalness";
  if (parameter_name == "emission_color")
    return "emission_color";
  return nullptr;
}

void add_scalar(mx::NodePtr node, const char *name, float value) {
  node->addInput(name, "float")->setValue(value);
}

void add_color(mx::NodePtr node, const char *name, float red, float green,
               float blue) {
  node->addInput(name, "color3")->setValue(mx::Color3(red, green, blue));
}

ParameterTransferStats
transfer_parameters(const mi::neuraylib::IFunction_definition *definition,
                    const mx::NodePtr &surface) {
  ParameterTransferStats stats;
  mi::base::Handle<const mi::neuraylib::IExpression_list> defaults(
      definition->get_defaults());
  if (!defaults)
    return stats;

  for (mi::Size index = 0; index < definition->get_parameter_count(); ++index) {
    const char *parameter_name = definition->get_parameter_name(index);
    mi::base::Handle<const mi::neuraylib::IExpression> expression(
        parameter_name ? defaults->get_expression(parameter_name) : nullptr);
    if (!expression ||
        expression->get_kind() != mi::neuraylib::IExpression::EK_CONSTANT) {
      ++stats.skipped;
      continue;
    }

    mi::base::Handle<const mi::neuraylib::IValue> value(
        static_cast<const mi::neuraylib::IExpression_constant *>(
            expression.get())
            ->get_value());
    if (!value)
      continue;

    const char *target_input =
        materialx_input_for(parameter_name ? parameter_name : "");
    if (!target_input) {
      ++stats.skipped;
      continue;
    }

    if (value->get_kind() == mi::neuraylib::IValue::VK_FLOAT) {
      const auto *scalar =
          static_cast<const mi::neuraylib::IValue_float *>(value.get());
      add_scalar(surface, target_input, scalar->get_value());
      ++stats.transferred;
    } else if (value->get_kind() == mi::neuraylib::IValue::VK_COLOR) {
      const auto *color =
          static_cast<const mi::neuraylib::IValue_color *>(value.get());
      mi::base::Handle<const mi::neuraylib::IValue_float> red(
          color->get_value(0));
      mi::base::Handle<const mi::neuraylib::IValue_float> green(
          color->get_value(1));
      mi::base::Handle<const mi::neuraylib::IValue_float> blue(
          color->get_value(2));
      if (!red || !green || !blue)
        continue;
      add_color(surface, target_input, red->get_value(), green->get_value(),
                blue->get_value());
      ++stats.transferred;
    } else {
      ++stats.skipped;
    }
  }
  return stats;
}

std::string short_material_name(const char *mdl_definition_name) {
  std::string name = mdl_definition_name;
  const std::size_t signature = name.find('(');
  if (signature != std::string::npos)
    name.resize(signature);
  const std::size_t separator = name.rfind("::");
  if (separator != std::string::npos)
    name.erase(0, separator + 2);
  return name;
}

bool matches_requested_material(const Options &options,
                                const std::string &short_name,
                                const char *full_name) {
  return options.material.empty() || options.material == short_name ||
         options.material == full_name;
}

void add_material(mx::DocumentPtr document,
                  const mi::neuraylib::IFunction_definition *definition,
                  const std::string &name, ParameterTransferStats &stats) {
  const std::string safe_name = safe_material_name(name);
  mx::NodePtr surface = document->addNode(
      "standard_surface", safe_name + "_surface", "surfaceshader");
  mx::NodePtr material =
      document->addNode("surfacematerial", safe_name, "material");
  material->addInput("surfaceshader", "surfaceshader")
      ->setConnectedNode(surface);

  const ParameterTransferStats material_stats =
      transfer_parameters(definition, surface);
  stats.transferred += material_stats.transferred;
  stats.skipped += material_stats.skipped;
}

void validate_materialx_document(const mx::DocumentPtr &document) {
  std::string message;
  if (!document->validate(&message))
    throw std::runtime_error("Generated MaterialX document is invalid: " +
                             message);
}

void write_materialx_document(const mx::DocumentPtr &document,
                              const std::filesystem::path &output) {
  mx::XmlWriteOptions write_options;
  write_options.writeXIncludeEnable = false;
  write_options.elementPredicate = [](mx::ConstElementPtr element) {
    return element->getSourceUri().empty();
  };
  mx::writeToXmlFile(document, output.string(), &write_options);
}

} // namespace

int mdl_to_materialx(const Options &options) {
  MdlRuntime runtime(options);
  mi::neuraylib::INeuray *neuray = runtime.get();

  mi::base::Handle<mi::neuraylib::IMdl_impexp_api> mdl_impexp(
      neuray->get_api_component<mi::neuraylib::IMdl_impexp_api>());
  mi::base::Handle<mi::neuraylib::IMdl_factory> mdl_factory(
      neuray->get_api_component<mi::neuraylib::IMdl_factory>());
  if (!mdl_impexp || !mdl_factory) {
    throw std::runtime_error(
        "The MDL SDK runtime is missing required API components.");
  }

  mi::base::Handle<mi::neuraylib::IDatabase> database(
      neuray->get_api_component<mi::neuraylib::IDatabase>());
  if (!database)
    throw std::runtime_error("The MDL SDK runtime has no database component.");
  mi::base::Handle<mi::neuraylib::IScope> scope(database->get_global_scope());
  if (!scope)
    throw std::runtime_error("The MDL SDK runtime has no global scope.");
  mi::base::Handle<mi::neuraylib::ITransaction> transaction(
      scope->create_transaction());
  if (!transaction)
    throw std::runtime_error("Could not create an MDL SDK transaction.");

  mx::DocumentPtr document = mx::createDocument();
  ParameterTransferStats stats;
  std::size_t emitted_materials = 0;
  {
    const std::filesystem::path input =
        std::filesystem::absolute(options.input);
    mi::base::Handle<const mi::IString> mdl_module_name(
        mdl_impexp->get_mdl_module_name(input.string().c_str()));
    if (!mdl_module_name || !mdl_module_name->get_c_str()[0]) {
      throw std::runtime_error(
          "Could not derive an MDL module name from the input path.");
    }

    const std::string module_name = mdl_module_name->get_c_str();
    if (mdl_impexp->load_module(transaction.get(), module_name.c_str()) != 0) {
      throw std::runtime_error(
          "MDL SDK could not load module " + module_name +
          "; check the module and --mdl-path directories.");
    }

    mi::base::Handle<const mi::IString> database_module_name(
        mdl_factory->get_db_module_name(module_name.c_str()));
    if (!database_module_name)
      throw std::runtime_error("Could not resolve MDL module database name.");
    mi::base::Handle<const mi::neuraylib::IModule> module(
        transaction->access<mi::neuraylib::IModule>(
            database_module_name->get_c_str()));
    if (!module) {
      throw std::runtime_error(
          "MDL SDK did not expose the loaded module in its database.");
    }

    const std::size_t material_count = module->get_material_count();
    if (material_count == 0)
      throw std::runtime_error("The MDL module does not export any materials.");

    for (std::size_t index = 0; index < material_count; ++index) {
      const char *definition_name = module->get_material(index);
      mi::base::Handle<const mi::neuraylib::IFunction_definition> definition(
          transaction->access<mi::neuraylib::IFunction_definition>(
              definition_name));
      if (!definition)
        continue;

      const char *mdl_definition_name = definition->get_mdl_name();
      if (!mdl_definition_name)
        continue;

      const std::string name = short_material_name(mdl_definition_name);
      if (!matches_requested_material(options, name, mdl_definition_name))
        continue;

      add_material(document, definition.get(), name, stats);
      ++emitted_materials;
    }
  }

  if (emitted_materials == 0) {
    throw std::runtime_error(
        options.material.empty()
            ? "No MDL materials with supported constant parameters were found."
            : "No matching MDL material was found: " + options.material);
  }

  const mx::FileSearchPath search_path = materialx_search_path();
  load_materialx_libraries(document, search_path);
  validate_materialx_document(document);
  transaction->abort();
  write_materialx_document(document, options.output);

  std::cout << "Wrote MaterialX document: " << options.output << '\n'
            << "Transferred " << stats.transferred
            << " constant scalar/color parameters from " << emitted_materials
            << " material(s). "
               "The standard_surface result is not behaviorally equivalent "
               "to the source MDL.\n";
  if (stats.skipped != 0) {
    std::cout
        << "Warning: skipped " << stats.skipped
        << " unsupported or non-constant parameters; arbitrary "
           "expressions, textures, and BSDF behavior are not preserved.\n";
  }
  return 0;
}

} // namespace matconv::detail
