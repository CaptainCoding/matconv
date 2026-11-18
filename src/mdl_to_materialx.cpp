#include "conversion.h"
#include "materialx_utils.h"
#include "mdl_runtime.h"

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
#include <mi/neuraylib/iimage.h>
#include <mi/neuraylib/itexture.h>
#include <mi/neuraylib/imdl_execution_context.h>
#include <mi/neuraylib/imdl_factory.h>
#include <mi/neuraylib/imdl_impexp_api.h>
#include <mi/neuraylib/imodule.h>
#include <mi/neuraylib/iscope.h>
#include <mi/neuraylib/istring.h>
#include <mi/neuraylib/itransaction.h>
#include <mi/neuraylib/ivalue.h>

#include <cstddef>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace matconv::detail {
namespace {

namespace mx = MaterialX;
namespace mdl = mi::neuraylib;

struct ConversionReport {
  std::size_t graph_inputs = 0;
  std::size_t translated_nodes = 0;
  std::vector<std::string> warnings;

  void warn(std::string message) { warnings.push_back(std::move(message)); }
};

struct MxExpression {
  mx::NodePtr node;
  std::string interface_name;
  std::string type;

  bool valid() const { return node || !interface_name.empty(); }
};

struct ThinFilmParameters {
  MxExpression thickness;
  MxExpression ior;
  MxExpression scalar_ior;
  std::size_t scope_id = 0;
  bool applied = false;
};

bool connect_expression(const MxExpression &expression,
                        const mx::InputPtr &input,
                        ConversionReport &report,
                        const std::string &material,
                        const std::string &slot);

std::string mdl_type_to_materialx(const mdl::IType *type) {
  if (!type)
    return {};
  switch (type->get_kind()) {
  case mdl::IType::TK_BOOL:
    return "boolean";
  case mdl::IType::TK_INT:
    return "integer";
  case mdl::IType::TK_FLOAT:
    return "float";
  case mdl::IType::TK_COLOR:
    return "color3";
  case mdl::IType::TK_BSDF:
    return "BSDF";
  case mdl::IType::TK_EDF:
    return "EDF";
  case mdl::IType::TK_VDF:
    return "VDF";
  case mdl::IType::TK_VECTOR: {
    mi::base::Handle<const mdl::IType_vector> vector(
        type->get_interface<mdl::IType_vector>());
    if (!vector)
      return {};
    const mi::Size size = vector->get_size();
    return size >= 2 && size <= 4 ? "vector" + std::to_string(size) : "";
  }
  default:
    return {};
  }
}

std::string format_float(float value) {
  std::ostringstream stream;
  stream << std::setprecision(9) << value;
  return stream.str();
}

bool serialize_value(const mdl::IValue *value, std::string &type,
                     std::string &text) {
  if (!value)
    return false;
  mi::base::Handle<const mdl::IType> mdl_type(value->get_type());
  type = mdl_type_to_materialx(mdl_type.get());
  if (type.empty())
    return false;

  switch (value->get_kind()) {
  case mdl::IValue::VK_BOOL:
    text = static_cast<const mdl::IValue_bool *>(value)->get_value() ? "true"
                                                                     : "false";
    return true;
  case mdl::IValue::VK_INT:
    text = std::to_string(
        static_cast<const mdl::IValue_int *>(value)->get_value());
    return true;
  case mdl::IValue::VK_FLOAT:
    text = format_float(
        static_cast<const mdl::IValue_float *>(value)->get_value());
    return true;
  case mdl::IValue::VK_COLOR:
  case mdl::IValue::VK_VECTOR: {
    const auto *compound = static_cast<const mdl::IValue_compound *>(value);
    for (mi::Size index = 0; index < compound->get_size(); ++index) {
      mi::base::Handle<const mdl::IValue_float> component(
          compound->get_value<mdl::IValue_float>(index));
      if (!component)
        return false;
      if (!text.empty())
        text += ", ";
      text += format_float(component->get_value());
    }
    return true;
  }
  default:
    return false;
  }
}

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

std::string unique_element_name(const mx::ElementPtr &parent,
                               const std::string &base) {
  const std::string safe_base = safe_material_name(base);
  std::string name = safe_base;
  unsigned int suffix = 1;
  while (parent->getChild(name))
    name = safe_base + "_" + std::to_string(suffix++);
  return name;
}

void add_node_inputs_from_definition(const mx::NodePtr &node) {
  node->addInputsFromNodeDef();
  mx::NodeDefPtr definition = node->getNodeDef();
  if (!definition)
    return;
  for (const mx::InputPtr &definition_input : definition->getActiveInputs()) {
    if (!definition_input->hasDefaultGeomPropString())
      continue;
    mx::InputPtr input = node->getInput(definition_input->getName());
    if (!input || input->hasValueString() || input->hasNodeName())
      continue;
    mx::NodeGraphPtr graph = node->getParent()->asA<mx::NodeGraph>();
    if (!graph)
      continue;
    const std::string geom_name = unique_element_name(
        graph, node->getName() + "_" + input->getName() + "_geom");
    mx::NodePtr geom =
        graph->addNode("geompropvalue", geom_name, input->getType());
    geom->addInputsFromNodeDef();
    geom->getInput("geomprop")
        ->setValueString(definition_input->getDefaultGeomPropString());
    input->setConnectedNode(geom);
  }
}

bool is_neutral_value(const mdl::IValue *value) {
  if (!value)
    return true;
  switch (value->get_kind()) {
  case mdl::IValue::VK_INVALID_DF:
    return true;
  case mdl::IValue::VK_BOOL:
    return !static_cast<const mdl::IValue_bool *>(value)->get_value();
  case mdl::IValue::VK_INT:
    return static_cast<const mdl::IValue_int *>(value)->get_value() == 0;
  case mdl::IValue::VK_FLOAT:
    return static_cast<const mdl::IValue_float *>(value)->get_value() == 0.0f;
  case mdl::IValue::VK_COLOR:
  case mdl::IValue::VK_VECTOR: {
    const auto *compound = static_cast<const mdl::IValue_compound *>(value);
    for (mi::Size index = 0; index < compound->get_size(); ++index) {
      mi::base::Handle<const mdl::IValue_float> component(
          compound->get_value<mdl::IValue_float>(index));
      if (!component || component->get_value() != 0.0f)
        return false;
    }
    return true;
  }
  default:
    return false;
  }
}

bool is_neutral_expression(const mdl::IExpression *expression) {
  if (!expression)
    return true;
  if (expression->get_kind() != mdl::IExpression::EK_CONSTANT)
    return false;
  mi::base::Handle<const mdl::IValue> value(
      static_cast<const mdl::IExpression_constant *>(expression)->get_value());
  return is_neutral_value(value.get());
}

const char *math_node_category(mdl::IFunction_definition::Semantics semantic) {
  using S = mdl::IFunction_definition;
  switch (semantic) {
  case S::DS_PLUS:
    return "add";
  case S::DS_MINUS:
    return "subtract";
  case S::DS_MULTIPLY:
    return "multiply";
  case S::DS_DIVIDE:
    return "divide";
  case S::DS_INTRINSIC_MATH_ABS:
    return "absval";
  case S::DS_INTRINSIC_MATH_POW:
    return "power";
  case S::DS_INTRINSIC_MATH_MIN:
    return "min";
  case S::DS_INTRINSIC_MATH_MAX:
    return "max";
  case S::DS_INTRINSIC_MATH_AVERAGE:
    return "dotproduct";
  case S::DS_INTRINSIC_MATH_CLAMP:
    return "clamp";
  case S::DS_INTRINSIC_MATH_SATURATE:
    return "clamp";
  case S::DS_INTRINSIC_MATH_FLOOR:
    return "floor";
  case S::DS_INTRINSIC_MATH_CEIL:
    return "ceil";
  case S::DS_INTRINSIC_MATH_ROUND:
    return "round";
  case S::DS_INTRINSIC_MATH_SQRT:
    return "sqrt";
  case S::DS_INTRINSIC_MATH_EXP:
    return "exp";
  case S::DS_INTRINSIC_MATH_LOG:
    return "ln";
  case S::DS_INTRINSIC_MATH_SIN:
    return "sin";
  case S::DS_INTRINSIC_MATH_COS:
    return "cos";
  case S::DS_INTRINSIC_MATH_TAN:
    return "tan";
  case S::DS_INTRINSIC_MATH_DOT:
    return "dotproduct";
  case S::DS_INTRINSIC_MATH_CROSS:
    return "crossproduct";
  case S::DS_INTRINSIC_MATH_LENGTH:
    return "magnitude";
  case S::DS_INTRINSIC_MATH_NORMALIZE:
    return "normalize";
  case S::DS_INTRINSIC_MATH_LERP:
    return "mix";
  case S::DS_INTRINSIC_MATH_SIGN:
    return "sign";
  case S::DS_INTRINSIC_MATH_SMOOTHSTEP:
    return "smoothstep";
  default:
    return nullptr;
  }
}

class ExpressionTranslator {
public:
  ExpressionTranslator(const mdl::ICompiled_material *compiled,
                       mdl::ITransaction *transaction,
                       const mx::NodeGraphPtr &graph,
                       ConversionReport &report, const std::string &material,
                       std::filesystem::path output_directory,
                       std::filesystem::path resource_directory)
      : compiled_(compiled), transaction_(transaction), graph_(graph),
        report_(report), material_(material),
        output_directory_(std::move(output_directory)),
        resource_directory_(std::move(resource_directory)) {
    for (mi::Size index = 0; index < compiled_->get_parameter_count(); ++index) {
      const char *parameter_name = compiled_->get_parameter_name(index);
      std::string type;
      std::string value;
      mi::base::Handle<const mdl::IValue> argument(
          compiled_->get_argument(index));
      if (!parameter_name ||
          !serialize_value(argument.get(), type, value)) {
        report_.warn(material_ +
                     ": omitted a class-compilation parameter with an "
                     "unsupported MaterialX type.");
        continue;
      }

      const std::string interface_name =
          unique_element_name(graph_, parameter_name);
      parameter_names_[index] = interface_name;

      mx::InputPtr input = graph_->addInput(interface_name, type);
      input->setValueString(value);
      ++report_.graph_inputs;
    }
  }

