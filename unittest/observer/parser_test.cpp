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
#include "sql/expr/expression_iterator.h"
#include "sql/expr/vector_function_expression.h"
#include "sql/parser/parse.h"
#include "sql/stmt/calc_stmt.h"
#include "sql/stmt/create_table_stmt.h"
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

TEST(ParserTest, vector_dimensions_and_default)
{
  ParsedSqlResult result;
  ASSERT_EQ(parse("create table vectors (id int, a vector(0003), b vector, c vector(16383));", &result), RC::SUCCESS);
  ASSERT_EQ(result.sql_nodes().size(), 1U);
  ASSERT_EQ(result.sql_nodes()[0]->flag, SCF_CREATE_TABLE);
  const auto &fields = result.sql_nodes()[0]->create_table.attr_infos;
  ASSERT_EQ(fields.size(), 4U);
  EXPECT_EQ(fields[1].type, AttrType::VECTORS);
  EXPECT_EQ(fields[1].length, 3U);
  EXPECT_EQ(fields[2].length, 2048U);
  EXPECT_EQ(fields[3].length, 16383U);
}

TEST(ParserTest, vector_dimension_errors)
{
  for (const char *sql : {"create table t (id int, a vector(0));",
           "create table t (a vector(16384));", "create table t (a vector(4294967299));"}) {
    SCOPED_TRACE(sql);
    ParsedSqlResult result;
    parse(sql, &result);
    ASSERT_EQ(result.sql_nodes().size(), 1U);
    ASSERT_EQ(result.sql_nodes()[0]->flag, SCF_CREATE_TABLE);
    EXPECT_EQ(result.sql_nodes()[0]->create_table.attr_infos.back().length, 0U);
    Stmt *stmt = nullptr;
    EXPECT_NE(CreateTableStmt::create(nullptr, result.sql_nodes()[0]->create_table, stmt), RC::SUCCESS);
    EXPECT_EQ(stmt, nullptr);
  }
  for (const char *sql : {"create table t (a vector(-1));", "create table t (a vector());"}) {
    SCOPED_TRACE(sql);
    ParsedSqlResult result;
    parse(sql, &result);
    ASSERT_EQ(result.sql_nodes().size(), 1U);
    EXPECT_EQ(result.sql_nodes()[0]->flag, SCF_ERROR);
  }
}

TEST(ParserTest, vector_insert_and_update_literals)
{
  ParsedSqlResult result;
  ASSERT_EQ(parse("insert into t values (1, string_to_vector('[1, -2.5, 3e-2]'));", &result), RC::SUCCESS);
  ASSERT_EQ(result.sql_nodes().size(), 1U);
  ASSERT_EQ(result.sql_nodes()[0]->flag, SCF_INSERT);
  const auto &values = result.sql_nodes()[0]->insertion.values;
  ASSERT_EQ(values.size(), 2U);
  EXPECT_EQ(values[1].attr_type(), AttrType::VECTORS);
  ASSERT_EQ(values[1].get_vector().size(), 3U);
  EXPECT_FLOAT_EQ(values[1].get_vector()[0], 1.0F);
  EXPECT_FLOAT_EQ(values[1].get_vector()[1], -2.5F);
  EXPECT_FLOAT_EQ(values[1].get_vector()[2], 0.03F);

  ParsedSqlResult update;
  ASSERT_EQ(parse("update t set a = to_vector('[4, 5, 6]') where id = 1;", &update), RC::SUCCESS);
  ASSERT_EQ(update.sql_nodes().size(), 1U);
  ASSERT_EQ(update.sql_nodes()[0]->flag, SCF_UPDATE);
  EXPECT_EQ(update.sql_nodes()[0]->update.value.get_vector(), (vector<float>{4, 5, 6}));
}

TEST(ParserTest, vector_invalid_literals_remain_semantic_errors)
{
  for (const char *literal : {"[]", "[1,]", "[1,,2]", "[NaN]", "[1e999]", "[1]suffix", "abc"}) {
    string sql = string("insert into t values (1, string_to_vector('") + literal + "'));";
    SCOPED_TRACE(sql);
    ParsedSqlResult result;
    parse(sql.c_str(), &result);
    ASSERT_EQ(result.sql_nodes().size(), 1U);
    ASSERT_EQ(result.sql_nodes()[0]->flag, SCF_INSERT);
    ASSERT_EQ(result.sql_nodes()[0]->insertion.values.size(), 2U);
    EXPECT_EQ(result.sql_nodes()[0]->insertion.values[1].attr_type(), AttrType::UNDEFINED);

    string update_sql = string("update t set v = string_to_vector('") + literal + "') where id = 1;";
    ParsedSqlResult update;
    parse(update_sql.c_str(), &update);
    ASSERT_EQ(update.sql_nodes().size(), 1U);
    ASSERT_EQ(update.sql_nodes()[0]->flag, SCF_UPDATE);
    EXPECT_EQ(update.sql_nodes()[0]->update.value.attr_type(), AttrType::UNDEFINED);
  }
}

