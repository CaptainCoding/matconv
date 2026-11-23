#include "mdl_expression_translator.h"
#include "texture_resources.h"

#include <mi/neuraylib/iimage.h>
#include <mi/neuraylib/itexture.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <system_error>

namespace matconv::detail::mdl_translation {

std::optional<mi::Sint32> ExpressionTranslator::texture_coordinate_index(
    const mdl::IExpression *expression) {
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
      if (!argument || argument->get_kind() != mdl::IExpression::EK_CONSTANT)
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

MxExpression
ExpressionTranslator::matrix_component(const mdl::IExpression *expression,
                                       mi::Size column, mi::Size row) {
  if (!expression || column >= 4 || row >= 4)
    return {};
  if (expression->get_kind() == mdl::IExpression::EK_TEMPORARY) {
    mi::base::Handle<const mdl::IExpression> resolved(compiled_->get_temporary(
        static_cast<const mdl::IExpression_temporary *>(expression)
            ->get_index()));
    return matrix_component(resolved.get(), column, row);
  }
  if (expression->get_kind() == mdl::IExpression::EK_CONSTANT) {
    mi::base::Handle<const mdl::IValue> value(
        static_cast<const mdl::IExpression_constant *>(expression)
            ->get_value());
    if (!value || value->get_kind() != mdl::IValue::VK_MATRIX)
      return {};
    const auto *matrix = static_cast<const mdl::IValue_compound *>(value.get());
    mi::base::Handle<const mdl::IValue_vector> vector(
        matrix->get_value<mdl::IValue_vector>(column));
    mi::base::Handle<const mdl::IValue_float> item(
        vector ? vector->get_value<mdl::IValue_float>(row) : nullptr);
    return item ? make_constant_expression("float",
                                           format_float(item->get_value()),
                                           "matrix_component")
                : MxExpression{};
  }
  if (expression->get_kind() != mdl::IExpression::EK_DIRECT_CALL)
    return {};
  const auto *call =
      static_cast<const mdl::IExpression_direct_call *>(expression);
  const char *name = call->get_definition();
  mi::base::Handle<const mdl::IFunction_definition> definition(
      name ? transaction_->access<mdl::IFunction_definition>(name) : nullptr);
  mi::base::Handle<const mdl::IExpression_list> args(call->get_arguments());
  if (!definition || !args)
    return {};
  if (definition->get_semantic() == mdl::IFunction_definition::DS_MULTIPLY &&
      args->get_size() == 2) {
    mi::base::Handle<const mdl::IExpression> left(
        args->get_expression(mi::Size(0)));
    mi::base::Handle<const mdl::IExpression> right(
        args->get_expression(mi::Size(1)));
    MxExpression result;
    for (mi::Size k = 0; k < 4; ++k) {
      MxExpression a = matrix_component(left.get(), k, row);
      MxExpression b = matrix_component(right.get(), column, k);
      if (!a.valid() || !b.valid())
        return {};
      MxExpression product =
          make_binary_expression("multiply", "float", a, b, "matrix_product");
      result = result.valid() ? make_binary_expression("add", "float", result,
                                                       product, "matrix_sum")
                              : product;
      if (!result.valid())
        return {};
    }
    return result;
  }
  if (definition->get_semantic() !=
          mdl::IFunction_definition::DS_MATRIX_ELEM_CONSTRUCTOR ||
      args->get_size() != 16)
    return {};
  mi::base::Handle<const mdl::IExpression> component(
      args->get_expression(column * 4 + row));
  MxExpression result = translate(component.get());
  return result.type == "float" ? result : MxExpression{};
}

MxExpression
ExpressionTranslator::texture_coordinates(const mdl::IExpression *expression) {
  if (!expression)
    return {};
  if (expression->get_kind() == mdl::IExpression::EK_TEMPORARY) {
    mi::base::Handle<const mdl::IExpression> resolved(compiled_->get_temporary(
        static_cast<const mdl::IExpression_temporary *>(expression)
            ->get_index()));
    return texture_coordinates(resolved.get());
  }
  if (expression->get_kind() != mdl::IExpression::EK_DIRECT_CALL)
    return {};
  const auto *call =
      static_cast<const mdl::IExpression_direct_call *>(expression);
  const char *name = call->get_definition();
  mi::base::Handle<const mdl::IFunction_definition> definition(
      name ? transaction_->access<mdl::IFunction_definition>(name) : nullptr);
  const char *mdl_name = definition ? definition->get_mdl_name() : nullptr;
  if (!mdl_name)
    return {};
  const std::string function(mdl_name);
  if (definition->get_semantic() ==
      mdl::IFunction_definition::DS_INTRINSIC_STATE_TEXTURE_COORDINATE) {
    const std::optional<mi::Sint32> index =
        texture_coordinate_index(expression);
    if (!index || *index < 0)
      return {};
    mx::NodePtr node = graph_->addNode(
        "texcoord", next_name("texture_coordinates"), "vector3");
    node->setNodeDefString("ND_texcoord_vector3");
    add_node_inputs_from_definition(node);
    node->getInput("index")->setValueString(std::to_string(*index));
    ++report_.translated_nodes;
    return {node, {}, "vector3"};
  }
  if (function.find("::base::coordinate_source(") == 0) {
    const std::string system =
        enum_name(argument(call, "coordinate_system").get());
    if (system == "texture_coordinate_object" ||
        system == "texture_coordinate_world") {
      mx::NodePtr node =
          graph_->addNode("position", next_name("texture_position"), "vector3");
      node->setNodeDefString("ND_position_vector3");
      add_node_inputs_from_definition(node);
      node->getInput("space")->setValueString(
          system == "texture_coordinate_object" ? "object" : "world");
      ++report_.translated_nodes;
      return {node, {}, "vector3"};
    }
    if (system != "texture_coordinate_uvw")
      return {};
    mi::base::Handle<const mdl::IExpression> space =
        argument(call, "texture_space");
    MxExpression index = translate(space.get());
    if (!index.valid() || index.type != "integer")
      return {};
    mx::NodePtr node = graph_->addNode(
        "texcoord", next_name("texture_coordinates"), "vector3");
    node->setNodeDefString("ND_texcoord_vector3");
    add_node_inputs_from_definition(node);
    if (!connect_expression(index, node->getInput("index"), report_, material_,
                            "texture coordinate index"))
      return {};
    ++report_.translated_nodes;
    return {node, {}, "vector3"};
  }
  if (function.find("::base::texture_coordinate_info(") == 0) {
    mi::base::Handle<const mdl::IExpression> position =
        argument(call, "position");
    MxExpression result = translate(position.get());
    return result.type == "vector3" ? result : MxExpression{};
  }
  if (function.find("::base::transform_coordinate(") != 0)
    return {};
  MxExpression source = texture_coordinates(argument(call, "coordinate").get());
  mi::base::Handle<const mdl::IExpression> matrix = argument(call, "transform");
  if (!source.valid() || !matrix)
    return {};
  mx::NodePtr output =
      graph_->addNode("combine3", next_name("transformed_uv"), "vector3");
  add_node_inputs_from_definition(output);
  for (mi::Size row = 0; row < 3; ++row) {
    MxExpression sum = matrix_component(matrix.get(), 3, row);
    if (!sum.valid())
      return {};
    for (mi::Size column = 0; column < 3; ++column) {
      MxExpression coefficient = matrix_component(matrix.get(), column, row);
      if (!coefficient.valid())
        return {};
      mx::NodePtr channel =
          graph_->addNode("extract", next_name("uv_component"), "float");
      channel->setNodeDefString("ND_extract_vector3");
      add_node_inputs_from_definition(channel);
      channel->getInput("index")->setValueString(std::to_string(column));
      if (!connect_expression(source, channel->getInput("in"), report_,
                              material_, "texture coordinate"))
        return {};
      ++report_.translated_nodes;
      MxExpression product =
          make_binary_expression("multiply", "float", coefficient,
                                 {channel, {}, "float"}, "uv_product");
      sum = make_binary_expression("add", "float", sum, product, "uv_sum");
      if (!sum.valid())
        return {};
    }
    if (!connect_expression(
            sum, output->getInput("in" + std::to_string(row + 1)), report_,
            material_, "transformed texture coordinate"))
      return {};
  }
  ++report_.translated_nodes;
  return {output, {}, "vector3"};
}

MxExpression
ExpressionTranslator::texture_tangent(const mdl::IExpression *expression,
                                      bool u) {
  if (!expression)
    return {};
  if (expression->get_kind() == mdl::IExpression::EK_TEMPORARY) {
    mi::base::Handle<const mdl::IExpression> resolved(compiled_->get_temporary(
        static_cast<const mdl::IExpression_temporary *>(expression)
            ->get_index()));
    return texture_tangent(resolved.get(), u);
  }
  if (expression->get_kind() != mdl::IExpression::EK_DIRECT_CALL)
    return {};
  const auto *call =
      static_cast<const mdl::IExpression_direct_call *>(expression);
  const char *name = call->get_definition();
  mi::base::Handle<const mdl::IFunction_definition> definition(
      name ? transaction_->access<mdl::IFunction_definition>(name) : nullptr);
  const char *mdl_name = definition ? definition->get_mdl_name() : nullptr;
  if (!mdl_name)
    return {};
  const std::string function(mdl_name);
  if (function.find("::base::texture_coordinate_info(") == 0) {
    MxExpression result =
        translate(argument(call, u ? "tangent_u" : "tangent_v").get());
    return result.type == "vector3" ? result : MxExpression{};
  }
  if (function.find("::base::coordinate_source(") == 0) {
    if (enum_name(argument(call, "coordinate_system").get()) !=
        "texture_coordinate_uvw")
      return {};
    MxExpression index = translate(argument(call, "texture_space").get());
    if (!index.valid() || index.type != "integer")
      return {};
    const std::string kind = u ? "tangent" : "bitangent";
    mx::NodePtr node =
        graph_->addNode(kind, next_name("uv_" + kind), "vector3");
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
  mi::base::Handle<const mdl::IExpression> coordinate =
      argument(call, "coordinate");
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
  MxExpression zero =
      make_constant_expression("float", "0.0", "uv_tangent_zero");
  MxExpression neg_m01 =
      make_binary_expression("subtract", "float", zero, m01, "uv_tangent_neg");
  mx::NodePtr components = graph_->addNode(
      "combine2", next_name("uv_tangent_components"), "vector2");
  add_node_inputs_from_definition(components);
  if (!connect_expression(m11, components->getInput("in1"), report_, material_,
                          "UV tangent") ||
      !connect_expression(neg_m01, components->getInput("in2"), report_,
                          material_, "UV tangent"))
    return {};
  mx::NodePtr normalized = graph_->addNode(
      "normalize", next_name("uv_tangent_normalize"), "vector2");
  add_node_inputs_from_definition(normalized);
  if (!connect_expression({components, {}, "vector2"},
                          normalized->getInput("in"), report_, material_,
                          "UV tangent"))
    return {};
  report_.translated_nodes += 2;
  std::array<MxExpression, 2> c;
  for (mi::Size i = 0; i < c.size(); ++i) {
    mx::NodePtr extract =
        graph_->addNode("extract", next_name("uv_tangent_component"), "float");
    extract->setNodeDefString("ND_extract_vector2");
    add_node_inputs_from_definition(extract);
    extract->getInput("index")->setValueString(std::to_string(i));
    if (!connect_expression({normalized, {}, "vector2"},
                            extract->getInput("in"), report_, material_,
                            "UV tangent"))
      return {};
    c[i] = {extract, {}, "float"};
    ++report_.translated_nodes;
  }
  if (u) {
    MxExpression dot =
        make_binary_expression("add", "float",
                               make_binary_expression("multiply", "float", m00,
                                                      c[0], "uv_tangent_dot"),
                               make_binary_expression("multiply", "float", m10,
                                                      c[1], "uv_tangent_dot"),
                               "uv_tangent_dot");
    for (MxExpression &component : c) {
      MxExpression neg = make_binary_expression("subtract", "float", zero,
                                                component, "uv_tangent_neg");
      mx::NodePtr select =
          graph_->addNode("ifgreater", next_name("uv_tangent_flip"), "float");
      add_node_inputs_from_definition(select);
      if (!connect_expression(zero, select->getInput("value1"), report_,
                              material_, "UV tangent") ||
          !connect_expression(dot, select->getInput("value2"), report_,
                              material_, "UV tangent") ||
          !connect_expression(neg, select->getInput("in1"), report_, material_,
                              "UV tangent") ||
          !connect_expression(component, select->getInput("in2"), report_,
                              material_, "UV tangent"))
        return {};
      component = {select, {}, "float"};
      ++report_.translated_nodes;
    }
  } else {
    MxExpression x = make_binary_expression("subtract", "float", zero, c[1],
                                            "uv_tangent_neg");
    c[1] = c[0];
    c[0] = x;
  }
  const auto scaled = [&](const MxExpression &vector,
                          const MxExpression &weight) {
    mx::NodePtr node =
        graph_->addNode("multiply", next_name("uv_tangent_scaled"), "vector3");
    node->setNodeDefString("ND_multiply_vector3FA");
    add_node_inputs_from_definition(node);
    if (!connect_expression(vector, node->getInput("in1"), report_, material_,
                            "UV tangent") ||
        !connect_expression(weight, node->getInput("in2"), report_, material_,
                            "UV tangent"))
      return MxExpression{};
    ++report_.translated_nodes;
    return MxExpression{node, {}, "vector3"};
  };
  MxExpression result = make_binary_expression(
      "add", "vector3", scaled(tu, c[0]), scaled(tv, c[1]), "uv_tangent");
  MxExpression length_squared =
      make_binary_expression("add", "float",
                             make_binary_expression("multiply", "float", m01,
                                                    m01, "uv_tangent_length"),
                             make_binary_expression("multiply", "float", m11,
                                                    m11, "uv_tangent_length"),
                             "uv_tangent_length");
  mx::NodePtr select =
      graph_->addNode("ifgreater", next_name("uv_tangent_valid"), "vector3");
  add_node_inputs_from_definition(select);
  if (!connect_expression(length_squared, select->getInput("value1"), report_,
                          material_, "UV tangent") ||
      !connect_expression(zero, select->getInput("value2"), report_, material_,
                          "UV tangent") ||
      !connect_expression(result, select->getInput("in1"), report_, material_,
                          "UV tangent") ||
      !connect_expression(u ? tu : tv, select->getInput("in2"), report_,
                          material_, "UV tangent"))
    return {};
  ++report_.translated_nodes;
  return {select, {}, "vector3"};
}

MxExpression ExpressionTranslator::translate_texture_lookup(
    const mdl::IExpression_direct_call *expression, bool base_file_texture,
    const std::string &base_output_type) {
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
    resource_value =
        static_cast<const mdl::IExpression_constant *>(resource_expression)
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
  mi::base::Handle<const mdl::IType_texture> texture_type(texture->get_type());
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
        image_name ? transaction_->access<mdl::IImage>(image_name) : nullptr);
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
    report_.warn(material_ +
                 ": MDL texture file does not exist: " + texture_path.string());
    return {};
  }

  const std::string materialx_path = texture_packager_.package(texture_path);

  mi::base::Handle<const mdl::IType> expression_type(expression->get_type());
  const std::string output_type =
      base_file_texture ? base_output_type
                        : mdl_type_to_materialx(expression_type.get());
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
    report_.warn(material_ + ": MaterialX image node has no filename input.");
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
    report_.warn(material_ + ": MDL texture gamma " + format_float(gamma) +
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
    mx::NodePtr coordinates = graph_->addNode(
        "texcoord", next_name("texture_coordinates"), "vector2");
    coordinates->setNodeDefString("ND_texcoord_vector2");
    add_node_inputs_from_definition(coordinates);
    coordinates->getInput("index")->setValueString(std::to_string(index));
    image->getInput("texcoord")->setConnectedNode(coordinates);
    ++report_.translated_nodes;
  };
  MxExpression coordinates =
      base_file_texture ? texture_coordinates(coordinate_expression.get())
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

MxExpression ExpressionTranslator::translate_normal_texture(
    const mdl::IExpression_direct_call *call) {
  MxExpression image = translate_texture_lookup(call, true, "vector3");
  MxExpression factor = translate(argument(call, "factor").get());
  MxExpression flip_u = translate(argument(call, "flip_tangent_u").get());
  MxExpression flip_v = translate(argument(call, "flip_tangent_v").get());
  if (!image.valid() || !factor.valid() || factor.type != "float" ||
      !flip_u.valid() || flip_u.type != "boolean" || !flip_v.valid() ||
      flip_v.type != "boolean")
    return {};
  MxExpression zero = make_constant_expression("float", "0.0", "normal_zero");
  MxExpression negative =
      make_binary_expression("subtract", "float", zero, factor, "normal_flip");
  const auto select_scale = [&](const MxExpression &flip) -> MxExpression {
    mx::NodePtr select =
        graph_->addNode("ifequal", next_name("normal_scale"), "float");
    select->setNodeDefString("ND_ifequal_floatB");
    add_node_inputs_from_definition(select);
    select->getInput("value2")->setValueString("true");
    if (!connect_expression(flip, select->getInput("value1"), report_,
                            material_, "normal map flip") ||
        !connect_expression(negative, select->getInput("in1"), report_,
                            material_, "normal map flipped scale") ||
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
    mx::NodePtr extract =
        graph_->addNode("extract", next_name("normal_channel"), "float");
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
  MxExpression z =
      make_binary_expression("multiply", "float", channels[2], two, "normal_z");
  z = make_binary_expression("subtract", "float", z, one, "normal_z");
  z = make_binary_expression("multiply", "float", z, factor, "normal_z");
  if (!z.valid())
    return {};
  mx::NodePtr absolute =
      graph_->addNode("absval", next_name("normal_z_abs"), "float");
  add_node_inputs_from_definition(absolute);
  if (!connect_expression(z, absolute->getInput("in"), report_, material_,
                          "normal map Z"))
    return {};
  ++report_.translated_nodes;
  MxExpression baseline = make_binary_expression("subtract", "float", one,
                                                 factor, "normal_z_baseline");
  z = make_binary_expression("add", "float", {absolute, {}, "float"}, baseline,
                             "normal_z");
  z = make_binary_expression("add", "float", z, one, "normal_z");
  z = make_binary_expression("divide", "float", z, two, "normal_z");
  if (!z.valid())
    return {};
  mx::NodePtr encoded =
      graph_->addNode("combine3", next_name("normal_encoded"), "vector3");
  add_node_inputs_from_definition(encoded);
  if (!connect_expression(channels[0], encoded->getInput("in1"), report_,
                          material_, "normal map X") ||
      !connect_expression(channels[1], encoded->getInput("in2"), report_,
                          material_, "normal map Y") ||
      !connect_expression(z, encoded->getInput("in3"), report_, material_,
                          "normal map Z"))
    return {};
  ++report_.translated_nodes;
  mx::NodePtr scale =
      graph_->addNode("combine2", next_name("normal_scale"), "vector2");
  add_node_inputs_from_definition(scale);
  if (!connect_expression(scale_u, scale->getInput("in1"), report_, material_,
                          "normal map U scale") ||
      !connect_expression(scale_v, scale->getInput("in2"), report_, material_,
                          "normal map V scale"))
    return {};
  mx::NodePtr normal =
      graph_->addNode("normalmap", next_name("normal_map"), "vector3");
  normal->setNodeDefString("ND_normalmap_vector2");
  add_node_inputs_from_definition(normal);
  mx::NodePtr direction =
      graph_->addNode("normal", next_name("normal_direction"), "vector3");
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
      mx::NodePtr fallback =
          graph_->addNode(name, next_name("normal_" + name), "vector3");
      fallback->setNodeDefString("ND_" + name + "_vector3");
      add_node_inputs_from_definition(fallback);
      fallback->getInput("space")->setValueString("world");
      tangent = {fallback, {}, "vector3"};
      ++report_.translated_nodes;
      report_.warn(material_ +
                   ": the normal map tangent could not be represented; "
                   "the default UV0 tangent is used.");
    }
    if (!connect_expression(tangent, normal->getInput(name), report_, material_,
                            "normal map tangent"))
      return {};
  }
  if (!connect_expression({encoded, {}, "vector3"}, normal->getInput("in"),
                          report_, material_, "normal map image") ||
      !connect_expression({scale, {}, "vector2"}, normal->getInput("scale"),
                          report_, material_, "normal map scale"))
    return {};
  report_.translated_nodes += 2;
  report_.warn(material_ +
               ": tangent-space normal texture was approximated with a "
               "MaterialX normalmap; mirrored wrapping, clipping and crop "
               "may differ.");
  return {normal, {}, "vector3"};
}

MxExpression ExpressionTranslator::translate_color_layers(
    const mdl::IExpression_direct_call *call, bool mono) {
  if (mono &&
      enum_name(argument(call, "mono_source").get()) != "mono_average") {
    report_.warn(material_ + ": blend_color_layers mono supports only "
                             "mono_average; the mono output was omitted.");
    return {};
  }
  mi::base::Handle<const mdl::IExpression> layers = argument(call, "layers");
  while (layers && layers->get_kind() == mdl::IExpression::EK_TEMPORARY)
    layers = compiled_->get_temporary(
        static_cast<const mdl::IExpression_temporary *>(layers.get())
            ->get_index());
  if (!layers || layers->get_kind() != mdl::IExpression::EK_DIRECT_CALL)
    return {};
  const auto *array =
      static_cast<const mdl::IExpression_direct_call *>(layers.get());
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
          static_cast<const mdl::IExpression_temporary *>(entry.get())
              ->get_index());
    if (!entry || entry->get_kind() != mdl::IExpression::EK_DIRECT_CALL)
      return {};
    const auto *layer =
        static_cast<const mdl::IExpression_direct_call *>(entry.get());
    const char *name = layer->get_definition();
    mi::base::Handle<const mdl::IFunction_definition> definition(
        name ? transaction_->access<mdl::IFunction_definition>(name) : nullptr);
    const char *mdl_name = definition ? definition->get_mdl_name() : nullptr;
    if (!mdl_name || std::string(mdl_name).find("::base::color_layer(") != 0)
      return {};
    const std::string mode = enum_name(argument(layer, "mode").get());
    if (mode != "color_layer_blend" && mode != "color_layer_add" &&
        mode != "color_layer_multiply" && mode != "color_layer_overlay") {
      report_.warn(material_ + ": unsupported blend_color_layers mode " +
                   (mode.empty() ? "<nonconstant>" : mode) + ".");
      return {};
    }
    MxExpression color = translate(argument(layer, "layer_color").get());
    MxExpression weight = translate(argument(layer, "weight").get());
    if (!color.valid() || color.type != "color3" || !weight.valid() ||
        weight.type != "float")
      return {};
    if (mode == "color_layer_overlay") {
      mx::NodePtr overlay = graph_->addNode(
          "overlay", next_name("color_layer_overlay"), "color3");
      overlay->setNodeDefString("ND_overlay_color3");
      add_node_inputs_from_definition(overlay);
      if (!connect_expression(result, overlay->getInput("bg"), report_,
                              material_, "color layer base") ||
          !connect_expression(color, overlay->getInput("fg"), report_,
                              material_, "color layer color") ||
          !connect_expression(weight, overlay->getInput("mix"), report_,
                              material_, "color layer weight"))
        return {};
      ++report_.translated_nodes;
      result = {overlay, {}, "color3"};
      continue;
    }
    if (mode == "color_layer_add")
      color = make_binary_expression("add", "color3", result, color,
                                     "color_layer_add");
    else if (mode == "color_layer_multiply")
      color = make_binary_expression("multiply", "color3", result, color,
                                     "color_layer_multiply");
    if (!color.valid())
      return {};
    mx::NodePtr mix =
        graph_->addNode("mix", next_name("color_layer"), "color3");
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

MxExpression ExpressionTranslator::translate_perlin_texture(
    const mdl::IExpression_direct_call *call, bool mono) {
  const auto constant = [&](const char *name) {
    mi::base::Handle<const mdl::IExpression> expression = argument(call, name);
    while (expression &&
           expression->get_kind() == mdl::IExpression::EK_TEMPORARY)
      expression = compiled_->get_temporary(
          static_cast<const mdl::IExpression_temporary *>(expression.get())
              ->get_index());
    return mi::base::Handle<const mdl::IValue>(
        expression && expression->get_kind() == mdl::IExpression::EK_CONSTANT
            ? static_cast<const mdl::IExpression_constant *>(expression.get())
                  ->get_value()
            : nullptr);
  };
  const auto is_zero = [&](const char *name) {
    auto value = constant(name);
    return value && is_neutral_value(value.get());
  };
  auto levels_value = constant("noise_levels");
  auto size_value = constant("size");
  auto absolute_value = constant("absolute_noise");
  auto bands_value = constant("noise_bands");
  if (mono || !levels_value ||
      levels_value->get_kind() != mdl::IValue::VK_INT || !size_value ||
      size_value->get_kind() != mdl::IValue::VK_FLOAT || !absolute_value ||
      absolute_value->get_kind() != mdl::IValue::VK_BOOL || !bands_value ||
      bands_value->get_kind() != mdl::IValue::VK_FLOAT ||
      static_cast<const mdl::IValue_float *>(bands_value.get())->get_value() !=
          1.0f ||
      !is_zero("noise_phase") || !is_zero("noise_distortion") ||
      !is_zero("ridged_noise") || !is_zero("apply_marble") ||
      !is_zero("apply_dent")) {
    report_.warn(
        material_ +
        ": base::perlin_noise_texture has unsupported "
        "dynamic or modified noise parameters; its output was omitted.");
    return {};
  }
  const int levels =
      static_cast<const mdl::IValue_int *>(levels_value.get())->get_value();
  const float size =
      static_cast<const mdl::IValue_float *>(size_value.get())->get_value();
  const bool absolute =
      static_cast<const mdl::IValue_bool *>(absolute_value.get())->get_value();
  if (levels < 0 || levels > 8) {
    report_.warn(material_ +
                 ": base::perlin_noise_texture octave count is unsupported.");
    return {};
  }
  MxExpression color1 = translate(argument(call, "color1").get());
  MxExpression color2 = translate(argument(call, "color2").get());
  if (!color1.valid() || !color2.valid())
    return {};
  if (size == 0.0f)
    return color1;
  if (!std::isfinite(size) ||
      (levels > 0 && !std::isfinite(std::ldexp(1.0f, levels - 1) / size))) {
    report_.warn(material_ + ": base::perlin_noise_texture size produces "
                             "non-finite coordinates; its output was omitted.");
    return {};
  }
  MxExpression position = texture_coordinates(argument(call, "uvw").get());
  MxExpression low = translate(argument(call, "noise_threshold_low").get());
  MxExpression high = translate(argument(call, "noise_threshold_high").get());
  if (!position.valid() || !low.valid() || !high.valid()) {
    report_.warn(material_ +
                 ": base::perlin_noise_texture coordinates or thresholds "
                 "could not be represented.");
    return {};
  }
  MxExpression sum = make_constant_expression("float", "0.0", "noise_sum");
  for (int octave = 0; octave < levels; ++octave) {
    const float scale = std::ldexp(1.0f, octave) / size;
    const std::string factor = format_float(scale);
    MxExpression vector = make_constant_expression(
        "vector3", factor + ", " + factor + ", " + factor, "noise_scale");
    MxExpression point = make_binary_expression("multiply", "vector3", position,
                                                vector, "noise_position");
    mx::NodePtr noise =
        graph_->addNode("noise3d", next_name("perlin_noise"), "float");
    noise->setNodeDefString("ND_noise3d_float");
    add_node_inputs_from_definition(noise);
    if (!point.valid() ||
        !connect_expression(point, noise->getInput("position"), report_,
                            material_, "noise position"))
      return {};
    ++report_.translated_nodes;
    MxExpression value{noise, {}, "float"};
    if (absolute) {
      mx::NodePtr absolute_node =
          graph_->addNode("absval", next_name("noise_abs"), "float");
      add_node_inputs_from_definition(absolute_node);
      if (!connect_expression(value, absolute_node->getInput("in"), report_,
                              material_, "absolute noise"))
        return {};
      ++report_.translated_nodes;
      value = {absolute_node, {}, "float"};
    }
    MxExpression weight = make_constant_expression(
        "float", format_float(std::ldexp(1.0f, -octave)), "noise_weight");
    MxExpression weighted = make_binary_expression("multiply", "float", value,
                                                   weight, "noise_octave");
    sum = make_binary_expression("add", "float", sum, weighted, "noise_sum");
    if (!sum.valid())
      return {};
  }
  MxExpression one = make_constant_expression("float", "1.0", "noise_one");
  MxExpression zero = make_constant_expression("float", "0.0", "noise_zero");
  if (!absolute) {
    MxExpression half = make_constant_expression("float", "0.5", "noise_half");
    sum = make_binary_expression(
        "add", "float",
        make_binary_expression("multiply", "float", sum, half, "noise_signed"),
        half, "noise_signed");
  }
  MxExpression range =
      make_binary_expression("subtract", "float", high, low, "noise_range");
  MxExpression safe_range =
      select_float(high, low, range, one, "noise_safe_range");
  MxExpression normalized = make_binary_expression(
      "divide", "float",
      make_binary_expression("subtract", "float", sum, low, "noise_threshold"),
      safe_range, "noise_threshold");
  MxExpression clamped = make_binary_expression(
      "min", "float",
      make_binary_expression("max", "float", normalized, zero, "noise_clamp"),
      one, "noise_clamp");
  MxExpression amount =
      select_float(high, low, clamped, sum, "noise_threshold_select");
  MxExpression color = mix_expressions(color1, color2, amount, "perlin_color");
  if (!color.valid())
    return {};
  report_.warn(material_ +
               ": base::perlin_noise_texture was approximated with "
               "MaterialX 3D noise; its pattern differs from MDL's 4D noise.");
  return color;
}

MxExpression ExpressionTranslator::translate_file_texture_field(
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
  if (mdl_name &&
      std::string(mdl_name).find("::base::blend_color_layers(") == 0)
    return translate_color_layers(call, mono);
  if (mdl_name &&
      std::string(mdl_name).find("::base::perlin_noise_texture(") == 0)
    return translate_perlin_texture(call, mono);
  if (!mdl_name || std::string(mdl_name).find("::base::file_texture(") != 0) {
    report_.warn(material_ + ": unsupported texture_return source " +
                 (mdl_name ? mdl_name : "<unresolved>") + ".");
    return {};
  }

  if (mono &&
      enum_name(argument(call, "mono_source").get()) != "mono_average") {
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
  MxExpression scaled = make_binary_expression("multiply", "color3", image,
                                               scale, "file_texture_scale");
  if (!scaled.valid())
    return {};
  MxExpression color = make_binary_expression("add", "color3", scaled, offset,
                                              "file_texture_offset");
  if (!color.valid())
    return {};
  report_.warn(material_ +
               ": base::file_texture crop, clip, animation and coordinate "
               "transforms may differ.");
  return mono ? scalarize_color(color, "file_texture_average") : color;
}

} // namespace matconv::detail::mdl_translation
