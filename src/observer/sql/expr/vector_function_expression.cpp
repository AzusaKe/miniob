/* Copyright (c) 2026 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/expr/vector_function_expression.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <strings.h>

#include "common/type/vector_type.h"

namespace {
bool valid_metric(const string &metric)
{
  return strcasecmp(metric.c_str(), "COSINE") == 0 || strcasecmp(metric.c_str(), "EUCLIDEAN") == 0 ||
         strcasecmp(metric.c_str(), "DOT") == 0;
}
}  // namespace

AttrType VectorFunctionExpr::value_type() const
{
  switch (function_type_) {
    case Type::STRING_TO_VECTOR: return AttrType::VECTORS;
    case Type::VECTOR_TO_STRING: return AttrType::CHARS;
    case Type::DISTANCE: return AttrType::FLOATS;
  }
  return AttrType::UNDEFINED;
}

int VectorFunctionExpr::value_length() const
{
  if (function_type_ == Type::DISTANCE) {
    return sizeof(float);
  }
  return -1;
}

unique_ptr<Expression> VectorFunctionExpr::copy() const
{
  vector<unique_ptr<Expression>> children;
  for (const auto &child : children_) {
    children.emplace_back(child->copy());
  }
  auto result = make_unique<VectorFunctionExpr>(function_type_, std::move(children));
  result->set_name(name());
  result->set_pos(pos());
  if (has_alias()) {
    result->set_alias(name());
  }
  return result;
}

bool VectorFunctionExpr::equal(const Expression &other) const
{
  if (other.type() != ExprType::VECTOR_FUNCTION) {
    return false;
  }
  const auto &function = static_cast<const VectorFunctionExpr &>(other);
  if (function_type_ != function.function_type_ || children_.size() != function.children_.size()) {
    return false;
  }
  for (size_t i = 0; i < children_.size(); i++) {
    if (!children_[i]->equal(*function.children_[i])) {
      return false;
    }
  }
  return true;
}

RC VectorFunctionExpr::get_value(const Tuple &tuple, Value &value) const
{
  vector<Value> arguments(children_.size());
  for (size_t i = 0; i < children_.size(); i++) {
    RC rc = children_[i]->get_value(tuple, arguments[i]);
    if (rc != RC::SUCCESS) {
      return rc;
    }
  }
  return evaluate(arguments, value);
}

RC VectorFunctionExpr::try_get_value(Value &value) const
{
  vector<Value> arguments(children_.size());
  for (size_t i = 0; i < children_.size(); i++) {
    RC rc = children_[i]->try_get_value(arguments[i]);
    if (rc != RC::SUCCESS) {
      return rc;
    }
  }
  return evaluate(arguments, value);
}

RC VectorFunctionExpr::validate() const
{
  const bool distance = function_type_ == Type::DISTANCE;
  if (children_.size() != (distance ? 3U : 1U)) {
    return RC::INVALID_ARGUMENT;
  }
  const AttrType first_type = function_type_ == Type::STRING_TO_VECTOR ? AttrType::CHARS : AttrType::VECTORS;
  if (children_[0]->value_type() != first_type ||
      (distance && (children_[1]->value_type() != AttrType::VECTORS || children_[2]->value_type() != AttrType::CHARS))) {
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  }

  vector<Value> arguments(children_.size());
  bool all_constant = true;
  vector<bool> constant(children_.size(), false);
  for (size_t i = 0; i < children_.size(); i++) {
    RC rc = children_[i]->try_get_value(arguments[i]);
    if (rc == RC::SUCCESS) {
      constant[i] = true;
    } else if (rc == RC::UNIMPLEMENTED) {
      all_constant = false;
    } else {
      return rc;
    }
  }
  if (distance) {
    if (constant[2] && !valid_metric(arguments[2].get_string())) {
      return RC::INVALID_ARGUMENT;
    }
    if (constant[0] && constant[1] && arguments[0].get_vector().size() != arguments[1].get_vector().size()) {
      return RC::INVALID_ARGUMENT;
    }
  }
  if (all_constant) {
    Value ignored;
    return evaluate(arguments, ignored);
  }
  return RC::SUCCESS;
}

RC VectorFunctionExpr::evaluate(const vector<Value> &arguments, Value &value) const
{
  if (function_type_ == Type::STRING_TO_VECTOR) {
    if (arguments.size() != 1 || arguments[0].attr_type() != AttrType::CHARS) {
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    }
    return VectorType().set_value_from_str(value, arguments[0].get_string());
  }
  if (function_type_ == Type::VECTOR_TO_STRING) {
    if (arguments.size() != 1 || arguments[0].attr_type() != AttrType::VECTORS) {
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    }
    string text;
    RC rc = VectorType().to_string(arguments[0], text);
    if (rc == RC::SUCCESS) {
      value.set_string(text.c_str());
    }
    return rc;
  }
  if (arguments.size() != 3 || arguments[0].attr_type() != AttrType::VECTORS ||
      arguments[1].attr_type() != AttrType::VECTORS || arguments[2].attr_type() != AttrType::CHARS) {
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  }
  const auto &left = arguments[0].get_vector();
  const auto &right = arguments[1].get_vector();
  const string metric = arguments[2].get_string();
  if (left.empty() || left.size() != right.size() || !valid_metric(metric)) {
    return RC::INVALID_ARGUMENT;
  }
  double dot = 0;
  double left_norm = 0;
  double right_norm = 0;
  double squared_distance = 0;
  for (size_t i = 0; i < left.size(); i++) {
    const double a = left[i];
    const double b = right[i];
    dot += a * b;
    left_norm += a * a;
    right_norm += b * b;
    squared_distance += (a - b) * (a - b);
  }
  double result = dot;
  if (strcasecmp(metric.c_str(), "EUCLIDEAN") == 0) {
    result = sqrt(squared_distance);
  } else if (strcasecmp(metric.c_str(), "COSINE") == 0) {
    if (left_norm == 0 || right_norm == 0) {
      return RC::INVALID_ARGUMENT;
    }
    result = 1.0 - std::clamp(dot / sqrt(left_norm * right_norm), -1.0, 1.0);
  }
  if (!std::isfinite(result) || std::abs(result) > std::numeric_limits<float>::max()) {
    return RC::INVALID_ARGUMENT;
  }
  value.set_float(static_cast<float>(result));
  return RC::SUCCESS;
}
