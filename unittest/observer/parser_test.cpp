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

TEST(ParserTest, inner_join_multiple_on_conditions)
{
  ParsedSqlResult result;
  ASSERT_EQ(parse("select a.id, b.id from a inner join b on a.id = b.id and b.age > -2 "
                  "where a.score < 4;", &result), RC::SUCCESS);
  ASSERT_EQ(result.sql_nodes().size(), 1U);
  ASSERT_EQ(result.sql_nodes()[0]->flag, SCF_SELECT);
  const SelectSqlNode &select = result.sql_nodes()[0]->selection;
  EXPECT_EQ(select.relations, (vector<string>{"a", "b"}));
  ASSERT_EQ(select.joins.size(), 1U);
  const InnerJoinSqlNode &join = select.joins[0];
  EXPECT_EQ(join.relation_begin, 0U);
  EXPECT_EQ(join.relation_end, 2U);
  ASSERT_EQ(join.conditions.size(), 2U);
  EXPECT_EQ(join.conditions[0].left_attr.relation_name, "a");
  EXPECT_EQ(join.conditions[0].right_attr.relation_name, "b");
  EXPECT_EQ(join.conditions[1].right_value.get_int(), -2);
  ASSERT_EQ(select.conditions.size(), 1U);
  EXPECT_EQ(select.conditions[0].left_attr.attribute_name, "score");
}

TEST(ParserTest, inner_join_chain_scopes)
{
  ParsedSqlResult result;
  ASSERT_EQ(parse("SELECT * FROM a INNER JOIN b ON a.id = b.id "
                  "INNER JOIN c ON a.id = c.id AND b.id = c.id "
                  "INNER JOIN d ON c.id = d.id;", &result), RC::SUCCESS);
  ASSERT_EQ(result.sql_nodes().size(), 1U);
  ASSERT_EQ(result.sql_nodes()[0]->flag, SCF_SELECT);
  const SelectSqlNode &select = result.sql_nodes()[0]->selection;
  EXPECT_EQ(select.relations, (vector<string>{"a", "b", "c", "d"}));
  ASSERT_EQ(select.joins.size(), 3U);
  for (size_t i = 0; i < select.joins.size(); i++) {
    EXPECT_EQ(select.joins[i].relation_begin, 0U);
    EXPECT_EQ(select.joins[i].relation_end, i + 2);
  }
  EXPECT_EQ(select.joins[1].conditions.size(), 2U);
  EXPECT_TRUE(select.conditions.empty());
}

TEST(ParserTest, inner_join_and_comma_precedence)
{
  ParsedSqlResult result;
  ASSERT_EQ(parse("select * from a, b inner join c on b.id = c.id "
                  "inner join d on c.id = d.id, e inner join f on e.id = f.id "
                  "where a.id = b.id and d.id = e.id;", &result), RC::SUCCESS);
  ASSERT_EQ(result.sql_nodes().size(), 1U);
  ASSERT_EQ(result.sql_nodes()[0]->flag, SCF_SELECT);
  const SelectSqlNode &select = result.sql_nodes()[0]->selection;
  EXPECT_EQ(select.relations, (vector<string>{"a", "b", "c", "d", "e", "f"}));
  ASSERT_EQ(select.joins.size(), 3U);
  EXPECT_EQ(select.joins[0].relation_begin, 1U);
  EXPECT_EQ(select.joins[0].relation_end, 3U);
  EXPECT_EQ(select.joins[1].relation_begin, 1U);
  EXPECT_EQ(select.joins[1].relation_end, 4U);
  EXPECT_EQ(select.joins[2].relation_begin, 4U);
  EXPECT_EQ(select.joins[2].relation_end, 6U);
  EXPECT_EQ(select.conditions.size(), 2U);
}

TEST(ParserTest, inner_join_before_comma)
{
  ParsedSqlResult result;
  ASSERT_EQ(parse("select * from a inner join b on a.id = b.id, c where c.id = a.id;", &result), RC::SUCCESS);
  ASSERT_EQ(result.sql_nodes().size(), 1U);
  ASSERT_EQ(result.sql_nodes()[0]->flag, SCF_SELECT);
  const SelectSqlNode &select = result.sql_nodes()[0]->selection;
  EXPECT_EQ(select.relations, (vector<string>{"a", "b", "c"}));
  ASSERT_EQ(select.joins.size(), 1U);
  EXPECT_EQ(select.joins[0].relation_begin, 0U);
  EXPECT_EQ(select.joins[0].relation_end, 2U);
}

TEST(ParserTest, inner_join_requires_complete_on_conditions)
{
  const char *invalid_sql[] = {
      "select * from a inner join b;",
      "select * from a inner join b on;",
      "select * from a inner join b on where a.id = b.id;",
      "select * from a inner join b on a.id = b.id and;",
      "select * from a inner join b on a.id = b.id and where a.id = 1;",
      "select * from a inner join b on a.id = b.id inner join c on;",
  };
  for (const char *sql : invalid_sql) {
    SCOPED_TRACE(sql);
    ParsedSqlResult result;
    parse(sql, &result);
    ASSERT_EQ(result.sql_nodes().size(), 1U);
    EXPECT_EQ(result.sql_nodes()[0]->flag, SCF_ERROR);
  }
}

TEST(ParserTest, inner_join_qualified_wildcard)
{
  ParsedSqlResult result;
  ASSERT_EQ(parse("select a.*, b.id from a inner join b on a.id = b.id;", &result), RC::SUCCESS);
  ASSERT_EQ(result.sql_nodes().size(), 1U);
  ASSERT_EQ(result.sql_nodes()[0]->flag, SCF_SELECT);
  const SelectSqlNode &select = result.sql_nodes()[0]->selection;
  ASSERT_EQ(select.expressions.size(), 2U);
  ASSERT_EQ(select.expressions[0]->type(), ExprType::STAR);
  EXPECT_STREQ(static_cast<StarExpr *>(select.expressions[0].get())->table_name(), "a");
  EXPECT_STREQ(select.expressions[0]->name(), "a.*");
  ASSERT_EQ(select.expressions[1]->type(), ExprType::UNBOUND_FIELD);
}

TEST(ParserTest, comma_join_retains_relation_order)
{
  ParsedSqlResult result;
  ASSERT_EQ(parse("select * from a, b, c where a.id = b.id and b.id = c.id;", &result), RC::SUCCESS);
  ASSERT_EQ(result.sql_nodes().size(), 1U);
  ASSERT_EQ(result.sql_nodes()[0]->flag, SCF_SELECT);
  const SelectSqlNode &select = result.sql_nodes()[0]->selection;
  EXPECT_EQ(select.relations, (vector<string>{"a", "b", "c"}));
  EXPECT_TRUE(select.joins.empty());
  EXPECT_EQ(select.conditions.size(), 2U);
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
