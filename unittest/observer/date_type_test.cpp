/* Copyright (c) 2026 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <cstring>
#include <memory>
#include <utility>

#include "common/type/attr_type.h"
#include "common/type/date_type.h"
#include "common/value.h"
#include "gtest/gtest.h"
#include "sql/expr/expression.h"
#include "storage/common/codec.h"

namespace {
Value make_date_value(int encoded)
{
  Value value;
  value.set_date(encoded);
  return value;
}
}  // namespace

TEST(DateTypeTest, valid_dates_are_normalized)
{
  const struct
  {
    const char *input;
    const char *expected;
    int         encoded;
  } cases[] = {
      {"0001-1-1", "0001-01-01", 10101},
      {"1000-1-1", "1000-01-01", 10000101},
      {"1969-12-31", "1969-12-31", 19691231},
      {"1970-1-1", "1970-01-01", 19700101},
      {"2000-2-29", "2000-02-29", 20000229},
      {"2020-01-1", "2020-01-01", 20200101},
      {"2020-1-01", "2020-01-01", 20200101},
      {"2038-1-20", "2038-01-20", 20380120},
      {"2100-3-1", "2100-03-01", 21000301},
      {"2400-2-29", "2400-02-29", 24000229},
      {"9999-12-31", "9999-12-31", 99991231},
  };

  for (const auto &test : cases) {
    SCOPED_TRACE(test.input);
    Value date;
    ASSERT_EQ(RC::SUCCESS, Value::cast_to(Value(test.input), AttrType::DATES, date));
    EXPECT_EQ(AttrType::DATES, date.attr_type());
    EXPECT_EQ(test.encoded, date.get_int());
    EXPECT_EQ(test.expected, date.to_string());
    EXPECT_EQ(sizeof(int32_t), static_cast<size_t>(date.length()));
  }
}

TEST(DateTypeTest, rejects_invalid_calendar_dates)
{
  const char *cases[] = {
      "0000-01-01", "10000-01-01", "1900-02-29", "2001-02-29", "2100-02-29", "2020-00-10",
      "2020-13-10", "2020-01-00", "2020-01-32", "2020-04-31", "2020-06-31", "2020-09-31",
      "2020-11-31", "2020-02-30", "2017-2-29", "2017-21-29", "2017-12-32", "2017-11-31",
  };

  for (const char *input : cases) {
    SCOPED_TRACE(input);
    Value date;
    EXPECT_NE(RC::SUCCESS, Value::cast_to(Value(input), AttrType::DATES, date));
  }
}

TEST(DateTypeTest, rejects_malformed_dates)
{
  const char *cases[] = {
      "", "2020", "2020-01", "20200101", "2020/01/01", "2020--1-1", "2020-1--1", "-2020-1-1",
      "+2020-1-1", "2020-1-1x", "x2020-1-1", "2020-1-1-1", "2020-1-1 00:00:00", "abcd-ef-gh",
      " 2020-1-1", "2020-1-1 ", "2020-001-01", "2020-01-001", "20-1-1", "1-1-1",
      "9999999999999999999999-1-1", "2020-9999999999999999999999-1", "2020-1-9999999999999999999999",
  };

  for (const char *input : cases) {
    SCOPED_TRACE(input);
    Value date;
    EXPECT_NE(RC::SUCCESS, Value::cast_to(Value(input), AttrType::DATES, date));
  }
}

TEST(DateTypeTest, compares_in_calendar_order)
{
  const int dates[] = {10000101, 19691231, 19700101, 19991231, 20000228, 20000229, 20000301, 20380120, 99991231};
  for (size_t i = 0; i < sizeof(dates) / sizeof(dates[0]); ++i) {
    Value left;
    left.set_date(dates[i]);
    for (size_t j = 0; j < sizeof(dates) / sizeof(dates[0]); ++j) {
      SCOPED_TRACE(testing::Message() << dates[i] << " vs " << dates[j]);
      Value right;
      right.set_date(dates[j]);
      if (i < j) {
        EXPECT_LT(left.compare(right), 0);
      } else if (i > j) {
        EXPECT_GT(left.compare(right), 0);
      } else {
        EXPECT_EQ(0, left.compare(right));
      }
    }
  }
}

TEST(DateTypeTest, copy_move_and_record_round_trip)
{
  Value original;
  original.set_date(24000229);

  Value copied(original);
  Value assigned;
  assigned = original;
  Value moved(std::move(copied));
  Value move_assigned;
  move_assigned = std::move(assigned);

  char record[sizeof(int32_t)];
  ASSERT_EQ(sizeof(record), static_cast<size_t>(original.length()));
  std::memcpy(record, original.data(), sizeof(record));
  Value restored(AttrType::DATES, record, sizeof(record));

  for (const Value *date : {&original, &moved, &move_assigned, &restored}) {
    EXPECT_EQ(AttrType::DATES, date->attr_type());
    EXPECT_EQ(24000229, date->get_int());
    EXPECT_EQ("2400-02-29", date->to_string());
    EXPECT_EQ(0, date->compare(original));
  }
}

TEST(DateTypeTest, type_name_round_trip)
{
  EXPECT_STREQ("dates", attr_type_to_string(AttrType::DATES));
  EXPECT_EQ(AttrType::DATES, attr_type_from_string("dates"));
  EXPECT_EQ(AttrType::DATES, attr_type_from_string("DATES"));
}

TEST(DateTypeTest, column_comparison_respects_row_indices_and_constants)
{
  Column dates(AttrType::DATES, sizeof(int), 3);
  for (int encoded : {19691231, 20000229, 20380120}) {
    ASSERT_EQ(RC::SUCCESS, dates.append_value(make_date_value(encoded)));
  }
  Column constant;
  constant.init(make_date_value(20000229), 3);

  DateType type;
  EXPECT_LT(type.compare(dates, dates, 0, 2), 0);
  EXPECT_GT(type.compare(dates, dates, 2, 0), 0);
  EXPECT_EQ(0, type.compare(dates, dates, 1, 1));
  EXPECT_LT(type.compare(dates, constant, 0, 2), 0);
  EXPECT_EQ(0, type.compare(dates, constant, 1, 0));
  EXPECT_GT(type.compare(constant, dates, 2, 0), 0);
}

TEST(DateTypeTest, vector_comparison_supports_literals_on_either_side)
{
  auto dates = std::make_unique<Column>(AttrType::DATES, sizeof(int), 3);
  for (int encoded : {19691231, 20000229, 20380120}) {
    ASSERT_EQ(RC::SUCCESS, dates->append_value(make_date_value(encoded)));
  }
  Chunk chunk;
  chunk.add_column(std::move(dates), 0);
  FieldMeta meta("birthday", AttrType::DATES, 0, sizeof(int), true, 0);

  const struct
  {
    CompOp  op;
    uint8_t field_on_left[3];
    uint8_t field_on_right[3];
  } cases[] = {
      {EQUAL_TO, {0, 1, 0}, {0, 1, 0}},
      {NOT_EQUAL, {1, 0, 1}, {1, 0, 1}},
      {LESS_THAN, {1, 0, 0}, {0, 0, 1}},
      {LESS_EQUAL, {1, 1, 0}, {0, 1, 1}},
      {GREAT_THAN, {0, 0, 1}, {1, 0, 0}},
      {GREAT_EQUAL, {0, 1, 1}, {1, 1, 0}},
  };
  for (const auto &test : cases) {
    for (bool field_on_left : {true, false}) {
      SCOPED_TRACE(testing::Message() << "operator " << test.op << ", field on left " << field_on_left);
      std::unique_ptr<Expression> field = std::make_unique<FieldExpr>(Field(nullptr, &meta));
      std::unique_ptr<Expression> literal = std::make_unique<ValueExpr>(make_date_value(20000229));
      ComparisonExpr comparison(test.op,
          field_on_left ? std::move(field) : std::move(literal),
          field_on_left ? std::move(literal) : std::move(field));
      vector<uint8_t> selected(3, 1);
      ASSERT_EQ(RC::SUCCESS, comparison.eval(chunk, selected));
      for (int i = 0; i < 3; i++) {
        EXPECT_EQ(field_on_left ? test.field_on_left[i] : test.field_on_right[i], selected[i]);
      }
      // Predicate evaluation must preserve a row excluded by an earlier predicate.
      selected.assign(3, 0);
      ASSERT_EQ(RC::SUCCESS, comparison.eval(chunk, selected));
      EXPECT_EQ(vector<uint8_t>({0, 0, 0}), selected);
    }
  }
}

TEST(DateTypeTest, vector_comparison_supports_two_columns_and_two_constants)
{
  auto left = std::make_unique<Column>(AttrType::DATES, sizeof(int), 3);
  auto right = std::make_unique<Column>(AttrType::DATES, sizeof(int), 3);
  for (int encoded : {19691231, 20000229, 20380120}) {
    ASSERT_EQ(RC::SUCCESS, left->append_value(make_date_value(encoded)));
    ASSERT_EQ(RC::SUCCESS, right->append_value(make_date_value(20000229)));
  }
  Chunk chunk;
  chunk.add_column(std::move(left), 0);
  chunk.add_column(std::move(right), 1);
  FieldMeta left_meta("left_date", AttrType::DATES, 0, sizeof(int), true, 0);
  FieldMeta right_meta("right_date", AttrType::DATES, sizeof(int), sizeof(int), true, 1);
  ComparisonExpr columns(LESS_EQUAL,
      std::make_unique<FieldExpr>(Field(nullptr, &left_meta)),
      std::make_unique<FieldExpr>(Field(nullptr, &right_meta)));
  vector<uint8_t> selected(3, 1);
  ASSERT_EQ(RC::SUCCESS, columns.eval(chunk, selected));
  EXPECT_EQ(vector<uint8_t>({1, 1, 0}), selected);

  ComparisonExpr constants(EQUAL_TO,
      std::make_unique<ValueExpr>(make_date_value(20000229)),
      std::make_unique<ValueExpr>(make_date_value(20000229)));
  selected = {1, 0, 1};
  ASSERT_EQ(RC::SUCCESS, constants.eval(chunk, selected));
  EXPECT_EQ(vector<uint8_t>({1, 0, 1}), selected);
}

TEST(DateTypeTest, character_column_cast_produces_packed_date_values)
{
  auto strings = std::make_unique<Column>(AttrType::CHARS, 32, 3);
  for (const char *text : {"1969-12-31", "2000-2-29", "2038-1-20"}) {
    ASSERT_EQ(RC::SUCCESS, strings->append_value(Value(text)));
  }
  Chunk chunk;
  chunk.add_column(std::move(strings), 0);
  FieldMeta meta("date_text", AttrType::CHARS, 0, 32, true, 0);
  auto cast = std::make_unique<CastExpr>(std::make_unique<FieldExpr>(Field(nullptr, &meta)), AttrType::DATES);
  Column result;
  ASSERT_EQ(RC::SUCCESS, cast->get_column(chunk, result));
  EXPECT_EQ(AttrType::DATES, result.attr_type());
  EXPECT_EQ(static_cast<int>(sizeof(int)), result.attr_len());
  ASSERT_EQ(3, result.count());
  EXPECT_EQ(static_cast<int>(3 * sizeof(int)), result.data_len());
  const int expected[] = {19691231, 20000229, 20380120};
  for (int i = 0; i < 3; i++) {
    int stored;
    std::memcpy(&stored, result.data() + i * sizeof(int), sizeof(stored));
    EXPECT_EQ(expected[i], stored);
    EXPECT_EQ(expected[i], result.get_value(i).get_int());
  }

  ComparisonExpr comparison(LESS_THAN,
      std::move(cast), std::make_unique<ValueExpr>(make_date_value(20000229)));
  vector<uint8_t> selected(3, 1);
  ASSERT_EQ(RC::SUCCESS, comparison.eval(chunk, selected));
  EXPECT_EQ(vector<uint8_t>({1, 0, 0}), selected);
}

TEST(DateTypeTest, character_column_cast_rejects_invalid_values)
{
  for (const char *invalid : {"1900-02-29", "2020-04-31", "2038-1-20x", "not a date"}) {
    SCOPED_TRACE(invalid);
    auto strings = std::make_unique<Column>(AttrType::CHARS, 32, 2);
    ASSERT_EQ(RC::SUCCESS, strings->append_value(Value("2000-2-29")));
    ASSERT_EQ(RC::SUCCESS, strings->append_value(Value(invalid)));
    Chunk chunk;
    chunk.add_column(std::move(strings), 0);
    FieldMeta meta("date_text", AttrType::CHARS, 0, 32, true, 0);
    CastExpr cast(std::make_unique<FieldExpr>(Field(nullptr, &meta)), AttrType::DATES);
    Column result;
    EXPECT_NE(RC::SUCCESS, cast.get_column(chunk, result));
  }
}

TEST(DateTypeTest, ordered_keys_preserve_calendar_order)
{
  bytes previous;
  for (int encoded : {10101, 19691231, 19700101, 20000228, 20000229, 20000301, 20380120, 99991231}) {
    SCOPED_TRACE(encoded);
    bytes key;
    ASSERT_EQ(RC::SUCCESS, Codec::encode_value(make_date_value(encoded), key));
    bytes integer_key;
    ASSERT_EQ(RC::SUCCESS, Codec::encode_value(Value(encoded), integer_key));
    EXPECT_EQ(integer_key, key);
    if (!previous.empty()) {
      EXPECT_LT(previous, key);
    }
    span<byte_t> remaining(key);
    int64_t decoded = 0;
    ASSERT_EQ(RC::SUCCESS, OrderedCode::parse(remaining, OrderedCode::increasing, decoded));
    EXPECT_EQ(encoded, decoded);
    EXPECT_EQ(0U, remaining.size());
    previous = std::move(key);
  }
}
