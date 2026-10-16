/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by WangYunlai on 2022/12/30.
//

#include "sql/operator/nested_loop_join_physical_operator.h"

NestedLoopJoinPhysicalOperator::NestedLoopJoinPhysicalOperator() {}

RC NestedLoopJoinPhysicalOperator::open(Trx *trx)
{
  if (children_.size() != 2) {
    LOG_WARN("nlj operator should have 2 children");
    return RC::INTERNAL;
  }

  RC rc         = RC::SUCCESS;
  left_         = children_[0].get();
  right_        = children_[1].get();
  left_tuple_   = nullptr;
  right_tuple_  = nullptr;
  joined_tuple_.set_left(nullptr);
  joined_tuple_.set_right(nullptr);
  left_closed_  = true;
  right_closed_ = true;
  round_done_   = true;

  trx_ = trx;
  rc = left_->open(trx);
  if (rc != RC::SUCCESS) {
    RC close_rc = left_->close();
    if (close_rc != RC::SUCCESS) {
      LOG_WARN("failed to clean up left oper after open error. rc=%s", strrc(close_rc));
    }
    return rc;
  }
  left_closed_ = false;
  return rc;
}

RC NestedLoopJoinPhysicalOperator::next()
{
  RC rc = RC::SUCCESS;
  while (true) {
    if (left_tuple_ == nullptr || round_done_) {
      rc = left_next();
      if (rc != RC::SUCCESS) {
        return rc;
      }
    }

    rc = right_next();
    if (rc != RC::SUCCESS) {
      if (rc == RC::RECORD_EOF) {
        round_done_ = true;
        continue;
      } else {
        return rc;
      }
    }

    bool matched = false;
    rc = filter(matched);
    if (rc != RC::SUCCESS || matched) {
      return rc;
    }
  }
}

RC NestedLoopJoinPhysicalOperator::close()
{
  RC rc = RC::SUCCESS;
  if (!left_closed_) {
    rc = left_->close();
    left_closed_ = true;
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to close left oper. rc=%s", strrc(rc));
    }
  }

  if (!right_closed_) {
    RC right_rc = right_->close();
    right_closed_ = true;
    if (right_rc != RC::SUCCESS) {
      LOG_WARN("failed to close right oper. rc=%s", strrc(right_rc));
      if (rc == RC::SUCCESS) {
        rc = right_rc;
      }
    }
  }
  left_tuple_ = nullptr;
  right_tuple_ = nullptr;
  joined_tuple_.set_left(nullptr);
  joined_tuple_.set_right(nullptr);
  round_done_ = true;
  return rc;
}

Tuple *NestedLoopJoinPhysicalOperator::current_tuple() { return &joined_tuple_; }

RC NestedLoopJoinPhysicalOperator::left_next()
{
  RC rc = RC::SUCCESS;
  rc    = left_->next();
  if (rc != RC::SUCCESS) {
    return rc;
  }

  left_tuple_ = left_->current_tuple();
  if (left_tuple_ == nullptr) {
    return RC::INTERNAL;
  }
  joined_tuple_.set_left(left_tuple_);
  return rc;
}

RC NestedLoopJoinPhysicalOperator::right_next()
{
  RC rc = RC::SUCCESS;
  if (round_done_) {
    if (!right_closed_) {
      rc = right_->close();

      right_closed_ = true;
      if (rc != RC::SUCCESS) {
        return rc;
      }
    }

    rc = right_->open(trx_);
    if (rc != RC::SUCCESS) {
      RC close_rc = right_->close();
      if (close_rc != RC::SUCCESS) {
        LOG_WARN("failed to clean up right oper after open error. rc=%s", strrc(close_rc));
      }
      return rc;
    }
    right_closed_ = false;

    round_done_ = false;
  }

  rc = right_->next();
  if (rc != RC::SUCCESS) {
    if (rc == RC::RECORD_EOF) {
      round_done_ = true;
    }
    return rc;
  }

  right_tuple_ = right_->current_tuple();
  if (right_tuple_ == nullptr) {
    return RC::INTERNAL;
  }
  joined_tuple_.set_right(right_tuple_);
  return rc;
}

RC NestedLoopJoinPhysicalOperator::filter(bool &result)
{
  result = false;
  for (const unique_ptr<Expression> &predicate : predicates_) {
    Value value;
    RC rc = predicate->get_value(joined_tuple_, value);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    if (!value.get_boolean()) {
      return RC::SUCCESS;
    }
  }
  result = true;
  return RC::SUCCESS;
}
