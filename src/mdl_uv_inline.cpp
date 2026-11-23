#include "mdl_expression_translator.h"

#include <mi/neuraylib/ifunction_call.h>
#include <mi/neuraylib/imaterial_instance.h>
#include <mi/neuraylib/imdl_execution_context.h>
#include <mi/neuraylib/imdl_factory.h>
#include <mi/neuraylib/imdl_impexp_api.h>
#include <mi/neuraylib/imdl_module_builder.h>
#include <mi/neuraylib/imodule.h>
#include <mi/neuraylib/iscope.h>
#include <mi/neuraylib/itype.h>
#include <mi/neuraylib/ivalue.h>

#include <stdexcept>
#include <string>

namespace matconv::detail::mdl_translation {

namespace {

bool supported_probe_type(const mdl::IType *type) {
  mi::base::Handle<const mdl::IType> resolved(
      type ? type->skip_all_type_aliases() : nullptr);
  if (!resolved)
    return false;
  switch (resolved->get_kind()) {
  case mdl::IType::TK_BOOL:
  case mdl::IType::TK_INT:
  case mdl::IType::TK_FLOAT:
  case mdl::IType::TK_COLOR:
    return true;
  case mdl::IType::TK_VECTOR: {
    mi::base::Handle<const mdl::IType_vector> vector(
        resolved->get_interface<mdl::IType_vector>());
    mi::base::Handle<const mdl::IType_atomic> element(
        vector ? vector->get_element_type() : nullptr);
    return vector && element && element->get_kind() == mdl::IType::TK_FLOAT &&
           vector->get_size() >= 2 && vector->get_size() <= 4;
  }
  case mdl::IType::TK_MATRIX: {
    mi::base::Handle<const mdl::IType_matrix> matrix(
        resolved->get_interface<mdl::IType_matrix>());
    mi::base::Handle<const mdl::IType_vector> column(
        matrix ? matrix->get_element_type() : nullptr);
    mi::base::Handle<const mdl::IType_atomic> element(
        column ? column->get_element_type() : nullptr);
    return matrix && column && element && matrix->get_size() == 4 &&
           column->get_size() == 4 &&
           element->get_kind() == mdl::IType::TK_FLOAT;
  }
  case mdl::IType::TK_STRUCT: {
    mi::base::Handle<const mdl::IType_struct> structure(
        resolved->get_interface<mdl::IType_struct>());
    const char *name = structure ? structure->get_symbol() : nullptr;
    return name && std::string(name) == "::base::texture_coordinate_info";
  }
  default:
    return false;
  }
}

} // namespace

MxExpression ExpressionTranslator::inline_uv_function(
    const mdl::IExpression_direct_call *call,
    const mdl::IFunction_definition *definition, bool tangent, bool u) {
  const char *module_name = definition->get_mdl_module_name();
  const char *module_db_name = definition->get_module();
  const char *function_db_name = call->get_definition();
  const char *function_name = definition->get_mdl_name();
  if (!module_name || !module_db_name || !function_db_name || !function_name)
    return {};
  const std::string module(module_name);
  const std::string module_db(module_db_name);
  const std::string function_db(function_db_name);
  for (FunctionScope *active = function_scope_; active; active = active->parent) {
    const char *name = active->definition->get_mdl_name();
    if (name && std::string(name) == function_name)
      return {};
  }

  // The owner module is required for private functions; the separate
  // transaction prevents its temporary probe from changing the source module.
  mi::base::Handle<mdl::ITransaction> transaction(scope_->create_transaction());
  if (!transaction)
    throw std::runtime_error("Could not create an isolated MDL UV transaction.");
  struct AbortTransaction {
    mdl::ITransaction *transaction;
    ~AbortTransaction() {
      if (transaction->is_open())
        transaction->abort();
    }
  } abort_transaction{transaction.get()};

  MxExpression result = [&]() -> MxExpression {
    mi::base::Handle<mdl::IMdl_execution_context> context(
        factory_->create_execution_context());
    if (!context)
      throw std::runtime_error(
          "Could not create an MDL UV compilation context.");
    auto failure = [&](const std::string &operation) {
      std::string message = material_ + ": could not inline UV function " +
                            function_db + " (" + operation + ")";
      for (mi::Size i = 0; i < context->get_error_messages_count(); ++i) {
        mi::base::Handle<const mdl::IMessage> error(
            context->get_error_message(i));
        if (error && error->get_string())
          message += ": " + std::string(error->get_string());
      }
      report_.warn(message + ".");
      return MxExpression{};
    };
    if (impexp_->load_module(transaction.get(), module.c_str(), context.get()) <
        0)
      return failure("loading its module");
    mi::base::Handle<const mdl::IFunction_definition> isolated_definition(
        transaction->access<mdl::IFunction_definition>(function_db.c_str()));
    mi::base::Handle<const mdl::IModule> owner(
        transaction->access<mdl::IModule>(module_db.c_str()));
    mi::base::Handle<mdl::IType_factory> types(
        factory_->create_type_factory(transaction.get()));
    mi::base::Handle<mdl::IValue_factory> values(
        factory_->create_value_factory(transaction.get()));
    mi::base::Handle<mdl::IExpression_factory> expressions(
        factory_->create_expression_factory(transaction.get()));
    if (!isolated_definition || !owner || !types || !values || !expressions)
      return failure("accessing its definition");

    std::string probe = "matconv_uv_probe";
    for (mi::Size suffix = 0;; ++suffix) {
      const std::string prefix = module_db + "::" + probe + "(";
      bool exists = false;
      for (mi::Size i = 0; i < owner->get_material_count(); ++i) {
        const char *name = owner->get_material(i);
        if (name && std::string(name).find(prefix) == 0) {
          exists = true;
          break;
        }
      }
      if (!exists)
        break;
      probe = "matconv_uv_probe_" + std::to_string(suffix);
    }

    mi::base::Handle<const mdl::IType_list> original_types(
        isolated_definition->get_parameter_types());
    mi::base::Handle<const mdl::IType_list> caller_types(
        definition->get_parameter_types());
    mi::base::Handle<mdl::IType_list> parameters(types->create_type_list());
    mi::base::Handle<mdl::IExpression_list> defaults(
        expressions->create_expression_list());
    mi::base::Handle<mdl::IExpression_list> arguments(
        expressions->create_expression_list());
    if (!original_types || !caller_types || !parameters || !defaults ||
        !arguments || original_types->get_size() != caller_types->get_size())
      return failure("creating parameter lists");
    for (mi::Size i = 0; i < original_types->get_size(); ++i) {
      mi::base::Handle<const mdl::IType> type(original_types->get_type(i));
      mi::base::Handle<const mdl::IType> caller_type(caller_types->get_type(i));
      const char *argument_name = isolated_definition->get_parameter_name(i);
      const char *caller_name = definition->get_parameter_name(i);
      if (!type || !caller_type || !argument_name || !caller_name ||
          std::string(argument_name) != caller_name ||
          types->compare(type.get(), caller_type.get()) != 0)
        return failure("reading parameter types");
      if (!supported_probe_type(type.get()))
        return failure("unsupported parameter type for " +
                       std::string(argument_name));
      const std::string name = "p" + std::to_string(i);
      mi::base::Handle<mdl::IValue> value(values->create(type.get()));
      mi::base::Handle<mdl::IExpression> default_value(
          value ? expressions->create_constant(value.get()) : nullptr);
      mi::base::Handle<mdl::IExpression> parameter(
          expressions->create_parameter(type.get(), i));
      if (!default_value || !parameter ||
          parameters->add_type(name.c_str(), type.get()) != 0 ||
          defaults->add_expression(name.c_str(), default_value.get()) != 0 ||
          arguments->add_expression(argument_name, parameter.get()) != 0)
        return failure("binding parameters");
    }
    mi::Sint32 error = 0;
    mi::base::Handle<const mdl::IExpression> invocation(
        expressions->create_direct_call(function_db.c_str(), arguments.get(),
                                        &error));
    if (!invocation)
      return failure("constructing the function call, SDK error " +
                     std::to_string(error));
    const std::string field =
        tangent ? (u ? "tangent_u" : "tangent_v") : "position";
    mi::base::Handle<mdl::IExpression_list> field_arguments(
        expressions->create_expression_list());
    if (!field_arguments ||
        field_arguments->add_expression("s", invocation.get()) != 0)
      return failure("constructing the UV field call");
    const std::string field_function =
        "mdl::base::texture_coordinate_info." + field +
        "(::base::texture_coordinate_info)";
    mi::base::Handle<const mdl::IExpression> selected(
        expressions->create_direct_call(field_function.c_str(),
                                        field_arguments.get(), &error));
    mi::base::Handle<mdl::IExpression_list> geometry_arguments(
        expressions->create_expression_list());
    if (!selected || !geometry_arguments ||
        geometry_arguments->add_expression("normal", selected.get()) != 0)
      return failure("selecting UV " + field);
    mi::base::Handle<const mdl::IExpression> geometry(
        expressions->create_direct_call(
            "mdl::material_geometry(float3,float,float3)",
            geometry_arguments.get(), &error));
    mi::base::Handle<mdl::IExpression_list> material_arguments(
        expressions->create_expression_list());
    if (!geometry || !material_arguments ||
        material_arguments->add_expression("geometry", geometry.get()) != 0)
      return failure("constructing probe geometry");
    mi::base::Handle<const mdl::IExpression> body(expressions->create_direct_call(
        "mdl::material(bool,material_surface,material_surface,color,material_volume,"
        "material_geometry,hair_bsdf)", material_arguments.get(), &error));
    mi::base::Handle<mdl::IMdl_module_builder> builder(
        factory_->create_module_builder(
            transaction.get(), module_db.c_str(), mdl::MDL_VERSION_1_0,
            mdl::MDL_VERSION_LATEST, context.get()));
    if (!body || !builder ||
        builder->add_function(probe.c_str(), body.get(), nullptr, parameters.get(),
                              defaults.get(), nullptr, nullptr, nullptr, true,
                              true, mdl::IType::MK_NONE, context.get()) != 0)
      return failure("building the isolated material");

    owner = transaction->access<mdl::IModule>(module_db.c_str());
    std::string probe_db;
    const std::string prefix = module_db + "::" + probe + "(";
    for (mi::Size i = 0; owner && i < owner->get_material_count(); ++i) {
      const char *name = owner->get_material(i);
      if (name && std::string(name).find(prefix) == 0) {
        probe_db = name;
        break;
      }
    }
    mi::base::Handle<const mdl::IFunction_definition> probe_definition(
        probe_db.empty() ? nullptr
                         : transaction->access<mdl::IFunction_definition>(
                               probe_db.c_str()));
    mi::base::Handle<mdl::IFunction_call> function(
        probe_definition ? probe_definition->create_function_call(nullptr, &error)
                         : nullptr);
    mi::base::Handle<mdl::IMaterial_instance> instance(
        function ? function->get_interface<mdl::IMaterial_instance>()
                 : nullptr);
    if (!instance || context->set_option("ignore_noinline", true) != 0)
      return failure("creating the isolated material");
    mi::base::Handle<const mdl::ICompiled_material> compiled(
        instance->create_compiled_material(
            mdl::IMaterial_instance::CLASS_COMPILATION, context.get()));
    if (!compiled)
      return failure("compiling the isolated material");
    std::vector<mi::Size> indices;
    for (mi::Size i = 0; i < compiled->get_parameter_count(); ++i) {
      const char *name = compiled->get_parameter_name(i);
      mi::Size match = original_types->get_size();
      for (mi::Size j = 0; name && j < original_types->get_size(); ++j) {
        if (name == "p" + std::to_string(j)) {
          match = j;
          break;
        }
      }
      if (match == original_types->get_size())
        return failure("mapping compiled parameters");
      indices.push_back(match);
    }
    mi::base::Handle<const mdl::IExpression> output(
        compiled->lookup_sub_expression("geometry.normal"));
    mi::base::Handle<const mdl::IExpression_list> original_arguments(
        call->get_arguments());
    if (!output || !original_arguments)
      return failure("reading the compiled UV expression");
    FunctionScope local{definition, original_arguments, function_scope_,
                        transaction.get(), compiled.get(), std::move(indices),
                        {}};
    TransactionGuard transaction_guard(transaction_, transaction.get());
    ScopeGuard scope_guard(function_scope_, &local);
    struct StrictUvGuard {
      std::size_t &depth;
      explicit StrictUvGuard(std::size_t &value) : depth(value) { ++depth; }
      ~StrictUvGuard() { --depth; }
    } strict_guard(strict_uv_depth_);
    MxExpression translated = translate(output.get());
    return translated.valid() ? translated
                              : failure("translating the isolated UV " + field);
  }();
  transaction->abort();
  if (!compiled_->is_valid(nullptr))
    throw std::runtime_error(
        "The original MDL material became invalid after isolated UV compilation.");
  return result;
}

} // namespace matconv::detail::mdl_translation