TEST(ParserTest, vector_function_nested_projection_aliases)
{
  ParsedSqlResult result;
  ASSERT_EQ(parse("select id as ident, vector_to_string(c1) as vec_str, "
                  "distance(c1, string_to_vector('[1, 2, 3]'), 'EUCLIDEAN') as dist_euc from t;",
      &result), RC::SUCCESS);
  ASSERT_EQ(result.sql_nodes().size(), 1U);
  ASSERT_EQ(result.sql_nodes()[0]->flag, SCF_SELECT);
  const auto &expressions = result.sql_nodes()[0]->selection.expressions;
  ASSERT_EQ(expressions.size(), 3U);
  EXPECT_STREQ(expressions[0]->name(), "ident");
  EXPECT_TRUE(expressions[0]->has_alias());
  EXPECT_STREQ(expressions[1]->name(), "vec_str");
  ASSERT_EQ(expressions[2]->type(), ExprType::VECTOR_FUNCTION);
  auto &distance = static_cast<VectorFunctionExpr &>(*expressions[2]);
  EXPECT_EQ(distance.function_type(), VectorFunctionExpr::Type::DISTANCE);
  ASSERT_EQ(distance.children().size(), 3U);
  EXPECT_EQ(distance.children()[1]->type(), ExprType::VECTOR_FUNCTION);
  EXPECT_STREQ(distance.name(), "dist_euc");
}

TEST(ParserTest, vector_select_without_from)
{
  ParsedSqlResult result;
  ASSERT_EQ(parse("select vector_distance(to_vector('[1, 2]'), string_to_vector('[4, 6]'), 'EUCLIDEAN') as d, "
                  "from_vector(to_vector('[1, 2]')) as formatted;", &result), RC::SUCCESS);
  ASSERT_EQ(result.sql_nodes().size(), 1U);
  ASSERT_EQ(result.sql_nodes()[0]->flag, SCF_CALC);
  Stmt *raw_stmt = nullptr;
  ASSERT_EQ(CalcStmt::create(result.sql_nodes()[0]->calc, raw_stmt), RC::SUCCESS);
  unique_ptr<Stmt> stmt(raw_stmt);
  auto &expressions = static_cast<CalcStmt &>(*stmt).expressions();
  ASSERT_EQ(expressions.size(), 2U);
  EXPECT_STREQ(expressions[0]->name(), "d");
  Value distance;
  ASSERT_EQ(expressions[0]->try_get_value(distance), RC::SUCCESS);
  EXPECT_FLOAT_EQ(distance.get_float(), 5.0F);
  Value formatted;
  ASSERT_EQ(expressions[1]->try_get_value(formatted), RC::SUCCESS);
  EXPECT_EQ(formatted.get_string(), "[1.00000e+00,2.00000e+00]");
}

TEST(ParserTest, select_without_from_binds_and_validates)
{
  for (const char *sql : {"select a;", "select distance(string_to_vector('[1]'), string_to_vector('[1,2]'), 'DOT');",
           "select string_to_vector('[1,]');", "select vector_to_string(1);",
           "select string_to_vector('[1]') + 1;", "select 1 - string_to_vector('[1]');",
           "select -string_to_vector('[1]');", "select distance();",
           "select vector_to_string();", "select string_to_vector('[1]', '[2]');"}) {
    SCOPED_TRACE(sql);
    ParsedSqlResult result;
    ASSERT_EQ(parse(sql, &result), RC::SUCCESS);
    ASSERT_EQ(result.sql_nodes().size(), 1U);
    ASSERT_EQ(result.sql_nodes()[0]->flag, SCF_CALC);
    Stmt *raw_stmt = nullptr;
    EXPECT_NE(CalcStmt::create(result.sql_nodes()[0]->calc, raw_stmt), RC::SUCCESS);
    EXPECT_EQ(raw_stmt, nullptr);
  }

  ParsedSqlResult result;
  ASSERT_EQ(parse("select -2 * -3 + 1 as result;", &result), RC::SUCCESS);
  ASSERT_EQ(result.sql_nodes().size(), 1U);
  Stmt *raw_stmt = nullptr;
  ASSERT_EQ(CalcStmt::create(result.sql_nodes()[0]->calc, raw_stmt), RC::SUCCESS);
  unique_ptr<Stmt> stmt(raw_stmt);
  Value value;
  ASSERT_EQ(static_cast<CalcStmt &>(*stmt).expressions()[0]->try_get_value(value), RC::SUCCESS);
  EXPECT_EQ(value.get_int(), 7);
}

TEST(ParserTest, unary_distance_expression_traversal_skips_missing_child)
{
  ParsedSqlResult result;
  ASSERT_EQ(parse("select -distance(a, b, 'DOT') from t;", &result), RC::SUCCESS);
  ASSERT_EQ(result.sql_nodes().size(), 1U);
  ASSERT_EQ(result.sql_nodes()[0]->flag, SCF_SELECT);
  auto &expr = result.sql_nodes()[0]->selection.expressions[0];
  size_t child_count = 0;
  function<RC(unique_ptr<Expression> &)> visit = [&](unique_ptr<Expression> &child) -> RC {
    EXPECT_NE(child, nullptr);
    if (child == nullptr) {
      return RC::INTERNAL;
    }
    child_count++;
    return ExpressionIterator::iterate_child_expr(*child, visit);
  };
  EXPECT_EQ(ExpressionIterator::iterate_child_expr(*expr, visit), RC::SUCCESS);
  EXPECT_EQ(child_count, 4U);
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
