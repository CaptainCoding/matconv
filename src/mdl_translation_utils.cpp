#include "materialx_utils.h"
#include "mdl_translation_utils.h"

#include <iomanip>
#include <sstream>
#include <string>

namespace matconv::detail::mdl_translation {

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

static bool is_unit_ior_value(const mdl::IValue *value) {
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
  return definition && definition->get_semantic() ==
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
  case S::DS_INTRINSIC_MATH_FRAC:
    return "fract";
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

bool connect_expression(const MxExpression &expression,
                        const mx::InputPtr &input, ConversionReport &report,
                        const std::string &material, const std::string &slot) {
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
        if (conversion_input && connect_expression(expression, conversion_input,
                                                   report, material, slot)) {
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

} // namespace matconv::detail::mdl_translation