  MxExpression translate(const mdl::IExpression *expression) {
    if (!expression)
      return {};
    switch (expression->get_kind()) {
    case mdl::IExpression::EK_CONSTANT:
      return translate_constant(
          static_cast<const mdl::IExpression_constant *>(expression));
    case mdl::IExpression::EK_PARAMETER:
      return translate_parameter(
          static_cast<const mdl::IExpression_parameter *>(expression));
    case mdl::IExpression::EK_TEMPORARY:
      return translate_temporary(
          static_cast<const mdl::IExpression_temporary *>(expression));
    case mdl::IExpression::EK_DIRECT_CALL:
      return translate_call(
          static_cast<const mdl::IExpression_direct_call *>(expression));
    default:
      report_.warn(material_ + ": unsupported MDL expression kind.");
      return {};
    }
  }

private:
  MxExpression make_constant(const mdl::IValue *value,
                             const std::string &prefix) {
    std::string type;
    std::string text;
    if (!serialize_value(value, type, text)) {
      report_.warn(material_ + ": cannot represent an MDL constant value.");
      return {};
    }
    mx::NodePtr node =
        graph_->addNode("constant", next_name(prefix), type);
    add_node_inputs_from_definition(node);
    mx::InputPtr input = node->getInput("value");
    if (!input) {
      report_.warn(material_ + ": MaterialX constant node has no value input.");
      return {};
    }
    input->setValueString(text);
    return {node, {}, type};
  }

  MxExpression translate_constant(const mdl::IExpression_constant *expression) {
    mi::base::Handle<const mdl::IValue> value(expression->get_value());
    return make_constant(value.get(), "constant");
  }

  MxExpression translate_parameter(
      const mdl::IExpression_parameter *expression) {
    const auto found = parameter_names_.find(expression->get_index());
    if (found == parameter_names_.end()) {
      report_.warn(material_ +
                   ": an MDL parameter reference has no representable "
                   "MaterialX graph input.");
      return {};
    }
    mi::base::Handle<const mdl::IType> type(expression->get_type());
    const std::string type_name = mdl_type_to_materialx(type.get());
    if (type_name.empty())
      return {};
    return {{}, found->second, type_name};
  }

  MxExpression translate_temporary(
      const mdl::IExpression_temporary *expression) {
    const mi::Size index = expression->get_index();
    const std::size_t scope_id =
        thin_films_.empty() ? 0 : thin_films_.front().scope_id;
    const auto key = std::make_pair(index, scope_id);
    const auto found = temporaries_.find(key);
    if (found != temporaries_.end())
      return found->second;
    mi::base::Handle<const mdl::IExpression> temporary(
        compiled_->get_temporary(index));
    MxExpression result = translate(temporary.get());
    temporaries_[key] = result;
    return result;
  }

  MxExpression translate_thin_film(
      const mdl::IExpression_direct_call *expression) {
    mi::base::Handle<const mdl::IExpression_list> arguments(
        expression->get_arguments());
    mi::base::Handle<const mdl::IExpression> thickness_expression;
    mi::base::Handle<const mdl::IExpression> ior_expression;
    mi::base::Handle<const mdl::IExpression> base_expression;
    if (arguments) {
      for (mi::Size index = 0; index < arguments->get_size(); ++index) {
        const char *argument_name = arguments->get_name(index);
        mi::base::Handle<const mdl::IExpression> argument(
            arguments->get_expression(index));
        if (!argument_name)
          continue;
        const std::string name(argument_name);
        if (name == "thickness")
          thickness_expression = argument;
        else if (name == "ior")
          ior_expression = argument;
        else if (name == "base")
          base_expression = argument;
      }
    }

    if (!base_expression) {
      report_.warn(material_ +
                   ": MDL thin-film call has no translatable base BSDF.");
      return {};
    }

    MxExpression thickness = translate(thickness_expression.get());
    MxExpression ior = translate(ior_expression.get());
    if (!thickness.valid() || thickness.type != "float" || !ior.valid() ||
        (ior.type != "float" && ior.type != "color3")) {
      report_.warn(material_ +
                   ": MDL thin-film parameters could not be represented; "
                   "preserving the underlying BSDF without its film.");
      return translate(base_expression.get());
    }
    if (ior.type == "color3") {
      report_.warn(material_ +
                   ": MDL thin-film color IOR was reduced to a scalar "
                   "average for MaterialX.");
    }

    ThinFilmParameters parameters;
    parameters.thickness = thickness;
    parameters.ior = ior;
    parameters.scope_id = next_thin_film_scope_id_++;
    thin_films_.push_back(std::move(parameters));
    MxExpression base = translate(base_expression.get());
    ThinFilmParameters translated_film = std::move(thin_films_.back());
    thin_films_.pop_back();

    if (!translated_film.applied) {
      report_.warn(material_ +
                   ": MDL thin-film effect was omitted because its BSDF has "
                   "no matching MaterialX thin-film inputs.");
    }
    return base;
  }

  void apply_thin_film(const mx::NodePtr &bsdf) {
    if (thin_films_.empty())
      return;
    mx::InputPtr thickness_input = bsdf->getInput("thinfilm_thickness");
    mx::InputPtr ior_input = bsdf->getInput("thinfilm_ior");
    if (!thickness_input || !ior_input)
      return;

    ThinFilmParameters &film = thin_films_.front();
    if (film.ior.type == "color3" && !film.scalar_ior.valid()) {
      mx::NodePtr average =
          graph_->addNode("dotproduct", next_name("thinfilm_ior"), "float");
      average->setNodeDefString("ND_dotproduct_vector3");
      add_node_inputs_from_definition(average);
      mx::InputPtr weights = average->getInput("in2");
      if (!weights) {
        report_.warn(material_ +
                     ": could not create a scalar MaterialX thin-film IOR.");
        return;
      }
      weights->setValueString(
          "0.333333333, 0.333333333, 0.333333333");
      if (!connect_expression(film.ior, average->getInput("in1"), report_,
                              material_, "thin-film IOR")) {
        return;
      }
      film.scalar_ior = {average, {}, "float"};
      ++report_.translated_nodes;
    }

    const MxExpression &ior =
        film.ior.type == "color3" ? film.scalar_ior : film.ior;
    const bool thickness_connected =
        connect_expression(film.thickness, thickness_input, report_,
                           material_, "thin-film thickness");
    const bool ior_connected =
        ior.valid() && connect_expression(ior, ior_input, report_, material_,
                                         "thin-film IOR");
    film.applied = film.applied || (thickness_connected && ior_connected);
  }

