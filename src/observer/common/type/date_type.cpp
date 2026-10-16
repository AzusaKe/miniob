/* Copyright (c) 2026 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "common/type/date_type.h"

#include <cstdio>

#include "common/value.h"
#include "storage/common/column.h"

int DateType::compare(const Value &left, const Value &right) const
{
  if (left.attr_type() != AttrType::DATES || right.attr_type() != AttrType::DATES) {
    return INT32_MAX;
  }
  const int left_date  = left.get_int();
  const int right_date = right.get_int();
  return (left_date > right_date) - (left_date < right_date);
}

int DateType::compare(const Column &left, const Column &right, int left_idx, int right_idx) const
{
  return compare(left.get_value(left_idx), right.get_value(right_idx));
}

RC DateType::set_value_from_str(Value &val, const string &data) const
{
  // Accept YYYY-M-D as well as zero-padded months and days. Parse each component
  // explicitly so signs, whitespace, trailing text and numeric overflow cannot slip through.
  int    parts[3] = {};
  size_t pos      = 0;
  for (int part = 0; part < 3; part++) {
    const size_t start      = pos;
    const size_t max_digits = part == 0 ? 4 : 2;
    while (pos < data.size() && data[pos] >= '0' && data[pos] <= '9') {
      if (pos - start >= max_digits) {
        return RC::SCHEMA_FIELD_TYPE_MISMATCH;
      }
      parts[part] = parts[part] * 10 + data[pos++] - '0';
    }
    if (pos == start || (part == 0 && pos - start != 4)) {
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    }
    if (part < 2 && (pos == data.size() || data[pos++] != '-')) {
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    }
  }
  const int year  = parts[0];
  const int month = parts[1];
  const int day   = parts[2];
  if (pos != data.size() || year < 1 || month < 1 || month > 12 || day < 1) {
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  }
  static constexpr int DAYS_PER_MONTH[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap_year = year % 400 == 0 || (year % 4 == 0 && year % 100 != 0);
  const int  max_day   = DAYS_PER_MONTH[month - 1] + (month == 2 && leap_year ? 1 : 0);
  if (day > max_day) {
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  }
  val.set_date(year * 10000 + month * 100 + day);
  return RC::SUCCESS;
}

RC DateType::to_string(const Value &val, string &result) const
{
  if (val.attr_type() != AttrType::DATES) {
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  }
  const int date = val.get_int();
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", date / 10000, date / 100 % 100, date % 100);
  result = buffer;
  return RC::SUCCESS;
}
