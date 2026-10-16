/* Copyright (c) 2026 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2. */

#include "common/type/vector_type.h"

#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

#include "common/value.h"

int VectorType::compare(const Value &left, const Value &right) const
{
  if (left.attr_type() != AttrType::VECTORS || right.attr_type() != AttrType::VECTORS) {
    return INT32_MAX;
  }
  const auto &a = left.get_vector();
  const auto &b = right.get_vector();
  return a == b ? 0 : 1;
}

RC VectorType::to_string(const Value &val, string &result) const
{
  if (val.attr_type() != AttrType::VECTORS) {
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  }
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << '[' << std::scientific << std::setprecision(5);
  const auto &values = val.get_vector();
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    out << values[i];
  }
  out << ']';
  result = out.str();
  return RC::SUCCESS;
}

RC VectorType::set_value_from_str(Value &val, const string &data) const
{
  // A classic-locale stream accepts decimal/scientific floats, while rejecting
  // hexadecimal numbers and nonfinite spellings accepted by strtof.
  std::istringstream input(data);
  input.imbue(std::locale::classic());
  char delimiter;
  input >> std::ws;
  if (!input.get(delimiter) || delimiter != '[') {
    return RC::INVALID_ARGUMENT;
  }

  vector<float> values;
  while (true) {
    float number;
    input >> std::ws;
    if (!(input >> number) || !std::isfinite(number)) {
      return RC::INVALID_ARGUMENT;
    }
    values.push_back(number);
    if (values.size() > MAX_DIMENSIONS) {
      return RC::INVALID_ARGUMENT;
    }
    input >> std::ws;
    if (!input.get(delimiter)) {
      return RC::INVALID_ARGUMENT;
    }
    if (delimiter == ']') {
      input >> std::ws;
      if (!input.eof()) {
        return RC::INVALID_ARGUMENT;
      }
      val.set_vector(values);
      return RC::SUCCESS;
    }
    if (delimiter != ',') {
      return RC::INVALID_ARGUMENT;
    }
  }
}