  MxExpression translate_tint(
      const mdl::IExpression_direct_call *expression) {
    mi::base::Handle<const mdl::IExpression_list> arguments(
        expression->get_arguments());
    mi::base::Handle<const mdl::IExpression> bsdf_expression;
    mi::base::Handle<const mdl::IExpression> tint_expression;
    if (arguments) {
      for (mi::Size index = 0; index < arguments->get_size(); ++index) {
        mi::base::Handle<const mdl::IExpression> argument(
            arguments->get_expression(index));
        if (!argument)
          continue;
        mi::base::Handle<const mdl::IType> type(argument->get_type());
        const std::string type_name = mdl_type_to_materialx(type.get());
        if (type_name == "BSDF")
          bsdf_expression = argument;
        else if (type_name == "color3")
          tint_expression = argument;
      }
    }

    MxExpression bsdf = translate(bsdf_expression.get());
    MxExpression tint = translate(tint_expression.get());
    if (!bsdf.valid() || bsdf.type != "BSDF") {
      std::string source_name;
      const mdl::IExpression *source_expression = bsdf_expression.get();
      mi::base::Handle<const mdl::IExpression> temporary;
      while (source_expression &&
             source_expression->get_kind() ==
                 mdl::IExpression::EK_TEMPORARY) {
        temporary = compiled_->get_temporary(
            static_cast<const mdl::IExpression_temporary *>(source_expression)
                ->get_index());
        source_expression = temporary.get();
      }
      if (source_expression &&
          source_expression->get_kind() == mdl::IExpression::EK_DIRECT_CALL) {
        const char *source_definition =
            static_cast<const mdl::IExpression_direct_call *>(source_expression)
                ->get_definition();
        mi::base::Handle<const mdl::IFunction_definition> source_function(
            source_definition
                ? transaction_->access<mdl::IFunction_definition>(
                      source_definition)
                : nullptr);
        const char *mdl_name =
            source_function ? source_function->get_mdl_name() : nullptr;
        if (mdl_name)
          source_name = std::string(" (") + mdl_name + ")";
      } else if (source_expression &&
                 source_expression->get_kind() ==
                     mdl::IExpression::EK_PARAMETER) {
        const mi::Size parameter_index =
            static_cast<const mdl::IExpression_parameter *>(source_expression)
                ->get_index();
        const char *parameter_name =
            compiled_->get_parameter_name(parameter_index);
        if (parameter_name)
          source_name = std::string(" (parameter ") + parameter_name + ")";
      } else if (source_expression) {
        source_name = " (expression kind " +
                      std::to_string(static_cast<int>(
                          source_expression->get_kind())) +
                      ")";
      }
      if (tint.valid() && tint.type == "color3") {
        mx::NodePtr diffuse =
            graph_->addNode("oren_nayar_diffuse_bsdf",
                            next_name("tinted_diffuse"), "BSDF");
        add_node_inputs_from_definition(diffuse);
        connect_expression(tint, diffuse->getInput("color"), report_,
                           material_, "df::tint color");
        report_.warn(material_ +
                     ": MDL tint's unsupported BSDF" + source_name +
                     " was approximated as a diffuse lobe.");
        ++report_.translated_nodes;
        return {diffuse, {}, "BSDF"};
      }
      report_.warn(material_ + ": MDL tint has no translatable BSDF" +
                   source_name + ".");
      return {};
    }
    if (!tint.valid() || tint.type != "color3") {
      report_.warn(material_ +
                   ": MDL tint color was omitted from the translated BSDF.");
      return bsdf;
    }

    mx::NodePtr multiply =
        graph_->addNode("multiply", next_name("tinted_bsdf"), "BSDF");
    multiply->addInput("in1", "BSDF");
    multiply->addInput("in2", "color3");
    add_node_inputs_from_definition(multiply);
    connect_expression(bsdf, multiply->getInput("in1"), report_, material_,
                       "df::tint BSDF");
    connect_expression(tint, multiply->getInput("in2"), report_, material_,
                       "df::tint color");
    ++report_.translated_nodes;
    return {multiply, {}, "BSDF"};
  }

  std::string enum_name(const mdl::IExpression *expression) {
    if (!expression)
      return {};
    if (expression->get_kind() == mdl::IExpression::EK_TEMPORARY) {
      const auto *temporary =
          static_cast<const mdl::IExpression_temporary *>(expression);
      mi::base::Handle<const mdl::IExpression> value(
          compiled_->get_temporary(temporary->get_index()));
      return enum_name(value.get());
    }
    if (expression->get_kind() != mdl::IExpression::EK_CONSTANT)
      return {};

    mi::base::Handle<const mdl::IValue> value(
        static_cast<const mdl::IExpression_constant *>(expression)
            ->get_value());
    if (!value || value->get_kind() != mdl::IValue::VK_ENUM)
      return {};
    const auto *enum_value = static_cast<const mdl::IValue_enum *>(value.get());
    mi::base::Handle<const mdl::IType_enum> type(enum_value->get_type());
    const char *name = type->get_value_name(enum_value->get_index());
    return name ? name : "";
  }

  MxExpression translate_microfacet(
      const mdl::IExpression_direct_call *expression) {
    mi::base::Handle<const mdl::IExpression_list> arguments(
        expression->get_arguments());
    mi::base::Handle<const mdl::IExpression> tint_expression;
    mi::base::Handle<const mdl::IExpression> mode_expression;
    if (arguments) {
      for (mi::Size index = 0; index < arguments->get_size(); ++index) {
        const char *argument_name = arguments->get_name(index);
        mi::base::Handle<const mdl::IExpression> argument(
            arguments->get_expression(index));
        if (!argument)
          continue;
        mi::base::Handle<const mdl::IType> type(argument->get_type());
        const std::string type_name = mdl_type_to_materialx(type.get());
        const std::string name = argument_name ? argument_name : "";
        if (name == "tint" ||
            (!tint_expression && type_name == "color3"))
          tint_expression = argument;
        else if (name == "mode")
          mode_expression = argument;
      }
    }

    MxExpression tint = translate(tint_expression.get());

    mx::NodePtr bsdf =
        graph_->addNode("dielectric_bsdf", next_name("microfacet_bsdf"),
                        "BSDF");
    add_node_inputs_from_definition(bsdf);
    apply_thin_film(bsdf);
    MxExpression roughness = translate_microfacet_roughness(expression);
    if (roughness.valid())
      connect_expression(roughness, bsdf->getInput("roughness"), report_,
                         material_, "microfacet roughness");
    else {
      bsdf->getInput("roughness")->setValueString("0.2, 0.2");
      report_.warn(material_ +
                   ": MDL GGX roughness was not translatable; using 0.2.");
    }
    if (tint.valid() && tint.type == "color3")
      connect_expression(tint, bsdf->getInput("tint"), report_, material_,
                         "microfacet tint");
    else
      report_.warn(material_ +
                   ": MDL GGX tint was not translatable; using white.");

    const std::string mdl_mode = enum_name(mode_expression.get());
    const std::string scatter_mode =
        mdl_mode == "scatter_transmit"
            ? "T"
            : (mdl_mode == "scatter_reflect_transmit" ? "RT" : "R");
    bsdf->getInput("scatter_mode")->setValueString(scatter_mode);
    if (!mode_expression || mdl_mode.empty())
      report_.warn(material_ +
                   ": MDL GGX scatter mode was not recognized; using "
                   "reflection mode.");
    report_.warn(material_ +
                 ": MDL GGX microfacet was approximated with a MaterialX "
                 "dielectric BSDF; its IOR and backscatter may differ.");
    ++report_.translated_nodes;
    return {bsdf, {}, "BSDF"};
  }

