/* Copyright (c) 2026 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <vector>

#include "gtest/gtest.h"
#include "common/log/log.h"
#include "common/value.h"

using namespace std;
using namespace common;

TEST(ValueTest, EmptyStringCopyOwnsItsTerminator)
{
  const Value original("");
  Value copied(original);
  ASSERT_EQ(AttrType::CHARS, copied.attr_type());
  EXPECT_EQ(0, copied.length());
  ASSERT_NE(nullptr, copied.data());
  EXPECT_EQ('\0', copied.data()[0]);
  EXPECT_NE(original.data(), copied.data());
  EXPECT_EQ("", copied.get_string());
  EXPECT_FLOAT_EQ(0.0f, copied.get_float());
}

TEST(ValueTest, EmptyStringAssignmentReplacesNumbersAndStrings)
{
  const Value original("");
  Value from_number(42);
  Value from_string("previous text");
  for (Value *target : {&from_number, &from_string}) {
    *target = original;
    ASSERT_EQ(AttrType::CHARS, target->attr_type());
    EXPECT_EQ(0, target->length());
    ASSERT_NE(nullptr, target->data());
    EXPECT_EQ('\0', target->data()[0]);
    EXPECT_NE(original.data(), target->data());
    EXPECT_EQ("", target->get_string());
  }
}

TEST(ValueTest, EmptyStringRemainsValidAcrossRepeatedCopies)
{
  const vector<Value> originals{Value(""), Value("16a")};
  const vector<Value> first_copy(originals);
  const vector<Value> second_copy(first_copy);
  for (const auto *values : {&originals, &first_copy, &second_copy}) {
    ASSERT_NE(nullptr, (*values)[0].data());
    EXPECT_EQ('\0', (*values)[0].data()[0]);
    EXPECT_EQ("", (*values)[0].get_string());
    EXPECT_FLOAT_EQ(0.0f, (*values)[0].get_float());
    EXPECT_EQ("16a", (*values)[1].get_string());
  }
}

TEST(ValueTest, NullStringAssignmentClearsPreviousNumericBits)
{
  Value null_string;
  null_string.set_string(nullptr);
  Value copied(42);
  copied = null_string;
  EXPECT_EQ(AttrType::CHARS, copied.attr_type());
  EXPECT_EQ(0, copied.length());
  EXPECT_EQ(nullptr, copied.data());
}

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  LoggerFactory::init_default("value_test.log", LOG_LEVEL_WARN);
  return RUN_ALL_TESTS();
}
