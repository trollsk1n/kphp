#include "compiler/data/lambda-generator.h"

#include <algorithm>
#include <functional>

#include "compiler/compiler-core.h"
#include "compiler/data/function-data.h"
#include "compiler/gentree.h"
#include "compiler/name-gen.h"
#include "compiler/utils/string-utils.h"
#include "compiler/vertex.h"

LambdaGenerator::LambdaGenerator(FunctionPtr function, const Location &location, bool is_static/* = false*/)
  : created_location(location)
  , lambda_class_name(create_name(gen_anonymous_function_name(function)))
{
  generated_lambda = create_class(lambda_class_name);
  generated_lambda->is_static = is_static || !function->is_instance_function();
}

LambdaGenerator &LambdaGenerator::add_uses(std::vector<VertexAdaptor<op_func_param>> uses) {
  kphp_assert_msg(generated_lambda, "lambda was already generated by this class");

  if (!generated_lambda->is_static) {
    auto implicit_captured_var_parent_this = VertexAdaptor<op_var>::create();
    implicit_captured_var_parent_this->set_string(LambdaClassData::get_parent_this_name());
    set_location(implicit_captured_var_parent_this, created_location);

    auto func_param = VertexAdaptor<op_func_param>::create(implicit_captured_var_parent_this);
    set_location(func_param, created_location);

    uses.insert(uses.begin(), func_param);
  }

  for (auto param_as_use : uses) {
    auto variable_in_use = VertexAdaptor<op_var>::create();
    variable_in_use->str_val = param_as_use->var()->get_string();
    set_location(variable_in_use, param_as_use->location);
    generated_lambda->members.add_instance_field(variable_in_use, {}, access_private, vk::string_view{});

    auto field = generated_lambda->members.get_instance_field(variable_in_use->get_string());
    field->var->marked_as_const = true;
  }

  this->uses = std::move(uses);

  return *this;
}

LambdaGenerator &LambdaGenerator::add_invoke_method(const VertexAdaptor<op_function> &function) {
  auto name = create_name("__invoke");
  auto params = create_invoke_params(function);
  auto cmd = create_invoke_cmd(function);
  auto invoke_function = VertexAdaptor<op_function>::create(name, params, cmd);
  set_location(invoke_function, created_location);

  auto invoke_fun = register_invoke_method(invoke_function);
  invoke_fun->has_variadic_param = function->get_func_id() && function->get_func_id()->has_variadic_param;

  return *this;
}

LambdaGenerator &LambdaGenerator::add_constructor_from_uses() {
  std::vector<VertexAdaptor<meta_op_func_param>> conv_uses(uses.begin(), uses.end());
  generated_lambda->create_constructor_with_args(created_location, conv_uses);
  generated_lambda->construct_function->is_template = !uses.empty();

  return *this;
}

LambdaGenerator &LambdaGenerator::add_invoke_method_which_call_method(FunctionPtr called_method) {
  generated_lambda->members.add_instance_field(get_var_of_captured_array_arg<op_var>(), {}, access_private, vk::string_view{});

  add_uses_for_captured_class_from_array();
  auto lambda_params = create_params_for_invoke_which_call_method(called_method);

  auto call_function = VertexAdaptor<op_func_call>::create(lambda_params);
  call_function->extra_type = op_ex_func_call_arrow;
  call_function->set_string(called_method->local_name());
  call_function->set_func_id(called_method);

  auto params_of_called_method = called_method->get_params();
  kphp_assert(!params_of_called_method.empty());
  auto params_without_arg_of_captured_class = VertexRange(std::next(params_of_called_method.begin()), params_of_called_method.end());

  return create_invoke_fun_returning_call(called_method, call_function, VertexAdaptor<op_func_param_list>::create(params_without_arg_of_captured_class));
}

LambdaGenerator &LambdaGenerator::add_invoke_method_which_call_function(FunctionPtr called_function) {
  auto lambda_params = called_function->get_params_as_vector_of_vars();
  auto call_function = VertexAdaptor<op_func_call>::create(lambda_params);

  call_function->set_string(called_function->name);
  call_function->set_func_id(called_function);
  return create_invoke_fun_returning_call(called_function, call_function, called_function->root->params());
}

LambdaPtr LambdaGenerator::generate_and_require(FunctionPtr parent_function, DataStream<FunctionPtr> &os) {
  auto lambda_class = generate(std::move(parent_function));

  auto invoke_method = lambda_class->members.get_instance_method("__invoke");
  kphp_assert(invoke_method && invoke_method->function && !invoke_method->function->is_required);
  G->require_function(invoke_method->function, os);

  auto constructor = lambda_class->construct_function;
  kphp_assert(constructor && !constructor->is_required);
  if (!constructor->is_lambda_with_uses()) {
    G->require_function(constructor, os);
  }

  G->register_and_require_function(lambda_class->gen_holder_function(lambda_class->name), os, true);

  return lambda_class;
}

LambdaPtr LambdaGenerator::generate(FunctionPtr parent_function) {
  kphp_assert(generated_lambda);
  generated_lambda->members.for_each([&parent_function](ClassMemberInstanceMethod &m) {
    m.function->function_in_which_lambda_was_created = parent_function;
  });

  G->register_class(generated_lambda);
  ++G->stats.total_lambdas;
  return std::move(generated_lambda);
}

VertexAdaptor<op_func_name> LambdaGenerator::create_name(const std::string &name) {
  auto res_name = VertexAdaptor<op_func_name>::create();
  res_name->set_string(name);
  set_location(res_name, created_location);

  return res_name;
}

