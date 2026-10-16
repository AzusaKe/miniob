/* Copyright (c) 2026 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/value.h"
#include "gtest/gtest.h"
#include "sql/expr/expression.h"
#include "sql/expr/tuple.h"
#include "sql/expr/vector_function_expression.h"

namespace {
using FunctionType = VectorFunctionExpr::Type;

Value vector_value(std::initializer_list<float> values)
{
  Value result;
  result.set_vector(std::vector<float>(values));
  return result;
}

std::unique_ptr<VectorFunctionExpr> make_function(FunctionType type, std::initializer_list<Value> values)
{
  std::vector<std::unique_ptr<Expression>> arguments;
  for (const Value &value : values) {
    arguments.emplace_back(std::make_unique<ValueExpr>(value));
  }
  return std::make_unique<VectorFunctionExpr>(type, std::move(arguments));
}

RC parse_vector(const char *text, Value &result)
{
  return make_function(FunctionType::STRING_TO_VECTOR, {Value(text)})->try_get_value(result);
}

class RuntimeValueExpr final : public Expression
{
public:
  explicit RuntimeValueExpr(const Value &value) : value_(value) {}
  std::unique_ptr<Expression> copy() const override { return std::make_unique<RuntimeValueExpr>(value_); }
  ExprType type() const override { return ExprType::VALUE; }
  AttrType value_type() const override { return value_.attr_type(); }
  RC get_value(const Tuple &, Value &result) const override
  {
    result = value_;
    return RC::SUCCESS;
  }

private:
  Value value_;
};
}  // namespace

TEST(VectorFunctionTest, ParsesDecimalScientificAndWhitespace)
{
  const struct
  {
    const char        *text;
    std::vector<float> expected;
  } cases[] = {
      {"[1,2,3]", {1, 2, 3}},
      {" \t[ +1.25, -.5, 2., 6.25e-2, -1E+2 ]\r\n", {1.25f, -0.5f, 2, 0.0625f, -100}},
      {"[0]", {0}},
      {"[-3.4028234e38,3.4028234e38]", {-3.4028234e38f, 3.4028234e38f}},
  };
  for (const auto &test : cases) {
    SCOPED_TRACE(test.text);
    Value result;
    ASSERT_EQ(RC::SUCCESS, parse_vector(test.text, result));
    ASSERT_EQ(AttrType::VECTORS, result.attr_type());
    ASSERT_EQ(test.expected.size(), result.get_vector().size());
    for (size_t i = 0; i < test.expected.size(); ++i) {
      EXPECT_FLOAT_EQ(test.expected[i], result.get_vector()[i]);
    }
  }
}

TEST(VectorFunctionTest, RejectsMalformedAndNonFiniteInputs)
{
  const char *cases[] = {"", "[]", "[ ]", "1,2", "[1", "1]", "[,1]", "[1,]", "[1,,2]", "[1 2]",
      "[1foo]", "[1]tail", "[[1]]", "[nan]", "[NaN]", "[inf]", "[-Infinity]", "[1e999]",
      "[3.5e38]", "[0x1p2]", "[1e]", "[+ 1]"};
  for (const char *text : cases) {
    SCOPED_TRACE(text);
    Value result;
    EXPECT_NE(RC::SUCCESS, parse_vector(text, result));
  }
}

TEST(VectorFunctionTest, EnforcesMaximumElementCount)
{
  std::string text = "[0";
  for (int i = 1; i < 16383; ++i) {
    text += ",0";
  }
  text += "]";
  Value result;
  ASSERT_EQ(RC::SUCCESS, parse_vector(text.c_str(), result));
  EXPECT_EQ(16383U, result.get_vector().size());
  text.insert(text.size() - 1, ",0");
  EXPECT_NE(RC::SUCCESS, parse_vector(text.c_str(), result));
}

TEST(VectorFunctionTest, FormatsWithFiveScientificDecimalPlaces)
{
  Value result;
  ASSERT_EQ(RC::SUCCESS, make_function(FunctionType::VECTOR_TO_STRING,
      {vector_value({1, -2.5f, 0.125f, 0, 1000000})})->try_get_value(result));
  EXPECT_EQ(AttrType::CHARS, result.attr_type());
  EXPECT_EQ("[1.00000e+00,-2.50000e+00,1.25000e-01,0.00000e+00,1.00000e+06]", result.get_string());
  Value restored;
  ASSERT_EQ(RC::SUCCESS, parse_vector(result.get_string().c_str(), restored));
  EXPECT_EQ((std::vector<float>{1, -2.5f, 0.125f, 0, 1000000}), restored.get_vector());
}

TEST(VectorFunctionTest, DistancesFollowTheirMathematicalDefinitions)
{
  const struct
  {
    const char *metric;
    Value       left;
    Value       right;
    float       expected;
  } cases[] = {
      {"EUCLIDEAN", vector_value({1, 2, 3}), vector_value({4, 6, 3}), 5},
      {"DOT", vector_value({1, 2, 3}), vector_value({4, -2, 1}), 3},
      {"DOT", vector_value({1, 2}), vector_value({-1, -2}), -5},
      {"COSINE", vector_value({1, 0}), vector_value({0, 1}), 1},
      {"COSINE", vector_value({1, 0}), vector_value({-1, 0}), 2},
      {"COSINE", vector_value({3, 4}), vector_value({6, 8}), 0},
      {"cosine", vector_value({1, 2, 3}), vector_value({2, 3, 4}),
          static_cast<float>(1 - 20 / std::sqrt(14.0 * 29.0))},
      {"euclidean", vector_value({3, 4}), vector_value({0, 0}), 5},
      {"dot", vector_value({1.25f, -2.5f}), vector_value({2, 4}), -7.5f},
  };
  for (const auto &test : cases) {
    SCOPED_TRACE(test.metric);
    Value result;
    ASSERT_EQ(RC::SUCCESS, make_function(FunctionType::DISTANCE,
        {test.left, test.right, Value(test.metric)})->try_get_value(result));
    EXPECT_EQ(AttrType::FLOATS, result.attr_type());
    EXPECT_NEAR(test.expected, result.get_float(), 1e-6f);
  }
}

TEST(VectorFunctionTest, CosineUsesWideIntermediateAccumulation)
{
  for (float magnitude : {1e20f, 1e-20f}) {
    SCOPED_TRACE(magnitude);
    Value result;
    ASSERT_EQ(RC::SUCCESS, make_function(FunctionType::DISTANCE,
        {vector_value({magnitude, 0}), vector_value({magnitude, magnitude}), Value("COSINE")})
                              ->try_get_value(result));
    EXPECT_NEAR(1 - 1 / std::sqrt(2.0), result.get_float(), 1e-6);
  }
}

TEST(VectorFunctionTest, RejectsWrongArgumentTypesArityAndMetric)
{
  for (auto type : {FunctionType::STRING_TO_VECTOR, FunctionType::VECTOR_TO_STRING, FunctionType::DISTANCE}) {
    Value result;
    EXPECT_NE(RC::SUCCESS, make_function(type, {})->try_get_value(result));
  }
  Value result;
  EXPECT_NE(RC::SUCCESS, make_function(FunctionType::STRING_TO_VECTOR, {Value(1)})->try_get_value(result));
  EXPECT_NE(RC::SUCCESS, make_function(FunctionType::STRING_TO_VECTOR, {Value("[1]"), Value("[2]")})
                            ->try_get_value(result));
  EXPECT_NE(RC::SUCCESS, make_function(FunctionType::VECTOR_TO_STRING, {Value("[1]")})->try_get_value(result));
  EXPECT_NE(RC::SUCCESS, make_function(FunctionType::VECTOR_TO_STRING, {vector_value({1}), Value(1)})
                            ->try_get_value(result));
  EXPECT_NE(RC::SUCCESS, make_function(FunctionType::DISTANCE, {vector_value({1}), vector_value({2})})
                            ->try_get_value(result));
  for (const auto &arguments : {std::vector<Value>{Value("[1]"), vector_value({2}), Value("DOT")},
           std::vector<Value>{vector_value({1}), Value("[2]"), Value("DOT")},
           std::vector<Value>{vector_value({1}), vector_value({2}), Value(1)},
           std::vector<Value>{vector_value({1}), vector_value({2}), Value("MANHATTAN")},
           std::vector<Value>{vector_value({1}), vector_value({2}), Value("")},
           std::vector<Value>{vector_value({1}), vector_value({2, 3}), Value("DOT")},
           std::vector<Value>{vector_value({0, 0}), vector_value({1, 2}), Value("COSINE")},
           std::vector<Value>{vector_value({1, 2}), vector_value({0, 0}), Value("COSINE")}}) {
    std::vector<std::unique_ptr<Expression>> children;
    for (const Value &argument : arguments) {
      children.emplace_back(std::make_unique<ValueExpr>(argument));
    }
    VectorFunctionExpr expression(FunctionType::DISTANCE, std::move(children));
    EXPECT_NE(RC::SUCCESS, expression.try_get_value(result));
  }
}

TEST(VectorFunctionTest, EvaluatesRuntimeChildrenAndCopiesNestedExpressions)
{
  std::vector<std::unique_ptr<Expression>> arguments;
  arguments.emplace_back(std::make_unique<RuntimeValueExpr>(Value("[1, -2.5, 0]")));
  auto parse = std::make_unique<VectorFunctionExpr>(FunctionType::STRING_TO_VECTOR, std::move(arguments));
  arguments.emplace_back(std::move(parse));
  auto format = std::make_unique<VectorFunctionExpr>(FunctionType::VECTOR_TO_STRING, std::move(arguments));
  std::unique_ptr<Expression> copy = format->copy();
  format.reset();
  Value result;
  EXPECT_NE(RC::SUCCESS, copy->try_get_value(result));
  ValueListTuple tuple;
  ASSERT_EQ(RC::SUCCESS, copy->get_value(tuple, result));
  EXPECT_EQ("[1.00000e+00,-2.50000e+00,0.00000e+00]", result.get_string());
}

TEST(VectorFunctionTest, VectorValuesOwnCopiedAndRestoredStorage)
{
  Value original = vector_value({1, 2.5f, -3});
  Value copied = original;
  Value moved = std::move(original);
  Value assigned;
  assigned = moved;
  Value restored(AttrType::VECTORS, assigned.data(), assigned.length());
  moved.set_vector(std::vector<float>{9});
  assigned.reset();
  for (const Value *value : {&copied, &restored}) {
    EXPECT_EQ((std::vector<float>{1, 2.5f, -3}), value->get_vector());
    EXPECT_EQ(sizeof(float) * 3, static_cast<size_t>(value->length()));
  }
}
