#pragma once

#include "mdl_translation_utils.h"

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mi::neuraylib {
class IMdl_factory;
class IMdl_impexp_api;
class IScope;
}

namespace matconv::detail {
class TextureResourcePackager;
}

namespace matconv::detail::mdl_translation {

struct ThinFilmParameters {
  MxExpression thickness;
  MxExpression ior;
  MxExpression scalar_ior;
  std::size_t scope_id = 0;
  bool applied = false;
};

class ExpressionTranslator {
public:
  ExpressionTranslator(const mdl::ICompiled_material *compiled,
                       mdl::ITransaction *transaction,
                       const mx::NodeGraphPtr &graph, ConversionReport &report,
                       const std::string &material,
                       TextureResourcePackager &texture_packager,
                       std::filesystem::path resource_directory,
                       mdl::IMdl_factory *factory, mdl::IMdl_impexp_api *impexp,
                       mdl::IScope *scope);

  MxExpression translate(const mdl::IExpression *expression);

  MxExpression scalarize_color(const MxExpression &expression,
                               const std::string &prefix);

  bool approximate_bsdf(const mdl::IExpression *expression,
                        const mx::NodePtr &surface,
                        const MxExpression &layer_weight = {},
                        const MxExpression &tint_override = {},
                        const MxExpression &transmission_override = {});

private:
  mi::base::Handle<const mdl::IExpression>
  argument(const mdl::IExpression_direct_call *call,
           const std::string &name) const;

  bool connect_preview_input(const MxExpression &expression,
                             const mx::NodePtr &surface,
                             const std::string &input_name,
                             const std::string &slot);

  void set_preview_value(const mx::NodePtr &surface,
                         const std::string &input_name,
                         const std::string &value);

  MxExpression input_expression(const mx::InputPtr &input);

  bool approximate_weighted_subtree(const mdl::IExpression *expression,
                                    const mx::NodePtr &surface,
                                    const MxExpression &weight,
                                    const MxExpression &tint_override,
                                    const MxExpression &transmission_override);

  bool approximate_conditional_bsdf(const mdl::IExpression_direct_call *call,
                                    const mx::NodePtr &surface,
                                    const MxExpression &layer_weight,
                                    const MxExpression &tint_override,
                                    const MxExpression &transmission_override);

  void set_diffuse_fallback(const mx::NodePtr &surface,
                            const std::string &reason);

  MxExpression make_constant_expression(const std::string &type,
                                        const std::string &value,
                                        const std::string &prefix);

  MxExpression make_binary_expression(const std::string &category,
                                      const std::string &type,
                                      const MxExpression &left,
                                      const MxExpression &right,
                                      const std::string &prefix);

  MxExpression multiply_colors(const MxExpression &left,
                               const MxExpression &right,
                               const std::string &prefix);

  MxExpression mix_expressions(const MxExpression &base,
                               const MxExpression &layer,
                               const MxExpression &weight,
                               const std::string &name);

  MxExpression make_logical_expression(const std::string &category,
                                       const MxExpression &left,
                                       const MxExpression &right = {});

  MxExpression standard_surface_roughness(const MxExpression &roughness);

  bool same_expression(const MxExpression &left, const MxExpression &right);

  MxExpression select_float(const MxExpression &left, const MxExpression &right,
                            const MxExpression &greater,
                            const MxExpression &otherwise,
                            const std::string &name);

  MxExpression select_vector3(const MxExpression &left,
                              const MxExpression &right,
                              const MxExpression &greater,
                              const MxExpression &otherwise,
                              const std::string &name);

  MxExpression standard_surface_microfacet_roughness(
      const mdl::IExpression_direct_call *call, const mx::NodePtr &surface,
      const std::string &lobe = "specular");

  MxExpression translate_roughness(const mdl::IExpression *expression);

  bool approximate_diffuse(const mdl::IExpression_direct_call *call,
                           const mx::NodePtr &surface,
                           const MxExpression &layer_weight,
                           const MxExpression &tint_override);

  bool approximate_microfacet(const mdl::IExpression_direct_call *call,
                              const mx::NodePtr &surface,
                              const MxExpression &layer_weight,
                              const MxExpression &tint_override,
                              const MxExpression &transmission_override,
                              bool smooth_specular = false,
                              bool simple_glossy = false);

  bool approximate_specular(const mdl::IExpression_direct_call *call,
                            const mx::NodePtr &surface,
                            const MxExpression &layer_weight,
                            const MxExpression &tint_override,
                            const MxExpression &transmission_override);

