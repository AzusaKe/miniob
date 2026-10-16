/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <memory>
#include <string>
#include <vector>

#include "sql/expr/expression.h"
#include "sql/parser/parse.h"
#include "gtest/gtest.h"

using namespace std;

TEST(ParserTest, update_negative_integer)
{
  ParsedSqlResult result;
  ASSERT_EQ(parse("update t set id = -7 where age <= -2;", &result), RC::SUCCESS);
  ASSERT_EQ(result.sql_nodes().size(), 1U);
  ASSERT_EQ(result.sql_nodes()[0]->flag, SCF_UPDATE);
  const UpdateSqlNode &update = result.sql_nodes()[0]->update;
  EXPECT_EQ(update.value.attr_type(), AttrType::INTS);
  EXPECT_EQ(update.value.get_int(), -7);
  ASSERT_EQ(update.conditions.size(), 1U);
  EXPECT_EQ(update.conditions[0].right_value.get_int(), -2);
}

TEST(ParserTest, update_negative_float_and_left_condition)
{
  ParsedSqlResult result;
  ASSERT_EQ(parse("update t set score = -1.25 where -2.5 < score;", &result), RC::SUCCESS);
  ASSERT_EQ(result.sql_nodes().size(), 1U);
  ASSERT_EQ(result.sql_nodes()[0]->flag, SCF_UPDATE);
  const UpdateSqlNode &update = result.sql_nodes()[0]->update;
  EXPECT_EQ(update.value.attr_type(), AttrType::FLOATS);
  EXPECT_FLOAT_EQ(update.value.get_float(), -1.25F);
  ASSERT_EQ(update.conditions.size(), 1U);
  EXPECT_EQ(update.conditions[0].left_is_attr, 0);
  EXPECT_FLOAT_EQ(update.conditions[0].left_value.get_float(), -2.5F);
}

TEST(ParserTest, signed_literals_preserve_arithmetic_precedence)
{
  ParsedSqlResult result;
  ASSERT_EQ(parse("calc -2 * -3 + 1;", &result), RC::SUCCESS);
  ASSERT_EQ(result.sql_nodes().size(), 1U);
  ASSERT_EQ(result.sql_nodes()[0]->flag, SCF_CALC);
  ASSERT_EQ(result.sql_nodes()[0]->calc.expressions.size(), 1U);
  Value value;
  ASSERT_EQ(result.sql_nodes()[0]->calc.expressions[0]->try_get_value(value), RC::SUCCESS);
  EXPECT_EQ(value.get_int(), 7);
}

TEST(ParserTest, DISABLED_aggregation_test)
{
  ParsedSqlResult result;
  {
    const char *sql = "select sum(a), sum(b) from tab";
    ASSERT_EQ(parse(sql, &result), RC::SUCCESS);
  }
  {
    const char *sql = "select sum(a+b*c+d/e-f*(g-h)) from tab";
    ASSERT_EQ(parse(sql, &result), RC::SUCCESS);
  }
}

TEST(ParserTest, DISABLED_group_by_test)
{
  ParsedSqlResult result;
  {
    const char *sql = "select a, sum(b) from tab group by a";
    ASSERT_EQ(parse(sql, &result), RC::SUCCESS);
  }
  {
    const char *sql = "select sum(a+b*c+d/e-f*(g+1)) from tab group by c+d-e";
    ASSERT_EQ(parse(sql, &result), RC::SUCCESS);
  }
  {
    const char *sql = "select a,b,sum(a+b*c+d/e-f*(g+1)),e,f,d from tab group by c+d-e";
    ASSERT_EQ(parse(sql, &result), RC::SUCCESS);
  }
  {
    const char *sql = "select a,b,sum(a+b*c+d/e-f*(g+1)),e,f,d from tab where a>1 and b< 10 and c='abc' group by c+d-e";
    ASSERT_EQ(parse(sql, &result), RC::SUCCESS);
  }
  {
    const char *sql = "select t.a,s.c,sum(u.rr) from t,s,u where t.a<10 and t.b!='vfdv' and u.c='abc' group by s.c+u.q";
    ASSERT_EQ(parse(sql, &result), RC::SUCCESS);
  }
  {
    const char *sql = "select a+a,sum(b+b)+sum(b) from tab group by a";
    ASSERT_EQ(parse(sql, &result), RC::SUCCESS);
  }
  {
    const char *sql = "select a+a,sum(b+b)+sum(b) from tab group by a+a";
    ASSERT_EQ(parse(sql, &result), RC::SUCCESS);
  }
}

int main(int argc, char **argv)
{

  // 分析gtest程序的命令行参数
  testing::InitGoogleTest(&argc, argv);

  // 调用RUN_ALL_TESTS()运行所有测试用例
  // main函数返回RUN_ALL_TESTS()的运行结果
  return RUN_ALL_TESTS();
}