  MxExpression translate_fresnel_factor(
      const mdl::IExpression_direct_call *expression) {
    mi::base::Handle<const mdl::IExpression_list> arguments(
        expression->get_arguments());
    mi::base::Handle<const mdl::IExpression> ior_expression;
    mi::base::Handle<const mdl::IExpression> extinction_expression;
    mi::base::Handle<const mdl::IExpression> base_expression;
    if (arguments) {
      for (mi::Size index = 0; index < arguments->get_size(); ++index) {
        const char *argument_name = arguments->get_name(index);
        mi::base::Handle<const mdl::IExpression> argument(
            arguments->get_expression(index));
        if (!argument_name)
          continue;
        const std::string name(argument_name);
        if (name == "ior")
          ior_expression = argument;
        else if (name == "extinction_coefficient")
          extinction_expression = argument;
        else if (name == "base")
          base_expression = argument;
      }
    }

    mx::NodePtr bsdf =
        graph_->addNode("conductor_bsdf", next_name("conductor_bsdf"),
                        "BSDF");
    add_node_inputs_from_definition(bsdf);
    apply_thin_film(bsdf);

    MxExpression ior = translate(ior_expression.get());
    if (ior.valid())
      connect_expression(ior, bsdf->getInput("ior"), report_, material_,
                         "conductor IOR");
    MxExpression extinction = translate(extinction_expression.get());
    if (extinction.valid())
      connect_expression(extinction, bsdf->getInput("extinction"), report_,
                         material_, "conductor extinction");

    MxExpression roughness =
        translate_microfacet_roughness(base_expression.get());
    if (roughness.valid())
      connect_expression(roughness, bsdf->getInput("roughness"), report_,
                         material_, "conductor roughness");
    else
      report_.warn(material_ +
                   ": MDL conductor roughness was not translatable; using "
                   "the MaterialX default.");

    if (!ior.valid() || !extinction.valid())
      report_.warn(material_ +
                   ": MDL conductor IOR or extinction was not translatable; "
                   "using MaterialX defaults.");
    report_.warn(material_ +
                 ": MDL Fresnel factor was approximated with a MaterialX "
                 "conductor BSDF; backscatter may differ.");
    ++report_.translated_nodes;
    return {bsdf, {}, "BSDF"};
  }

  std::optional<mi::Sint32>
  texture_coordinate_index(const mdl::IExpression *expression) {
    if (!expression)
      return {};
    if (expression->get_kind() == mdl::IExpression::EK_TEMPORARY) {
      const auto *temporary =
          static_cast<const mdl::IExpression_temporary *>(expression);
      mi::base::Handle<const mdl::IExpression> value(
          compiled_->get_temporary(temporary->get_index()));
      return texture_coordinate_index(value.get());
    }
    if (expression->get_kind() != mdl::IExpression::EK_DIRECT_CALL)
      return {};

    const auto *call =
        static_cast<const mdl::IExpression_direct_call *>(expression);
    const char *definition_name = call->get_definition();
    mi::base::Handle<const mdl::IFunction_definition> definition(
        definition_name
            ? transaction_->access<mdl::IFunction_definition>(definition_name)
            : nullptr);
    if (!definition)
      return {};
    if (definition->get_semantic() ==
        mdl::IFunction_definition::DS_INTRINSIC_STATE_TEXTURE_COORDINATE) {
      mi::base::Handle<const mdl::IExpression_list> arguments(
          call->get_arguments());
      if (!arguments)
        return {};
      for (mi::Size index = 0; index < arguments->get_size(); ++index) {
        const char *argument_name = arguments->get_name(index);
        if (!argument_name || std::string(argument_name) != "index")
          continue;
        mi::base::Handle<const mdl::IExpression> argument(
            arguments->get_expression(index));
        if (!argument ||
            argument->get_kind() != mdl::IExpression::EK_CONSTANT)
          return {};
        mi::base::Handle<const mdl::IValue> value(
            static_cast<const mdl::IExpression_constant *>(argument.get())
                ->get_value());
        if (!value || value->get_kind() != mdl::IValue::VK_INT)
          return {};
        return static_cast<const mdl::IValue_int *>(value.get())->get_value();
      }
      return {};
    }

    mi::base::Handle<const mdl::IExpression_list> arguments(
        call->get_arguments());
    if (!arguments)
      return {};
    std::optional<mi::Sint32> result;
    for (mi::Size index = 0; index < arguments->get_size(); ++index) {
      mi::base::Handle<const mdl::IExpression> argument(
          arguments->get_expression(index));
      const std::optional<mi::Sint32> nested =
          texture_coordinate_index(argument.get());
      if (!nested)
        continue;
      if (result && result != nested)
        return {};
      result = nested;
    }
    return result;
  }

  MxExpression translate_texture_lookup(
      const mdl::IExpression_direct_call *expression) {
    mi::base::Handle<const mdl::IExpression_list> arguments(
        expression->get_arguments());
    mi::base::Handle<const mdl::IExpression> texture_expression;
    mi::base::Handle<const mdl::IExpression> coordinate_expression;
    mi::base::Handle<const mdl::IExpression> wrap_u_expression;
    mi::base::Handle<const mdl::IExpression> wrap_v_expression;
    if (arguments) {
      for (mi::Size index = 0; index < arguments->get_size(); ++index) {
        const char *argument_name = arguments->get_name(index);
        if (!argument_name)
          continue;
        mi::base::Handle<const mdl::IExpression> argument(
            arguments->get_expression(index));
        const std::string name(argument_name);
        if (name == "tex")
          texture_expression = argument;
        else if (name == "coord")
          coordinate_expression = argument;
        else if (name == "wrap_u")
          wrap_u_expression = argument;
        else if (name == "wrap_v")
          wrap_v_expression = argument;
      }
    }

    const mdl::IExpression *resource_expression = texture_expression.get();
    mi::base::Handle<const mdl::IExpression> temporary;
    while (resource_expression &&
           resource_expression->get_kind() == mdl::IExpression::EK_TEMPORARY) {
      temporary = compiled_->get_temporary(
          static_cast<const mdl::IExpression_temporary *>(resource_expression)
              ->get_index());
      resource_expression = temporary.get();
    }
    mi::base::Handle<const mdl::IValue> resource_value;
    if (resource_expression &&
        resource_expression->get_kind() == mdl::IExpression::EK_CONSTANT) {
      resource_value = static_cast<const mdl::IExpression_constant *>(
                           resource_expression)
                           ->get_value();
    }
    if (!resource_value ||
        resource_value->get_kind() != mdl::IValue::VK_TEXTURE) {
      report_.warn(material_ +
                   ": MDL texture lookup has no constant texture resource.");
      return {};
    }

    const auto *texture =
        static_cast<const mdl::IValue_texture *>(resource_value.get());
    mi::base::Handle<const mdl::IType_texture> texture_type(
        texture->get_type());
    if (texture_type->get_shape() != mdl::IType_texture::TS_2D) {
      report_.warn(material_ +
                   ": only MDL 2D textures can be represented as MaterialX "
                   "image nodes.");
      return {};
    }
    const char *resolved_file_path = nullptr;
    const char *original_file_path = nullptr;
    if (const char *texture_name = texture->get_value();
        texture_name && texture_name[0]) {
      mi::base::Handle<const mdl::ITexture> texture_object(
          transaction_->access<mdl::ITexture>(texture_name));
      const char *image_name =
          texture_object ? texture_object->get_image() : nullptr;
      mi::base::Handle<const mdl::IImage> image(
          image_name ? transaction_->access<mdl::IImage>(image_name)
                     : nullptr);
      if (image) {
        resolved_file_path = image->get_filename(0, 0);
        original_file_path = image->get_original_filename();
      }
    }

    const char *file_path = texture->get_file_path();
    std::filesystem::path texture_path;
    std::filesystem::path first_candidate;
    const auto consider_file_path = [&](const char *candidate) {
      if (!candidate || !candidate[0])
        return false;
      std::filesystem::path path(candidate);
      const auto consider_path = [&](std::filesystem::path candidate_path) {
        candidate_path = std::filesystem::absolute(candidate_path);
        if (first_candidate.empty())
          first_candidate = candidate_path;
        if (!std::filesystem::exists(candidate_path))
          return false;
        texture_path = std::move(candidate_path);
        return true;
      };
      if (path.is_absolute()) {
        if (consider_path(path))
          return true;
        return consider_path(resource_directory_ / path.relative_path());
      }
      return consider_path(resource_directory_ / path);
    };
    if (!consider_file_path(resolved_file_path) &&
        !consider_file_path(original_file_path)) {
      consider_file_path(file_path);
    }

    if (texture_path.empty())
      texture_path = std::move(first_candidate);

    if (texture_path.empty()) {
      report_.warn(material_ +
                   ": MDL texture resource has no usable source filename.");
      return {};
    }

    if (!std::filesystem::exists(texture_path)) {
      report_.warn(material_ + ": MDL texture file does not exist: " +
                   texture_path.string());
      return {};
    }

    std::error_code path_error;
    const std::filesystem::path relative_path =
        std::filesystem::relative(texture_path, output_directory_, path_error);
    const std::string materialx_path =
        path_error ? texture_path.generic_string()
                   : relative_path.generic_string();

    mi::base::Handle<const mdl::IType> expression_type(
        expression->get_type());
    const std::string output_type =
        mdl_type_to_materialx(expression_type.get());
    if (output_type != "float" && output_type != "color3" &&
        output_type != "vector2" && output_type != "vector3" &&
        output_type != "vector4") {
      report_.warn(material_ +
                   ": MDL texture lookup has an unsupported output type.");
      return {};
    }

    mx::NodePtr image =
        graph_->addNode("image", next_name("texture_image"), output_type);
    image->setNodeDefString("ND_image_" + output_type);
    add_node_inputs_from_definition(image);
    mx::InputPtr file_input = image->getInput("file");
    if (!file_input) {
      report_.warn(material_ +
                   ": MaterialX image node has no filename input.");
      return {};
    }
    file_input->setValueString(materialx_path);
    if (texture->get_gamma() > 1.8f && texture->get_gamma() < 2.5f)
      file_input->setColorSpace("srgb_texture");

    const auto connect_wrap_mode = [&image](const std::string &input,
                                            const std::string &mode) {
      const std::string materialx_mode =
          mode == "wrap_clamp"
              ? "clamp"
              : (mode == "wrap_mirrored_repeat"
                     ? "mirror"
                     : (mode == "wrap_clip" ? "constant" : "periodic"));
      if (mx::InputPtr target = image->getInput(input))
        target->setValueString(materialx_mode);
    };
    if (wrap_u_expression)
      connect_wrap_mode("uaddressmode", enum_name(wrap_u_expression.get()));
    if (wrap_v_expression)
      connect_wrap_mode("vaddressmode", enum_name(wrap_v_expression.get()));

    const std::optional<mi::Sint32> coordinate_index =
        texture_coordinate_index(coordinate_expression.get());
    if (coordinate_index) {
      mx::NodePtr coordinates =
          graph_->addNode("texcoord", next_name("texture_coordinates"),
                          "vector2");
      coordinates->setNodeDefString("ND_texcoord_vector2");
      add_node_inputs_from_definition(coordinates);
      coordinates->getInput("index")->setValueString(
          std::to_string(*coordinate_index));
      image->getInput("texcoord")->setConnectedNode(coordinates);
      ++report_.translated_nodes;
      report_.warn(
          material_ +
          ": MDL texture coordinates were mapped to the matching MaterialX "
          "UV set; coordinate transforms may differ.");
    } else {
      MxExpression coordinates = translate(coordinate_expression.get());
      if (coordinates.valid() && coordinates.type == "vector2") {
        connect_expression(coordinates, image->getInput("texcoord"), report_,
                           material_, "texture coordinates");
      } else {
        report_.warn(material_ +
                     ": MDL texture coordinates could not be represented; "
                     "MaterialX's default UV0 is used.");
      }
    }

    ++report_.translated_nodes;
    return {image, {}, output_type};
  }

