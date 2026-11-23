#include "mdl_expression_translator.h"

namespace matconv::detail::mdl_translation {

bool ExpressionTranslator::approximate_bsdf(
    const mdl::IExpression *expression, const mx::NodePtr &surface,
    const MxExpression &layer_weight, const MxExpression &tint_override,
    const MxExpression &transmission_override) {
  if (!expression)
    return false;
  if (expression->get_kind() == mdl::IExpression::EK_TEMPORARY) {
    const auto *temporary =
        static_cast<const mdl::IExpression_temporary *>(expression);
    mi::base::Handle<const mdl::IExpression> value(
        compiled_->get_temporary(temporary->get_index()));
    return approximate_bsdf(value.get(), surface, layer_weight, tint_override,
                            transmission_override);
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
  if (layer_weight.valid() &&
      (semantic == S::DS_INTRINSIC_DF_FRESNEL_FACTOR ||
       semantic == S::DS_INTRINSIC_DF_THIN_FILM ||
       semantic == S::DS_INTRINSIC_DF_WEIGHTED_LAYER ||
       semantic == S::DS_INTRINSIC_DF_FRESNEL_LAYER ||
       semantic == S::DS_INTRINSIC_DF_CUSTOM_CURVE_LAYER ||
       semantic == S::DS_INTRINSIC_DF_COLOR_CUSTOM_CURVE_LAYER))
    return approximate_weighted_subtree(expression, surface, layer_weight,
                                        tint_override, transmission_override);
  if (semantic == S::DS_TERNARY)
    return approximate_conditional_bsdf(call, surface, layer_weight,
                                        tint_override, transmission_override);
  if (semantic == S::DS_INTRINSIC_DF_DIFFUSE_REFLECTION_BSDF)
    return approximate_diffuse(call, surface, layer_weight, tint_override);
  if (semantic == S::DS_INTRINSIC_DF_MICROFACET_GGX_SMITH_BSDF ||
      semantic == S::DS_INTRINSIC_DF_SIMPLE_GLOSSY_BSDF)
    return approximate_microfacet(
        call, surface, layer_weight, tint_override, transmission_override,
        false, semantic == S::DS_INTRINSIC_DF_SIMPLE_GLOSSY_BSDF);
  if (semantic == S::DS_INTRINSIC_DF_SPECULAR_BSDF)
    return approximate_specular(call, surface, layer_weight, tint_override,
                                transmission_override);
  if (semantic == S::DS_INTRINSIC_DF_SHEEN_BSDF)
    return approximate_sheen(call, surface, layer_weight, tint_override,
                             transmission_override);
  if (semantic == S::DS_INTRINSIC_DF_FRESNEL_FACTOR)
    return approximate_conductor(call, surface, tint_override);
  if (semantic == S::DS_INTRINSIC_DF_THIN_FILM)
    return approximate_thin_film(call, surface, tint_override,
                                 transmission_override);
  if (semantic == S::DS_INTRINSIC_DF_WEIGHTED_LAYER ||
      semantic == S::DS_INTRINSIC_DF_FRESNEL_LAYER ||
      semantic == S::DS_INTRINSIC_DF_CUSTOM_CURVE_LAYER ||
      semantic == S::DS_INTRINSIC_DF_COLOR_CUSTOM_CURVE_LAYER)
    return approximate_layer(call, surface, tint_override,
                             transmission_override, semantic);
  if (semantic == S::DS_INTRINSIC_DF_TINT) {
    mi::base::Handle<const mdl::IExpression> base = argument(call, "base");
    mi::base::Handle<const mdl::IExpression> tint = argument(call, "tint");
    mi::base::Handle<const mdl::IExpression> reflection =
        tint ? tint : argument(call, "reflection_tint");
    mi::base::Handle<const mdl::IExpression> transmission =
        tint ? tint : argument(call, "transmission_tint");
    MxExpression translated_reflection = translate(reflection.get());
    MxExpression translated_transmission = translate(transmission.get());
    if (!base || !translated_reflection.valid() ||
        !translated_transmission.valid()) {
      report_.warn(material_ +
                   ": BSDF tint or its base could not be represented.");
      return false;
    }
    translated_reflection = multiply_colors(translated_reflection,
                                            tint_override, "reflection_tint");
    translated_transmission = multiply_colors(
        translated_transmission, transmission_override, "transmission_tint");
    return approximate_bsdf(base.get(), surface, layer_weight,
                            translated_reflection, translated_transmission);
  }
  if (semantic == S::DS_INTRINSIC_DF_DIRECTIONAL_FACTOR ||
      semantic == S::DS_INTRINSIC_DF_MEASURED_CURVE_FACTOR) {
    mi::base::Handle<const mdl::IExpression> base = argument(call, "base");
    if (base) {
      report_.warn(material_ + ": MDL " +
                   (semantic == S::DS_INTRINSIC_DF_DIRECTIONAL_FACTOR
                        ? "directional_factor"
                        : "measured_curve_factor") +
                   " was omitted; its underlying BSDF is approximated "
                   "without the directional modulation.");
      return approximate_bsdf(base.get(), surface, layer_weight, tint_override,
                              transmission_override);
    }
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

bool ExpressionTranslator::connect_preview_input(const MxExpression &expression,
                                                 const mx::NodePtr &surface,
                                                 const std::string &input_name,
                                                 const std::string &slot) {
  mx::InputPtr input = surface->getInput(input_name);
  if (!input) {
    report_.warn(material_ + ": standard_surface has no " + input_name +
                 " input for " + slot + ".");
    return false;
  }
  return connect_expression(expression, input, report_, material_, slot);
}

void ExpressionTranslator::set_preview_value(const mx::NodePtr &surface,
                                             const std::string &input_name,
                                             const std::string &value) {
  if (mx::InputPtr input = surface->getInput(input_name)) {
    input->setConnectedNode(nullptr);
    input->removeAttribute(mx::ValueElement::INTERFACE_NAME_ATTRIBUTE);
    input->setValueString(value);
  }
}

MxExpression ExpressionTranslator::input_expression(const mx::InputPtr &input) {
  if (mx::NodePtr node = input->getConnectedNode())
    return {node, {}, input->getType()};
  if (!input->getInterfaceName().empty())
    return {{}, input->getInterfaceName(), input->getType()};
  if (input->hasValueString())
    return make_constant_expression(input->getType(), input->getValueString(),
                                    "conditional_value");
  return {};
}

bool ExpressionTranslator::approximate_weighted_subtree(
    const mdl::IExpression *expression, const mx::NodePtr &surface,
    const MxExpression &weight, const MxExpression &tint_override,
    const MxExpression &transmission_override) {
  if (weight.type != "float") {
    report_.warn(material_ +
                 ": weighted BSDF subtree has a non-scalar weight and "
                 "could not be represented.");
    return false;
  }

  mx::NodePtr branch = graph_->addNode(
      "standard_surface", next_name("weighted_branch"), "surfaceshader");
  for (const mx::InputPtr &input : surface->getInputs()) {
    mx::InputPtr copy = branch->addInput(input->getName(), input->getType());
    copy->copyContentFrom(input);
  }
  const bool mapped = approximate_bsdf(expression, branch, {}, tint_override,
                                       transmission_override);
  bool blended = mapped;
  if (mapped) {
    for (const mx::InputPtr &target : surface->getInputs()) {
      mx::InputPtr layer = branch->getInput(target->getName());
      if (target->getNodeName() == layer->getNodeName() &&
          target->getInterfaceName() == layer->getInterfaceName() &&
          target->getValueString() == layer->getValueString())
        continue;
      const std::string type = target->getType();
      if (type != "float" && type != "color3" && type != "vector3") {
        report_.warn(material_ + ": weighted BSDF subtree input " +
                     target->getName() + " has an unsupported type.");
        blended = false;
        continue;
      }
      MxExpression value =
          mix_expressions(input_expression(target), input_expression(layer),
                          weight, "weighted_subtree");
      if (!value.valid() ||
          !connect_expression(value, target, report_, material_,
                              "weighted BSDF subtree")) {
        report_.warn(material_ + ": weighted BSDF subtree input " +
                     target->getName() + " could not be blended.");
        blended = false;
      }
    }
  }
  graph_->removeNode(branch->getName());
  report_.warn(material_ +
               ": weighted BSDF subtree was approximated by blending "
               "standard_surface inputs; angular response and layer "
               "interactions may differ.");
  return blended;
}

bool ExpressionTranslator::approximate_conditional_bsdf(
    const mdl::IExpression_direct_call *call, const mx::NodePtr &surface,
    const MxExpression &layer_weight, const MxExpression &tint_override,
    const MxExpression &transmission_override) {
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
                   tint_override, transmission_override);
  approximate_bsdf(argument(call, "false_exp").get(), on_false, layer_weight,
                   tint_override, transmission_override);

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
    mx::NodePtr select =
        graph_->addNode("ifequal", next_name("conditional_bsdf"), type);
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

void ExpressionTranslator::set_diffuse_fallback(const mx::NodePtr &surface,
                                                const std::string &reason) {
  set_preview_value(surface, "base", "1.0");
  set_preview_value(surface, "base_color", "0.18, 0.18, 0.18");
  set_preview_value(surface, "specular", "0.0");
  set_preview_value(surface, "transmission", "0.0");
  report_.warn(material_ + ": " + reason +
               " was approximated with neutral gray diffuse.");
}

MxExpression
ExpressionTranslator::make_constant_expression(const std::string &type,
                                               const std::string &value,
                                               const std::string &prefix) {
  mx::NodePtr node = graph_->addNode("constant", next_name(prefix), type);
  add_node_inputs_from_definition(node);
  mx::InputPtr input = node->getInput("value");
  if (!input)
    return {};
  input->setValueString(value);
  ++report_.translated_nodes;
  return {node, {}, type};
}

MxExpression ExpressionTranslator::make_binary_expression(
    const std::string &category, const std::string &type,
    const MxExpression &left, const MxExpression &right,
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

MxExpression ExpressionTranslator::multiply_colors(const MxExpression &left,
                                                   const MxExpression &right,
                                                   const std::string &prefix) {
  if (!left.valid())
    return right;
  if (!right.valid())
    return left;
  return make_binary_expression("multiply", "color3", left, right, prefix);
}

MxExpression ExpressionTranslator::mix_expressions(const MxExpression &base,
                                                   const MxExpression &layer,
                                                   const MxExpression &weight,
                                                   const std::string &name) {
  if (!base.valid() || !layer.valid() || base.type != layer.type)
    return {};
  mx::NodePtr node = graph_->addNode("mix", next_name(name), base.type);
  node->setNodeDefString("ND_mix_" + base.type);
  add_node_inputs_from_definition(node);
  if (!connect_expression(base, node->getInput("bg"), report_, material_,
                          name) ||
      !connect_expression(layer, node->getInput("fg"), report_, material_,
                          name) ||
      !connect_expression(weight, node->getInput("mix"), report_, material_,
                          name))
    return {};
  ++report_.translated_nodes;
  return {node, {}, base.type};
}

MxExpression
ExpressionTranslator::make_logical_expression(const std::string &category,
                                              const MxExpression &left,
                                              const MxExpression &right) {
  mx::NodePtr node = graph_->addNode(category, next_name("logical"), "boolean");
  node->setNodeDefString("ND_logical_" + category);
  add_node_inputs_from_definition(node);
  if (!connect_expression(left,
                          node->getInput(category == "not" ? "in" : "in1"),
                          report_, material_, "logical operand") ||
      (category != "not" &&
       !connect_expression(right, node->getInput("in2"), report_, material_,
                           "logical operand")))
    return {};
  ++report_.translated_nodes;
  return {node, {}, "boolean"};
}

MxExpression ExpressionTranslator::standard_surface_roughness(
    const MxExpression &roughness) {
  if (!roughness.valid())
    return {};
  MxExpression zero =
      make_constant_expression("float", "0.0", "roughness_zero");
  MxExpression nonnegative = make_binary_expression(
      "max", "float", roughness, zero, "roughness_nonnegative");
  if (!nonnegative.valid())
    return {};
  mx::NodePtr node =
      graph_->addNode("sqrt", next_name("roughness_sqrt"), "float");
  add_node_inputs_from_definition(node);
  if (!connect_expression(nonnegative, node->getInput("in"), report_, material_,
                          "microfacet roughness"))
    return {};
  ++report_.translated_nodes;
  return {node, {}, "float"};
}

bool ExpressionTranslator::same_expression(const MxExpression &left,
                                           const MxExpression &right) {
  if (!left.valid() || !right.valid() || left.type != right.type)
    return false;
  if (!left.interface_name.empty() || !right.interface_name.empty())
    return left.interface_name == right.interface_name;
  if (left.node == right.node)
    return true;
  if (!left.node || !right.node || left.node->getCategory() != "constant" ||
      right.node->getCategory() != "constant")
    return false;
  mx::InputPtr a = left.node->getInput("value");
  mx::InputPtr b = right.node->getInput("value");
  return a && b && a->getValueString() == b->getValueString();
}

MxExpression ExpressionTranslator::select_float(const MxExpression &left,
                                                const MxExpression &right,
                                                const MxExpression &greater,
                                                const MxExpression &otherwise,
                                                const std::string &name) {
  mx::NodePtr node = graph_->addNode("ifgreater", next_name(name), "float");
  add_node_inputs_from_definition(node);
  if (!connect_expression(left, node->getInput("value1"), report_, material_,
                          name) ||
      !connect_expression(right, node->getInput("value2"), report_, material_,
                          name) ||
      !connect_expression(greater, node->getInput("in1"), report_, material_,
                          name) ||
      !connect_expression(otherwise, node->getInput("in2"), report_, material_,
                          name))
    return {};
  ++report_.translated_nodes;
  return {node, {}, "float"};
}

MxExpression ExpressionTranslator::select_vector3(const MxExpression &left,
                                                  const MxExpression &right,
                                                  const MxExpression &greater,
                                                  const MxExpression &otherwise,
                                                  const std::string &name) {
  mx::NodePtr node = graph_->addNode("ifgreater", next_name(name), "vector3");
  node->setNodeDefString("ND_ifgreater_vector3");
  add_node_inputs_from_definition(node);
  if (!connect_expression(left, node->getInput("value1"), report_, material_,
                          name) ||
      !connect_expression(right, node->getInput("value2"), report_, material_,
                          name) ||
      !connect_expression(greater, node->getInput("in1"), report_, material_,
                          name) ||
      !connect_expression(otherwise, node->getInput("in2"), report_, material_,
                          name))
    return {};
  ++report_.translated_nodes;
  return {node, {}, "vector3"};
}

MxExpression ExpressionTranslator::standard_surface_microfacet_roughness(
    const mdl::IExpression_direct_call *call, const mx::NodePtr &surface,
    const std::string &lobe) {
  const std::string anisotropy_input = lobe + "_anisotropy";
  const std::string rotation_input = lobe + "_rotation";
  MxExpression u = translate(argument(call, "roughness_u").get());
  MxExpression v = translate(argument(call, "roughness_v").get());
  if (!u.valid() || u.type != "float") {
    report_.warn(material_ + ": GGX roughness_u could not be represented.");
    return {};
  }
  if (!v.valid() || v.type != "float") {
    set_preview_value(surface, anisotropy_input, "0.0");
    set_preview_value(surface, rotation_input, "0.0");
    report_.warn(material_ +
                 ": GGX roughness_v was not translatable; using roughness_u.");
    return standard_surface_roughness(u);
  }
  if (same_expression(u, v)) {
    set_preview_value(surface, anisotropy_input, "0.0");
    set_preview_value(surface, rotation_input, "0.0");
    return standard_surface_roughness(u);
  }

  MxExpression zero =
      make_constant_expression("float", "0.0", "anisotropy_zero");
  MxExpression one = make_constant_expression("float", "1.0", "anisotropy_one");
  MxExpression quarter =
      make_constant_expression("float", "0.25", "anisotropy_quarter");
  const auto clamp_axis = [&](const MxExpression &axis) {
    return make_binary_expression(
        "min", "float",
        make_binary_expression("max", "float", axis, zero,
                               "anisotropy_nonnegative"),
        one, "anisotropy_clamped");
  };
  u = clamp_axis(u);
  v = clamp_axis(v);
  MxExpression high =
      make_binary_expression("max", "float", u, v, "anisotropy_major");
  MxExpression low =
      make_binary_expression("min", "float", u, v, "anisotropy_minor");
  MxExpression safe_high =
      select_float(high, zero, high, one, "anisotropy_safe_major");
  MxExpression ratio = make_binary_expression("divide", "float", low, safe_high,
                                              "anisotropy_ratio");
  MxExpression amount = make_binary_expression("subtract", "float", one, ratio,
                                               "anisotropy_amount");
  amount = select_float(high, zero, amount, zero, "anisotropy_nonzero");
  MxExpression rotation =
      select_float(v, u, quarter, zero, "anisotropy_rotation");
  MxExpression product =
      make_binary_expression("multiply", "float", u, v, "anisotropy_product");
  MxExpression geometric_mean = standard_surface_roughness(product);
  MxExpression roughness = standard_surface_roughness(geometric_mean);
  if (!amount.valid() || !rotation.valid() || !roughness.valid() ||
      !connect_preview_input(amount, surface, anisotropy_input,
                             "GGX anisotropy") ||
      !connect_preview_input(rotation, surface, rotation_input,
                             "GGX axis rotation"))
    return {};

  MxExpression tangent = translate(argument(call, "tangent_u").get());
  if (!tangent.valid() || tangent.type != "vector3" ||
      !connect_preview_input(tangent, surface, "tangent", "GGX tangent"))
    report_.warn(material_ + ": GGX tangent could not be represented; "
                             "anisotropy orientation may differ.");
  report_.warn(material_ + ": standard_surface limits extreme GGX anisotropy "
                           "and clamps roughness to its supported range.");
  return roughness;
}

MxExpression
ExpressionTranslator::translate_roughness(const mdl::IExpression *expression) {
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

bool ExpressionTranslator::approximate_diffuse(
    const mdl::IExpression_direct_call *call, const mx::NodePtr &surface,
    const MxExpression &layer_weight, const MxExpression &tint_override) {
  mx::InputPtr metalness = surface->getInput("metalness");
  const bool over_metal =
      layer_weight.valid() && layer_weight.type == "float" && metalness &&
      !metalness->hasNodeName() && metalness->getInterfaceName().empty() &&
      metalness->getValueString() == "1.0";
  if (over_metal) {
    MxExpression base_color = input_expression(surface->getInput("base_color"));
    MxExpression tint = multiply_colors(translate(argument(call, "tint").get()),
                                        tint_override, "diffuse_tint");
    if (!base_color.valid() || !tint.valid())
      return false;
    mx::NodePtr mix =
        graph_->addNode("mix", next_name("metal_diffuse_mix"), "color3");
    mix->setNodeDefString("ND_mix_color3");
    add_node_inputs_from_definition(mix);
    if (!connect_expression(base_color, mix->getInput("bg"), report_, material_,
                            "metal base color") ||
        !connect_expression(tint, mix->getInput("fg"), report_, material_,
                            "diffuse layer color") ||
        !connect_expression(layer_weight, mix->getInput("mix"), report_,
                            material_, "diffuse layer weight"))
      return false;
    MxExpression one =
        make_constant_expression("float", "1.0", "metal_weight_one");
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

  MxExpression roughness = translate(argument(call, "roughness").get());
  if (roughness.valid())
    connect_preview_input(roughness, surface, "diffuse_roughness",
                          "diffuse roughness");
  report_.warn(material_ +
               ": MDL diffuse was approximated with standard_surface base; "
               "MDL multiscatter tint is not represented.");
  return true;
}

bool ExpressionTranslator::approximate_microfacet(
    const mdl::IExpression_direct_call *call, const mx::NodePtr &surface,
    const MxExpression &layer_weight, const MxExpression &tint_override,
    const MxExpression &transmission_override, bool smooth_specular,
    bool simple_glossy) {
  mi::base::Handle<const mdl::IExpression> mode = argument(call, "mode");
  const std::string mode_name = enum_name(mode.get());
  if (mode_name != "scatter_reflect" && mode_name != "scatter_transmit" &&
      mode_name != "scatter_reflect_transmit")
    report_.warn(material_ + ": microfacet scatter mode could not be resolved; "
                             "using a reflection approximation.");
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
  MxExpression reflection_tint =
      multiply_colors(tint, tint_override, "microfacet_tint");
  MxExpression transmission_tint = multiply_colors(
      tint, transmission_override, "microfacet_transmission_tint");
  if (!transmit && reflection_tint.valid())
    connect_preview_input(reflection_tint, surface,
                          standalone_reflection ? "base_color"
                                                : "specular_color",
                          "microfacet reflection tint");
  if ((transmit || reflect_transmit) && transmission_tint.valid())
    connect_preview_input(transmission_tint, surface, "transmission_color",
                          "microfacet transmission tint");

  if (smooth_specular) {
    set_preview_value(surface, "specular_anisotropy", "0.0");
    set_preview_value(surface, "specular_rotation", "0.0");
  }
  MxExpression roughness =
      smooth_specular
          ? make_constant_expression("float", "0.0", "specular_roughness")
          : standard_surface_microfacet_roughness(call, surface);
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
  if (simple_glossy)
    report_.warn(
        material_ +
        ": MDL simple_glossy was approximated with standard_surface GGX; "
        "its Phong distribution, backscatter and multiscatter tint may "
        "differ.");
  else if (!smooth_specular)
    report_.warn(material_ +
                 ": MDL GGX was approximated with standard_surface; "
                 "backscatter and multiscatter tint may differ.");
  if (standalone_reflection)
    report_.warn(material_ +
                 ": reflection-only glossy BSDF was approximated as metallic; "
                 "its grazing-angle reflectance may differ.");
  return true;
}

bool ExpressionTranslator::approximate_specular(
    const mdl::IExpression_direct_call *call, const mx::NodePtr &surface,
    const MxExpression &layer_weight, const MxExpression &tint_override,
    const MxExpression &transmission_override) {
  if (layer_weight.valid() &&
      enum_name(argument(call, "mode").get()) == "scatter_reflect") {
    connect_preview_input(layer_weight, surface, "coat",
                          "specular coat weight");
    MxExpression tint = multiply_colors(translate(argument(call, "tint").get()),
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
                                transmission_override, true);
}

bool ExpressionTranslator::approximate_sheen(
    const mdl::IExpression_direct_call *call, const mx::NodePtr &surface,
    const MxExpression &layer_weight, const MxExpression &tint_override,
    const MxExpression &transmission_override) {
  mi::base::Handle<const mdl::IExpression> base =
      argument(call, "multiscatter");
  if (base) {
    if (!approximate_bsdf(base.get(), surface, {}, tint_override,
                          transmission_override))
      report_.warn(material_ +
                   ": sheen multiscatter BSDF could not be represented.");
  } else {
    set_preview_value(surface, "base", "1.0");
    set_preview_value(surface, "specular", "0.0");
    set_preview_value(surface, "transmission", "0.0");
    MxExpression color =
        multiply_colors(translate(argument(call, "multiscatter_tint").get()),
                        tint_override, "sheen_base");
    if (color.valid())
      connect_preview_input(color, surface, "base_color",
                            "sheen multiscatter color");
  }
  if (layer_weight.valid())
    connect_preview_input(layer_weight, surface, "sheen", "sheen weight");
  else
    set_preview_value(surface, "sheen", "1.0");
  MxExpression tint = multiply_colors(translate(argument(call, "tint").get()),
                                      tint_override, "sheen_tint");
  MxExpression roughness = translate(argument(call, "roughness").get());
  if (tint.valid())
    connect_preview_input(tint, surface, "sheen_color", "sheen tint");
  if (roughness.valid())
    connect_preview_input(roughness, surface, "sheen_roughness",
                          "sheen roughness");
  report_.warn(material_ +
               ": MDL sheen was approximated with standard_surface sheen; "
               "multiscatter tint, layer order, and distribution may differ.");
  return true;
}

bool ExpressionTranslator::approximate_conductor(
    const mdl::IExpression_direct_call *call, const mx::NodePtr &surface,
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
    MxExpression ior_minus_one = make_binary_expression(
        "subtract", "color3", ior, one, "conductor_ior_minus_one");
    MxExpression ior_plus_one = make_binary_expression(
        "add", "color3", ior, one, "conductor_ior_plus_one");
    MxExpression numerator = make_binary_expression(
        "add", "color3",
        make_binary_expression("multiply", "color3", ior_minus_one,
                               ior_minus_one, "conductor_ior_square"),
        make_binary_expression("multiply", "color3", extinction, extinction,
                               "conductor_extinction_square"),
        "conductor_reflectance_numerator");
    MxExpression denominator = make_binary_expression(
        "add", "color3",
        make_binary_expression("multiply", "color3", ior_plus_one, ior_plus_one,
                               "conductor_ior_denominator"),
        make_binary_expression("multiply", "color3", extinction, extinction,
                               "conductor_extinction_denominator"),
        "conductor_reflectance_denominator");
    reflectance = make_binary_expression("divide", "color3", numerator,
                                         denominator, "conductor_reflectance");
  }
  reflectance = multiply_colors(reflectance, tint_override, "conductor_tint");
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
  mi::base::Handle<const mdl::IExpression> resolved_base(base_expression);
  while (resolved_base &&
         resolved_base->get_kind() == mdl::IExpression::EK_TEMPORARY)
    resolved_base = compiled_->get_temporary(
        static_cast<const mdl::IExpression_temporary *>(resolved_base.get())
            ->get_index());
  MxExpression roughness;
  if (resolved_base &&
      resolved_base->get_kind() == mdl::IExpression::EK_DIRECT_CALL) {
    const auto *base_call =
        static_cast<const mdl::IExpression_direct_call *>(resolved_base.get());
    const char *name = base_call->get_definition();
    mi::base::Handle<const mdl::IFunction_definition> definition(
        name ? transaction_->access<mdl::IFunction_definition>(name) : nullptr);
    if (definition && definition->get_semantic() ==
                          mdl::IFunction_definition::
                              DS_INTRINSIC_DF_MICROFACET_GGX_SMITH_BSDF)
      roughness = standard_surface_microfacet_roughness(base_call, surface);
  }
  if (!roughness.valid())
    roughness =
        standard_surface_roughness(translate_roughness(base_expression.get()));
  if (roughness.valid())
    connect_preview_input(roughness, surface, "specular_roughness",
                          "conductor roughness");
  report_.warn(material_ +
               ": MDL conductor was approximated with standard_surface "
               "metalness using normal-incidence reflectance.");
  return true;
}

bool ExpressionTranslator::approximate_thin_film(
    const mdl::IExpression_direct_call *call, const mx::NodePtr &surface,
    const MxExpression &tint_override,
    const MxExpression &transmission_override) {
  mi::base::Handle<const mdl::IExpression> base = argument(call, "base");
  mi::base::Handle<const mdl::IExpression> thickness =
      argument(call, "thickness");
  mi::base::Handle<const mdl::IExpression> ior_expression =
      argument(call, "ior");
  const bool base_mapped = approximate_bsdf(
      base.get(), surface, {}, tint_override, transmission_override);
  MxExpression translated_thickness = translate(thickness.get());
  if (translated_thickness.valid())
    connect_preview_input(translated_thickness, surface, "thin_film_thickness",
                          "thin-film thickness");
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

void ExpressionTranslator::apply_layer_normal(
    const mdl::IExpression_direct_call *call, const mx::NodePtr &surface,
    bool coat) {
  mi::base::Handle<const mdl::IExpression> normal = argument(call, "normal");
  if (!normal ||
      is_state_normal_expression(normal.get(), compiled_, transaction_))
    return;
  MxExpression translated = translate(normal.get());
  if (!translated.valid() || translated.type != "vector3") {
    report_.warn(material_ +
                 ": the BSDF layer normal could not be represented.");
    return;
  }
  if (connect_preview_input(translated, surface,
                            coat ? "coat_normal" : "normal",
                            "BSDF layer normal"))
    report_.warn(
        material_ +
        (coat ? ": the BSDF layer normal was applied to the standard_surface "
                "coat."
              : ": the BSDF layer normal was applied to the standard_surface "
                "base normal; independently layered normals may differ."));
}

bool ExpressionTranslator::is_reflective_ggx_tree(
    const mdl::IExpression *expression, bool allow_weighted) {
  if (!expression)
    return false;
  if (expression->get_kind() == mdl::IExpression::EK_TEMPORARY) {
    mi::base::Handle<const mdl::IExpression> value(compiled_->get_temporary(
        static_cast<const mdl::IExpression_temporary *>(expression)
            ->get_index()));
    return is_reflective_ggx_tree(value.get(), allow_weighted);
  }
  if (expression->get_kind() != mdl::IExpression::EK_DIRECT_CALL)
    return false;
  const auto *call =
      static_cast<const mdl::IExpression_direct_call *>(expression);
  const char *name = call->get_definition();
  mi::base::Handle<const mdl::IFunction_definition> definition(
      name ? transaction_->access<mdl::IFunction_definition>(name) : nullptr);
  if (!definition)
    return false;
  using S = mdl::IFunction_definition;
  if (definition->get_semantic() ==
      S::DS_INTRINSIC_DF_MICROFACET_GGX_SMITH_BSDF)
    return enum_name(argument(call, "mode").get()) == "scatter_reflect";
  if (allow_weighted &&
      definition->get_semantic() == S::DS_INTRINSIC_DF_WEIGHTED_LAYER)
    return is_reflective_ggx_tree(argument(call, "base").get(), false) &&
           is_reflective_ggx_tree(argument(call, "layer").get(), false);
  return false;
}

bool ExpressionTranslator::approximate_layer(
    const mdl::IExpression_direct_call *call, const mx::NodePtr &surface,
    const MxExpression &tint_override,
    const MxExpression &transmission_override,
    mdl::IFunction_definition::Semantics semantic) {
  mi::base::Handle<const mdl::IExpression> base = argument(call, "base");
  mi::base::Handle<const mdl::IExpression> layer = argument(call, "layer");
  mi::base::Handle<const mdl::IExpression> resolved_base(base);
  while (resolved_base &&
         resolved_base->get_kind() == mdl::IExpression::EK_TEMPORARY)
    resolved_base = compiled_->get_temporary(
        static_cast<const mdl::IExpression_temporary *>(resolved_base.get())
            ->get_index());
  mi::base::Handle<const mdl::IExpression> resolved_layer(layer);
  while (resolved_layer &&
         resolved_layer->get_kind() == mdl::IExpression::EK_TEMPORARY)
    resolved_layer = compiled_->get_temporary(
        static_cast<const mdl::IExpression_temporary *>(resolved_layer.get())
            ->get_index());
  if (semantic ==
          mdl::IFunction_definition::DS_INTRINSIC_DF_COLOR_CUSTOM_CURVE_LAYER &&
      resolved_base && is_neutral_expression(resolved_base.get()) &&
      is_reflective_ggx_tree(resolved_layer.get())) {
    MxExpression reflectivity =
        translate(argument(call, "normal_reflectivity").get());
    MxExpression weight = translate(argument(call, "weight").get());
    if (reflectivity.valid() && reflectivity.type == "color3" &&
        weight.valid() && weight.type == "color3") {
      MxExpression color =
          multiply_colors(reflectivity, weight, "metal_curve_weight");
      if (tint_override.valid())
        color = multiply_colors(color, tint_override, "metal_curve_override");
      if (color.valid() && approximate_bsdf(layer.get(), surface, {}, color,
                                            transmission_override)) {
        apply_layer_normal(call, surface);
        report_.warn(
            material_ +
            ": color custom-curve reflection over an empty BSDF "
            "was approximated as metallic; its grazing curve may differ.");
        return true;
      }
    }
    report_.warn(
        material_ +
        ": color custom-curve normal reflectivity could not be "
        "represented; falling back to the general layer approximation.");
  }
  mi::base::Handle<const mdl::IExpression> weight_expression =
      argument(call, "weight");
  MxExpression weight = translate(weight_expression.get());
  if (!weight.valid()) {
    weight = make_constant_expression("float", "0.5", "layer_weight");
    report_.warn(material_ +
                 ": layer weight could not be translated; using 0.5.");
  }

  const bool base_mapped = approximate_bsdf(
      base.get(), surface, {}, tint_override, transmission_override);
  mx::InputPtr metalness = surface->getInput("metalness");
  const auto *layer_call =
      resolved_layer &&
              resolved_layer->get_kind() == mdl::IExpression::EK_DIRECT_CALL
          ? static_cast<const mdl::IExpression_direct_call *>(
                resolved_layer.get())
          : nullptr;
  const char *layer_name = layer_call ? layer_call->get_definition() : nullptr;
  mi::base::Handle<const mdl::IFunction_definition> layer_definition(
      layer_name ? transaction_->access<mdl::IFunction_definition>(layer_name)
                 : nullptr);
  mx::InputPtr base_transmission = surface->getInput("transmission");
  const bool preserve_transmission_roughness =
      base_mapped && base_transmission &&
      (base_transmission->hasNodeName() ||
       !base_transmission->getInterfaceName().empty() ||
       (base_transmission->getValueString() != "0" &&
        base_transmission->getValueString() != "0.0")) &&
      layer_definition &&
      (layer_definition->get_semantic() ==
           mdl::IFunction_definition::
               DS_INTRINSIC_DF_MICROFACET_GGX_SMITH_BSDF ||
       layer_definition->get_semantic() ==
           mdl::IFunction_definition::DS_INTRINSIC_DF_SIMPLE_GLOSSY_BSDF) &&
      enum_name(argument(layer_call, "mode").get()) == "scatter_reflect";
  MxExpression transmission_roughness;
  if (preserve_transmission_roughness) {
    MxExpression base_roughness =
        input_expression(surface->getInput("specular_roughness"));
    MxExpression extra =
        input_expression(surface->getInput("transmission_extra_roughness"));
    if (base_roughness.valid() && extra.valid()) {
      MxExpression sum = make_binary_expression(
          "add", "float", base_roughness, extra, "transmission_roughness");
      MxExpression zero =
          make_constant_expression("float", "0.0", "roughness_zero");
      MxExpression one =
          make_constant_expression("float", "1.0", "roughness_one");
      transmission_roughness = make_binary_expression(
          "min", "float",
          make_binary_expression("max", "float", sum, zero,
                                 "transmission_roughness_min"),
          one, "transmission_roughness_max");
    }
  }
  const bool coat_over_metal =
      semantic == mdl::IFunction_definition::DS_INTRINSIC_DF_WEIGHTED_LAYER &&
      weight.type == "float" && metalness && !metalness->hasNodeName() &&
      metalness->getInterfaceName().empty() &&
      metalness->getValueString() == "1.0" && layer_definition &&
      layer_definition->get_semantic() ==
          mdl::IFunction_definition::
              DS_INTRINSIC_DF_MICROFACET_GGX_SMITH_BSDF &&
      enum_name(argument(layer_call, "mode").get()) == "scatter_reflect";
  bool layer_mapped;
  if (coat_over_metal) {
    MxExpression base_roughness =
        input_expression(surface->getInput("specular_roughness"));
    MxExpression base_anisotropy =
        input_expression(surface->getInput("specular_anisotropy"));
    MxExpression base_rotation =
        input_expression(surface->getInput("specular_rotation"));
    MxExpression base_color = input_expression(surface->getInput("base_color"));
    MxExpression base_tangent = input_expression(surface->getInput("tangent"));
    MxExpression roughness =
        standard_surface_microfacet_roughness(layer_call, surface, "coat");
    MxExpression coat_anisotropy =
        input_expression(surface->getInput("coat_anisotropy"));
    MxExpression coat_rotation =
        input_expression(surface->getInput("coat_rotation"));
    MxExpression coat_tangent = input_expression(surface->getInput("tangent"));
    MxExpression coat_tint =
        multiply_colors(translate(argument(layer_call, "tint").get()),
                        tint_override, "ggx_layer_tint");
    MxExpression mixed_roughness = mix_expressions(
        base_roughness, roughness, weight, "ggx_layer_roughness");
    MxExpression mixed_anisotropy = mix_expressions(
        base_anisotropy, coat_anisotropy, weight, "ggx_layer_anisotropy");
    MxExpression mixed_color =
        mix_expressions(base_color, coat_tint, weight, "ggx_layer_color");
    MxExpression one =
        make_constant_expression("float", "1.0", "ggx_layer_one");
    MxExpression zero =
        make_constant_expression("float", "0.0", "ggx_layer_zero");
    MxExpression half =
        make_constant_expression("float", "0.5", "ggx_layer_half");
    MxExpression upper_rotation =
        select_float(coat_anisotropy, zero, coat_rotation, base_rotation,
                     "ggx_layer_upper_rotation");
    MxExpression rotation = select_float(weight, half, upper_rotation,
                                         base_rotation, "ggx_layer_rotation");
    MxExpression remaining = make_binary_expression(
        "subtract", "float", one, weight, "ggx_layer_remaining");
    MxExpression coat_weight = make_binary_expression(
        "multiply", "float", weight, remaining, "ggx_layer_coat");
    layer_mapped = mixed_roughness.valid() && mixed_anisotropy.valid() &&
                   mixed_color.valid() && coat_weight.valid() &&
                   rotation.valid();
    if (layer_mapped) {
      connect_preview_input(mixed_roughness, surface, "specular_roughness",
                            "layered GGX roughness");
      connect_preview_input(mixed_anisotropy, surface, "specular_anisotropy",
                            "layered GGX anisotropy");
      connect_preview_input(mixed_color, surface, "base_color",
                            "layered GGX color");
      connect_preview_input(coat_weight, surface, "coat", "GGX coat weight");
      connect_preview_input(rotation, surface, "specular_rotation",
                            "layered GGX rotation");
      if (base_tangent.valid() && coat_tangent.valid() &&
          !same_expression(base_tangent, coat_tangent)) {
        MxExpression upper_tangent =
            select_vector3(coat_anisotropy, zero, coat_tangent, base_tangent,
                           "ggx_layer_upper_tangent");
        MxExpression tangent = select_vector3(
            weight, half, upper_tangent, base_tangent, "ggx_layer_tangent");
        if (tangent.valid())
          connect_preview_input(tangent, surface, "tangent",
                                "layered GGX tangent");
      }
    }
    if (roughness.valid())
      connect_preview_input(roughness, surface, "coat_roughness",
                            "GGX coat roughness");
    report_.warn(material_ +
                 ": reflective GGX over metal was approximated with blended "
                 "roughness and a standard_surface coat; its Fresnel response, "
                 "tint, and shared tangent may differ.");
  } else {
    layer_mapped = approximate_bsdf(layer.get(), surface, weight, tint_override,
                                    transmission_override);
  }
  if (layer_mapped && transmission_roughness.valid()) {
    MxExpression reflection_roughness =
        input_expression(surface->getInput("specular_roughness"));
    if (reflection_roughness.valid()) {
      MxExpression extra = make_binary_expression(
          "subtract", "float", transmission_roughness, reflection_roughness,
          "transmission_roughness_offset");
      connect_preview_input(extra, surface, "transmission_extra_roughness",
                            "independent transmission roughness");
    }
  }
  apply_layer_normal(call, surface, coat_over_metal);
  if (semantic == mdl::IFunction_definition::DS_INTRINSIC_DF_WEIGHTED_LAYER) {
    report_.warn(material_ +
                 ": MDL weighted_layer was approximated with standard_surface "
                 "base and specular/transmission weights.");
  } else if (semantic ==
             mdl::IFunction_definition::DS_INTRINSIC_DF_FRESNEL_LAYER) {
    report_.warn(material_ +
                 ": MDL fresnel_layer was approximated with standard_surface; "
                 "layer IOR and normal may differ.");
  } else {
    report_.warn(
        material_ +
        ": MDL custom-curve layer was approximated with standard_surface; "
        "its Fresnel curve is not reproduced.");
  }
  return base_mapped || layer_mapped;
}

} // namespace matconv::detail::mdl_translation