LambdaPtr LambdaGenerator::create_class(VertexAdaptor<op_func_name> name) {
  LambdaPtr anon_class(new LambdaClassData());
  anon_class->set_name_and_src_name(LambdaClassData::get_lambda_namespace() + "\\" + name->get_string());

  return anon_class;
}

VertexAdaptor<op_seq> LambdaGenerator::create_invoke_cmd(VertexAdaptor<op_function> function) {
  VertexPtr new_cmd = function->cmd().clone();
  // if we didn't do it early
  if (!function->get_func_id() || !function->get_func_id()->function_in_which_lambda_was_created) {
    add_this_to_captured_variables(new_cmd);
  }
  return new_cmd.as<op_seq>();
}

VertexAdaptor<op_func_param_list> LambdaGenerator::create_invoke_params(VertexAdaptor<op_function> function) {
  std::vector<VertexAdaptor<meta_op_func_param>> func_parameters;
  generated_lambda->patch_func_add_this(func_parameters, created_location);
  auto params_range = get_function_params(function);
  auto params_begin = params_range.begin();
  auto params_end = params_range.end();
  if (function->get_func_id() && (function->get_func_id()->function_in_which_lambda_was_created || function->get_func_id()->is_lambda())) {
    kphp_assert(!params_range.empty());
    // skip $this parameter, which was added to `function` previously
    std::advance(params_begin, 1);
  }
  // need transformation for params_range to meta_op_func_param vertices, due to we don't have typed VertexRange
  std::transform(params_begin, params_end, std::back_inserter(func_parameters), std::mem_fn(&VertexPtr::as<meta_op_func_param>));

  // every parameter (excluding $this) could be any class_instance
  for (size_t i = 1, id = 0; i < func_parameters.size(); ++i) {
    auto param = func_parameters[i].as<op_func_param>();
    if (param->type_declaration == "callable") {
      param->template_type_id = static_cast<int>(id);
      param->is_callable = true;
      id++;
    } else if (param->type_declaration.empty()) {
      param->template_type_id = static_cast<int>(id);
      id++;
    }
  }

  auto params = VertexAdaptor<op_func_param_list>::create(func_parameters);
  params->location.line = function->params()->location.line;

  return params;
}

void LambdaGenerator::add_this_to_captured_variables(VertexPtr &root) {
  if (root->type() != op_var) {
    for (auto &v : *root) {
      add_this_to_captured_variables(v);
    }
    return;
  }

  if (generated_lambda->members.get_instance_field(root->get_string())) {
    auto inst_prop = VertexAdaptor<op_instance_prop>::create(ClassData::gen_vertex_this({}));
    inst_prop->location = root->location;
    inst_prop->str_val = root->get_string();
    root = inst_prop;
  } else if (!generated_lambda->is_static && root->get_string() == "this") {
    // replace `$this` with `$this->parent$this`
    auto new_root = VertexAdaptor<op_instance_prop>::create(root);
    new_root->set_string(LambdaClassData::get_parent_this_name());
    set_location(new_root, root->location);
    root = new_root;
  }
}

template<Operation op>
VertexAdaptor<op> LambdaGenerator::get_var_of_captured_array_arg() {
  auto var_of_captured_array_arg = VertexAdaptor<op>::create();
  var_of_captured_array_arg->set_string("captured_array_arg");
  set_location(created_location, var_of_captured_array_arg);

  return var_of_captured_array_arg;
}

void LambdaGenerator::add_uses_for_captured_class_from_array() {
  auto captured_class_from_array = get_var_of_captured_array_arg();
  auto func_param = VertexAdaptor<op_func_param>::create(captured_class_from_array);
  set_location(created_location, func_param);
  uses.emplace_back(func_param);
}

std::vector<VertexAdaptor<op_var>> LambdaGenerator::create_params_for_invoke_which_call_method(FunctionPtr called_method) {
  auto captured_class_from_array = get_var_of_captured_array_arg();
  auto lambda_params = called_method->get_params_as_vector_of_vars(1);
  lambda_params.insert(lambda_params.begin(), captured_class_from_array);

  return lambda_params;
}

FunctionPtr LambdaGenerator::register_invoke_method(VertexAdaptor<op_function> fun) {
  auto fun_name = fun->name()->get_string();
  fun->name()->set_string(replace_backslashes(generated_lambda->name) + "$$" + fun_name);
  auto invoke_function = FunctionData::create_function(fun, FunctionData::func_type_t::func_local);
  invoke_function->update_location_in_body();
  generated_lambda->members.add_instance_method(invoke_function, AccessType::access_public);

  auto params = invoke_function->get_params();
  invoke_function->is_template = generated_lambda->members.has_any_instance_var() || params.size() > 1;
  invoke_function->is_inline = true;

  //TODO: need set function_in_which_created for all lambdas inside
  //invoke_function->lambdas_inside = std::move(lambdas_inside);
  //for (auto &l : invoke_function->lambdas_inside) {
  //  l->function_in_which_lambda_was_created = invoke_function;
  //}

  G->register_function(invoke_function);

  return invoke_function;
}

LambdaGenerator &LambdaGenerator::create_invoke_fun_returning_call(FunctionPtr base_fun, VertexAdaptor<op_func_call> &call_function, VertexAdaptor<op_func_param_list> invoke_params) {
  auto return_call = VertexAdaptor<op_return>::create(call_function);
  auto lambda_body = VertexAdaptor<op_seq>::create(return_call);

  set_location(created_location, call_function, return_call, lambda_body);

  auto fun = VertexAdaptor<op_function>::create(lambda_class_name, invoke_params, lambda_body);
  fun->set_func_id(base_fun);
  add_invoke_method(fun);

  return *this;
}

