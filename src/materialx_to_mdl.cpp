#include "conversion.h"
#include "materialx_utils.h"
#include "texture_resources.h"

#include <MaterialXCore/Document.h>
#include <MaterialXFormat/XmlIo.h>
#include <MaterialXGenMdl/MdlShaderGenerator.h>
#include <MaterialXGenShader/DefaultColorManagementSystem.h>
#include <MaterialXGenShader/GenContext.h>
#include <MaterialXGenShader/Shader.h>
#include <MaterialXGenShader/Util.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace matconv::detail {
namespace {

namespace mx = MaterialX;

void connect_standard_surface_thin_walled(const mx::DocumentPtr &document) {
  mx::NodeGraphPtr graph =
      document->getNodeGraph("NG_standard_surface_surfaceshader_100");
  mx::NodePtr surface = graph ? graph->getNode("shader_constructor") : nullptr;
  if (!surface || surface->getCategory() != "surface" ||
      surface->getInput("thin_walled"))
    return;
  mx::NodeDefPtr definition =
      document->getNodeDef("ND_standard_surface_surfaceshader_100");
  mx::InputPtr thin_walled =
      definition ? definition->getInput("thin_walled") : nullptr;
  if (!thin_walled)
    throw std::runtime_error(
        "MaterialX standard_surface has no thin_walled definition.");
  thin_walled->setIsUniform(true);
  surface->addInput("thin_walled", "boolean")->setInterfaceName("thin_walled");
}

mx::TypedElementPtr
select_renderable(const std::vector<mx::TypedElementPtr> &renderables,
                  const std::string &requested_name) {
  for (const mx::TypedElementPtr &renderable : renderables) {
    if (requested_name.empty() || renderable->getName() == requested_name ||
        renderable->getNamePath() == requested_name) {
      return renderable;
    }
  }
  return nullptr;
}

void write_shader(const mx::ShaderPtr &shader,
                  const std::filesystem::path &output_path) {
  std::ofstream output(output_path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("Could not open output file: " +
                             output_path.string());
  }

  output << shader->getSourceCode();
  if (!output) {
    throw std::runtime_error("Failed while writing output file: " +
                             output_path.string());
  }
}

void package_renderable_textures(
    const mx::DocumentPtr &document, const mx::TypedElementPtr &renderable,
    const std::filesystem::path &input_path,
    const mx::FileSearchPath &search_path,
    TextureResourcePackager &texture_packager) {
  mx::FileSearchPath resource_search_path;
  resource_search_path.append(
      mx::FilePath(std::filesystem::absolute(input_path).parent_path().string()));
  resource_search_path.append(search_path);

  std::vector<mx::ElementPtr> pending;
  pending.push_back(renderable);
  std::set<const mx::Element *> visited;
  std::set<const mx::Input *> visited_inputs;
  while (!pending.empty()) {
    mx::ElementPtr root = std::move(pending.back());
    pending.pop_back();
    if (!visited.insert(root.get()).second)
      continue;

    for (mx::ElementPtr element : root->traverseTree()) {
      mx::InputPtr input = element->asA<mx::Input>();
      if (!input || !visited_inputs.insert(input.get()).second)
        continue;

      if (mx::InputPtr interface_input = input->getInterfaceInput())
        pending.push_back(interface_input);
      if (mx::NodePtr connected = input->getConnectedNode())
        pending.push_back(connected);

      if (input->getType() == "filename" && input->hasValueString() &&
          !input->getValueString().empty()) {
        const std::string filename = input->getValueString();
        const mx::FilePath resolved =
            resource_search_path.find(mx::FilePath(filename));
        if (!resolved.exists()) {
          throw std::runtime_error(
              "Could not resolve MaterialX texture resource '" + filename +
              "' referenced by " + input->getNamePath() + ".");
        }
        input->setValueString(
            "./" + texture_packager.package(
                       std::filesystem::path(resolved.asString())));
      }

      if (input->hasNodeGraphString()) {
        mx::NodeGraphPtr graph =
            document->getNodeGraph(input->getNodeGraphString());
        if (graph)
          pending.push_back(graph);
      }
    }
  }
}

} // namespace

int materialx_to_mdl(const Options &options) {
  const mx::FileSearchPath search_path = materialx_search_path();
  mx::DocumentPtr document = mx::createDocument();
  load_materialx_libraries(document, search_path);
  connect_standard_surface_thin_walled(document);
  mx::readFromXmlFile(document, mx::FilePath(options.input.string()),
                      search_path);

  const auto generator = mx::MdlShaderGenerator::create();
  mx::GenContext context(generator);
  mx::DefaultColorManagementSystemPtr color_management =
      mx::DefaultColorManagementSystem::create(generator->getTarget());
  color_management->loadLibrary(document);
  generator->setColorManagementSystem(color_management);
  context.getOptions().targetColorSpaceOverride = "lin_rec709";
  context.registerSourceCodeSearchPath(
      mx::FilePath(options.input.parent_path().string()));
  context.registerSourceCodeSearchPath(search_path);

  const std::vector<mx::TypedElementPtr> renderables =
      mx::findRenderableElements(document);
  if (renderables.empty()) {
    throw std::runtime_error(
        "MaterialX document contains no renderable materials or node graphs.");
  }

  const mx::TypedElementPtr selected =
      select_renderable(renderables, options.material);
  if (!selected)
    throw std::runtime_error("No MaterialX renderable matches: " +
                             options.material);

  TextureResourcePackager texture_packager(
      std::filesystem::absolute(options.output).parent_path());
  package_renderable_textures(document, selected, options.input, search_path,
                              texture_packager);

  const std::string shader_name = safe_material_name(selected->getName());
  const mx::ShaderPtr shader =
      generator->generate(shader_name, selected, context);
  if (!shader || shader->getSourceCode().empty()) {
    throw std::runtime_error(
        "MaterialX MDL generator produced no source code.");
  }

  write_shader(shader, options.output);
  std::cout << "Wrote MDL source: " << options.output << '\n';
  return 0;
}

} // namespace matconv::detail
