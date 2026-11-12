#include "conversion.h"
#include "materialx_utils.h"

#include <MaterialXCore/Document.h>
#include <MaterialXFormat/XmlIo.h>
#include <MaterialXGenMdl/MdlShaderGenerator.h>
#include <MaterialXGenShader/GenContext.h>
#include <MaterialXGenShader/Shader.h>
#include <MaterialXGenShader/Util.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace matconv::detail {
namespace {

namespace mx = MaterialX;

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

} // namespace

int materialx_to_mdl(const Options &options) {
  const mx::FileSearchPath search_path = materialx_search_path();
  mx::DocumentPtr document = mx::createDocument();
  load_materialx_libraries(document, search_path);
  mx::readFromXmlFile(document, mx::FilePath(options.input.string()),
                      search_path);

  const auto generator = mx::MdlShaderGenerator::create();
  mx::GenContext context(generator);
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