  MxExpression translate_weighted_layer(
      const mdl::IExpression_direct_call *expression) {
    mi::base::Handle<const mdl::IExpression_list> arguments(
        expression->get_arguments());
    mi::base::Handle<const mdl::IExpression> weight_expression;
    mi::base::Handle<const mdl::IExpression> layer_expression;
    mi::base::Handle<const mdl::IExpression> base_expression;
    if (arguments) {
      for (mi::Size index = 0; index < arguments->get_size(); ++index) {
        const char *argument_name = arguments->get_name(index);
        if (!argument_name)
          continue;
        mi::base::Handle<const mdl::IExpression> argument(
            arguments->get_expression(index));
        const std::string name(argument_name);
        if (name == "weight")
          weight_expression = argument;
        else if (name == "layer")
          layer_expression = argument;
        else if (name == "base")
          base_expression = argument;
      }
    }

    MxExpression layer = translate(layer_expression.get());
    MxExpression base = translate(base_expression.get());
    MxExpression weight = translate(weight_expression.get());
    if (!layer.valid() || layer.type != "BSDF" || !base.valid() ||
        base.type != "BSDF" || !weight.valid() || weight.type != "float") {
      MxExpression approximation = base.valid() ? base : layer;
      if (approximation.valid())
        report_.warn(material_ +
                     ": MDL weighted_layer was reduced to one BSDF "
                     "component because its inputs could not be translated.");
      return approximation;
    }

    mx::NodePtr weighted_layer =
        graph_->addNode("multiply", next_name("weighted_layer_top"), "BSDF");
    weighted_layer->setNodeDefString("ND_multiply_bsdfF");
    add_node_inputs_from_definition(weighted_layer);
    connect_expression(layer, weighted_layer->getInput("in1"), report_,
                       material_, "weighted_layer layer");
    connect_expression(weight, weighted_layer->getInput("in2"), report_,
                       material_, "weighted_layer weight");

    mx::NodePtr layer_node =
        graph_->addNode("layer", next_name("weighted_layer"), "BSDF");
    layer_node->setNodeDefString("ND_layer_bsdf");
    add_node_inputs_from_definition(layer_node);
    connect_expression({weighted_layer, {}, "BSDF"},
                       layer_node->getInput("top"), report_, material_,
                       "weighted_layer top");
    connect_expression(base, layer_node->getInput("base"), report_,
                       material_, "weighted_layer base");
    report_.warn(material_ +
                 ": MDL weighted_layer was mapped to MaterialX vertical "
                 "layering; its explicit normal input is not preserved.");
    report_.translated_nodes += 2;
    return {layer_node, {}, "BSDF"};
  }

  MxExpression translate_color_lerp(
      const mdl::IExpression_direct_call *expression) {
    mi::base::Handle<const mdl::IExpression_list> arguments(
        expression->get_arguments());
    mi::base::Handle<const mdl::IExpression> x_expression;
    mi::base::Handle<const mdl::IExpression> y_expression;
    mi::base::Handle<const mdl::IExpression> weight_expression;
    if (arguments && arguments->get_size() >= 3) {
      x_expression = arguments->get_expression(mi::Size(0));
      y_expression = arguments->get_expression(mi::Size(1));
      weight_expression = arguments->get_expression(mi::Size(2));
    }

    MxExpression x = translate(x_expression.get());
    MxExpression y = translate(y_expression.get());
    MxExpression weight = translate(weight_expression.get());
    if (!x.valid() || !y.valid() || !weight.valid() ||
        weight.type != "color3") {
      report_.warn(material_ +
                   ": componentwise color lerp could not be represented.");
      return {};
    }

    mx::NodePtr difference =
        graph_->addNode("subtract", next_name("color_lerp_delta"), "color3");
    add_node_inputs_from_definition(difference);
    connect_expression(y, difference->getInput("in1"), report_, material_,
                       "color lerp upper value");
    connect_expression(x, difference->getInput("in2"), report_, material_,
                       "color lerp lower value");

    mx::NodePtr scaled =
        graph_->addNode("multiply", next_name("color_lerp_scaled"), "color3");
    add_node_inputs_from_definition(scaled);
    connect_expression({difference, {}, "color3"}, scaled->getInput("in1"),
                       report_, material_, "color lerp delta");
    connect_expression(weight, scaled->getInput("in2"), report_, material_,
                       "color lerp weight");

    mx::NodePtr result =
        graph_->addNode("add", next_name("color_lerp"), "color3");
    add_node_inputs_from_definition(result);
    connect_expression(x, result->getInput("in1"), report_, material_,
                       "color lerp lower value");
    connect_expression({scaled, {}, "color3"}, result->getInput("in2"),
                       report_, material_, "color lerp scaled delta");
    report_.translated_nodes += 3;
    return {result, {}, "color3"};
  }

