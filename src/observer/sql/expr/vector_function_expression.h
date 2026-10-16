/* Copyright (c) 2026 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include "sql/expr/expression.h"

class VectorFunctionExpr : public Expression
{
public:
  enum class Type { STRING_TO_VECTOR, VECTOR_TO_STRING, DISTANCE };

  VectorFunctionExpr(Type type, vector<unique_ptr<Expression>> &&children)
      : function_type_(type), children_(std::move(children)) {}

  ExprType type() const override { return ExprType::VECTOR_FUNCTION; }
  Type function_type() const { return function_type_; }
  AttrType value_type() const override;
  int value_length() const override;
  unique_ptr<Expression> copy() const override;
  bool equal(const Expression &other) const override;
  RC get_value(const Tuple &tuple, Value &value) const override;
  RC try_get_value(Value &value) const override;
  RC validate() const;
  vector<unique_ptr<Expression>> &children() { return children_; }
  const vector<unique_ptr<Expression>> &children() const { return children_; }

private:
  RC evaluate(const vector<Value> &arguments, Value &value) const;
  Type function_type_;
  vector<unique_ptr<Expression>> children_;
};
