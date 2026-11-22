#include "conversion.h"
#include "materialx_utils.h"
#include "texture_resources.h"
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

#include <array>
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

bool is_unit_ior_value(const mdl::IValue *value) {
  if (!value)
    return false;
  if (value->get_kind() == mdl::IValue::VK_FLOAT)
    return static_cast<const mdl::IValue_float *>(value)->get_value() == 1.0f;
  if (value->get_kind() != mdl::IValue::VK_COLOR)
    return false;
  const auto *color = static_cast<const mdl::IValue_compound *>(value);
  for (mi::Size index = 0; index < color->get_size(); ++index) {
    mi::base::Handle<const mdl::IValue_float> component(
        color->get_value<mdl::IValue_float>(index));
    if (!component || component->get_value() != 1.0f)
      return false;
  }
  return true;
}

bool is_unit_ior_expression(const mdl::IExpression *expression,
                            const mdl::ICompiled_material *compiled) {
  if (!expression)
    return false;
  if (expression->get_kind() == mdl::IExpression::EK_TEMPORARY) {
    const auto *temporary =
        static_cast<const mdl::IExpression_temporary *>(expression);
    mi::base::Handle<const mdl::IExpression> value(
        compiled->get_temporary(temporary->get_index()));
    return is_unit_ior_expression(value.get(), compiled);
  }
  if (expression->get_kind() != mdl::IExpression::EK_CONSTANT)
    return false;
  mi::base::Handle<const mdl::IValue> value(
      static_cast<const mdl::IExpression_constant *>(expression)->get_value());
  return is_unit_ior_value(value.get());
}

bool is_state_normal_expression(const mdl::IExpression *expression,
                                const mdl::ICompiled_material *compiled,
                                mdl::ITransaction *transaction) {
  if (!expression)
    return false;
  if (expression->get_kind() == mdl::IExpression::EK_TEMPORARY) {
    const auto *temporary =
        static_cast<const mdl::IExpression_temporary *>(expression);
    mi::base::Handle<const mdl::IExpression> value(
        compiled->get_temporary(temporary->get_index()));
    return is_state_normal_expression(value.get(), compiled, transaction);
  }
  if (expression->get_kind() != mdl::IExpression::EK_DIRECT_CALL)
    return false;
  const char *definition_name =
      static_cast<const mdl::IExpression_direct_call *>(expression)
          ->get_definition();
  mi::base::Handle<const mdl::IFunction_definition> definition(
      definition_name
          ? transaction->access<mdl::IFunction_definition>(definition_name)
          : nullptr);
  return definition &&
         definition->get_semantic() ==
             mdl::IFunction_definition::DS_INTRINSIC_STATE_NORMAL;
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
                       TextureResourcePackager &texture_packager,
                       std::filesystem::path resource_directory)
      : compiled_(compiled), transaction_(transaction), graph_(graph),
        report_(report), material_(material),
        texture_packager_(texture_packager),
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

  MxExpression scalarize_color(const MxExpression &expression,
                               const std::string &prefix) {
    if (expression.type != "color3")
      return expression;

    mx::NodePtr average =
        graph_->addNode("dotproduct", next_name(prefix), "float");
    average->setNodeDefString("ND_dotproduct_vector3");
    add_node_inputs_from_definition(average);
    mx::InputPtr weights = average->getInput("in2");
    if (!weights) {
      report_.warn(material_ + ": could not create a scalar MaterialX " +
                   prefix + ".");
      return {};
    }
    weights->setValueString("0.333333333, 0.333333333, 0.333333333");
    if (!connect_expression(expression, average->getInput("in1"), report_,
                            material_, prefix))
      return {};

    ++report_.translated_nodes;
    return {average, {}, "float"};
  }

  bool approximate_bsdf(const mdl::IExpression *expression,
                        const mx::NodePtr &surface,
                        const MxExpression &layer_weight = {},
                        const MxExpression &tint_override = {}) {
    if (!expression)
      return false;
    if (expression->get_kind() == mdl::IExpression::EK_TEMPORARY) {
      const auto *temporary =
          static_cast<const mdl::IExpression_temporary *>(expression);
      mi::base::Handle<const mdl::IExpression> value(
          compiled_->get_temporary(temporary->get_index()));
      return approximate_bsdf(value.get(), surface, layer_weight,
                              tint_override);
    }
    if (expression->get_kind() == mdl::IExpression::EK_CONSTANT) {
      const auto *constant =
          static_cast<const mdl::IExpression_constant *>(expression);
      mi::base::Handle<const mdl::IValue> value(constant->get_value());
      if (value && value->get_kind() == mdl::IValue::VK_INVALID_DF) {
        if (!layer_weight.valid()) {
          set_preview_value(surface, "base", "0.0");
          set_preview_value(surface, "specular", "0.0");
          set_preview_value(surface, "transmission", "0.0");
        }
        return true;
      }
    }
    if (expression->get_kind() != mdl::IExpression::EK_DIRECT_CALL) {
      set_diffuse_fallback(surface, "non-call BSDF expression");
      return true;
    }

    const auto *call =
        static_cast<const mdl::IExpression_direct_call *>(expression);
    const char *definition_name = call->get_definition();
    mi::base::Handle<const mdl::IFunction_definition> definition(
        definition_name
            ? transaction_->access<mdl::IFunction_definition>(definition_name)
            : nullptr);
    if (!definition) {
      set_diffuse_fallback(surface, "unresolved BSDF call");
      return true;
    }

    using S = mdl::IFunction_definition;
    const auto semantic = definition->get_semantic();
    if (semantic == S::DS_TERNARY)
      return approximate_conditional_bsdf(call, surface, layer_weight,
                                          tint_override);
    if (semantic == S::DS_INTRINSIC_DF_DIFFUSE_REFLECTION_BSDF)
      return approximate_diffuse(call, surface, layer_weight, tint_override);
    if (semantic == S::DS_INTRINSIC_DF_MICROFACET_GGX_SMITH_BSDF)
      return approximate_microfacet(call, surface, layer_weight,
                                    tint_override);
    if (semantic == S::DS_INTRINSIC_DF_SPECULAR_BSDF)
      return approximate_specular(call, surface, layer_weight, tint_override);
    if (semantic == S::DS_INTRINSIC_DF_FRESNEL_FACTOR)
      return approximate_conductor(call, surface, tint_override);
    if (semantic == S::DS_INTRINSIC_DF_THIN_FILM)
      return approximate_thin_film(call, surface, tint_override);
    if (semantic == S::DS_INTRINSIC_DF_WEIGHTED_LAYER ||
        semantic == S::DS_INTRINSIC_DF_FRESNEL_LAYER ||
        semantic == S::DS_INTRINSIC_DF_CUSTOM_CURVE_LAYER ||
        semantic == S::DS_INTRINSIC_DF_COLOR_CUSTOM_CURVE_LAYER)
      return approximate_layer(call, surface, tint_override, semantic);
    if (semantic == S::DS_INTRINSIC_DF_TINT) {
      mi::base::Handle<const mdl::IExpression> bsdf = argument(call, "bsdf");
      mi::base::Handle<const mdl::IExpression> tint = argument(call, "tint");
      MxExpression translated_tint = translate(tint.get());
      if (tint_override.valid())
        translated_tint =
            multiply_colors(translated_tint, tint_override, "tint");
      return approximate_bsdf(bsdf.get(), surface, layer_weight,
                              translated_tint);
    }

    MxExpression tint = translate(argument(call, "tint").get());
    if (tint_override.valid())
      tint = multiply_colors(tint, tint_override, "fallback_tint");
    set_diffuse_fallback(surface, "unsupported BSDF");
    if (tint.valid())
      connect_preview_input(tint, surface, "base_color", "fallback tint");
    const char *mdl_name = definition->get_mdl_name();
    report_.warn(material_ + ": MDL BSDF " +
                 (mdl_name ? mdl_name : definition_name) +
                 " was approximated as diffuse because no standard_surface "
                 "mapping exists.");
    return true;
  }

