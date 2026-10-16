/* Copyright (c) 2026 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "gtest/gtest.h"
#include "sql/operator/nested_loop_join_physical_operator.h"

namespace {

class TestScan : public PhysicalOperator
{
public:
  explicit TestScan(vector<vector<int>> rows) : rows_(std::move(rows)) {}

  PhysicalOperatorType type() const override { return PhysicalOperatorType::TABLE_SCAN; }
  RC open(Trx *) override
  {
    position_ = 0;
    ++opens;
    return RC::SUCCESS;
  }
  RC next() override
  {
    if (position_ == rows_.size()) {
      return RC::RECORD_EOF;
    }
    vector<Value> cells;
    for (int value : rows_[position_++]) {
      cells.emplace_back(value);
    }
    tuple_.set_cells(cells);
    return RC::SUCCESS;
  }
  RC close() override
  {
    ++closes;
    return RC::SUCCESS;
  }
  Tuple *current_tuple() override { return &tuple_; }

  int opens = 0;
  int closes = 0;

private:
  vector<vector<int>> rows_;
  size_t position_ = 0;
  ValueListTuple tuple_;
};

class CellExpr : public Expression
{
public:
  explicit CellExpr(int index) : index_(index) {}
  unique_ptr<Expression> copy() const override { return make_unique<CellExpr>(index_); }
  RC get_value(const Tuple &tuple, Value &value) const override { return tuple.cell_at(index_, value); }
  ExprType type() const override { return ExprType::FIELD; }
  AttrType value_type() const override { return AttrType::INTS; }

private:
  int index_;
};

unique_ptr<Expression> compare_cells(int left, CompOp op, int right)
{
  return make_unique<ComparisonExpr>(op, make_unique<CellExpr>(left), make_unique<CellExpr>(right));
}

unique_ptr<NestedLoopJoinPhysicalOperator> join(unique_ptr<PhysicalOperator> left,
    unique_ptr<PhysicalOperator> right, vector<unique_ptr<Expression>> predicates = {})
{
  auto result = make_unique<NestedLoopJoinPhysicalOperator>();
  result->add_child(std::move(left));
  result->add_child(std::move(right));
  result->set_predicates(std::move(predicates));
  return result;
}

vector<vector<int>> collect(PhysicalOperator &oper)
{
  vector<vector<int>> rows;
  RC rc;
  while ((rc = oper.next()) == RC::SUCCESS) {
    Tuple *tuple = oper.current_tuple();
    vector<int> row;
    for (int i = 0; i < tuple->cell_num(); ++i) {
      Value cell;
      EXPECT_EQ(RC::SUCCESS, tuple->cell_at(i, cell));
      row.push_back(cell.get_int());
    }
    rows.push_back(std::move(row));
  }
  EXPECT_EQ(RC::RECORD_EOF, rc);
  return rows;
}

}  // namespace

TEST(NestedLoopJoin, CartesianProductReturnsEveryPair)
{
  auto oper = join(make_unique<TestScan>(vector<vector<int>>{{1}, {2}}),
      make_unique<TestScan>(vector<vector<int>>{{10}, {20}, {30}}));
  ASSERT_EQ(RC::SUCCESS, oper->open(nullptr));
  EXPECT_EQ((vector<vector<int>>{{1, 10}, {1, 20}, {1, 30}, {2, 10}, {2, 20}, {2, 30}}), collect(*oper));
  EXPECT_EQ(RC::SUCCESS, oper->close());
}

TEST(NestedLoopJoin, MultiplePredicatesPreserveDuplicateMatches)
{
  vector<unique_ptr<Expression>> predicates;
  predicates.push_back(compare_cells(0, EQUAL_TO, 2));
  predicates.push_back(compare_cells(1, LESS_THAN, 3));
  auto oper = join(make_unique<TestScan>(vector<vector<int>>{{1, 5}, {1, 5}, {2, 1}}),
      make_unique<TestScan>(vector<vector<int>>{{1, 6}, {1, 4}, {1, 6}, {2, 1}}), std::move(predicates));
  ASSERT_EQ(RC::SUCCESS, oper->open(nullptr));
  EXPECT_EQ((vector<vector<int>>{{1, 5, 1, 6}, {1, 5, 1, 6}, {1, 5, 1, 6}, {1, 5, 1, 6}}), collect(*oper));
  EXPECT_EQ(RC::SUCCESS, oper->close());
}

TEST(NestedLoopJoin, EitherEmptyInputProducesNoRows)
{
  for (bool empty_left : {true, false}) {
    vector<vector<int>> left = empty_left ? vector<vector<int>>{} : vector<vector<int>>{{1}, {2}};
    vector<vector<int>> right = empty_left ? vector<vector<int>>{{1}, {2}} : vector<vector<int>>{};
    auto oper = join(make_unique<TestScan>(left), make_unique<TestScan>(right));
    ASSERT_EQ(RC::SUCCESS, oper->open(nullptr));
    EXPECT_TRUE(collect(*oper).empty());
    EXPECT_EQ(RC::SUCCESS, oper->close());
  }
}

TEST(NestedLoopJoin, ReopenAfterPartialAndCompleteScan)
{
  auto oper = join(make_unique<TestScan>(vector<vector<int>>{{1}, {2}}),
      make_unique<TestScan>(vector<vector<int>>{{3}, {4}}));
  ASSERT_EQ(RC::SUCCESS, oper->open(nullptr));
  ASSERT_EQ(RC::SUCCESS, oper->next());
  ASSERT_EQ(RC::SUCCESS, oper->close());
  for (int i = 0; i < 2; ++i) {
    ASSERT_EQ(RC::SUCCESS, oper->open(nullptr));
    EXPECT_EQ((vector<vector<int>>{{1, 3}, {1, 4}, {2, 3}, {2, 4}}), collect(*oper));
    ASSERT_EQ(RC::SUCCESS, oper->close());
  }
}

TEST(NestedLoopJoin, ReopensNestedRightSubtreeForEveryLeftRow)
{
  auto right = join(make_unique<TestScan>(vector<vector<int>>{{1}, {2}}),
      make_unique<TestScan>(vector<vector<int>>{{10}, {20}}));
  auto oper = join(make_unique<TestScan>(vector<vector<int>>{{100}, {200}}), std::move(right));
  ASSERT_EQ(RC::SUCCESS, oper->open(nullptr));
  EXPECT_EQ((vector<vector<int>>{{100, 1, 10}, {100, 1, 20}, {100, 2, 10}, {100, 2, 20},
      {200, 1, 10}, {200, 1, 20}, {200, 2, 10}, {200, 2, 20}}), collect(*oper));
  EXPECT_EQ(RC::SUCCESS, oper->close());
}

TEST(NestedLoopJoin, PredicateErrorIsReturnedAndChildrenAreClosed)
{
  auto left = make_unique<TestScan>(vector<vector<int>>{{1}});
  auto right = make_unique<TestScan>(vector<vector<int>>{{2}});
  auto *left_scan = left.get();
  auto *right_scan = right.get();
  vector<unique_ptr<Expression>> predicates;
  predicates.push_back(compare_cells(0, EQUAL_TO, 5));
  auto oper = join(std::move(left), std::move(right), std::move(predicates));
  ASSERT_EQ(RC::SUCCESS, oper->open(nullptr));
  EXPECT_EQ(RC::NOTFOUND, oper->next());
  EXPECT_EQ(RC::SUCCESS, oper->close());
  EXPECT_EQ(left_scan->opens, left_scan->closes);
  EXPECT_EQ(right_scan->opens, right_scan->closes);
  EXPECT_EQ(RC::SUCCESS, oper->close());
  EXPECT_EQ(left_scan->opens, left_scan->closes);
  EXPECT_EQ(right_scan->opens, right_scan->closes);
}
