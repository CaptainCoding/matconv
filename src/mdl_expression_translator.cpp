#include "mdl_expression_translator.h"
#include "materialx_utils.h"

namespace matconv::detail::mdl_translation {

ExpressionTranslator::ExpressionTranslator(
    const mdl::ICompiled_material *compiled, mdl::ITransaction *transaction,
    const mx::NodeGraphPtr &graph, ConversionReport &report,
    const std::string &material, TextureResourcePackager &texture_packager,
    std::filesystem::path resource_directory)
    : compiled_(compiled), transaction_(transaction), graph_(graph),
      report_(report), material_(material), texture_packager_(texture_packager),
      resource_directory_(std::move(resource_directory)) {
  for (mi::Size index = 0; index < compiled_->get_parameter_count(); ++index) {
    const char *parameter_name = compiled_->get_parameter_name(index);
    std::string type;
    std::string value;
    mi::base::Handle<const mdl::IValue> argument(
        compiled_->get_argument(index));
    if (!parameter_name || !serialize_value(argument.get(), type, value)) {
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

MxExpression
ExpressionTranslator::translate(const mdl::IExpression *expression) {
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

MxExpression
ExpressionTranslator::scalarize_color(const MxExpression &expression,
                                      const std::string &prefix) {
  if (expression.type != "color3")
    return expression;

  mx::NodePtr average =
      graph_->addNode("dotproduct", next_name(prefix), "float");
  average->setNodeDefString("ND_dotproduct_vector3");
  add_node_inputs_from_definition(average);
  mx::InputPtr weights = average->getInput("in2");
  if (!weights) {
    report_.warn(material_ + ": could not create a scalar MaterialX " + prefix +
                 ".");
    return {};
  }
  weights->setValueString("0.333333333, 0.333333333, 0.333333333");
  if (!connect_expression(expression, average->getInput("in1"), report_,
                          material_, prefix))
    return {};

  ++report_.translated_nodes;
  return {average, {}, "float"};
}

mi::base::Handle<const mdl::IExpression>
ExpressionTranslator::argument(const mdl::IExpression_direct_call *call,
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

MxExpression ExpressionTranslator::make_constant(const mdl::IValue *value,
                                                 const std::string &prefix) {
  std::string type;
  std::string text;
  if (!serialize_value(value, type, text)) {
    report_.warn(material_ + ": cannot represent an MDL constant value.");
    return {};
  }
  mx::NodePtr node = graph_->addNode("constant", next_name(prefix), type);
  add_node_inputs_from_definition(node);
  mx::InputPtr input = node->getInput("value");
  if (!input) {
    report_.warn(material_ + ": MaterialX constant node has no value input.");
    return {};
  }
  input->setValueString(text);
  return {node, {}, type};
}

MxExpression ExpressionTranslator::translate_constant(
    const mdl::IExpression_constant *expression) {
  mi::base::Handle<const mdl::IValue> value(expression->get_value());
  return make_constant(value.get(), "constant");
}

MxExpression ExpressionTranslator::translate_parameter(
    const mdl::IExpression_parameter *expression) {
  if (function_scope_)
    return with_bound_argument(expression,
                               [this](const mdl::IExpression *argument) {
                                 return translate(argument);
                               });
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

MxExpression ExpressionTranslator::translate_temporary(
    const mdl::IExpression_temporary *expression) {
  const mi::Size index = expression->get_index();
  if (function_scope_) {
    mi::base::Handle<const mdl::IExpression> temporary(resolve_temporary(index));
    return translate(temporary.get());
  }
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

mi::base::Handle<const mdl::IExpression>
ExpressionTranslator::resolve_temporary(mi::Size index) {
  return mi::base::Handle<const mdl::IExpression>(
      function_scope_ ? function_scope_->definition->get_temporary(index)
                      : compiled_->get_temporary(index));
}

std::string
ExpressionTranslator::enum_name(const mdl::IExpression *expression) {
  if (!expression)
    return {};
  if (expression->get_kind() == mdl::IExpression::EK_TEMPORARY) {
    const auto *temporary =
        static_cast<const mdl::IExpression_temporary *>(expression);
    mi::base::Handle<const mdl::IExpression> value(
        resolve_temporary(temporary->get_index()));
    return enum_name(value.get());
  }
  if (function_scope_ &&
      expression->get_kind() == mdl::IExpression::EK_PARAMETER)
    return with_bound_argument(
        static_cast<const mdl::IExpression_parameter *>(expression),
        [this](const mdl::IExpression *argument) {
          return enum_name(argument);
        });
  mi::base::Handle<const mdl::IValue> value;
  if (expression->get_kind() == mdl::IExpression::EK_CONSTANT) {
    value =
        static_cast<const mdl::IExpression_constant *>(expression)->get_value();
  } else if (expression->get_kind() == mdl::IExpression::EK_PARAMETER) {
    const mi::Size index =
        static_cast<const mdl::IExpression_parameter *>(expression)
            ->get_index();
    value = compiled_->get_argument(index);
    if (value && value->get_kind() == mdl::IValue::VK_ENUM &&
        warned_enum_parameters_.insert(index).second) {
      const char *name = compiled_->get_parameter_name(index);
      report_.warn(material_ + ": enum parameter " +
                   (name ? name : "<unnamed>") +
                   " is fixed to its default value in MaterialX.");
    }
  } else {
    return {};
  }
  if (!value || value->get_kind() != mdl::IValue::VK_ENUM)
    return {};
  const auto *enum_value = static_cast<const mdl::IValue_enum *>(value.get());
  mi::base::Handle<const mdl::IType_enum> type(enum_value->get_type());
  const char *name = type->get_value_name(enum_value->get_index());
  return name ? name : "";
}

MxExpression ExpressionTranslator::translate_color_lerp(
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
  if (!x.valid() || !y.valid() || !weight.valid() || weight.type != "color3") {
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
  connect_expression({scaled, {}, "color3"}, result->getInput("in2"), report_,
                     material_, "color lerp scaled delta");
  report_.translated_nodes += 3;
  return {result, {}, "color3"};
}

MxExpression ExpressionTranslator::translate_call(
    const mdl::IExpression_direct_call *expression) {
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
  if (semantic ==
          mdl::IFunction_definition::DS_INTRINSIC_STATE_TEXTURE_TANGENT_U ||
      semantic ==
          mdl::IFunction_definition::DS_INTRINSIC_STATE_TEXTURE_TANGENT_V) {
    MxExpression index = translate(argument(expression, "index").get());
    if (!index.valid() || index.type != "integer")
      return {};
    const std::string kind =
        semantic ==
                mdl::IFunction_definition::DS_INTRINSIC_STATE_TEXTURE_TANGENT_U
            ? "tangent"
            : "bitangent";
    mx::NodePtr tangent =
        graph_->addNode(kind, next_name("texture_" + kind), "vector3");
    tangent->setNodeDefString("ND_" + kind + "_vector3");
    add_node_inputs_from_definition(tangent);
    tangent->getInput("space")->setValueString("world");
    if (!connect_expression(index, tangent->getInput("index"), report_,
                            material_, "texture tangent index"))
      return {};
    ++report_.translated_nodes;
    return {tangent, {}, "vector3"};
  }
  if (mdl_name &&
      std::string(mdl_name).find("::base::tangent_space_normal_texture(") == 0)
    return translate_normal_texture(expression);
  if (semantic ==
      mdl::IFunction_definition::DS_INTRINSIC_STATE_TEXTURE_COORDINATE)
    return texture_coordinates(expression);
  if (semantic == mdl::IFunction_definition::DS_INTRINSIC_DAG_FIELD_ACCESS &&
      mdl_name) {
    const std::string name(mdl_name);
    if (name == "::base::texture_coordinate_info.position(::base::texture_"
                "coordinate_info)") {
      mi::base::Handle<const mdl::IExpression_list> args(
          expression->get_arguments());
      mi::base::Handle<const mdl::IExpression> source(
          args && args->get_size() == 1 ? args->get_expression(mi::Size(0))
                                        : nullptr);
      return texture_coordinates(source.get());
    }
    if (name == "::base::texture_coordinate_info.tangent_u(::base::texture_"
                "coordinate_info)" ||
        name == "::base::texture_coordinate_info.tangent_v(::base::texture_"
                "coordinate_info)") {
      mi::base::Handle<const mdl::IExpression_list> args(
          expression->get_arguments());
      mi::base::Handle<const mdl::IExpression> source(
          args && args->get_size() == 1 ? args->get_expression(mi::Size(0))
                                        : nullptr);
      return texture_tangent(source.get(),
                             name.find(".tangent_u(") != std::string::npos);
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
  if (mdl_name &&
      std::string(mdl_name) == "::base::abbe_number_ior(float,float)") {
    report_.warn(material_ +
                 ": Abbe dispersion is omitted from the scalar MaterialX IOR.");
    return translate(argument(expression, "ior").get());
  }
  using S = mdl::IFunction_definition;
  if (semantic == S::DS_INTRINSIC_MATH_RADIANS) {
    mi::base::Handle<const mdl::IExpression_list> args(
        expression->get_arguments());
    if (!args || args->get_size() != 1)
      return {};
    MxExpression angle = translate(mi::base::Handle<const mdl::IExpression>(
                                       args->get_expression(mi::Size(0)))
                                       .get());
    if (!angle.valid())
      return {};
    const std::string factor = "0.017453292519943295";
    std::string value = factor;
    if (angle.type == "vector2" || angle.type == "vector3" ||
        angle.type == "vector4") {
      for (int i = 1; i < angle.type.back() - '0'; ++i)
        value += ", " + factor;
    } else if (angle.type != "float") {
      report_.warn(material_ + ": math::radians argument type is unsupported.");
      return {};
    }
    return make_binary_expression(
        "multiply", angle.type, angle,
        make_constant_expression(angle.type, value, "radians_factor"),
        "radians");
  }
  if (semantic == S::DS_INTRINSIC_MATH_FMOD) {
    mi::base::Handle<const mdl::IExpression_list> args(
        expression->get_arguments());
    if (!args || args->get_size() != 2)
      return {};
    MxExpression x = translate(mi::base::Handle<const mdl::IExpression>(
                                   args->get_expression(mi::Size(0)))
                                   .get());
    MxExpression y = translate(mi::base::Handle<const mdl::IExpression>(
                                   args->get_expression(mi::Size(1)))
                                   .get());
    if (!x.valid() || !y.valid() || x.type != "float" || y.type != "float") {
      report_.warn(material_ + ": only scalar math::fmod is supported.");
      return {};
    }
    MxExpression quotient =
        make_binary_expression("divide", "float", x, y, "fmod_quotient");
    mx::NodePtr absolute =
        graph_->addNode("absval", next_name("fmod_abs"), "float");
    add_node_inputs_from_definition(absolute);
    mx::NodePtr sign = graph_->addNode("sign", next_name("fmod_sign"), "float");
    add_node_inputs_from_definition(sign);
    mx::NodePtr floor =
        graph_->addNode("floor", next_name("fmod_floor"), "float");
    add_node_inputs_from_definition(floor);
    if (!connect_expression(quotient, absolute->getInput("in"), report_,
                            material_, "fmod quotient") ||
        !connect_expression(quotient, sign->getInput("in"), report_, material_,
                            "fmod quotient") ||
        !connect_expression({absolute, {}, "float"}, floor->getInput("in"),
                            report_, material_, "fmod absolute quotient"))
      return {};
    report_.translated_nodes += 3;
    MxExpression truncated =
        make_binary_expression("multiply", "float", {floor, {}, "float"},
                               {sign, {}, "float"}, "fmod_truncated");
    MxExpression multiple = make_binary_expression(
        "multiply", "float", truncated, y, "fmod_multiple");
    return make_binary_expression("subtract", "float", x, multiple, "fmod");
  }
  if (semantic == S::DS_ARRAY_INDEX) {
    mi::base::Handle<const mdl::IExpression> source = argument(expression, "a");
    mi::base::Handle<const mdl::IExpression> index = argument(expression, "i");
    mi::base::Handle<const mdl::IType> source_type(source ? source->get_type()
                                                          : nullptr);
    const std::string type = mdl_type_to_materialx(source_type.get());
    if ((type != "vector2" && type != "vector3" && type != "vector4") ||
        !index || index->get_kind() != mdl::IExpression::EK_CONSTANT) {
      report_.warn(material_ + ": MDL indexed access requires a vector and "
                               "a constant component index.");
      return {};
    }
    mi::base::Handle<const mdl::IValue> index_value(
        static_cast<const mdl::IExpression_constant *>(index.get())
            ->get_value());
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
    mx::NodePtr extract =
        graph_->addNode("extract", next_name("channel"), "float");
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
      report_.warn(material_ +
                   ": conditional expression cannot be represented.");
      return {};
    }
    if (yes.type == "boolean") {
      MxExpression not_condition = make_logical_expression("not", condition);
      MxExpression yes_branch = make_logical_expression("and", condition, yes);
      MxExpression no_branch =
          make_logical_expression("and", not_condition, no);
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
    mi::base::Handle<const mdl::IExpression_list> args(
        expression->get_arguments());
    const mi::Size expected = semantic == S::DS_LOGICAL_NOT ? 1 : 2;
    if (!args || args->get_size() != expected)
      return {};
    MxExpression left = translate(mi::base::Handle<const mdl::IExpression>(
                                      args->get_expression(mi::Size(0)))
                                      .get());
    MxExpression right;
    if (expected == 2)
      right = translate(mi::base::Handle<const mdl::IExpression>(
                            args->get_expression(mi::Size(1)))
                            .get());
    if (!left.valid() || left.type != "boolean" ||
        (expected == 2 && (!right.valid() || right.type != "boolean")))
      return {};
    return make_logical_expression(semantic == S::DS_LOGICAL_NOT  ? "not"
                                   : semantic == S::DS_LOGICAL_OR ? "or"
                                                                  : "and",
                                   left, right);
  }
  if (semantic == S::DS_CONV_CONSTRUCTOR && mdl_name &&
      (std::string(mdl_name) == "float3(color)" ||
       std::string(mdl_name) == "color(float3)")) {
    mi::base::Handle<const mdl::IExpression_list> args(
        expression->get_arguments());
    mi::base::Handle<const mdl::IExpression> input(
        args && args->get_size() == 1 ? args->get_expression(mi::Size(0))
                                      : nullptr);
    MxExpression value = translate(input.get());
    if (!value.valid())
      return {};
    const std::string type =
        std::string(mdl_name) == "float3(color)" ? "vector3" : "color3";
    mx::NodePtr convert =
        graph_->addNode("convert", next_name("convert"), type);
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
    mi::base::Handle<const mdl::IExpression_list> args(
        expression->get_arguments());
    mi::base::Handle<const mdl::IExpression> input(
        args && args->get_size() == 1 ? args->get_expression(mi::Size(0))
                                      : nullptr);
    MxExpression value = translate(input.get());
    if (!value.valid() || value.type != "float") {
      report_.warn(material_ + ": unsupported MDL unary negation.");
      return {};
    }
    MxExpression zero =
        make_constant_expression("float", "0.0", "negative_zero");
    return make_binary_expression("subtract", "float", zero, value, "negative");
  }
  if (semantic == S::DS_GREATER || semantic == S::DS_LESS ||
      semantic == S::DS_GREATER_OR_EQUAL || semantic == S::DS_LESS_OR_EQUAL ||
      semantic == S::DS_EQUAL) {
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
    const std::string suffix = lhs.type == "integer"   ? "I"
                               : lhs.type == "boolean" ? "B"
                                                       : "";
    mx::NodePtr node =
        graph_->addNode(category, next_name("comparison"), "boolean");
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
        mi::base::Handle<const mdl::IType> type(argument ? argument->get_type()
                                                         : nullptr);
        if (mdl_type_to_materialx(type.get()) == "color3")
          return translate_color_lerp(expression);
      }
    }
  }
  if (semantic ==
      mdl::IFunction_definition::DS_INTRINSIC_DF_MICROFACET_GGX_SMITH_BSDF)
    return translate_microfacet(expression);
  if (semantic == mdl::IFunction_definition::DS_INTRINSIC_DF_FRESNEL_FACTOR)
    return translate_fresnel_factor(expression);
  if (semantic == mdl::IFunction_definition::DS_INTRINSIC_TEX_LOOKUP_COLOR ||
      semantic == mdl::IFunction_definition::DS_INTRINSIC_TEX_LOOKUP_FLOAT ||
      semantic == mdl::IFunction_definition::DS_INTRINSIC_TEX_LOOKUP_FLOAT2 ||
      semantic == mdl::IFunction_definition::DS_INTRINSIC_TEX_LOOKUP_FLOAT3 ||
      semantic == mdl::IFunction_definition::DS_INTRINSIC_TEX_LOOKUP_FLOAT4)
    return translate_texture_lookup(expression);
  if (semantic == mdl::IFunction_definition::DS_INTRINSIC_DF_WEIGHTED_LAYER)
    return translate_weighted_layer(expression);

  if (semantic == mdl::IFunction_definition::DS_INTRINSIC_DF_THIN_FILM) {
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
    if (base_bsdf.valid() && base_bsdf.type == "BSDF" && layer_bsdf.valid() &&
        layer_bsdf.type == "BSDF" && layer_bsdf.node &&
        layer_bsdf.node->getInput("weight") && layer_weight.valid() &&
        layer_weight.type == "float") {
      mx::NodePtr layer_node =
          graph_->addNode("layer", next_name("custom_curve_layer"), "BSDF");
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
    if (base_bsdf.valid() && base_bsdf.type == "BSDF" && layer_bsdf.valid() &&
        layer_bsdf.type == "BSDF" && layer_weight.valid() &&
        layer_weight.type == "float") {
      mx::NodePtr mix =
          graph_->addNode("mix", next_name("custom_curve_layer"), "BSDF");
      add_node_inputs_from_definition(mix);
      connect_expression(base_bsdf, mix->getInput("bg"), report_, material_,
                         "custom-curve base");
      connect_expression(layer_bsdf, mix->getInput("fg"), report_, material_,
                         "custom-curve layer");
      connect_expression(layer_weight, mix->getInput("mix"), report_, material_,
                         "custom-curve weight");
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
  if (semantic ==
      mdl::IFunction_definition::DS_INTRINSIC_DF_DIFFUSE_REFLECTION_BSDF) {
    category = "oren_nayar_diffuse_bsdf";
    diffuse_approximation = true;
  } else if (semantic ==
             mdl::IFunction_definition::DS_INTRINSIC_DF_DIFFUSE_EDF) {
    category = "uniform_edf";
    diffuse_emission = true;
  } else if (semantic == mdl::IFunction_definition::DS_INTRINSIC_STATE_NORMAL) {
    category = "normal";
    state_normal = true;
  } else if (semantic == mdl::IFunction_definition::DS_ELEM_CONSTRUCTOR ||
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

  mi::base::Handle<const mdl::IType> expression_type(expression->get_type());
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

  mx::NodePtr node =
      graph_->addNode(category, next_name(category), output_type);
  if (color_average)
    node->setNodeDefString("ND_dotproduct_vector3");
  if (semantic == mdl::IFunction_definition::DS_INTRINSIC_MATH_DOT) {
    mi::base::Handle<const mdl::IExpression_list> args(
        expression->get_arguments());
    mi::base::Handle<const mdl::IExpression> first(
        args && args->get_size() ? args->get_expression(mi::Size(0)) : nullptr);
    mi::base::Handle<const mdl::IType> type(first ? first->get_type()
                                                  : nullptr);
    const std::string input_type = mdl_type_to_materialx(type.get());
    if (input_type != "vector2" && input_type != "vector3" &&
        input_type != "vector4") {
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
  if (semantic == mdl::IFunction_definition::DS_INTRINSIC_MATH_SATURATE) {
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
    if (arguments->get_size() != 1 && arguments->get_size() != targets.size()) {
      report_.warn(material_ +
                   ": an MDL vector/color constructor has an unsupported "
                   "number of components.");
      return {};
    }
    for (mi::Size index = 0; index < targets.size(); ++index) {
      const mi::Size argument_index = arguments->get_size() == 1 ? 0 : index;
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
                   (argument_name ? argument_name : "<unnamed>") + " from " +
                   (mdl_name ? mdl_name : definition_name) +
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

mx::InputPtr
ExpressionTranslator::diffuse_input(const mx::NodePtr &node,
                                    const std::string &argument_name) {
  if (argument_name == "tint")
    return node->getInput("color");
  return node->getInput(argument_name);
}

mx::InputPtr ExpressionTranslator::math_input(
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
  if (semantic == mdl::IFunction_definition::DS_INTRINSIC_MATH_SMOOTHSTEP) {
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

std::string ExpressionTranslator::next_name(const std::string &prefix) {
  return unique_element_name(graph_, safe_material_name(prefix) + "_" +
                                         std::to_string(node_index_++));
}

} // namespace matconv::detail::mdl_translation