private:
  mi::base::Handle<const mdl::IExpression>
  argument(const mdl::IExpression_direct_call *call,
           const std::string &name) const {
    mi::base::Handle<const mdl::IExpression_list> arguments(
        call->get_arguments());
    if (!arguments)
      return {};
    for (mi::Size index = 0; index < arguments->get_size(); ++index) {
      const char *argument_name = arguments->get_name(index);
      if (argument_name && name == argument_name)
        return mi::base::Handle<const mdl::IExpression>(
            arguments->get_expression(index));
    }
    return {};
  }

  bool connect_preview_input(const MxExpression &expression,
                             const mx::NodePtr &surface,
                             const std::string &input_name,
                             const std::string &slot) {
    mx::InputPtr input = surface->getInput(input_name);
    if (!input) {
      report_.warn(material_ +
                   ": standard_surface has no " + input_name +
                   " input for " + slot + ".");
      return false;
    }
    return connect_expression(expression, input, report_, material_, slot);
  }

  void set_preview_value(const mx::NodePtr &surface,
                         const std::string &input_name,
                         const std::string &value) {
    if (mx::InputPtr input = surface->getInput(input_name)) {
      input->setConnectedNode(nullptr);
      input->removeAttribute(mx::ValueElement::INTERFACE_NAME_ATTRIBUTE);
      input->setValueString(value);
    }
  }

  MxExpression input_expression(const mx::InputPtr &input) {
    if (mx::NodePtr node = input->getConnectedNode())
      return {node, {}, input->getType()};
    if (!input->getInterfaceName().empty())
      return {{}, input->getInterfaceName(), input->getType()};
    if (input->hasValueString())
      return make_constant_expression(input->getType(), input->getValueString(),
                                      "conditional_value");
    return {};
  }

  bool approximate_conditional_bsdf(const mdl::IExpression_direct_call *call,
                                    const mx::NodePtr &surface,
                                    const MxExpression &layer_weight,
                                    const MxExpression &tint_override) {
    MxExpression condition = translate(argument(call, "cond").get());
    if (!condition.valid() || condition.type != "boolean") {
      set_diffuse_fallback(surface, "untranslatable BSDF condition");
      return true;
    }

    const auto make_branch = [&]() {
      mx::NodePtr branch = graph_->addNode(
          "standard_surface", next_name("conditional_branch"), "surfaceshader");
      for (const mx::InputPtr &input : surface->getInputs()) {
        mx::InputPtr copy = branch->addInput(input->getName(), input->getType());
        copy->copyContentFrom(input);
      }
      return branch;
    };
    mx::NodePtr on_true = make_branch();
    mx::NodePtr on_false = make_branch();
    approximate_bsdf(argument(call, "true_exp").get(), on_true, layer_weight,
                     tint_override);
    approximate_bsdf(argument(call, "false_exp").get(), on_false, layer_weight,
                     tint_override);

    for (const mx::InputPtr &target : surface->getInputs()) {
      mx::InputPtr yes = on_true->getInput(target->getName());
      mx::InputPtr no = on_false->getInput(target->getName());
      if (yes->getNodeName() == no->getNodeName() &&
          yes->getInterfaceName() == no->getInterfaceName() &&
          yes->getValueString() == no->getValueString()) {
        target->copyContentFrom(yes);
        continue;
      }
      const std::string type = target->getType();
      if (type != "float" && type != "color3" && type != "vector3") {
        report_.warn(material_ + ": conditional BSDF input " + target->getName() +
                     " has an unsupported MaterialX type.");
        continue;
      }
      MxExpression yes_value = input_expression(yes);
      MxExpression no_value = input_expression(no);
      if (!yes_value.valid() || !no_value.valid()) {
        report_.warn(material_ + ": conditional BSDF input " + target->getName() +
                     " could not be represented in both branches.");
        continue;
      }
      mx::NodePtr select = graph_->addNode(
          "ifequal", next_name("conditional_bsdf"), type);
      select->setNodeDefString("ND_ifequal_" + type + "B");
      add_node_inputs_from_definition(select);
      select->getInput("value2")->setValueString("true");
      connect_expression(condition, select->getInput("value1"), report_,
                         material_, "BSDF condition");
      connect_expression(yes_value, select->getInput("in1"), report_, material_,
                         "true BSDF branch");
      connect_expression(no_value, select->getInput("in2"), report_, material_,
                         "false BSDF branch");
      connect_expression({select, {}, type}, target, report_, material_,
                         "conditional BSDF input");
      ++report_.translated_nodes;
    }
    graph_->removeNode(on_true->getName());
    graph_->removeNode(on_false->getName());
    return true;
  }

  void set_diffuse_fallback(const mx::NodePtr &surface,
                            const std::string &reason) {
    set_preview_value(surface, "base", "1.0");
    set_preview_value(surface, "base_color", "0.18, 0.18, 0.18");
    set_preview_value(surface, "specular", "0.0");
    set_preview_value(surface, "transmission", "0.0");
    report_.warn(material_ + ": " + reason +
                 " was approximated with neutral gray diffuse.");
  }

  MxExpression make_constant_expression(const std::string &type,
                                        const std::string &value,
                                        const std::string &prefix) {
    mx::NodePtr node =
        graph_->addNode("constant", next_name(prefix), type);
    add_node_inputs_from_definition(node);
    mx::InputPtr input = node->getInput("value");
    if (!input)
      return {};
    input->setValueString(value);
    ++report_.translated_nodes;
    return {node, {}, type};
  }

  MxExpression make_binary_expression(const std::string &category,
                                      const std::string &type,
                                      const MxExpression &left,
                                      const MxExpression &right,
                                      const std::string &prefix) {
    mx::NodePtr node = graph_->addNode(category, next_name(prefix), type);
    add_node_inputs_from_definition(node);
    if (!connect_expression(left, node->getInput("in1"), report_, material_,
                            prefix) ||
        !connect_expression(right, node->getInput("in2"), report_, material_,
                            prefix))
      return {};
    ++report_.translated_nodes;
    return {node, {}, type};
  }

  MxExpression multiply_colors(const MxExpression &left,
                               const MxExpression &right,
                               const std::string &prefix) {
    if (!left.valid())
      return right;
    if (!right.valid())
      return left;
    return make_binary_expression("multiply", "color3", left, right, prefix);
  }

  MxExpression make_logical_expression(const std::string &category,
                                       const MxExpression &left,
                                       const MxExpression &right = {}) {
    mx::NodePtr node = graph_->addNode(category, next_name("logical"), "boolean");
    node->setNodeDefString("ND_logical_" + category);
    add_node_inputs_from_definition(node);
    if (!connect_expression(left, node->getInput(category == "not" ? "in" : "in1"),
                            report_, material_, "logical operand") ||
        (category != "not" &&
         !connect_expression(right, node->getInput("in2"), report_, material_,
                             "logical operand")))
      return {};
    ++report_.translated_nodes;
    return {node, {}, "boolean"};
  }

  MxExpression standard_surface_roughness(const MxExpression &roughness) {
    if (!roughness.valid())
      return {};
    MxExpression zero = make_constant_expression("float", "0.0", "roughness_zero");
    MxExpression nonnegative = make_binary_expression(
        "max", "float", roughness, zero, "roughness_nonnegative");
    if (!nonnegative.valid())
      return {};
    mx::NodePtr node = graph_->addNode("sqrt", next_name("roughness_sqrt"), "float");
    add_node_inputs_from_definition(node);
    if (!connect_expression(nonnegative, node->getInput("in"), report_,
                            material_, "microfacet roughness"))
      return {};
    ++report_.translated_nodes;
    return {node, {}, "float"};
  }

  MxExpression translate_roughness(const mdl::IExpression *expression) {
    if (!expression)
      return {};
    if (expression->get_kind() == mdl::IExpression::EK_TEMPORARY) {
      const auto *temporary =
          static_cast<const mdl::IExpression_temporary *>(expression);
      mi::base::Handle<const mdl::IExpression> value(
          compiled_->get_temporary(temporary->get_index()));
      return translate_roughness(value.get());
    }
    if (expression->get_kind() != mdl::IExpression::EK_DIRECT_CALL)
      return {};

    const auto *call =
        static_cast<const mdl::IExpression_direct_call *>(expression);
    mi::base::Handle<const mdl::IExpression> roughness =
        argument(call, "roughness_u");
    if (roughness)
      return translate(roughness.get());
    for (const char *name : {"base", "layer"}) {
      mi::base::Handle<const mdl::IExpression> nested = argument(call, name);
      MxExpression result = translate_roughness(nested.get());
      if (result.valid())
        return result;
    }
    return {};
  }

  bool approximate_diffuse(const mdl::IExpression_direct_call *call,
                           const mx::NodePtr &surface,
                           const MxExpression &layer_weight,
                           const MxExpression &tint_override) {
    mx::InputPtr metalness = surface->getInput("metalness");
    const bool over_metal = layer_weight.valid() && layer_weight.type == "float" &&
        metalness && !metalness->hasNodeName() &&
        metalness->getInterfaceName().empty() &&
        metalness->getValueString() == "1.0";
    if (over_metal) {
      MxExpression base_color = input_expression(surface->getInput("base_color"));
      MxExpression tint = multiply_colors(translate(argument(call, "tint").get()),
                                          tint_override, "diffuse_tint");
      if (!base_color.valid() || !tint.valid())
        return false;
      mx::NodePtr mix = graph_->addNode("mix", next_name("metal_diffuse_mix"), "color3");
      mix->setNodeDefString("ND_mix_color3");
      add_node_inputs_from_definition(mix);
      if (!connect_expression(base_color, mix->getInput("bg"), report_, material_,
                              "metal base color") ||
          !connect_expression(tint, mix->getInput("fg"), report_, material_,
                              "diffuse layer color") ||
          !connect_expression(layer_weight, mix->getInput("mix"), report_, material_,
                              "diffuse layer weight"))
        return false;
      MxExpression one = make_constant_expression("float", "1.0", "metal_weight_one");
      MxExpression remaining = make_binary_expression(
          "subtract", "float", one, layer_weight, "metal_weight");
      if (!remaining.valid())
        return false;
      set_preview_value(surface, "base", "1.0");
      connect_preview_input({mix, {}, "color3"}, surface, "base_color",
                            "metal and diffuse color");
      connect_preview_input(remaining, surface, "metalness", "metal fraction");
      ++report_.translated_nodes;
      report_.warn(material_ +
                   ": diffuse over metal was approximated by a standard_surface "
                   "color and metalness mix.");
      return true;
    }
    if (layer_weight.valid())
      connect_preview_input(layer_weight, surface, "base", "diffuse weight");
    else {
      set_preview_value(surface, "base", "1.0");
      set_preview_value(surface, "specular", "0.0");
      set_preview_value(surface, "transmission", "0.0");
    }

    MxExpression tint = translate(argument(call, "tint").get());
    tint = multiply_colors(tint, tint_override, "diffuse_tint");
    if (tint.valid())
      connect_preview_input(tint, surface, "base_color", "diffuse tint");

    MxExpression roughness =
        translate(argument(call, "roughness").get());
    if (roughness.valid())
      connect_preview_input(roughness, surface, "diffuse_roughness",
                            "diffuse roughness");
    report_.warn(material_ +
                 ": MDL diffuse was approximated with standard_surface base; "
                 "MDL multiscatter tint is not represented.");
    return true;
  }

  bool approximate_microfacet(const mdl::IExpression_direct_call *call,
                              const mx::NodePtr &surface,
                              const MxExpression &layer_weight,
                              const MxExpression &tint_override,
                              bool smooth_specular = false) {
    mi::base::Handle<const mdl::IExpression> mode =
        argument(call, "mode");
    const std::string mode_name = enum_name(mode.get());
    const bool transmit = mode_name == "scatter_transmit";
    const bool reflect_transmit = mode_name == "scatter_reflect_transmit";
    const bool standalone_reflection =
        mode_name == "scatter_reflect" && !layer_weight.valid();
    const std::string weight = transmit ? "transmission" : "specular";
    if (!layer_weight.valid()) {
      set_preview_value(surface, "base", standalone_reflection ? "1.0" : "0.0");
      if (standalone_reflection)
        set_preview_value(surface, "metalness", "1.0");
      set_preview_value(surface, "specular", transmit ? "0.0" : "1.0");
      set_preview_value(surface, "transmission",
                        transmit || reflect_transmit ? "1.0" : "0.0");
    }

    if (layer_weight.valid()) {
      connect_preview_input(layer_weight, surface, weight,
                            "microfacet layer weight");
      if (reflect_transmit)
        connect_preview_input(layer_weight, surface, "transmission",
                              "microfacet transmission layer weight");
    }

    MxExpression tint = translate(argument(call, "tint").get());
    tint = multiply_colors(tint, tint_override, "microfacet_tint");
    if (tint.valid()) {
      connect_preview_input(tint, surface,
                           standalone_reflection ? "base_color"
                           : transmit ? "transmission_color" : "specular_color",
                           "microfacet tint");
      if (reflect_transmit)
        connect_preview_input(tint, surface, "transmission_color",
                              "microfacet transmission tint");
    }

    MxExpression roughness = smooth_specular
        ? make_constant_expression("float", "0.0", "specular_roughness")
        : standard_surface_roughness(
              translate(argument(call, "roughness_u").get()));
    if (roughness.valid()) {
      if (transmit && layer_weight.valid()) {
        connect_preview_input(roughness, surface, "transmission_extra_roughness",
                              "microfacet transmission roughness");
        report_.warn(material_ +
                     ": layered transmission roughness is added to the "
                     "standard_surface specular roughness.");
      } else {
        connect_preview_input(roughness, surface, "specular_roughness",
                              "microfacet roughness");
        if (!layer_weight.valid() && (transmit || reflect_transmit))
          set_preview_value(surface, "transmission_extra_roughness", "0.0");
      }
    }
    if (!smooth_specular)
      report_.warn(material_ +
                   ": MDL GGX was approximated with standard_surface; "
                   "anisotropy, backscatter, and multiscatter tint may differ.");
    if (standalone_reflection)
      report_.warn(material_ +
                   ": reflection-only GGX was approximated as metallic; "
                   "its grazing-angle reflectance may differ.");
    return true;
  }

  bool approximate_specular(const mdl::IExpression_direct_call *call,
                            const mx::NodePtr &surface,
                            const MxExpression &layer_weight,
                            const MxExpression &tint_override) {
    if (layer_weight.valid() &&
        enum_name(argument(call, "mode").get()) == "scatter_reflect") {
      connect_preview_input(layer_weight, surface, "coat", "specular coat weight");
      MxExpression tint =
          multiply_colors(translate(argument(call, "tint").get()),
                          tint_override, "specular_coat_tint");
      if (tint.valid())
        connect_preview_input(tint, surface, "coat_color", "specular coat tint");
      set_preview_value(surface, "coat_roughness", "0.0");
      report_.warn(material_ +
                   ": layered MDL specular reflection was approximated as a "
                   "standard_surface coat.");
      return true;
    }
    return approximate_microfacet(call, surface, layer_weight, tint_override,
                                  true);
  }

  bool approximate_conductor(const mdl::IExpression_direct_call *call,
                             const mx::NodePtr &surface,
                             const MxExpression &tint_override) {
    mi::base::Handle<const mdl::IExpression> ior_expression =
        argument(call, "ior");
    mi::base::Handle<const mdl::IExpression> extinction_expression =
        argument(call, "extinction_coefficient");
    mi::base::Handle<const mdl::IExpression> base_expression =
        argument(call, "base");
    MxExpression ior = translate(ior_expression.get());
    MxExpression extinction = translate(extinction_expression.get());
    MxExpression reflectance;
    if (ior.valid() && extinction.valid() && ior.type == "color3" &&
        extinction.type == "color3") {
      MxExpression one =
          make_constant_expression("color3", "1.0, 1.0, 1.0", "conductor_one");
      MxExpression ior_minus_one =
          make_binary_expression("subtract", "color3", ior, one,
                                 "conductor_ior_minus_one");
      MxExpression ior_plus_one =
          make_binary_expression("add", "color3", ior, one,
                                 "conductor_ior_plus_one");
      MxExpression numerator = make_binary_expression(
          "add", "color3",
          make_binary_expression("multiply", "color3", ior_minus_one,
                                 ior_minus_one, "conductor_ior_square"),
          make_binary_expression("multiply", "color3", extinction,
                                 extinction, "conductor_extinction_square"),
          "conductor_reflectance_numerator");
      MxExpression denominator = make_binary_expression(
          "add", "color3",
          make_binary_expression("multiply", "color3", ior_plus_one,
                                 ior_plus_one, "conductor_ior_denominator"),
          make_binary_expression("multiply", "color3", extinction,
                                 extinction, "conductor_extinction_denominator"),
          "conductor_reflectance_denominator");
      reflectance = make_binary_expression(
          "divide", "color3", numerator, denominator,
          "conductor_reflectance");
    }
    reflectance = multiply_colors(reflectance, tint_override,
                                  "conductor_tint");
    set_preview_value(surface, "base", "1.0");
    set_preview_value(surface, "metalness", "1.0");
    set_preview_value(surface, "specular", "0.0");
    if (reflectance.valid())
      connect_preview_input(reflectance, surface, "base_color",
                            "conductor reflectance");
    else {
      set_preview_value(surface, "base_color", "0.8, 0.8, 0.8");
      report_.warn(material_ +
                   ": conductor Fresnel values were not translatable; using "
                   "a gray metallic approximation.");
    }
    MxExpression roughness =
        standard_surface_roughness(translate_roughness(base_expression.get()));
    if (roughness.valid())
      connect_preview_input(roughness, surface, "specular_roughness",
                            "conductor roughness");
    report_.warn(material_ +
                 ": MDL conductor was approximated with standard_surface "
                 "metalness using normal-incidence reflectance.");
    return true;
  }

  bool approximate_thin_film(const mdl::IExpression_direct_call *call,
                             const mx::NodePtr &surface,
                             const MxExpression &tint_override) {
    mi::base::Handle<const mdl::IExpression> base =
        argument(call, "base");
    mi::base::Handle<const mdl::IExpression> thickness =
        argument(call, "thickness");
    mi::base::Handle<const mdl::IExpression> ior_expression =
        argument(call, "ior");
    const bool base_mapped =
        approximate_bsdf(base.get(), surface, {}, tint_override);
    MxExpression translated_thickness = translate(thickness.get());
    if (translated_thickness.valid())
      connect_preview_input(translated_thickness, surface,
                            "thin_film_thickness", "thin-film thickness");
    MxExpression ior = translate(ior_expression.get());
    if (ior.type == "color3")
      ior = scalarize_color(ior, "thinfilm_ior");
    if (ior.valid())
      connect_preview_input(ior, surface, "thin_film_IOR", "thin-film IOR");
    report_.warn(material_ +
                 ": MDL thin film was approximated with standard_surface "
                 "thin-film controls.");
    return base_mapped;
  }

  void apply_layer_normal(const mdl::IExpression_direct_call *call,
                          const mx::NodePtr &surface) {
    mi::base::Handle<const mdl::IExpression> normal = argument(call, "normal");
    if (!normal || is_state_normal_expression(normal.get(), compiled_, transaction_))
      return;
    MxExpression translated = translate(normal.get());
    if (!translated.valid() || translated.type != "vector3") {
      report_.warn(material_ + ": the BSDF layer normal could not be represented.");
      return;
    }
    if (connect_preview_input(translated, surface, "normal", "BSDF layer normal"))
      report_.warn(material_ +
                   ": the BSDF layer normal was applied to the standard_surface "
                   "base normal; independently layered normals may differ.");
  }

  bool approximate_layer(const mdl::IExpression_direct_call *call,
                         const mx::NodePtr &surface,
                         const MxExpression &tint_override,
                         mdl::IFunction_definition::Semantics semantic) {
    mi::base::Handle<const mdl::IExpression> base =
        argument(call, "base");
    mi::base::Handle<const mdl::IExpression> layer =
        argument(call, "layer");
    mi::base::Handle<const mdl::IExpression> resolved_base(base);
    while (resolved_base && resolved_base->get_kind() == mdl::IExpression::EK_TEMPORARY)
      resolved_base = compiled_->get_temporary(
          static_cast<const mdl::IExpression_temporary *>(resolved_base.get())->get_index());
    mi::base::Handle<const mdl::IExpression> resolved_layer(layer);
    while (resolved_layer && resolved_layer->get_kind() == mdl::IExpression::EK_TEMPORARY)
      resolved_layer = compiled_->get_temporary(
          static_cast<const mdl::IExpression_temporary *>(resolved_layer.get())->get_index());
    if (semantic == mdl::IFunction_definition::DS_INTRINSIC_DF_COLOR_CUSTOM_CURVE_LAYER &&
        resolved_base && is_neutral_expression(resolved_base.get()) &&
        resolved_layer && resolved_layer->get_kind() == mdl::IExpression::EK_DIRECT_CALL) {
      const auto *layer_call =
          static_cast<const mdl::IExpression_direct_call *>(resolved_layer.get());
      const char *name = layer_call->get_definition();
      mi::base::Handle<const mdl::IFunction_definition> definition(
          name ? transaction_->access<mdl::IFunction_definition>(name) : nullptr);
      if (definition &&
          definition->get_semantic() == mdl::IFunction_definition::DS_INTRINSIC_DF_MICROFACET_GGX_SMITH_BSDF &&
          enum_name(argument(layer_call, "mode").get()) == "scatter_reflect") {
        MxExpression reflectivity = translate(argument(call, "normal_reflectivity").get());
        MxExpression weight = translate(argument(call, "weight").get());
        MxExpression tint = translate(argument(layer_call, "tint").get());
        if (reflectivity.valid() && weight.valid() && tint.valid()) {
          MxExpression color = multiply_colors(reflectivity, weight, "metal_curve_weight");
          color = multiply_colors(color, tint, "metal_curve_tint");
          if (tint_override.valid())
            color = multiply_colors(color, tint_override, "metal_curve_override");
          if (color.valid()) {
            set_preview_value(surface, "base", "1.0");
            set_preview_value(surface, "metalness", "1.0");
            set_preview_value(surface, "specular", "0.0");
            connect_preview_input(color, surface, "base_color", "metal curve reflectance");
            MxExpression roughness =
                standard_surface_roughness(translate(argument(layer_call, "roughness_u").get()));
            if (roughness.valid())
              connect_preview_input(roughness, surface, "specular_roughness", "metal curve roughness");
            apply_layer_normal(call, surface);
            report_.warn(material_ +
                         ": color custom-curve reflection over an empty BSDF "
                         "was approximated as metallic; its grazing curve may differ.");
            return true;
          }
        }
      }
    }
    mi::base::Handle<const mdl::IExpression> weight_expression =
        argument(call, "weight");
    MxExpression weight = translate(weight_expression.get());
    if (!weight.valid()) {
      weight = make_constant_expression("float", "0.5", "layer_weight");
      report_.warn(material_ +
                   ": layer weight could not be translated; using 0.5.");
    }

    const bool base_mapped =
        approximate_bsdf(base.get(), surface, {}, tint_override);
    const bool layer_mapped =
        approximate_bsdf(layer.get(), surface, weight, tint_override);
    apply_layer_normal(call, surface);
    if (semantic ==
        mdl::IFunction_definition::DS_INTRINSIC_DF_WEIGHTED_LAYER) {
      report_.warn(material_ +
                   ": MDL weighted_layer was approximated with standard_surface "
                   "base and specular/transmission weights.");
    } else if (semantic ==
               mdl::IFunction_definition::DS_INTRINSIC_DF_FRESNEL_LAYER) {
      report_.warn(material_ +
                   ": MDL fresnel_layer was approximated with standard_surface; "
                   "layer IOR and normal may differ.");
    } else {
      report_.warn(material_ +
                   ": MDL custom-curve layer was approximated with standard_surface; "
                   "its Fresnel curve is not reproduced.");
    }
    return base_mapped || layer_mapped;
  }

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
    if (film.ior.type == "color3" && !film.scalar_ior.valid())
      film.scalar_ior = scalarize_color(film.ior, "thinfilm_ior");

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
                 "dielectric BSDF; its backscatter may differ.");
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

    return {};
  }

  MxExpression matrix_component(const mdl::IExpression *expression,
                                mi::Size column, mi::Size row) {
    if (!expression || column >= 4 || row >= 4)
      return {};
    if (expression->get_kind() == mdl::IExpression::EK_TEMPORARY) {
      mi::base::Handle<const mdl::IExpression> resolved(
          compiled_->get_temporary(
              static_cast<const mdl::IExpression_temporary *>(expression)->get_index()));
      return matrix_component(resolved.get(), column, row);
    }
    if (expression->get_kind() == mdl::IExpression::EK_CONSTANT) {
      mi::base::Handle<const mdl::IValue> value(
          static_cast<const mdl::IExpression_constant *>(expression)->get_value());
      if (!value || value->get_kind() != mdl::IValue::VK_MATRIX)
        return {};
      const auto *matrix = static_cast<const mdl::IValue_compound *>(value.get());
      mi::base::Handle<const mdl::IValue_vector> vector(matrix->get_value<mdl::IValue_vector>(column));
      mi::base::Handle<const mdl::IValue_float> item(
          vector ? vector->get_value<mdl::IValue_float>(row) : nullptr);
      return item ? make_constant_expression("float", format_float(item->get_value()),
                                             "matrix_component")
                  : MxExpression{};
    }
    if (expression->get_kind() != mdl::IExpression::EK_DIRECT_CALL)
      return {};
    const auto *call = static_cast<const mdl::IExpression_direct_call *>(expression);
    const char *name = call->get_definition();
    mi::base::Handle<const mdl::IFunction_definition> definition(
        name ? transaction_->access<mdl::IFunction_definition>(name) : nullptr);
    mi::base::Handle<const mdl::IExpression_list> args(call->get_arguments());
    if (!definition || !args)
      return {};
    if (definition->get_semantic() == mdl::IFunction_definition::DS_MULTIPLY &&
        args->get_size() == 2) {
      mi::base::Handle<const mdl::IExpression> left(args->get_expression(mi::Size(0)));
      mi::base::Handle<const mdl::IExpression> right(args->get_expression(mi::Size(1)));
      MxExpression result;
      for (mi::Size k = 0; k < 4; ++k) {
        MxExpression a = matrix_component(left.get(), k, row);
        MxExpression b = matrix_component(right.get(), column, k);
        if (!a.valid() || !b.valid())
          return {};
        MxExpression product = make_binary_expression(
            "multiply", "float", a, b, "matrix_product");
        result = result.valid()
                     ? make_binary_expression("add", "float", result, product,
                                              "matrix_sum")
                     : product;
        if (!result.valid())
          return {};
      }
      return result;
    }
    if (definition->get_semantic() != mdl::IFunction_definition::DS_MATRIX_ELEM_CONSTRUCTOR ||
        args->get_size() != 16)
      return {};
    mi::base::Handle<const mdl::IExpression> component(
        args->get_expression(column * 4 + row));
    MxExpression result = translate(component.get());
    return result.type == "float" ? result : MxExpression{};
  }

  MxExpression texture_coordinates(const mdl::IExpression *expression) {
    if (!expression)
      return {};
    if (expression->get_kind() == mdl::IExpression::EK_TEMPORARY) {
      mi::base::Handle<const mdl::IExpression> resolved(
          compiled_->get_temporary(
              static_cast<const mdl::IExpression_temporary *>(expression)->get_index()));
      return texture_coordinates(resolved.get());
    }
    if (expression->get_kind() != mdl::IExpression::EK_DIRECT_CALL)
      return {};
    const auto *call = static_cast<const mdl::IExpression_direct_call *>(expression);
    const char *name = call->get_definition();
    mi::base::Handle<const mdl::IFunction_definition> definition(
        name ? transaction_->access<mdl::IFunction_definition>(name) : nullptr);
    const char *mdl_name = definition ? definition->get_mdl_name() : nullptr;
    if (!mdl_name)
      return {};
    const std::string function(mdl_name);
    if (definition->get_semantic() ==
        mdl::IFunction_definition::DS_INTRINSIC_STATE_TEXTURE_COORDINATE) {
      const std::optional<mi::Sint32> index = texture_coordinate_index(expression);
      if (!index || *index < 0)
        return {};
      mx::NodePtr node = graph_->addNode("texcoord", next_name("texture_coordinates"), "vector3");
      node->setNodeDefString("ND_texcoord_vector3");
      add_node_inputs_from_definition(node);
      node->getInput("index")->setValueString(std::to_string(*index));
      ++report_.translated_nodes;
      return {node, {}, "vector3"};
    }
    if (function.find("::base::coordinate_source(") == 0) {
      if (enum_name(argument(call, "coordinate_system").get()) !=
          "texture_coordinate_uvw")
        return {};
      mi::base::Handle<const mdl::IExpression> space = argument(call, "texture_space");
      MxExpression index = translate(space.get());
      if (!index.valid() || index.type != "integer")
        return {};
      mx::NodePtr node = graph_->addNode("texcoord", next_name("texture_coordinates"), "vector3");
      node->setNodeDefString("ND_texcoord_vector3");
      add_node_inputs_from_definition(node);
      if (!connect_expression(index, node->getInput("index"), report_, material_,
                              "texture coordinate index"))
        return {};
      ++report_.translated_nodes;
      return {node, {}, "vector3"};
    }
    if (function.find("::base::texture_coordinate_info(") == 0) {
      mi::base::Handle<const mdl::IExpression> position = argument(call, "position");
      MxExpression result = translate(position.get());
      return result.type == "vector3" ? result : MxExpression{};
    }
    if (function.find("::base::transform_coordinate(") != 0)
      return {};
    MxExpression source = texture_coordinates(argument(call, "coordinate").get());
    mi::base::Handle<const mdl::IExpression> matrix = argument(call, "transform");
    if (!source.valid() || !matrix)
      return {};
    mx::NodePtr output = graph_->addNode("combine3", next_name("transformed_uv"), "vector3");
    add_node_inputs_from_definition(output);
    for (mi::Size row = 0; row < 3; ++row) {
      MxExpression sum = matrix_component(matrix.get(), 3, row);
      if (!sum.valid())
        return {};
      for (mi::Size column = 0; column < 3; ++column) {
        MxExpression coefficient = matrix_component(matrix.get(), column, row);
        if (!coefficient.valid())
          return {};
        mx::NodePtr channel = graph_->addNode("extract", next_name("uv_component"), "float");
        channel->setNodeDefString("ND_extract_vector3");
        add_node_inputs_from_definition(channel);
        channel->getInput("index")->setValueString(std::to_string(column));
        if (!connect_expression(source, channel->getInput("in"), report_, material_,
                                "texture coordinate"))
          return {};
        ++report_.translated_nodes;
        MxExpression product = make_binary_expression(
            "multiply", "float", coefficient, {channel, {}, "float"}, "uv_product");
        sum = make_binary_expression("add", "float", sum, product, "uv_sum");
        if (!sum.valid())
          return {};
      }
      if (!connect_expression(sum, output->getInput("in" + std::to_string(row + 1)),
                              report_, material_, "transformed texture coordinate"))
        return {};
    }
    ++report_.translated_nodes;
    return {output, {}, "vector3"};
  }

  MxExpression texture_tangent(const mdl::IExpression *expression, bool u) {
    if (!expression)
      return {};
    if (expression->get_kind() == mdl::IExpression::EK_TEMPORARY) {
      mi::base::Handle<const mdl::IExpression> resolved(
          compiled_->get_temporary(
              static_cast<const mdl::IExpression_temporary *>(expression)->get_index()));
      return texture_tangent(resolved.get(), u);
    }
    if (expression->get_kind() != mdl::IExpression::EK_DIRECT_CALL)
      return {};
    const auto *call = static_cast<const mdl::IExpression_direct_call *>(expression);
    const char *name = call->get_definition();
    mi::base::Handle<const mdl::IFunction_definition> definition(
        name ? transaction_->access<mdl::IFunction_definition>(name) : nullptr);
    const char *mdl_name = definition ? definition->get_mdl_name() : nullptr;
    if (!mdl_name)
      return {};
    const std::string function(mdl_name);
    if (function.find("::base::texture_coordinate_info(") == 0) {
      MxExpression result = translate(argument(call, u ? "tangent_u" : "tangent_v").get());
      return result.type == "vector3" ? result : MxExpression{};
    }
    if (function.find("::base::coordinate_source(") == 0) {
      if (enum_name(argument(call, "coordinate_system").get()) != "texture_coordinate_uvw")
        return {};
      MxExpression index = translate(argument(call, "texture_space").get());
      if (!index.valid() || index.type != "integer")
        return {};
      const std::string kind = u ? "tangent" : "bitangent";
      mx::NodePtr node = graph_->addNode(kind, next_name("uv_" + kind), "vector3");
      node->setNodeDefString("ND_" + kind + "_vector3");
      add_node_inputs_from_definition(node);
      node->getInput("space")->setValueString("world");
      if (!connect_expression(index, node->getInput("index"), report_, material_,
                              "UV tangent index"))
        return {};
      ++report_.translated_nodes;
      return {node, {}, "vector3"};
    }
    if (function.find("::base::transform_coordinate(") != 0)
      return {};
    mi::base::Handle<const mdl::IExpression> coordinate = argument(call, "coordinate");
    mi::base::Handle<const mdl::IExpression> matrix = argument(call, "transform");
    MxExpression tu = texture_tangent(coordinate.get(), true);
    MxExpression tv = texture_tangent(coordinate.get(), false);
    MxExpression m00 = matrix_component(matrix.get(), 0, 0);
    MxExpression m10 = matrix_component(matrix.get(), 1, 0);
    MxExpression m01 = matrix_component(matrix.get(), 0, 1);
    MxExpression m11 = matrix_component(matrix.get(), 1, 1);
    if (!tu.valid() || !tv.valid() || !m00.valid() || !m10.valid() ||
        !m01.valid() || !m11.valid())
      return {};
    MxExpression zero = make_constant_expression("float", "0.0", "uv_tangent_zero");
    MxExpression neg_m01 =
        make_binary_expression("subtract", "float", zero, m01, "uv_tangent_neg");
    mx::NodePtr components = graph_->addNode("combine2", next_name("uv_tangent_components"), "vector2");
    add_node_inputs_from_definition(components);
    if (!connect_expression(m11, components->getInput("in1"), report_, material_, "UV tangent") ||
        !connect_expression(neg_m01, components->getInput("in2"), report_, material_, "UV tangent"))
      return {};
    mx::NodePtr normalized = graph_->addNode("normalize", next_name("uv_tangent_normalize"), "vector2");
    add_node_inputs_from_definition(normalized);
    if (!connect_expression({components, {}, "vector2"}, normalized->getInput("in"),
                            report_, material_, "UV tangent"))
      return {};
    report_.translated_nodes += 2;
    std::array<MxExpression, 2> c;
    for (mi::Size i = 0; i < c.size(); ++i) {
      mx::NodePtr extract = graph_->addNode("extract", next_name("uv_tangent_component"), "float");
      extract->setNodeDefString("ND_extract_vector2");
      add_node_inputs_from_definition(extract);
      extract->getInput("index")->setValueString(std::to_string(i));
      if (!connect_expression({normalized, {}, "vector2"}, extract->getInput("in"),
                              report_, material_, "UV tangent"))
        return {};
      c[i] = {extract, {}, "float"};
      ++report_.translated_nodes;
    }
    if (u) {
      MxExpression dot = make_binary_expression(
          "add", "float",
          make_binary_expression("multiply", "float", m00, c[0], "uv_tangent_dot"),
          make_binary_expression("multiply", "float", m10, c[1], "uv_tangent_dot"),
          "uv_tangent_dot");
      for (MxExpression &component : c) {
        MxExpression neg = make_binary_expression(
            "subtract", "float", zero, component, "uv_tangent_neg");
        mx::NodePtr select = graph_->addNode("ifgreater", next_name("uv_tangent_flip"), "float");
        add_node_inputs_from_definition(select);
        if (!connect_expression(zero, select->getInput("value1"), report_, material_, "UV tangent") ||
            !connect_expression(dot, select->getInput("value2"), report_, material_, "UV tangent") ||
            !connect_expression(neg, select->getInput("in1"), report_, material_, "UV tangent") ||
            !connect_expression(component, select->getInput("in2"), report_, material_, "UV tangent"))
          return {};
        component = {select, {}, "float"};
        ++report_.translated_nodes;
      }
    } else {
      MxExpression x = make_binary_expression("subtract", "float", zero, c[1], "uv_tangent_neg");
      c[1] = c[0];
      c[0] = x;
    }
    const auto scaled = [&](const MxExpression &vector, const MxExpression &weight) {
      mx::NodePtr node = graph_->addNode("multiply", next_name("uv_tangent_scaled"), "vector3");
      node->setNodeDefString("ND_multiply_vector3FA");
      add_node_inputs_from_definition(node);
      if (!connect_expression(vector, node->getInput("in1"), report_, material_, "UV tangent") ||
          !connect_expression(weight, node->getInput("in2"), report_, material_, "UV tangent"))
        return MxExpression{};
      ++report_.translated_nodes;
      return MxExpression{node, {}, "vector3"};
    };
    MxExpression result = make_binary_expression(
        "add", "vector3", scaled(tu, c[0]), scaled(tv, c[1]), "uv_tangent");
    MxExpression length_squared = make_binary_expression(
        "add", "float",
        make_binary_expression("multiply", "float", m01, m01, "uv_tangent_length"),
        make_binary_expression("multiply", "float", m11, m11, "uv_tangent_length"),
        "uv_tangent_length");
    mx::NodePtr select = graph_->addNode("ifgreater", next_name("uv_tangent_valid"), "vector3");
    add_node_inputs_from_definition(select);
    if (!connect_expression(length_squared, select->getInput("value1"), report_, material_, "UV tangent") ||
        !connect_expression(zero, select->getInput("value2"), report_, material_, "UV tangent") ||
        !connect_expression(result, select->getInput("in1"), report_, material_, "UV tangent") ||
        !connect_expression(u ? tu : tv, select->getInput("in2"), report_, material_, "UV tangent"))
      return {};
    ++report_.translated_nodes;
    return {select, {}, "vector3"};
  }

  MxExpression translate_texture_lookup(
      const mdl::IExpression_direct_call *expression,
      bool base_file_texture = false,
      const std::string &base_output_type = "color3") {
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
        if (name == (base_file_texture ? "texture" : "tex"))
          texture_expression = argument;
        else if (name == (base_file_texture ? "uvw" : "coord"))
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
    float gamma = texture->get_gamma();
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
      if (texture_object && gamma == 0.0f)
        gamma = texture_object->get_effective_gamma(0, 0);
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

    const std::string materialx_path =
        texture_packager_.package(texture_path);

    mi::base::Handle<const mdl::IType> expression_type(
        expression->get_type());
    const std::string output_type =
        base_file_texture ? base_output_type : mdl_type_to_materialx(expression_type.get());
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
    if (gamma == 2.2f) {
      file_input->setColorSpace("g22_rec709");
      if (output_type != "color3")
        report_.warn(material_ +
                     ": gamma correction for non-color texture lookups "
                     "may not be applied by MaterialX shader generators.");
    } else if (gamma != 1.0f)
      report_.warn(material_ + ": MDL texture gamma " +
                   format_float(gamma) +
                   " cannot be represented by the supported color spaces.");

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

    const auto connect_uv_set = [&](mi::Sint32 index) {
      mx::NodePtr coordinates =
          graph_->addNode("texcoord", next_name("texture_coordinates"),
                          "vector2");
      coordinates->setNodeDefString("ND_texcoord_vector2");
      add_node_inputs_from_definition(coordinates);
      coordinates->getInput("index")->setValueString(
          std::to_string(index));
      image->getInput("texcoord")->setConnectedNode(coordinates);
      ++report_.translated_nodes;
    };
    MxExpression coordinates = base_file_texture
                                   ? texture_coordinates(coordinate_expression.get())
                                   : translate(coordinate_expression.get());
    if (coordinates.valid() && coordinates.type == "vector3") {
      mx::NodePtr convert =
          graph_->addNode("convert", next_name("uv_vector2"), "vector2");
      convert->setNodeDefString("ND_convert_vector3_vector2");
      add_node_inputs_from_definition(convert);
      if (connect_expression(coordinates, convert->getInput("in"), report_,
                             material_, "texture coordinates")) {
        coordinates = {convert, {}, "vector2"};
        ++report_.translated_nodes;
      } else {
        coordinates = {};
      }
    }
    if (coordinates.valid() && coordinates.type == "vector2") {
      if (!connect_expression(coordinates, image->getInput("texcoord"), report_,
                              material_, "texture coordinates"))
        return {};
    } else {
      connect_uv_set(0);
      report_.warn(material_ +
                   ": MDL texture coordinates could not be represented; "
                   "MaterialX's default UV0 is used.");
    }

    ++report_.translated_nodes;
    return {image, {}, output_type};
  }

  MxExpression translate_normal_texture(
      const mdl::IExpression_direct_call *call) {
    MxExpression image = translate_texture_lookup(call, true, "vector3");
    MxExpression factor = translate(argument(call, "factor").get());
    MxExpression flip_u = translate(argument(call, "flip_tangent_u").get());
    MxExpression flip_v = translate(argument(call, "flip_tangent_v").get());
    if (!image.valid() || !factor.valid() || factor.type != "float" ||
        !flip_u.valid() || flip_u.type != "boolean" ||
        !flip_v.valid() || flip_v.type != "boolean")
      return {};
    MxExpression zero = make_constant_expression("float", "0.0", "normal_zero");
    MxExpression negative =
        make_binary_expression("subtract", "float", zero, factor, "normal_flip");
    const auto select_scale = [&](const MxExpression &flip) -> MxExpression {
      mx::NodePtr select = graph_->addNode("ifequal", next_name("normal_scale"), "float");
      select->setNodeDefString("ND_ifequal_floatB");
      add_node_inputs_from_definition(select);
      select->getInput("value2")->setValueString("true");
      if (!connect_expression(flip, select->getInput("value1"), report_, material_,
                              "normal map flip") ||
          !connect_expression(negative, select->getInput("in1"), report_, material_,
                              "normal map flipped scale") ||
          !connect_expression(factor, select->getInput("in2"), report_, material_,
                              "normal map scale"))
        return {};
      ++report_.translated_nodes;
      return {select, {}, "float"};
    };
    MxExpression scale_u = select_scale(flip_u);
    MxExpression scale_v = select_scale(flip_v);
    if (!scale_u.valid() || !scale_v.valid())
      return {};
    std::array<MxExpression, 3> channels;
    for (mi::Size i = 0; i < channels.size(); ++i) {
      mx::NodePtr extract = graph_->addNode("extract", next_name("normal_channel"), "float");
      extract->setNodeDefString("ND_extract_vector3");
      add_node_inputs_from_definition(extract);
      extract->getInput("index")->setValueString(std::to_string(i));
      if (!connect_expression(image, extract->getInput("in"), report_, material_,
                              "normal map channel"))
        return {};
      channels[i] = {extract, {}, "float"};
      ++report_.translated_nodes;
    }
    MxExpression one = make_constant_expression("float", "1.0", "normal_one");
    MxExpression two = make_constant_expression("float", "2.0", "normal_two");
    MxExpression z = make_binary_expression("multiply", "float", channels[2], two, "normal_z");
    z = make_binary_expression("subtract", "float", z, one, "normal_z");
    z = make_binary_expression("multiply", "float", z, factor, "normal_z");
    if (!z.valid())
      return {};
    mx::NodePtr absolute = graph_->addNode("absval", next_name("normal_z_abs"), "float");
    add_node_inputs_from_definition(absolute);
    if (!connect_expression(z, absolute->getInput("in"), report_, material_, "normal map Z"))
      return {};
    ++report_.translated_nodes;
    MxExpression baseline =
        make_binary_expression("subtract", "float", one, factor, "normal_z_baseline");
    z = make_binary_expression("add", "float", {absolute, {}, "float"}, baseline, "normal_z");
    z = make_binary_expression("add", "float", z, one, "normal_z");
    z = make_binary_expression("divide", "float", z, two, "normal_z");
    if (!z.valid())
      return {};
    mx::NodePtr encoded = graph_->addNode("combine3", next_name("normal_encoded"), "vector3");
    add_node_inputs_from_definition(encoded);
    if (!connect_expression(channels[0], encoded->getInput("in1"), report_, material_, "normal map X") ||
        !connect_expression(channels[1], encoded->getInput("in2"), report_, material_, "normal map Y") ||
        !connect_expression(z, encoded->getInput("in3"), report_, material_, "normal map Z"))
      return {};
    ++report_.translated_nodes;
    mx::NodePtr scale = graph_->addNode("combine2", next_name("normal_scale"), "vector2");
    add_node_inputs_from_definition(scale);
    if (!connect_expression(scale_u, scale->getInput("in1"), report_, material_,
                            "normal map U scale") ||
        !connect_expression(scale_v, scale->getInput("in2"), report_, material_,
                            "normal map V scale"))
      return {};
    mx::NodePtr normal = graph_->addNode("normalmap", next_name("normal_map"), "vector3");
    normal->setNodeDefString("ND_normalmap_vector2");
    add_node_inputs_from_definition(normal);
    mx::NodePtr direction = graph_->addNode("normal", next_name("normal_direction"), "vector3");
    direction->setNodeDefString("ND_normal_vector3");
    add_node_inputs_from_definition(direction);
    direction->getInput("space")->setValueString("world");
    normal->getInput("normal")->setConnectedNode(direction);
    ++report_.translated_nodes;
    mi::base::Handle<const mdl::IExpression> uvw = argument(call, "uvw");
    for (const bool u : {true, false}) {
      const std::string name = u ? "tangent" : "bitangent";
      MxExpression tangent = texture_tangent(uvw.get(), u);
      if (!tangent.valid()) {
        mx::NodePtr fallback = graph_->addNode(name, next_name("normal_" + name), "vector3");
        fallback->setNodeDefString("ND_" + name + "_vector3");
        add_node_inputs_from_definition(fallback);
        fallback->getInput("space")->setValueString("world");
        tangent = {fallback, {}, "vector3"};
        ++report_.translated_nodes;
        report_.warn(material_ + ": the normal map tangent could not be represented; "
                     "the default UV0 tangent is used.");
      }
      if (!connect_expression(tangent, normal->getInput(name), report_, material_,
                              "normal map tangent"))
        return {};
    }
    if (!connect_expression({encoded, {}, "vector3"}, normal->getInput("in"), report_, material_,
                            "normal map image") ||
        !connect_expression({scale, {}, "vector2"}, normal->getInput("scale"), report_,
                            material_, "normal map scale"))
      return {};
    report_.translated_nodes += 2;
    report_.warn(material_ +
                 ": tangent-space normal texture was approximated with a "
                 "MaterialX normalmap; mirrored wrapping, clipping and crop "
                 "may differ.");
    return {normal, {}, "vector3"};
  }

  MxExpression translate_color_layers(
      const mdl::IExpression_direct_call *call, bool mono) {
    if (mono && enum_name(argument(call, "mono_source").get()) != "mono_average") {
      report_.warn(material_ + ": blend_color_layers mono supports only "
                   "mono_average; the mono output was omitted.");
      return {};
    }
    mi::base::Handle<const mdl::IExpression> layers = argument(call, "layers");
    while (layers && layers->get_kind() == mdl::IExpression::EK_TEMPORARY)
      layers = compiled_->get_temporary(
          static_cast<const mdl::IExpression_temporary *>(layers.get())->get_index());
    if (!layers || layers->get_kind() != mdl::IExpression::EK_DIRECT_CALL)
      return {};
    const auto *array = static_cast<const mdl::IExpression_direct_call *>(layers.get());
    if (!array->get_definition() ||
        std::string(array->get_definition()) != "mdl::T[](...)")
      return {};
    mi::base::Handle<const mdl::IExpression_list> entries(array->get_arguments());
    MxExpression result = translate(argument(call, "base").get());
    if (!entries || !result.valid() || result.type != "color3")
      return {};
    for (mi::Size i = 0; i < entries->get_size(); ++i) {
      mi::base::Handle<const mdl::IExpression> entry(entries->get_expression(i));
      while (entry && entry->get_kind() == mdl::IExpression::EK_TEMPORARY)
        entry = compiled_->get_temporary(
            static_cast<const mdl::IExpression_temporary *>(entry.get())->get_index());
      if (!entry || entry->get_kind() != mdl::IExpression::EK_DIRECT_CALL)
        return {};
      const auto *layer = static_cast<const mdl::IExpression_direct_call *>(entry.get());
      const char *name = layer->get_definition();
      mi::base::Handle<const mdl::IFunction_definition> definition(
          name ? transaction_->access<mdl::IFunction_definition>(name) : nullptr);
      const char *mdl_name = definition ? definition->get_mdl_name() : nullptr;
      if (!mdl_name || std::string(mdl_name).find("::base::color_layer(") != 0)
        return {};
      const std::string mode = enum_name(argument(layer, "mode").get());
      if (mode != "color_layer_blend" && mode != "color_layer_add") {
        report_.warn(material_ + ": unsupported blend_color_layers mode " +
                     (mode.empty() ? "<nonconstant>" : mode) + ".");
        return {};
      }
      MxExpression color = translate(argument(layer, "layer_color").get());
      MxExpression weight = translate(argument(layer, "weight").get());
      if (!color.valid() || color.type != "color3" ||
          !weight.valid() || weight.type != "float")
        return {};
      if (mode == "color_layer_add")
        color = make_binary_expression("add", "color3", result, color,
                                       "color_layer_add");
      if (!color.valid())
        return {};
      mx::NodePtr mix = graph_->addNode("mix", next_name("color_layer"), "color3");
      mix->setNodeDefString("ND_mix_color3");
      add_node_inputs_from_definition(mix);
      if (!connect_expression(result, mix->getInput("bg"), report_, material_,
                              "color layer base") ||
          !connect_expression(color, mix->getInput("fg"), report_, material_,
                              "color layer color") ||
          !connect_expression(weight, mix->getInput("mix"), report_, material_,
                              "color layer weight"))
        return {};
      ++report_.translated_nodes;
      result = {mix, {}, "color3"};
    }
    return mono ? scalarize_color(result, "color_layer_average") : result;
  }

  MxExpression translate_file_texture_field(
      const mdl::IExpression_direct_call *field, bool mono) {
    mi::base::Handle<const mdl::IExpression_list> field_arguments(
        field->get_arguments());
    mi::base::Handle<const mdl::IExpression> source(
        field_arguments && field_arguments->get_size() == 1
            ? field_arguments->get_expression(mi::Size(0))
            : nullptr);
    while (source && source->get_kind() == mdl::IExpression::EK_TEMPORARY) {
      source = compiled_->get_temporary(
          static_cast<const mdl::IExpression_temporary *>(source.get())
              ->get_index());
    }
    if (!source || source->get_kind() != mdl::IExpression::EK_DIRECT_CALL)
      return {};
    const auto *call =
        static_cast<const mdl::IExpression_direct_call *>(source.get());
    const char *name = call->get_definition();
    mi::base::Handle<const mdl::IFunction_definition> definition(
        name ? transaction_->access<mdl::IFunction_definition>(name) : nullptr);
    const char *mdl_name = definition ? definition->get_mdl_name() : nullptr;
    if (mdl_name && std::string(mdl_name).find("::base::blend_color_layers(") == 0)
      return translate_color_layers(call, mono);
    if (!mdl_name || std::string(mdl_name).find("::base::file_texture(") != 0) {
      report_.warn(material_ + ": unsupported texture_return source " +
                   (mdl_name ? mdl_name : "<unresolved>") + ".");
      return {};
    }

    if (mono && enum_name(argument(call, "mono_source").get()) != "mono_average") {
      report_.warn(material_ +
                   ": base::file_texture mono supports only mono_average; "
                   "the mono output was omitted.");
      return {};
    }
    MxExpression image = translate_texture_lookup(call, true);
    MxExpression scale = translate(argument(call, "color_scale").get());
    MxExpression offset = translate(argument(call, "color_offset").get());
    if (!image.valid() || !scale.valid() || !offset.valid())
      return {};
    MxExpression scaled =
        make_binary_expression("multiply", "color3", image, scale,
                               "file_texture_scale");
    if (!scaled.valid())
      return {};
    MxExpression color =
        make_binary_expression("add", "color3", scaled, offset,
                               "file_texture_offset");
    if (!color.valid())
      return {};
    report_.warn(material_ +
                 ": base::file_texture crop, clip, animation and coordinate "
                 "transforms may differ.");
    return mono ? scalarize_color(color, "file_texture_average") : color;
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
    const char *mdl_name = definition->get_mdl_name();
    if (semantic == mdl::IFunction_definition::DS_INTRINSIC_STATE_TEXTURE_TANGENT_U ||
        semantic == mdl::IFunction_definition::DS_INTRINSIC_STATE_TEXTURE_TANGENT_V) {
      MxExpression index = translate(argument(expression, "index").get());
      if (!index.valid() || index.type != "integer")
        return {};
      const std::string kind =
          semantic == mdl::IFunction_definition::DS_INTRINSIC_STATE_TEXTURE_TANGENT_U
              ? "tangent" : "bitangent";
      mx::NodePtr tangent = graph_->addNode(kind, next_name("texture_" + kind), "vector3");
      tangent->setNodeDefString("ND_" + kind + "_vector3");
      add_node_inputs_from_definition(tangent);
      tangent->getInput("space")->setValueString("world");
      if (!connect_expression(index, tangent->getInput("index"), report_, material_,
                              "texture tangent index"))
        return {};
      ++report_.translated_nodes;
      return {tangent, {}, "vector3"};
    }
    if (mdl_name && std::string(mdl_name).find("::base::tangent_space_normal_texture(") == 0)
      return translate_normal_texture(expression);
    if (semantic == mdl::IFunction_definition::DS_INTRINSIC_STATE_TEXTURE_COORDINATE)
      return texture_coordinates(expression);
    if (semantic == mdl::IFunction_definition::DS_INTRINSIC_DAG_FIELD_ACCESS &&
        mdl_name) {
      const std::string name(mdl_name);
      if (name == "::base::texture_coordinate_info.position(::base::texture_coordinate_info)") {
        mi::base::Handle<const mdl::IExpression_list> args(expression->get_arguments());
        mi::base::Handle<const mdl::IExpression> source(
            args && args->get_size() == 1 ? args->get_expression(mi::Size(0)) : nullptr);
        return texture_coordinates(source.get());
      }
      if (name == "::base::texture_coordinate_info.tangent_u(::base::texture_coordinate_info)" ||
          name == "::base::texture_coordinate_info.tangent_v(::base::texture_coordinate_info)") {
        mi::base::Handle<const mdl::IExpression_list> args(expression->get_arguments());
        mi::base::Handle<const mdl::IExpression> source(
            args && args->get_size() == 1 ? args->get_expression(mi::Size(0)) : nullptr);
        return texture_tangent(source.get(), name.find(".tangent_u(") != std::string::npos);
      }
      if (name == "::base::texture_return.tint(::base::texture_return)" ||
          name == "::base::texture_return.mono(::base::texture_return)") {
        MxExpression result = translate_file_texture_field(
            expression, name.find(".mono(") != std::string::npos);
        if (!result.valid())
          report_.warn(material_ + ": unsupported " + name +
                       " source; that expression was omitted.");
        return result;
      }
    }
    if (mdl_name && std::string(mdl_name) ==
                        "::base::abbe_number_ior(float,float)") {
      report_.warn(material_ +
                   ": Abbe dispersion is omitted from the scalar MaterialX IOR.");
      return translate(argument(expression, "ior").get());
    }
    using S = mdl::IFunction_definition;
    if (semantic == S::DS_INTRINSIC_MATH_FMOD) {
      mi::base::Handle<const mdl::IExpression_list> args(expression->get_arguments());
      if (!args || args->get_size() != 2)
        return {};
      MxExpression x = translate(mi::base::Handle<const mdl::IExpression>(
          args->get_expression(mi::Size(0))).get());
      MxExpression y = translate(mi::base::Handle<const mdl::IExpression>(
          args->get_expression(mi::Size(1))).get());
      if (!x.valid() || !y.valid() || x.type != "float" || y.type != "float") {
        report_.warn(material_ + ": only scalar math::fmod is supported.");
        return {};
      }
      MxExpression quotient =
          make_binary_expression("divide", "float", x, y, "fmod_quotient");
      mx::NodePtr absolute = graph_->addNode("absval", next_name("fmod_abs"), "float");
      add_node_inputs_from_definition(absolute);
      mx::NodePtr sign = graph_->addNode("sign", next_name("fmod_sign"), "float");
      add_node_inputs_from_definition(sign);
      mx::NodePtr floor = graph_->addNode("floor", next_name("fmod_floor"), "float");
      add_node_inputs_from_definition(floor);
      if (!connect_expression(quotient, absolute->getInput("in"), report_, material_,
                              "fmod quotient") ||
          !connect_expression(quotient, sign->getInput("in"), report_, material_,
                              "fmod quotient") ||
          !connect_expression({absolute, {}, "float"}, floor->getInput("in"), report_,
                              material_, "fmod absolute quotient"))
        return {};
      report_.translated_nodes += 3;
      MxExpression truncated = make_binary_expression(
          "multiply", "float", {floor, {}, "float"}, {sign, {}, "float"},
          "fmod_truncated");
      MxExpression multiple =
          make_binary_expression("multiply", "float", truncated, y, "fmod_multiple");
      return make_binary_expression("subtract", "float", x, multiple, "fmod");
    }
    if (semantic == S::DS_ARRAY_INDEX) {
      mi::base::Handle<const mdl::IExpression> source = argument(expression, "a");
      mi::base::Handle<const mdl::IExpression> index = argument(expression, "i");
      mi::base::Handle<const mdl::IType> source_type(
          source ? source->get_type() : nullptr);
      const std::string type = mdl_type_to_materialx(source_type.get());
      if ((type != "vector2" && type != "vector3" && type != "vector4") ||
          !index || index->get_kind() != mdl::IExpression::EK_CONSTANT) {
        report_.warn(material_ + ": MDL indexed access requires a vector and "
                     "a constant component index.");
        return {};
      }
      mi::base::Handle<const mdl::IValue> index_value(
          static_cast<const mdl::IExpression_constant *>(index.get())->get_value());
      if (!index_value || index_value->get_kind() != mdl::IValue::VK_INT)
        return {};
      const mi::Sint32 component =
          static_cast<const mdl::IValue_int *>(index_value.get())->get_value();
      if (component < 0 || component >= type.back() - '0') {
        report_.warn(material_ + ": MDL vector component index is out of range.");
        return {};
      }
      MxExpression value = translate(source.get());
      if (!value.valid())
        return {};
      mx::NodePtr extract = graph_->addNode("extract", next_name("channel"), "float");
      extract->setNodeDefString("ND_extract_" + type);
      add_node_inputs_from_definition(extract);
      extract->getInput("index")->setValueString(std::to_string(component));
      if (!connect_expression(value, extract->getInput("in"), report_, material_,
                              "vector component"))
        return {};
      ++report_.translated_nodes;
      return {extract, {}, "float"};
    }
    if (semantic == S::DS_TERNARY) {
      MxExpression condition = translate(argument(expression, "cond").get());
      MxExpression yes = translate(argument(expression, "true_exp").get());
      MxExpression no = translate(argument(expression, "false_exp").get());
      if (!yes.valid() || !no.valid()) {
        if (yes.valid() != no.valid()) {
          report_.warn(material_ +
                       ": one conditional expression branch was omitted; "
                       "using the translatable branch for both cases.");
          return yes.valid() ? yes : no;
        }
        return {};
      }
      if (!condition.valid() || condition.type != "boolean" ||
          yes.type != no.type ||
          (yes.type != "boolean" && yes.type != "float" && yes.type != "color3" &&
           yes.type != "vector2" && yes.type != "vector3" &&
           yes.type != "vector4")) {
        report_.warn(material_ + ": conditional expression cannot be represented.");
        return {};
      }
      if (yes.type == "boolean") {
        MxExpression not_condition = make_logical_expression("not", condition);
        MxExpression yes_branch = make_logical_expression("and", condition, yes);
        MxExpression no_branch = make_logical_expression("and", not_condition, no);
        return make_logical_expression("or", yes_branch, no_branch);
      }
      mx::NodePtr select =
          graph_->addNode("ifequal", next_name("conditional"), yes.type);
      select->setNodeDefString("ND_ifequal_" + yes.type + "B");
      add_node_inputs_from_definition(select);
      select->getInput("value2")->setValueString("true");
      if (!connect_expression(condition, select->getInput("value1"), report_,
                              material_, "conditional expression") ||
          !connect_expression(yes, select->getInput("in1"), report_, material_,
                              "conditional true value") ||
          !connect_expression(no, select->getInput("in2"), report_, material_,
                              "conditional false value"))
        return {};
      ++report_.translated_nodes;
      return {select, {}, yes.type};
    }
    if (semantic == S::DS_LOGICAL_OR || semantic == S::DS_LOGICAL_AND ||
        semantic == S::DS_LOGICAL_NOT) {
      mi::base::Handle<const mdl::IExpression_list> args(expression->get_arguments());
      const mi::Size expected = semantic == S::DS_LOGICAL_NOT ? 1 : 2;
      if (!args || args->get_size() != expected)
        return {};
      MxExpression left = translate(mi::base::Handle<const mdl::IExpression>(
          args->get_expression(mi::Size(0))).get());
      MxExpression right;
      if (expected == 2)
        right = translate(mi::base::Handle<const mdl::IExpression>(
            args->get_expression(mi::Size(1))).get());
      if (!left.valid() || left.type != "boolean" ||
          (expected == 2 && (!right.valid() || right.type != "boolean")))
        return {};
      return make_logical_expression(
          semantic == S::DS_LOGICAL_NOT ? "not" :
          semantic == S::DS_LOGICAL_OR ? "or" : "and", left, right);
    }
    if (semantic == S::DS_CONV_CONSTRUCTOR && mdl_name &&
        (std::string(mdl_name) == "float3(color)" ||
         std::string(mdl_name) == "color(float3)")) {
      mi::base::Handle<const mdl::IExpression_list> args(expression->get_arguments());
      mi::base::Handle<const mdl::IExpression> input(
          args && args->get_size() == 1 ? args->get_expression(mi::Size(0))
                                        : nullptr);
      MxExpression value = translate(input.get());
      if (!value.valid())
        return {};
      const std::string type =
          std::string(mdl_name) == "float3(color)" ? "vector3" : "color3";
      mx::NodePtr convert = graph_->addNode("convert", next_name("convert"), type);
      convert->setNodeDefString("ND_convert_" + value.type + "_" + type);
      add_node_inputs_from_definition(convert);
      if (!convert->getInput("in") ||
          !connect_expression(value, convert->getInput("in"), report_, material_,
                              "color/vector conversion"))
        return {};
      ++report_.translated_nodes;
      return {convert, {}, type};
    }
    if (semantic == S::DS_NEGATIVE) {
      mi::base::Handle<const mdl::IExpression_list> args(expression->get_arguments());
      mi::base::Handle<const mdl::IExpression> input(
          args && args->get_size() == 1 ? args->get_expression(mi::Size(0))
                                        : nullptr);
      MxExpression value = translate(input.get());
      if (!value.valid() || value.type != "float") {
        report_.warn(material_ + ": unsupported MDL unary negation.");
        return {};
      }
      MxExpression zero = make_constant_expression("float", "0.0", "negative_zero");
      return make_binary_expression("subtract", "float", zero, value, "negative");
    }
    if (semantic == S::DS_GREATER || semantic == S::DS_LESS ||
        semantic == S::DS_GREATER_OR_EQUAL ||
        semantic == S::DS_LESS_OR_EQUAL || semantic == S::DS_EQUAL) {
      mi::base::Handle<const mdl::IExpression_list> args(
          expression->get_arguments());
      if (!args || args->get_size() != 2)
        return {};
      const bool reverse =
          semantic == S::DS_LESS || semantic == S::DS_LESS_OR_EQUAL;
      mi::base::Handle<const mdl::IExpression> left(
          args->get_expression(reverse ? 1 : 0));
      mi::base::Handle<const mdl::IExpression> right(
          args->get_expression(reverse ? 0 : 1));
      MxExpression lhs = translate(left.get());
      MxExpression rhs = translate(right.get());
      if (!lhs.valid() || !rhs.valid() || lhs.type != rhs.type ||
          (lhs.type != "float" && lhs.type != "integer" &&
           !(semantic == S::DS_EQUAL && lhs.type == "boolean"))) {
        report_.warn(material_ + ": MDL comparison could not be represented.");
        return {};
      }
      const std::string category =
          semantic == S::DS_EQUAL ? "ifequal"
          : semantic == S::DS_GREATER_OR_EQUAL || semantic == S::DS_LESS_OR_EQUAL
              ? "ifgreatereq"
              : "ifgreater";
      const std::string suffix =
          lhs.type == "integer" ? "I" : lhs.type == "boolean" ? "B" : "";
      mx::NodePtr node = graph_->addNode(category, next_name("comparison"),
                                         "boolean");
      node->setNodeDefString("ND_" + category + "_boolean" + suffix);
      add_node_inputs_from_definition(node);
      if (!connect_expression(lhs, node->getInput("value1"), report_, material_,
                              "comparison") ||
          !connect_expression(rhs, node->getInput("value2"), report_, material_,
                              "comparison"))
        return {};
      ++report_.translated_nodes;
      return {node, {}, "boolean"};
    }
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
                   mdl::IFunction_definition::DS_ELEM_CONSTRUCTOR ||
               (semantic == mdl::IFunction_definition::DS_CONV_CONSTRUCTOR &&
                mdl_name && std::string(mdl_name) == "color(float)")) {
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
    if (semantic == mdl::IFunction_definition::DS_INTRINSIC_MATH_DOT) {
      mi::base::Handle<const mdl::IExpression_list> args(expression->get_arguments());
      mi::base::Handle<const mdl::IExpression> first(
          args && args->get_size() ? args->get_expression(mi::Size(0)) : nullptr);
      mi::base::Handle<const mdl::IType> type(first ? first->get_type() : nullptr);
      const std::string input_type = mdl_type_to_materialx(type.get());
      if (input_type != "vector2" && input_type != "vector3" && input_type != "vector4") {
        report_.warn(material_ + ": unsupported math::dot input type.");
        return {};
      }
      node->setNodeDefString("ND_dotproduct_" + input_type);
    }
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
        return {};
      }
      for (mi::Size index = 0; index < targets.size(); ++index) {
        const mi::Size argument_index =
            arguments->get_size() == 1 ? 0 : index;
        mi::base::Handle<const mdl::IExpression> argument(
            arguments->get_expression(argument_index));
        MxExpression value = translate(argument.get());
        if (!value.valid() ||
            !connect_expression(value, targets[index], report_, material_,
                                "vector/color constructor component"))
          return {};
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
        if (!diffuse_approximation && !diffuse_emission)
          return {};
        continue;
      }
      MxExpression value = translate(argument.get());
      if (!value.valid()) {
        if (!diffuse_approximation && !diffuse_emission)
          return {};
        continue;
      }
      if (!connect_expression(value, target, report_, material_,
                              argument_name ? argument_name : "MDL argument") &&
          !diffuse_approximation && !diffuse_emission)
        return {};
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
  TextureResourcePackager &texture_packager_;
  std::filesystem::path resource_directory_;
  mi::Size node_index_ = 0;
  std::map<mi::Size, std::string> parameter_names_;
  std::map<std::pair<mi::Size, std::size_t>, MxExpression> temporaries_;
  std::vector<ThinFilmParameters> thin_films_;
  std::size_t next_thin_film_scope_id_ = 1;
};

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