  bool approximate_sheen(const mdl::IExpression_direct_call *call,
                         const mx::NodePtr &surface,
                         const MxExpression &layer_weight,
                         const MxExpression &tint_override,
                         const MxExpression &transmission_override);

  bool approximate_conductor(const mdl::IExpression_direct_call *call,
                             const mx::NodePtr &surface,
                             const MxExpression &tint_override);

  bool approximate_thin_film(const mdl::IExpression_direct_call *call,
                             const mx::NodePtr &surface,
                             const MxExpression &tint_override,
                             const MxExpression &transmission_override);

  void apply_layer_normal(const mdl::IExpression_direct_call *call,
                          const mx::NodePtr &surface, bool coat = false);

  bool is_reflective_ggx_tree(const mdl::IExpression *expression,
                              bool allow_weighted = true);

  bool approximate_layer(const mdl::IExpression_direct_call *call,
                         const mx::NodePtr &surface,
                         const MxExpression &tint_override,
                         const MxExpression &transmission_override,
                         mdl::IFunction_definition::Semantics semantic);

  MxExpression make_constant(const mdl::IValue *value,
                             const std::string &prefix);

  MxExpression translate_constant(const mdl::IExpression_constant *expression);

  MxExpression
  translate_parameter(const mdl::IExpression_parameter *expression);

  MxExpression
  translate_temporary(const mdl::IExpression_temporary *expression);

  MxExpression
  translate_thin_film(const mdl::IExpression_direct_call *expression);

  void apply_thin_film(const mx::NodePtr &bsdf);

  MxExpression translate_tint(const mdl::IExpression_direct_call *expression);

  std::string enum_name(const mdl::IExpression *expression);

  MxExpression
  translate_microfacet(const mdl::IExpression_direct_call *expression);

  MxExpression
  translate_fresnel_factor(const mdl::IExpression_direct_call *expression);

  std::optional<mi::Sint32>
  texture_coordinate_index(const mdl::IExpression *expression);

  struct FunctionScope {
    const mdl::IFunction_definition *definition;
    mi::base::Handle<const mdl::IExpression_list> arguments;
    FunctionScope *parent;
    mdl::ITransaction *transaction;
    const mdl::ICompiled_material *compiled = nullptr;
    std::vector<mi::Size> parameter_indices;
    std::map<std::pair<mi::Size, std::size_t>, MxExpression> temporaries;
  };

  class ScopeGuard {
  public:
    ScopeGuard(FunctionScope *&active, FunctionScope *scope)
        : active_(active), previous_(active) {
      active_ = scope;
    }
    ~ScopeGuard() { active_ = previous_; }
    ScopeGuard(const ScopeGuard &) = delete;
    ScopeGuard &operator=(const ScopeGuard &) = delete;

  private:
    FunctionScope *&active_;
    FunctionScope *previous_;
  };

  class TransactionGuard {
  public:
    TransactionGuard(mdl::ITransaction *&active, mdl::ITransaction *transaction)
        : active_(active), previous_(active) {
      active_ = transaction;
    }
    ~TransactionGuard() { active_ = previous_; }
    TransactionGuard(const TransactionGuard &) = delete;
    TransactionGuard &operator=(const TransactionGuard &) = delete;

  private:
    mdl::ITransaction *&active_;
    mdl::ITransaction *previous_;
  };

  template <typename F>
  auto with_bound_argument(const mdl::IExpression_parameter *parameter, F f)
      -> decltype(f(static_cast<const mdl::IExpression *>(nullptr))) {
    if (!function_scope_ || !function_scope_->arguments)
      return {};
    mi::Size index = parameter->get_index();
    if (function_scope_->compiled) {
      if (index >= function_scope_->parameter_indices.size())
        return {};
      index = function_scope_->parameter_indices[index];
    }
    const char *name = function_scope_->definition->get_parameter_name(index);
    if (!name)
      return {};
    mi::base::Handle<const mdl::IExpression> argument(
        function_scope_->arguments->get_expression(name));
    if (!argument)
      return {};
    FunctionScope *parent = function_scope_->parent;
    TransactionGuard transaction_guard(
        transaction_, parent ? parent->transaction : original_transaction_);
    ScopeGuard guard(function_scope_, parent);
    return f(argument.get());
  }

