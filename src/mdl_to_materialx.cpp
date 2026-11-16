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
#include <sstream>
#include <stdexcept>
#include <string>
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
  case S::DS_INTRINSIC_MATH_CLAMP:
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
                       ConversionReport &report, const std::string &material)
      : compiled_(compiled), transaction_(transaction), graph_(graph),
        report_(report), material_(material) {
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
    const auto found = temporaries_.find(index);
    if (found != temporaries_.end())
      return found->second;
    mi::base::Handle<const mdl::IExpression> temporary(
        compiled_->get_temporary(index));
    MxExpression result = translate(temporary.get());
    temporaries_[index] = result;
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
      category = "geompropvalue";
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

    mx::NodePtr node = graph_->addNode(category, next_name(category),
                                       output_type);
    add_node_inputs_from_definition(node);
    if (state_normal)
      node->getInput("geomprop")->setValueString("Nworld");
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
      if (!target->getType().empty() && target->getType() != value.type) {
        report_.warn(material_ + ": could not connect MDL argument " +
                     (argument_name ? argument_name : "<unnamed>") +
                     " (" + value.type + ") to MaterialX input " +
                     target->getName() + " (" + target->getType() + ").");
        continue;
      }
      if (value.node)
        target->setConnectedNode(value.node);
      else
        target->setConnectedInterfaceName(value.interface_name);
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
  mi::Size node_index_ = 0;
  std::map<mi::Size, std::string> parameter_names_;
  std::map<mi::Size, MxExpression> temporaries_;
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
                                  name);
  mi::base::Handle<const mdl::IExpression> scattering(
      compiled->lookup_sub_expression("surface.scattering"));
  MxExpression scattering_value = translator.translate(scattering.get());
  if (scattering_value.node && scattering_value.type == "BSDF") {
    surface->getInput("bsdf")->setConnectedNode(scattering_value.node);
  } else {
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
                   mdl_factory.get(), name, report);
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