  MxExpression translate_microfacet_roughness(
      const mdl::IExpression *expression) {
    if (!expression)
      return {};
    if (expression->get_kind() == mdl::IExpression::EK_TEMPORARY) {
      const auto *temporary =
          static_cast<const mdl::IExpression_temporary *>(expression);
      mi::base::Handle<const mdl::IExpression> value(
          compiled_->get_temporary(temporary->get_index()));
      return translate_microfacet_roughness(value.get());
    }
    if (expression->get_kind() != mdl::IExpression::EK_DIRECT_CALL)
      return {};

    const auto *call =
        static_cast<const mdl::IExpression_direct_call *>(expression);
    const char *definition_name = call->get_definition();
    mi::base::Handle<const mdl::IFunction_definition> definition(
        definition_name
            ? transaction_->access<mdl::IFunction_definition>(definition_name)
            : nullptr);
    if (!definition)
      return {};

    mi::base::Handle<const mdl::IExpression_list> arguments(
        call->get_arguments());
    mi::base::Handle<const mdl::IExpression> roughness_u;
    mi::base::Handle<const mdl::IExpression> roughness_v;
    mi::base::Handle<const mdl::IExpression> base;
    mi::base::Handle<const mdl::IExpression> layer;
    if (arguments) {
      for (mi::Size index = 0; index < arguments->get_size(); ++index) {
        const char *argument_name = arguments->get_name(index);
        if (!argument_name)
          continue;
        mi::base::Handle<const mdl::IExpression> argument(
            arguments->get_expression(index));
        const std::string name(argument_name);
        if (name == "roughness_u")
          roughness_u = argument;
        else if (name == "roughness_v")
          roughness_v = argument;
        else if (name == "base")
          base = argument;
        else if (name == "layer")
          layer = argument;
      }
    }

    const auto semantic = definition->get_semantic();
    if (semantic ==
            mdl::IFunction_definition::DS_INTRINSIC_DF_MICROFACET_GGX_SMITH_BSDF ||
        semantic ==
            mdl::IFunction_definition::DS_INTRINSIC_DF_MICROFACET_BECKMANN_SMITH_BSDF ||
        semantic ==
            mdl::IFunction_definition::DS_INTRINSIC_DF_MICROFACET_GGX_VCAVITIES_BSDF ||
        semantic ==
            mdl::IFunction_definition::DS_INTRINSIC_DF_MICROFACET_BECKMANN_VCAVITIES_BSDF) {
      if (!roughness_u)
        return {};
      MxExpression u = translate(roughness_u.get());
      if (!u.valid() || u.type != "float")
        return {};
      MxExpression v = roughness_v ? translate(roughness_v.get()) : u;
      if (!v.valid() || v.type != "float")
        v = u;

      mx::NodePtr vector =
          graph_->addNode("combine2", next_name("roughness"), "vector2");
      add_node_inputs_from_definition(vector);
      connect_expression(u, vector->getInput("in1"), report_, material_,
                         "roughness U");
      connect_expression(v, vector->getInput("in2"), report_, material_,
                         "roughness V");
      ++report_.translated_nodes;
      return {vector, {}, "vector2"};
    }

    MxExpression result = translate_microfacet_roughness(base.get());
    if (!result.valid())
      result = translate_microfacet_roughness(layer.get());
    return result;
  }

