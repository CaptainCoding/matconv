#include "mdl_expression_translator.h"

namespace matconv::detail::mdl_translation {

MxExpression ExpressionTranslator::translate_thin_film(
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

void ExpressionTranslator::apply_thin_film(const mx::NodePtr &bsdf) {
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
      connect_expression(film.thickness, thickness_input, report_, material_,
                         "thin-film thickness");
  const bool ior_connected =
      ior.valid() &&
      connect_expression(ior, ior_input, report_, material_, "thin-film IOR");
  film.applied = film.applied || (thickness_connected && ior_connected);
}

MxExpression ExpressionTranslator::translate_tint(
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
           source_expression->get_kind() == mdl::IExpression::EK_TEMPORARY) {
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
          source_definition ? transaction_->access<mdl::IFunction_definition>(
                                  source_definition)
                            : nullptr);
      const char *mdl_name =
          source_function ? source_function->get_mdl_name() : nullptr;
      if (mdl_name)
        source_name = std::string(" (") + mdl_name + ")";
    } else if (source_expression && source_expression->get_kind() ==
                                        mdl::IExpression::EK_PARAMETER) {
      const mi::Size parameter_index =
          static_cast<const mdl::IExpression_parameter *>(source_expression)
              ->get_index();
      const char *parameter_name =
          compiled_->get_parameter_name(parameter_index);
      if (parameter_name)
        source_name = std::string(" (parameter ") + parameter_name + ")";
    } else if (source_expression) {
      source_name =
          " (expression kind " +
          std::to_string(static_cast<int>(source_expression->get_kind())) + ")";
    }
    if (tint.valid() && tint.type == "color3") {
      mx::NodePtr diffuse = graph_->addNode(
          "oren_nayar_diffuse_bsdf", next_name("tinted_diffuse"), "BSDF");
      add_node_inputs_from_definition(diffuse);
      connect_expression(tint, diffuse->getInput("color"), report_, material_,
                         "df::tint color");
      report_.warn(material_ + ": MDL tint's unsupported BSDF" + source_name +
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

MxExpression ExpressionTranslator::translate_microfacet(
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
      if (name == "tint" || (!tint_expression && type_name == "color3"))
        tint_expression = argument;
      else if (name == "mode")
        mode_expression = argument;
    }
  }

  MxExpression tint = translate(tint_expression.get());

  mx::NodePtr bsdf =
      graph_->addNode("dielectric_bsdf", next_name("microfacet_bsdf"), "BSDF");
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
    report_.warn(material_ + ": MDL GGX scatter mode was not recognized; using "
                             "reflection mode.");
  report_.warn(material_ +
               ": MDL GGX microfacet was approximated with a MaterialX "
               "dielectric BSDF; its backscatter may differ.");
  ++report_.translated_nodes;
  return {bsdf, {}, "BSDF"};
}

MxExpression ExpressionTranslator::translate_fresnel_factor(
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
      graph_->addNode("conductor_bsdf", next_name("conductor_bsdf"), "BSDF");
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

MxExpression ExpressionTranslator::translate_weighted_layer(
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
  connect_expression(layer, weighted_layer->getInput("in1"), report_, material_,
                     "weighted_layer layer");
  connect_expression(weight, weighted_layer->getInput("in2"), report_,
                     material_, "weighted_layer weight");

  mx::NodePtr layer_node =
      graph_->addNode("layer", next_name("weighted_layer"), "BSDF");
  layer_node->setNodeDefString("ND_layer_bsdf");
  add_node_inputs_from_definition(layer_node);
  connect_expression({weighted_layer, {}, "BSDF"}, layer_node->getInput("top"),
                     report_, material_, "weighted_layer top");
  connect_expression(base, layer_node->getInput("base"), report_, material_,
                     "weighted_layer base");
  report_.warn(material_ +
               ": MDL weighted_layer was mapped to MaterialX vertical "
               "layering; its explicit normal input is not preserved.");
  report_.translated_nodes += 2;
  return {layer_node, {}, "BSDF"};
}

MxExpression ExpressionTranslator::translate_microfacet_roughness(
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
  if (semantic == mdl::IFunction_definition::
                      DS_INTRINSIC_DF_MICROFACET_GGX_SMITH_BSDF ||
      semantic == mdl::IFunction_definition::
                      DS_INTRINSIC_DF_MICROFACET_BECKMANN_SMITH_BSDF ||
      semantic == mdl::IFunction_definition::
                      DS_INTRINSIC_DF_MICROFACET_GGX_VCAVITIES_BSDF ||
      semantic == mdl::IFunction_definition::
                      DS_INTRINSIC_DF_MICROFACET_BECKMANN_VCAVITIES_BSDF) {
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

} // namespace matconv::detail::mdl_translation
