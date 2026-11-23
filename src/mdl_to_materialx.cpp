#include "conversion.h"
#include "materialx_utils.h"
#include "mdl_expression_translator.h"
#include "mdl_runtime.h"
#include "texture_resources.h"

#include <MaterialXCore/Document.h>
#include <MaterialXCore/Node.h>
#include <MaterialXFormat/XmlIo.h>

#include <mi/base/handle.h>
#include <mi/neuraylib/icompiled_material.h>
#include <mi/neuraylib/idatabase.h>
#include <mi/neuraylib/iexpression.h>
#include <mi/neuraylib/ifunction_definition.h>
#include <mi/neuraylib/ifunction_call.h>
#include <mi/neuraylib/imaterial_instance.h>
#include <mi/neuraylib/imdl_execution_context.h>
#include <mi/neuraylib/imdl_factory.h>
#include <mi/neuraylib/imdl_impexp_api.h>
#include <mi/neuraylib/imodule.h>
#include <mi/neuraylib/iscope.h>
#include <mi/neuraylib/istring.h>
#include <mi/neuraylib/itransaction.h>

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace matconv::detail {
namespace {

namespace mx = MaterialX;
namespace mdl = mi::neuraylib;

using mdl_translation::add_node_inputs_from_definition;
using mdl_translation::connect_expression;
using mdl_translation::ConversionReport;
using mdl_translation::ExpressionTranslator;
using mdl_translation::is_neutral_expression;
using mdl_translation::is_state_normal_expression;
using mdl_translation::is_unit_ior_expression;
using mdl_translation::MxExpression;
using mdl_translation::unique_element_name;

std::string short_material_name(const char *mdl_definition_name) {
  std::string name = mdl_definition_name ? mdl_definition_name : "";
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

mx::NodePtr create_standard_surface_shader(const mx::NodeGraphPtr &graph,
                                          const std::string &base_name) {
  mx::NodePtr surface = graph->addNode(
      "standard_surface", unique_element_name(graph, base_name),
      "surfaceshader");
  surface->addInputsFromNodeDef();

  mx::NodePtr normal =
      graph->addNode("normal", unique_element_name(graph, "surface_normal"),
                     "vector3");
  add_node_inputs_from_definition(normal);
  normal->getInput("space")->setValueString("world");
  surface->getInput("normal")->setConnectedNode(normal);
  surface->getInput("coat_normal")->setConnectedNode(normal);
  surface->getInput("tangent")->setValueString("1.0, 0.0, 0.0");
  surface->getInput("base")->setValueString("0.0");
  surface->getInput("base_color")->setValueString("0.18, 0.18, 0.18");
  surface->getInput("specular")->setValueString("0.0");
  surface->getInput("transmission")->setValueString("0.0");
  surface->getInput("emission")->setValueString("0.0");
  return surface;
}

MxExpression translate_slot(ExpressionTranslator &translator,
                            const mdl::ICompiled_material *compiled,
                            const char *path) {
  mi::base::Handle<const mdl::IExpression> expression(
      compiled->lookup_sub_expression(path));
  return translator.translate(expression.get());
}

void connect_standard_surface_emission(
    ExpressionTranslator &translator, const mdl::ICompiled_material *compiled,
    const mx::NodePtr &surface, ConversionReport &report,
    const std::string &material, const std::string &prefix) {
  mi::base::Handle<const mdl::IExpression> emission(
      compiled->lookup_sub_expression((prefix + ".emission").c_str()));
  if (!emission || is_neutral_expression(emission.get()))
    return;

  MxExpression intensity = translate_slot(
      translator, compiled, (prefix + ".intensity").c_str());
  if (intensity.type == "color3") {
    surface->getInput("emission")->setValueString("1.0");
    connect_expression(intensity, surface->getInput("emission_color"), report,
                       material, prefix + ".intensity");
  } else if (intensity.type == "float") {
    connect_expression(intensity, surface->getInput("emission"), report,
                       material, prefix + ".intensity");
  } else {
    report.warn(material +
                ": emission was approximated with unit white because its "
                "intensity is not a scalar or color.");
    surface->getInput("emission")->setValueString("1.0");
  }

  mi::base::Handle<const mdl::IExpression> mode(
      compiled->lookup_sub_expression((prefix + ".mode").c_str()));
  if (mode && !is_neutral_expression(mode.get()))
    report.warn(material +
                ": emission mode is not represented by standard_surface.");
  report.warn(material +
              ": MDL emission was approximated with standard_surface "
              "emission controls.");
}

void add_backface(const mx::NodeGraphPtr &graph,
                  ExpressionTranslator &translator,
                  const mdl::ICompiled_material *compiled,
                  mx::NodePtr material, ConversionReport &report,
                  const std::string &name) {
  mi::base::Handle<const mdl::IExpression> scattering(
      compiled->lookup_sub_expression("backface.scattering"));
  mi::base::Handle<const mdl::IExpression> emission(
      compiled->lookup_sub_expression("backface.emission.emission"));
  if (is_neutral_expression(scattering.get()) &&
      is_neutral_expression(emission.get()))
    return;

  mx::NodePtr shader =
      create_standard_surface_shader(graph, "backface_surface_shader");
  if (!is_neutral_expression(scattering.get())) {
    if (!translator.approximate_bsdf(scattering.get(), shader)) {
      shader->getInput("base")->setValueString("1.0");
      report.warn(name +
                  ": backface.scattering could not be translated; using "
                  "neutral gray diffuse.");
    }
  }
  connect_standard_surface_emission(translator, compiled, shader, report,
                                    name, "backface.emission");

  const std::string output_name =
      unique_element_name(graph, "backface_output");
  mx::OutputPtr output = graph->addOutput(output_name, "surfaceshader");
  output->setConnectedNode(shader);
  mx::InputPtr backface_input =
      material->addInput("backsurfaceshader", "surfaceshader");
  backface_input->setNodeGraphString(graph->getName());
  backface_input->setOutputString(output_name);
}

void add_displacement(const mx::NodeGraphPtr &graph,
                     ExpressionTranslator &translator,
                     const mdl::ICompiled_material *compiled,
                     mx::NodePtr material, ConversionReport &report,
                     const std::string &name) {
  mi::base::Handle<const mdl::IExpression> expression(
      compiled->lookup_sub_expression("geometry.displacement"));
  if (is_neutral_expression(expression.get()))
    return;
  MxExpression value = translator.translate(expression.get());
  if (!value.valid()) {
    report.warn(name +
                ": geometry.displacement could not be translated and was "
                "omitted.");
    return;
  }

  mx::NodePtr displacement =
      graph->addNode("displacement",
                     unique_element_name(graph, "material_displacement"),
                     "displacementshader");
  displacement->addInput("displacement", value.type);
  add_node_inputs_from_definition(displacement);
  if (!connect_expression(value, displacement->getInput("displacement"),
                          report, name, "geometry.displacement"))
    return;
  if (value.type == "vector3")
    report.warn(name +
                ": vector geometry.displacement was mapped directly; MDL "
                "and MaterialX displacement coordinate conventions may differ.");

  const std::string output_name =
      unique_element_name(graph, "displacement_output");
  mx::OutputPtr output = graph->addOutput(output_name,
                                          "displacementshader");
  output->setConnectedNode(displacement);
  mx::InputPtr input =
      material->addInput("displacementshader", "displacementshader");
  input->setNodeGraphString(graph->getName());
  input->setOutputString(output_name);
}

void report_unsupported_slot(const mdl::ICompiled_material *compiled,
                            const char *path, ConversionReport &report,
                            const std::string &material) {
  mi::base::Handle<const mdl::IExpression> expression(
      compiled->lookup_sub_expression(path));
  if (expression && !is_neutral_expression(expression.get()))
    report.warn(material + ": MDL material slot " + path +
                " is not yet translated to MaterialX.");
}

void apply_material_ior(const mdl::ICompiled_material *compiled,
                        ExpressionTranslator &translator,
                        const mx::NodePtr &surface,
                        ConversionReport &report,
                        const std::string &material) {
  mi::base::Handle<const mdl::IExpression> expression(
      compiled->lookup_sub_expression("ior"));
  if (!expression || is_neutral_expression(expression.get()) ||
      is_unit_ior_expression(expression.get(), compiled))
    return;

  MxExpression ior = translator.translate(expression.get());
  if (ior.type == "color3")
    ior = translator.scalarize_color(ior, "material_ior");
  if (!ior.valid() || ior.type != "float") {
    report.warn(material +
                ": material IOR could not be represented by a scalar "
                "MaterialX specular IOR.");
    return;
  }

  mx::InputPtr input = surface->getInput("specular_IOR");
  if (!input ||
      !connect_expression(ior, input, report, material, "material IOR"))
    report.warn(material +
                ": material IOR could not be applied to standard_surface.");
}

void add_material(mx::DocumentPtr document,
                  const mdl::IFunction_definition *definition,
                  mdl::ITransaction *transaction,
                  mdl::IMdl_factory *mdl_factory, const std::string &name,
                  TextureResourcePackager &texture_packager,
                  const std::filesystem::path &resource_directory,
                  ConversionReport &report) {
  mi::Sint32 creation_error = 0;
  mi::base::Handle<mdl::IFunction_call> function_call(
      definition->create_function_call(nullptr, &creation_error));
  if (!function_call) {
    throw std::runtime_error("Could not instantiate MDL material " + name +
                             " (SDK error " +
                             std::to_string(creation_error) + ").");
  }
  mi::base::Handle<mdl::IMaterial_instance> instance(
      function_call->get_interface<mdl::IMaterial_instance>());
  if (!instance)
    throw std::runtime_error("MDL material instance interface is unavailable "
                             "for " +
                             name + ".");

  mi::base::Handle<mdl::IMdl_execution_context> context(
      mdl_factory->create_execution_context());
  if (!context)
    throw std::runtime_error("Could not create an MDL compilation context.");
  mi::base::Handle<const mdl::ICompiled_material> compiled(
      instance->create_compiled_material(mdl::IMaterial_instance::
                                             CLASS_COMPILATION,
                                         context.get()));
  if (!compiled)
    throw std::runtime_error("Could not compile MDL material " + name +
                             " in class-compilation mode.");

  const std::string safe_name = unique_element_name(document, name);
  const std::string graph_name =
      unique_element_name(document, safe_name + "_graph");
  mx::NodeGraphPtr graph = document->addNodeGraph(graph_name);
  mx::NodePtr surface =
      create_standard_surface_shader(graph, "surface_shader");

  ExpressionTranslator translator(compiled.get(), transaction, graph, report,
                                  name, texture_packager, resource_directory);
  mi::base::Handle<const mdl::IExpression> scattering(
      compiled->lookup_sub_expression("surface.scattering"));
  if (!translator.approximate_bsdf(scattering.get(), surface)) {
    surface->getInput("base")->setValueString("1.0");
    report.warn(name +
                ": surface.scattering could not be translated; using neutral "
                "gray diffuse.");
  }

  mi::base::Handle<const mdl::IExpression> opacity_expression(
      compiled->lookup_sub_expression("geometry.cutout_opacity"));
  if (!is_neutral_expression(opacity_expression.get())) {
    MxExpression opacity = translator.translate(opacity_expression.get());
    connect_expression(opacity, surface->getInput("opacity"), report, name,
                       "geometry.cutout_opacity");
  }

  mi::base::Handle<const mdl::IExpression> thin_walled_expression(
      compiled->lookup_sub_expression("thin_walled"));
  if (!is_neutral_expression(thin_walled_expression.get())) {
    MxExpression thin_walled =
        translator.translate(thin_walled_expression.get());
    connect_expression(thin_walled, surface->getInput("thin_walled"), report,
                       name, "thin_walled");
  }
  connect_standard_surface_emission(translator, compiled.get(), surface,
                                    report, name, "surface.emission");

  const std::string output_name =
      unique_element_name(graph, "surface_output");
  mx::OutputPtr output = graph->addOutput(output_name, "surfaceshader");
  output->setConnectedNode(surface);

  mx::NodePtr material =
      document->addNode("surfacematerial", safe_name, "material");
  mx::InputPtr material_surface =
      material->addInput("surfaceshader", "surfaceshader");
  material_surface->setNodeGraphString(graph_name);
  material_surface->setOutputString(output_name);

  add_backface(graph, translator, compiled.get(), material, report, name);
  add_displacement(graph, translator, compiled.get(), material, report, name);
  mi::base::Handle<const mdl::IExpression> normal_expression(
      compiled->lookup_sub_expression("geometry.normal"));
  if (!is_neutral_expression(normal_expression.get()) &&
      !is_state_normal_expression(normal_expression.get(), compiled.get(),
                                  transaction)) {
    MxExpression normal = translator.translate(normal_expression.get());
    if (normal.valid() && normal.type == "vector3" && normal.node &&
        normal.node->getCategory() == "normal") {
      report.warn(name +
                  ": geometry.normal reduced to the unmodified normal; "
                  "the translated layer normal is retained.");
    } else if (normal.valid() && normal.type == "vector3") {
      connect_expression(normal, surface->getInput("normal"), report, name,
                         "geometry.normal");
    } else {
      report.warn(name +
                  ": geometry.normal could not be represented as a vector3.");
    }
  }
  apply_material_ior(compiled.get(), translator, surface, report, name);
  report_unsupported_slot(compiled.get(), "volume.scattering", report, name);
  report_unsupported_slot(compiled.get(), "volume.absorption_coefficient",
                          report, name);
  report_unsupported_slot(compiled.get(), "volume.scattering_coefficient",
                          report, name);
  report_unsupported_slot(compiled.get(), "volume.emission_intensity", report,
                          name);
  report_unsupported_slot(compiled.get(), "hair", report, name);
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

  mi::base::Handle<mdl::IMdl_impexp_api> mdl_impexp(
      neuray->get_api_component<mdl::IMdl_impexp_api>());
  mi::base::Handle<mdl::IMdl_factory> mdl_factory(
      neuray->get_api_component<mdl::IMdl_factory>());
  if (!mdl_impexp || !mdl_factory)
    throw std::runtime_error(
        "The MDL SDK runtime is missing required API components.");

  mi::base::Handle<mdl::IDatabase> database(
      neuray->get_api_component<mdl::IDatabase>());
  if (!database)
    throw std::runtime_error("The MDL SDK runtime has no database component.");
  mi::base::Handle<mdl::IScope> scope(database->get_global_scope());
  if (!scope)
    throw std::runtime_error("The MDL SDK runtime has no global scope.");
  mi::base::Handle<mdl::ITransaction> transaction(
      scope->create_transaction());
  if (!transaction)
    throw std::runtime_error("Could not create an MDL SDK transaction.");

  mx::DocumentPtr document = mx::createDocument();
  const mx::FileSearchPath search_path = materialx_search_path();
  load_materialx_libraries(document, search_path);
  ConversionReport report;
  TextureResourcePackager texture_packager(
      std::filesystem::absolute(options.output).parent_path());
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
    mi::base::Handle<const mdl::IModule> module(
        transaction->access<mdl::IModule>(database_module_name->get_c_str()));
    if (!module) {
      throw std::runtime_error(
          "MDL SDK did not expose the loaded module in its database.");
    }

    const std::size_t material_count = module->get_material_count();
    if (material_count == 0)
      throw std::runtime_error("The MDL module does not export any materials.");

    for (std::size_t index = 0; index < material_count; ++index) {
      const char *definition_name = module->get_material(index);
      mi::base::Handle<const mdl::IFunction_definition> definition(
          transaction->access<mdl::IFunction_definition>(definition_name));
      if (!definition)
        continue;

      const char *mdl_definition_name = definition->get_mdl_name();
      if (!mdl_definition_name)
        continue;

      const std::string name = short_material_name(mdl_definition_name);
      if (!matches_requested_material(options, name, mdl_definition_name))
        continue;

      add_material(document, definition.get(), transaction.get(),
                   mdl_factory.get(), name, texture_packager,
                   std::filesystem::absolute(options.input).parent_path(),
                   report);
      ++emitted_materials;
    }
  }

  if (emitted_materials == 0) {
    throw std::runtime_error(
        options.material.empty() ? "No MDL materials were compiled."
                                 : "No matching MDL material was found: " +
                                       options.material);
  }

  transaction->abort();
  validate_materialx_document(document);
  if (options.strict && !report.warnings.empty()) {
    for (const std::string &warning : report.warnings)
      std::cerr << "Warning: " << warning << '\n';
    throw std::runtime_error(
        "Strict conversion rejected " + std::to_string(report.warnings.size()) +
        " warning(s); the MaterialX document was not written.");
  }
  write_materialx_document(document, options.output);

  std::cout << "Wrote MaterialX document: " << options.output << '\n'
            << "Compiled and translated " << report.translated_nodes
            << " expression nodes and preserved " << report.graph_inputs
            << " class-compilation parameters across " << emitted_materials
            << " material(s).\n";
  for (const std::string &warning : report.warnings)
    std::cout << "Warning: " << warning << '\n';
  return 0;
}

} // namespace matconv::detail