  MxExpression translate_call(const mdl::IExpression_direct_call *expression) {
    const char *definition_name = expression->get_definition();
    mi::base::Handle<const mdl::IFunction_definition> definition(
        definition_name
            ? transaction_->access<mdl::IFunction_definition>(definition_name)
            : nullptr);
    if (!definition) {
      report_.warn(material_ +
                   ": could not resolve an MDL expression definition.");
      return {};
    }

    const auto semantic = definition->get_semantic();
    if (semantic == mdl::IFunction_definition::DS_INTRINSIC_DF_TINT)
      return translate_tint(expression);
    if (semantic == mdl::IFunction_definition::DS_INTRINSIC_MATH_LERP) {
      mi::base::Handle<const mdl::IExpression_list> arguments(
          expression->get_arguments());
      if (arguments) {
        for (mi::Size index = 0; index < arguments->get_size(); ++index) {
          const char *argument_name = arguments->get_name(index);
          if (!argument_name || std::string(argument_name) != "l")
            continue;
          mi::base::Handle<const mdl::IExpression> argument(
              arguments->get_expression(index));
          mi::base::Handle<const mdl::IType> type(
              argument ? argument->get_type() : nullptr);
          if (mdl_type_to_materialx(type.get()) == "color3")
            return translate_color_lerp(expression);
        }
      }
    }
    if (semantic == mdl::IFunction_definition::
                        DS_INTRINSIC_DF_MICROFACET_GGX_SMITH_BSDF)
      return translate_microfacet(expression);
    if (semantic ==
        mdl::IFunction_definition::DS_INTRINSIC_DF_FRESNEL_FACTOR)
      return translate_fresnel_factor(expression);
    if (semantic == mdl::IFunction_definition::DS_INTRINSIC_TEX_LOOKUP_COLOR ||
        semantic == mdl::IFunction_definition::DS_INTRINSIC_TEX_LOOKUP_FLOAT ||
        semantic == mdl::IFunction_definition::DS_INTRINSIC_TEX_LOOKUP_FLOAT2 ||
        semantic == mdl::IFunction_definition::DS_INTRINSIC_TEX_LOOKUP_FLOAT3 ||
        semantic == mdl::IFunction_definition::DS_INTRINSIC_TEX_LOOKUP_FLOAT4)
      return translate_texture_lookup(expression);
    if (semantic ==
        mdl::IFunction_definition::DS_INTRINSIC_DF_WEIGHTED_LAYER)
      return translate_weighted_layer(expression);

    if (semantic ==
        mdl::IFunction_definition::DS_INTRINSIC_DF_THIN_FILM) {
      return translate_thin_film(expression);
    }

    if (semantic ==
            mdl::IFunction_definition::DS_INTRINSIC_DF_CUSTOM_CURVE_LAYER ||
        semantic ==
            mdl::IFunction_definition::DS_INTRINSIC_DF_COLOR_CUSTOM_CURVE_LAYER) {
      mi::base::Handle<const mdl::IExpression_list> arguments(
          expression->get_arguments());
      mi::base::Handle<const mdl::IExpression> base;
      mi::base::Handle<const mdl::IExpression> layer;
      mi::base::Handle<const mdl::IExpression> weight;
      if (arguments) {
        for (mi::Size index = 0; index < arguments->get_size(); ++index) {
          const char *argument_name = arguments->get_name(index);
          mi::base::Handle<const mdl::IExpression> argument(
              arguments->get_expression(index));
          if (argument_name && std::string(argument_name) == "base")
            base = argument;
          else if (argument_name && std::string(argument_name) == "layer")
            layer = argument;
          else if (argument_name && std::string(argument_name) == "weight")
            weight = argument;
        }
      }

      MxExpression base_bsdf = translate(base.get());
      MxExpression layer_bsdf = translate(layer.get());
      MxExpression layer_weight = translate(weight.get());
      if (base_bsdf.valid() && base_bsdf.type == "BSDF" &&
          layer_bsdf.valid() && layer_bsdf.type == "BSDF" &&
          layer_bsdf.node && layer_bsdf.node->getInput("weight") &&
          layer_weight.valid() && layer_weight.type == "float") {
        mx::NodePtr layer_node = graph_->addNode(
            "layer", next_name("custom_curve_layer"), "BSDF");
        layer_node->addInput("top", "BSDF");
        layer_node->addInput("base", "BSDF");
        connect_expression(layer_weight, layer_bsdf.node->getInput("weight"),
                           report_, material_, "custom-curve weight");
        connect_expression(layer_bsdf, layer_node->getInput("top"), report_,
                           material_, "custom-curve layer");
        connect_expression(base_bsdf, layer_node->getInput("base"), report_,
                           material_, "custom-curve base");
        ++report_.translated_nodes;
        return {layer_node, {}, "BSDF"};
      }
      if (base_bsdf.valid() && base_bsdf.type == "BSDF" &&
          layer_bsdf.valid() && layer_bsdf.type == "BSDF" &&
          layer_weight.valid() && layer_weight.type == "float") {
        mx::NodePtr mix = graph_->addNode(
            "mix", next_name("custom_curve_layer"), "BSDF");
        add_node_inputs_from_definition(mix);
        connect_expression(base_bsdf, mix->getInput("bg"), report_, material_,
                           "custom-curve base");
        connect_expression(layer_bsdf, mix->getInput("fg"), report_,
                           material_, "custom-curve layer");
        connect_expression(layer_weight, mix->getInput("mix"), report_,
                           material_, "custom-curve weight");
        report_.warn(material_ +
                     ": MDL custom-curve layering was approximated with a "
                     "MaterialX BSDF mix; the Fresnel curve is not "
                     "reproduced.");
        ++report_.translated_nodes;
        return {mix, {}, "BSDF"};
      }

      MxExpression approximation = base_bsdf.valid() ? base_bsdf : layer_bsdf;
      if (approximation.valid()) {
        report_.warn(material_ +
                     ": MDL custom-curve layering was approximated by a "
                     "single component BSDF.");
      } else {
        report_.warn(material_ +
                     ": MDL custom-curve layer has no translatable BSDF "
                     "component.");
      }
      return approximation;
    }

    const char *category = nullptr;
    bool diffuse_approximation = false;
    bool diffuse_emission = false;
    bool state_normal = false;
    bool elemental_constructor = false;
    if (semantic == mdl::IFunction_definition::
                        DS_INTRINSIC_DF_DIFFUSE_REFLECTION_BSDF) {
      category = "oren_nayar_diffuse_bsdf";
      diffuse_approximation = true;
    } else if (semantic == mdl::IFunction_definition::
                               DS_INTRINSIC_DF_DIFFUSE_EDF) {
      category = "uniform_edf";
      diffuse_emission = true;
    } else if (semantic ==
               mdl::IFunction_definition::DS_INTRINSIC_STATE_NORMAL) {
      category = "normal";
      state_normal = true;
    } else if (semantic ==
               mdl::IFunction_definition::DS_ELEM_CONSTRUCTOR) {
      mi::base::Handle<const mdl::IType> type(expression->get_type());
      const std::string type_name = mdl_type_to_materialx(type.get());
      if (type_name == "color3" || type_name == "vector3")
        category = "combine3";
      else if (type_name == "vector2")
        category = "combine2";
      else if (type_name == "vector4")
        category = "combine4";
      elemental_constructor = category != nullptr;
    } else {
      category = math_node_category(semantic);
    }

    if (!category) {
      const char *mdl_name = definition->get_mdl_name();
      report_.warn(material_ + ": unsupported MDL call " +
                   (mdl_name ? mdl_name : definition_name) +
                   "; that expression was omitted.");
      return {};
    }

    mi::base::Handle<const mdl::IType> expression_type(
        expression->get_type());
    const std::string output_type =
        diffuse_approximation
              ? "BSDF"
              : ((diffuse_emission || state_normal)
                     ? (diffuse_emission ? "EDF" : "vector3")
                     : mdl_type_to_materialx(expression_type.get()));
    if (output_type.empty()) {
      report_.warn(material_ + ": unsupported output type for MDL call.");
      return {};
    }
    const bool color_sqrt =
        semantic == mdl::IFunction_definition::DS_INTRINSIC_MATH_SQRT &&
        output_type == "color3";
    const bool color_average =
        semantic == mdl::IFunction_definition::DS_INTRINSIC_MATH_AVERAGE &&
        output_type == "float";
    if (color_sqrt)
      category = "power";
    if (color_average)
      category = "dotproduct";

    mx::NodePtr node = graph_->addNode(category, next_name(category),
                                       output_type);
    if (color_average)
      node->setNodeDefString("ND_dotproduct_vector3");
    add_node_inputs_from_definition(node);
    if (state_normal)
      node->getInput("space")->setValueString("world");
    if (color_sqrt)
      node->getInput("in2")->setValueString("0.5, 0.5, 0.5");
    if (color_average)
      node->getInput("in2")->setValueString(
          "0.333333333, 0.333333333, 0.333333333");
    if (semantic ==
        mdl::IFunction_definition::DS_INTRINSIC_MATH_SATURATE) {
      node->getInput("low")->setValueString("0.0");
      node->getInput("high")->setValueString("1.0");
    }
    ++report_.translated_nodes;
    if (diffuse_approximation) {
      report_.warn(material_ +
                   ": MDL diffuse_reflection_bsdf was mapped to MaterialX "
                   "oren_nayar_diffuse_bsdf; MDL multiscatter_tint has no "
                   "corresponding standard input.");
    } else if (semantic ==
               mdl::IFunction_definition::DS_INTRINSIC_DF_DIFFUSE_EDF) {
      report_.warn(material_ +
                   ": MDL diffuse_edf was mapped to MaterialX uniform_edf; "
                   "emission normalization is not guaranteed equivalent.");
    }

    mi::base::Handle<const mdl::IExpression_list> arguments(
        expression->get_arguments());
    if (!arguments)
      return {node, {}, output_type};

    if (elemental_constructor) {
      const std::vector<mx::InputPtr> targets = node->getInputs();
      if (arguments->get_size() != 1 &&
          arguments->get_size() != targets.size()) {
        report_.warn(material_ +
                     ": an MDL vector/color constructor has an unsupported "
                     "number of components.");
        return {node, {}, output_type};
      }
      for (mi::Size index = 0; index < targets.size(); ++index) {
        const mi::Size argument_index =
            arguments->get_size() == 1 ? 0 : index;
        mi::base::Handle<const mdl::IExpression> argument(
            arguments->get_expression(argument_index));
        MxExpression value = translate(argument.get());
        if (value.valid())
          connect_expression(value, targets[index], report_, material_,
                             "vector/color constructor component");
      }
      return {node, {}, output_type};
    }

    for (mi::Size index = 0; index < arguments->get_size(); ++index) {
      const char *argument_name = arguments->get_name(index);
      mi::base::Handle<const mdl::IExpression> argument(
          arguments->get_expression(index));
      mx::InputPtr target =
          diffuse_approximation || diffuse_emission
              ? diffuse_input(node, argument_name ? argument_name : "")
              : math_input(node, index, semantic);
      if (!target) {
        const char *mdl_name = definition->get_mdl_name();
        report_.warn(material_ + ": omitted MDL argument " +
                     (argument_name ? argument_name : "<unnamed>") +
                     " from " + (mdl_name ? mdl_name : definition_name) +
                     " because there is no corresponding MaterialX input.");
        continue;
      }
      MxExpression value = translate(argument.get());
      if (!value.valid())
        continue;
      connect_expression(value, target, report_, material_,
                         argument_name ? argument_name : "MDL argument");
    }
    return {node, {}, output_type};
  }

  mx::InputPtr diffuse_input(const mx::NodePtr &node,
                             const std::string &argument_name) {
    if (argument_name == "tint")
      return node->getInput("color");
    return node->getInput(argument_name);
  }

  mx::InputPtr math_input(
      const mx::NodePtr &node, mi::Size index,
      mdl::IFunction_definition::Semantics semantic) {
    if (semantic == mdl::IFunction_definition::DS_INTRINSIC_MATH_LERP) {
      if (index == 0)
        return node->getInput("bg");
      if (index == 1)
        return node->getInput("fg");
      if (index == 2)
        return node->getInput("mix");
      return {};
    }
    if (semantic == mdl::IFunction_definition::
                        DS_INTRINSIC_MATH_SMOOTHSTEP) {
      if (index == 0)
        return node->getInput("low");
      if (index == 1)
        return node->getInput("high");
      if (index == 2)
        return node->getInput("in");
      return {};
    }
    if (semantic == mdl::IFunction_definition::DS_INTRINSIC_MATH_SATURATE) {
      return index == 0 ? node->getInput("in") : mx::InputPtr{};
    }
    const std::vector<mx::InputPtr> inputs = node->getInputs();
    if (index >= inputs.size())
      return {};
    return inputs[index];
  }

  std::string next_name(const std::string &prefix) {
    return unique_element_name(
        graph_, safe_material_name(prefix) + "_" +
                    std::to_string(node_index_++));
  }

