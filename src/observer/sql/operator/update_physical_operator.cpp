/* Copyright (c) 2026 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/operator/update_physical_operator.h"

#include <algorithm>
#include <cstring>

#include "common/log/log.h"
#include "storage/table/table.h"
#include "storage/trx/trx.h"

RC UpdatePhysicalOperator::open(Trx *trx)
{
  rc_ = update(trx);
  return rc_;
}

RC UpdatePhysicalOperator::update(Trx *trx)
{
  if (children_.size() != 1 || table_ == nullptr || field_meta_ == nullptr || trx == nullptr) {
    return RC::INTERNAL;
  }

  PhysicalOperator &child = *children_.front();
  RC                rc    = child.open(trx);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open update child operator. rc=%s", strrc(rc));
    child.close();
    return rc;
  }

  // Own every matched row and close the scan before updating records or indexes.
  // Otherwise an index scan could revisit a changed key or retain page latches.
  vector<Record> records;
  while ((rc = child.next()) == RC::SUCCESS) {
    Tuple *tuple = child.current_tuple();
    if (tuple == nullptr) {
      rc = RC::INTERNAL;
      break;
    }

    const Record &record = static_cast<RowTuple *>(tuple)->record();
    Record        saved_record;
    rc = saved_record.copy_data(record.data(), table_->table_meta().record_size());
    if (rc != RC::SUCCESS) {
      break;
    }
    saved_record.set_rid(record.rid());
    saved_record.set_key(record.key());
    records.emplace_back(std::move(saved_record));
  }

  RC close_rc = child.close();
  if (rc != RC::RECORD_EOF) {
    LOG_WARN("failed to collect records for update. rc=%s", strrc(rc));
    return rc;
  }
  if (close_rc != RC::SUCCESS) {
    return close_rc;
  }

  for (Record &old_record : records) {
    Record new_record;
    rc = new_record.copy_data(old_record.data(), old_record.len());
    if (rc != RC::SUCCESS) {
      return rc;
    }
    new_record.set_rid(old_record.rid());
    new_record.set_key(old_record.key());

    char *field_data = new_record.data() + field_meta_->offset();
    memset(field_data, 0, field_meta_->len());
    memcpy(field_data, value_.data(), std::min(field_meta_->len(), value_.length()));

    rc = trx->update_record(table_, old_record, new_record);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to update record. rc=%s", strrc(rc));
      return rc;
    }
  }

  return RC::SUCCESS;
}