  template <typename F>
  auto with_function_body(const mdl::IExpression_direct_call *call,
                          const mdl::IFunction_definition *definition, F f)
      -> decltype(f(static_cast<const mdl::IExpression *>(nullptr))) {
    if (!definition ||
        definition->get_semantic() != mdl::IFunction_definition::DS_UNKNOWN)
      return {};
    const char *name = definition->get_mdl_name();
    if (!name)
      return {};
    for (FunctionScope *scope = function_scope_; scope; scope = scope->parent) {
      const char *active = scope->definition->get_mdl_name();
      if (active && std::string(active) == name)
        return {};
    }
    mi::base::Handle<const mdl::IExpression> body(definition->get_body());
    mi::base::Handle<const mdl::IExpression_list> arguments(call->get_arguments());
    if (!body || !arguments)
      return {};
    FunctionScope scope{definition, arguments, function_scope_, transaction_,
                        nullptr, {}, {}};
    ScopeGuard guard(function_scope_, &scope);
    return f(body.get());
  }

  mi::base::Handle<const mdl::IExpression> resolve_temporary(mi::Size index);

  mi::base::Handle<const mdl::IValue>
  resolve_texture_resource(const mdl::IExpression *expression);

  MxExpression translate_uv_function(
      const mdl::IExpression_direct_call *call,
      const mdl::IFunction_definition *definition, bool tangent, bool u);

  MxExpression inline_uv_function(
      const mdl::IExpression_direct_call *call,
      const mdl::IFunction_definition *definition, bool tangent, bool u);

  MxExpression matrix_component(const mdl::IExpression *expression,
                                mi::Size column, mi::Size row);

  MxExpression matrix_column(const mdl::IExpression *expression,
                             mi::Size column);

  MxExpression matrix_vector_product(const mdl::IExpression *matrix,
                                     const mdl::IExpression *vector);

  MxExpression conditional_texture_coordinates(
      const mdl::IExpression_direct_call *call, bool tangent, bool u);

  MxExpression texture_coordinates(const mdl::IExpression *expression);

  MxExpression texture_tangent(const mdl::IExpression *expression, bool u);

  MxExpression
  translate_texture_lookup(const mdl::IExpression_direct_call *expression,
                           bool base_file_texture = false,
                           const std::string &base_output_type = "color3");

  MxExpression
  translate_normal_texture(const mdl::IExpression_direct_call *call);

  MxExpression translate_color_layers(const mdl::IExpression_direct_call *call,
                                      bool mono);

  MxExpression
  translate_perlin_texture(const mdl::IExpression_direct_call *call, bool mono);

  MxExpression
  translate_file_texture_field(const mdl::IExpression_direct_call *field,
                               bool mono);

  MxExpression translate_texture_return_field(const mdl::IExpression *source,
                                               bool mono);

  MxExpression conditional_expression(const MxExpression &condition,
                                      const MxExpression &yes,
                                      const MxExpression &no);

  MxExpression
  translate_weighted_layer(const mdl::IExpression_direct_call *expression);

  MxExpression
  translate_color_lerp(const mdl::IExpression_direct_call *expression);

  MxExpression
  translate_microfacet_roughness(const mdl::IExpression *expression);

  MxExpression translate_call(const mdl::IExpression_direct_call *expression);

  mx::InputPtr diffuse_input(const mx::NodePtr &node,
                             const std::string &argument_name);

  mx::InputPtr math_input(const mx::NodePtr &node, mi::Size index,
                          mdl::IFunction_definition::Semantics semantic);

  std::string next_name(const std::string &prefix);

  const mdl::ICompiled_material *compiled_;
  mdl::ITransaction *transaction_;
  mdl::ITransaction *original_transaction_;
  mdl::IMdl_factory *factory_;
  mdl::IMdl_impexp_api *impexp_;
  mdl::IScope *scope_;
  mx::NodeGraphPtr graph_;
  ConversionReport &report_;
  std::string material_;
  TextureResourcePackager &texture_packager_;
  std::filesystem::path resource_directory_;
  mi::Size node_index_ = 0;
  std::size_t strict_uv_depth_ = 0;
  FunctionScope *function_scope_ = nullptr;
  std::map<mi::Size, std::string> parameter_names_;
  std::set<mi::Size> warned_enum_parameters_;
  std::map<std::pair<mi::Size, std::size_t>, MxExpression> temporaries_;
  std::vector<ThinFilmParameters> thin_films_;
  std::size_t next_thin_film_scope_id_ = 1;
};

} // namespace matconv::detail::mdl_translation
