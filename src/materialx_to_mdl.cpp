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
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace matconv::detail {
namespace {

namespace mx = MaterialX;

class SharedGraphInputs final : public mx::ShaderGraphRefactor {
public:
  const std::string &getName() const override {
    static const std::string name = "matconv_shared_graph_inputs";
    return name;
  }

  size_t execute(mx::ShaderGraph &graph, mx::GenContext &) override {
    if (graph.getParent())
      return 0;
    std::map<std::string, std::vector<mx::ShaderInput *>> groups;
    std::set<std::string> reserved_names;
    for (mx::ShaderNode *node : graph.getNodes()) {
      for (mx::ShaderInput *input : node->getInputs()) {
        if (input->getConnection() || input->getType().isClosure() ||
            !node->isEditable(*input))
          continue;
        reserved_names.insert(node->getName() + "_" + input->getName());
        if (input->getPath().empty())
          continue;
        mx::ElementPtr element =
            graph.getDocument()->getDescendant(input->getPath());
        mx::InputPtr source = element ? element->asA<mx::Input>() : nullptr;
        mx::NodeGraphPtr source_graph =
            source && source->getParent()
                ? source->getParent()->asA<mx::NodeGraph>()
                : nullptr;
        if (source_graph && source_graph->getNodeDefString().empty())
          groups[input->getPath()].push_back(input);
      }
    }

    size_t edits = 0;
    for (const auto &[path, inputs] : groups) {
      if (inputs.size() < 2)
        continue;
      mx::ShaderInput *first = inputs.front();
      bool compatible = true;
      for (const mx::ShaderInput *input : inputs) {
        if (input->getType() != first->getType() ||
            input->isUniform() != first->isUniform() ||
            input->getValueString() != first->getValueString() ||
            input->getUnit() != first->getUnit() ||
            input->getColorSpace() != first->getColorSpace() ||
            input->getGeomProp() != first->getGeomProp()) {
          compatible = false;
          break;
        }
      }
      if (!compatible) {
        std::cerr << "Warning: MaterialX graph input " << path
                  << " has incompatible generated uses; MDL parameters "
                     "remain separate.\n";
        continue;
      }
      mx::ElementPtr source = graph.getDocument()->getDescendant(path);
      const std::string base = source->getName();
      std::string name = base;
      size_t suffix = 1;
      while (reserved_names.count(name) || graph.getInputSocket(name))
        name = base + "_" + std::to_string(suffix++);
      reserved_names.insert(name);
      mx::ShaderGraphInputSocket *socket =
          graph.addInputSocket(name, first->getType());
      socket->setPath(path);
      socket->setValue(first->getValue());
      socket->setUnit(first->getUnit());
      socket->setColorSpace(first->getColorSpace());
      socket->setGeomProp(first->getGeomProp());
      socket->setMetadata(first->getMetadata());
      if (first->isUniform())
        socket->setUniform();
      for (mx::ShaderInput *input : inputs)
        socket->makeConnection(input);
      ++edits;
    }
    return edits;
  }
};

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
  generator->registerRefactor(std::make_shared<SharedGraphInputs>());
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
