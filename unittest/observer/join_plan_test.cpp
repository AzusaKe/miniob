/* Copyright (c) 2026 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "common/log/log.h"
#include "sql/operator/join_logical_operator.h"
#include "sql/operator/table_get_logical_operator.h"
#include "sql/optimizer/logical_plan_generator.h"
#include "sql/optimizer/rewriter.h"
#include "sql/parser/parse.h"
#include "sql/stmt/select_stmt.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "storage/trx/trx.h"

using namespace std;
using namespace common;

namespace {

class JoinPlanTest : public testing::Test
{
protected:
  void SetUp() override
  {
    char directory_template[] = "/tmp/miniob_join_plan_XXXXXX";
    const char *directory = mkdtemp(directory_template);
    ASSERT_NE(nullptr, directory);
    path_ = directory;
    db_ = make_unique<Db>();
    ASSERT_EQ(RC::SUCCESS, db_->init("join_plan", path_.c_str(), "vacuous", "disk"));
    for (const char *table_name : {"a", "b", "c"}) {
      const string name(table_name);
      AttrInfoSqlNode id;
      id.name = "id";
      id.type = name == "b" ? AttrType::FLOATS : AttrType::INTS;
      id.length = sizeof(int);
      AttrInfoSqlNode payload;
      payload.name = name + "_only";
      payload.type = AttrType::INTS;
      payload.length = sizeof(int);
      const vector<AttrInfoSqlNode> fields = {id, payload};
      ASSERT_EQ(RC::SUCCESS, db_->create_table(name.c_str(), fields, {}));
    }
  }

  void TearDown() override
  {
    db_.reset();
    if (!path_.empty()) {
      filesystem::remove_all(path_);
    }
  }

  RC bind(const char *sql, unique_ptr<Stmt> &stmt)
  {
    ParsedSqlResult parsed;
    RC rc = parse(sql, &parsed);
    if (rc != RC::SUCCESS || parsed.sql_nodes().size() != 1 || parsed.sql_nodes()[0]->flag != SCF_SELECT) {
      return RC::INVALID_ARGUMENT;
    }
    Stmt *raw_stmt = nullptr;
    rc = SelectStmt::create(db_.get(), parsed.sql_nodes()[0]->selection, raw_stmt);
    stmt.reset(raw_stmt);
    return rc;
  }

  void rewrite(unique_ptr<LogicalOperator> &plan)
  {
    Rewriter rewriter;
    bool changed = true;
    for (int iteration = 0; iteration < 16 && changed; iteration++) {
      ASSERT_EQ(RC::SUCCESS, rewriter.rewrite(plan, changed));
    }
    ASSERT_FALSE(changed) << "logical rewrite did not converge";
  }

  filesystem::path path_;
  unique_ptr<Db> db_;
};

TEST_F(JoinPlanTest, BindFieldsWithinEachOnScope)
{
  unique_ptr<Stmt> stmt;
  ASSERT_EQ(RC::SUCCESS, bind("select a_only from a inner join b on a_only = b_only;", stmt));
  ASSERT_EQ(RC::SUCCESS, bind("select count(a.*) from a inner join b on a.id = b.id;", stmt));
  ASSERT_EQ(RC::SUCCESS,
      bind("select a_only from c, a inner join b on a.id = b.id where c.id = a.id;", stmt));
  ASSERT_EQ(RC::SUCCESS,
      bind("select a.id from a inner join b on a.id = b.id inner join c on a.id = c.id;", stmt));

  const char *invalid_queries[] = {
      "select id from a inner join b on a.id = b.id;",
      "select a.id from a inner join b on id = b.id;",
      "select a.id from a inner join b on c.id = b.id;",
      "select a.id from a inner join b on a.id = c.id inner join c on b.id = c.id;",
      "select a.id from a, b inner join c on a.id = b.id;",
      "select a.id from a inner join b on a.missing = b.id;",
      "select a.id from a inner join missing_table on a.id = missing_table.id;",
      "select count(missing.*) from a inner join b on a.id = b.id;",
  };
  for (const char *sql : invalid_queries) {
    SCOPED_TRACE(sql);
    EXPECT_NE(RC::SUCCESS, bind(sql, stmt));
    EXPECT_EQ(nullptr, stmt);
  }
}

TEST_F(JoinPlanTest, FilterExplicitGroupBeforeCombiningCommaInput)
{
  unique_ptr<Stmt> stmt;
  ASSERT_EQ(RC::SUCCESS,
      bind("select a.id from c, a inner join b on a.id = b.id and a_only > 1 "
           "where c.id = a.id and b_only < 4;", stmt));
  unique_ptr<LogicalOperator> plan;
  LogicalPlanGenerator generator;
  ASSERT_EQ(RC::SUCCESS, generator.create(stmt.get(), plan));
  ASSERT_NO_FATAL_FAILURE(rewrite(plan));
  ASSERT_EQ(LogicalOperatorType::PROJECTION, plan->type());
  ASSERT_EQ(1U, plan->children().size());
  auto *cross_join = dynamic_cast<JoinLogicalOperator *>(plan->children()[0].get());
  ASSERT_NE(nullptr, cross_join);
  ASSERT_EQ(2U, cross_join->children().size());
  EXPECT_EQ(1U, cross_join->get_join_predicates().size());
  auto *comma_input = dynamic_cast<TableGetLogicalOperator *>(cross_join->children()[0].get());
  ASSERT_NE(nullptr, comma_input);
  EXPECT_STREQ("c", comma_input->table()->name());
  EXPECT_TRUE(comma_input->predicates().empty());

  auto *explicit_join = dynamic_cast<JoinLogicalOperator *>(cross_join->children()[1].get());
  ASSERT_NE(nullptr, explicit_join);
  ASSERT_EQ(2U, explicit_join->children().size());
  ASSERT_EQ(1U, explicit_join->get_join_predicates().size());
  auto *comparison = dynamic_cast<ComparisonExpr *>(explicit_join->get_join_predicates()[0].get());
  ASSERT_NE(nullptr, comparison);
  EXPECT_TRUE(comparison->left()->type() == ExprType::CAST || comparison->right()->type() == ExprType::CAST);
  for (size_t i = 0; i < 2; i++) {
    auto *scan = dynamic_cast<TableGetLogicalOperator *>(explicit_join->children()[i].get());
    ASSERT_NE(nullptr, scan);
    EXPECT_STREQ(i == 0 ? "a" : "b", scan->table()->name());
    EXPECT_EQ(1U, scan->predicates().size());
  }
}

TEST_F(JoinPlanTest, FalseOnConditionEliminatesJoinEvenAfterWherePredicate)
{
  unique_ptr<Stmt> stmt;
  ASSERT_EQ(RC::SUCCESS,
      bind("select a.id from a inner join b on 1 = 0 inner join c on b.id = c.id "
           "where a_only > 0;", stmt));
  unique_ptr<LogicalOperator> plan;
  LogicalPlanGenerator generator;
  ASSERT_EQ(RC::SUCCESS, generator.create(stmt.get(), plan));
  ASSERT_NO_FATAL_FAILURE(rewrite(plan));
  ASSERT_EQ(LogicalOperatorType::PROJECTION, plan->type());
  EXPECT_TRUE(plan->children().empty());
}

}  // namespace

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  LoggerFactory::init_default("join_plan_test.log", LOG_LEVEL_TRACE);
  return RUN_ALL_TESTS();
}