  const mdl::ICompiled_material *compiled_;
  mdl::ITransaction *transaction_;
  mx::NodeGraphPtr graph_;
  ConversionReport &report_;
  std::string material_;
  std::filesystem::path output_directory_;
  std::filesystem::path resource_directory_;
  mi::Size node_index_ = 0;
  std::map<mi::Size, std::string> parameter_names_;
  std::map<std::pair<mi::Size, std::size_t>, MxExpression> temporaries_;
  std::vector<ThinFilmParameters> thin_films_;
  std::size_t next_thin_film_scope_id_ = 1;
};

void add_black_bsdf(const mx::NodeGraphPtr &graph,
                    const mx::NodePtr &surface, const std::string &node_name) {
  mx::NodePtr black =
      graph->addNode("oren_nayar_diffuse_bsdf",
                     unique_element_name(graph, node_name), "BSDF");
  add_node_inputs_from_definition(black);
  black->getInput("color")->setValueString("0.0, 0.0, 0.0");
  surface->getInput("bsdf")->setConnectedNode(black);
}

bool connect_expression(const MxExpression &expression,
                        const mx::InputPtr &input,
                        ConversionReport &report,
                        const std::string &material,
                        const std::string &slot) {
  if (!expression.valid())
    return false;
  if (input->getType() != expression.type) {
    const std::string target_type = input->getType();
    const bool scalar_promotion =
        expression.type == "float" &&
        (target_type == "color3" || target_type == "color4" ||
         target_type == "vector2" || target_type == "vector3" ||
         target_type == "vector4");
    const bool color_vector_conversion =
        (expression.type == "color3" && target_type == "vector3") ||
        (expression.type == "vector3" && target_type == "color3");
    if (scalar_promotion || color_vector_conversion) {
      mx::NodePtr target_node = input->getParent()->asA<mx::Node>();
      mx::NodeGraphPtr graph =
          target_node ? target_node->getParent()->asA<mx::NodeGraph>()
                      : nullptr;
      if (graph) {
        const std::string definition_name =
            "ND_convert_" + expression.type + "_" + target_type;
        mx::NodePtr conversion = graph->addNode(
            "convert", unique_element_name(graph, "type_conversion"),
            target_type);
        conversion->setNodeDefString(definition_name);
        if (conversion->getNodeDef())
          conversion->addInputsFromNodeDef();
        mx::InputPtr conversion_input =
            conversion ? conversion->getInput("in") : nullptr;
        if (conversion_input &&
            connect_expression(expression, conversion_input, report, material,
                              slot)) {
          input->setConnectedNode(conversion);
          ++report.translated_nodes;
          return true;
        }
      }
    }
    report.warn(material + ": " + slot + " has MaterialX type " +
                expression.type + ", but the target input requires " +
                input->getType() + ".");
    return false;
  }
  if (expression.node)
    input->setConnectedNode(expression.node);
  else
    input->setConnectedInterfaceName(expression.interface_name);
  return true;
}

MxExpression translate_slot(ExpressionTranslator &translator,
                            const mdl::ICompiled_material *compiled,
                            const char *path) {
  mi::base::Handle<const mdl::IExpression> expression(
      compiled->lookup_sub_expression(path));
  return translator.translate(expression.get());
}

void connect_emission(const mx::NodeGraphPtr &graph,
                      ExpressionTranslator &translator,
                      const mdl::ICompiled_material *compiled,
                      const mx::NodePtr &surface, ConversionReport &report,
                      const std::string &material, const std::string &prefix) {
  mi::base::Handle<const mdl::IExpression> emission_expression(
      compiled->lookup_sub_expression((prefix + ".emission").c_str()));
  if (!emission_expression ||
      is_neutral_expression(emission_expression.get()))
    return;

  MxExpression emission = translator.translate(emission_expression.get());
  if (!emission.valid() || emission.type != "EDF") {
    report.warn(material + ": " + prefix +
                ".emission could not be represented as an EDF.");
    return;
  }

  MxExpression intensity = translate_slot(
      translator, compiled, (prefix + ".intensity").c_str());
  if (intensity.valid()) {
    mx::NodePtr multiply = graph->addNode(
        "multiply",
        unique_element_name(graph, safe_material_name(prefix) + "_intensity"),
        "EDF");
    multiply->addInput("in1", "EDF");
    multiply->addInput("in2", intensity.type);
    add_node_inputs_from_definition(multiply);
    connect_expression(emission, multiply->getInput("in1"), report, material,
                       prefix + ".emission");
    connect_expression(intensity, multiply->getInput("in2"), report, material,
                       prefix + ".intensity");
    surface->getInput("edf")->setConnectedNode(multiply);
  } else {
    surface->getInput("edf")->setConnectedNode(emission.node);
    if (!emission.node)
      surface->getInput("edf")
          ->setConnectedInterfaceName(emission.interface_name);
  }

  mi::base::Handle<const mdl::IExpression> mode(
      compiled->lookup_sub_expression((prefix + ".mode").c_str()));
  if (mode && !is_neutral_expression(mode.get()))
    report.warn(material + ": " + prefix +
                ".mode is not represented by the MaterialX emission graph.");
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
      graph->addNode(
          "surface",
          unique_element_name(graph, "backface_surface_shader"),
          "surfaceshader");
  add_node_inputs_from_definition(shader);
  if (!is_neutral_expression(scattering.get())) {
    MxExpression value = translator.translate(scattering.get());
    if (!value.valid() || value.type != "BSDF") {
      report.warn(name +
                  ": backface.scattering is unsupported; using an explicit "
                  "black diffuse fallback.");
      add_black_bsdf(graph, shader, "unsupported_backface_bsdf");
    } else {
      connect_expression(value, shader->getInput("bsdf"), report, name,
                         "backface.scattering");
    }
  }
  connect_emission(graph, translator, compiled, shader, report, name,
                   "backface.emission");

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

void report_unsupported_ior(const mdl::ICompiled_material *compiled,
                           ConversionReport &report,
                           const std::string &material) {
  mi::base::Handle<const mdl::IExpression> expression(
      compiled->lookup_sub_expression("ior"));
  if (expression && !is_neutral_expression(expression.get()))
    report.warn(material +
                ": the material IOR slot is not yet translated to "
                "MaterialX.");
}

void add_material(mx::DocumentPtr document,
                  const mdl::IFunction_definition *definition,
                  mdl::ITransaction *transaction,
                  mdl::IMdl_factory *mdl_factory, const std::string &name,
                  const std::filesystem::path &output_directory,
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
      graph->addNode("surface", unique_element_name(graph, "surface_shader"),
                     "surfaceshader");
  add_node_inputs_from_definition(surface);

  ExpressionTranslator translator(compiled.get(), transaction, graph, report,
                                  name, output_directory, resource_directory);
  mi::base::Handle<const mdl::IExpression> scattering(
      compiled->lookup_sub_expression("surface.scattering"));
  MxExpression scattering_value = translator.translate(scattering.get());
  const bool scattering_connected =
      scattering_value.valid() && scattering_value.type == "BSDF" &&
      connect_expression(scattering_value, surface->getInput("bsdf"), report,
                         name, "surface.scattering");
  if (!scattering_connected) {
    report.warn(name +
                ": surface.scattering could not be translated; using an "
                "explicit black diffuse fallback.");
    add_black_bsdf(graph, surface, "unsupported_surface_bsdf");
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
  connect_emission(graph, translator, compiled.get(), surface, report, name,
                   "surface.emission");

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
  if (!is_neutral_expression(normal_expression.get())) {
    MxExpression normal = translator.translate(normal_expression.get());
    if (normal.valid() && normal.type == "vector3") {
      for (const mx::NodePtr &node : graph->getNodes()) {
        mx::InputPtr normal_input =
            node->getType() == "BSDF" ? node->getInput("normal")
                                      : mx::InputPtr();
        if (normal_input)
          connect_expression(normal, normal_input, report, name,
                             "geometry.normal");
      }
    } else {
      report.warn(name +
                  ": geometry.normal could not be represented as a vector3.");
    }
  }
  report_unsupported_ior(compiled.get(), report, name);
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
                   mdl_factory.get(), name,
                   std::filesystem::absolute(options.output).parent_path(),
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
