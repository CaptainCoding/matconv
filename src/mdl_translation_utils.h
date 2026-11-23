#pragma once

#include <MaterialXCore/Document.h>
#include <MaterialXCore/Node.h>

#include <mi/base/handle.h>
#include <mi/neuraylib/icompiled_material.h>
#include <mi/neuraylib/iexpression.h>
#include <mi/neuraylib/ifunction_definition.h>
#include <mi/neuraylib/itransaction.h>
#include <mi/neuraylib/ivalue.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace matconv::detail::mdl_translation {

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
                        const mx::InputPtr &input, ConversionReport &report,
                        const std::string &material, const std::string &slot);
std::string mdl_type_to_materialx(const mdl::IType *type);
std::string format_float(float value);
bool serialize_value(const mdl::IValue *value, std::string &type,
                     std::string &text);
std::string unique_element_name(const mx::ElementPtr &parent,
                                const std::string &base);
void add_node_inputs_from_definition(const mx::NodePtr &node);
bool is_neutral_value(const mdl::IValue *value);
bool is_neutral_expression(const mdl::IExpression *expression);
bool is_unit_ior_expression(const mdl::IExpression *expression,
                            const mdl::ICompiled_material *compiled);
bool is_state_normal_expression(const mdl::IExpression *expression,
                                const mdl::ICompiled_material *compiled,
                                mdl::ITransaction *transaction);
const char *math_node_category(mdl::IFunction_definition::Semantics semantic);

} // namespace matconv::detail::mdl_translation
