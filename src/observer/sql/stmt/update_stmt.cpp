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
// Created by Wangyunlai on 2022/5/22.
//

#include "sql/stmt/update_stmt.h"
#include "common/log/log.h"
#include "sql/stmt/filter_stmt.h"
#include "storage/db/db.h"
#include "storage/table/table.h"

UpdateStmt::UpdateStmt(Table *table, const FieldMeta *field_meta, const Value &value, FilterStmt *filter_stmt)
    : table_(table), field_meta_(field_meta), value_(value), filter_stmt_(filter_stmt)
{}

UpdateStmt::~UpdateStmt() { delete filter_stmt_; }

RC UpdateStmt::create(Db *db, const UpdateSqlNode &update, Stmt *&stmt)
{
  stmt = nullptr;
  if (nullptr == db || update.relation_name.empty() || update.attribute_name.empty()) {
    return RC::INVALID_ARGUMENT;
  }

  Table *table = db->find_table(update.relation_name.c_str());
  if (nullptr == table) {
    LOG_WARN("no such table. db=%s, table_name=%s", db->name(), update.relation_name.c_str());
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  const FieldMeta *field_meta = table->table_meta().field(update.attribute_name.c_str());
  if (nullptr == field_meta || !field_meta->visible()) {
    LOG_WARN("no such field in table. table=%s, field=%s", table->name(), update.attribute_name.c_str());
    return RC::SCHEMA_FIELD_NOT_EXIST;
  }

  // Validate and own the replacement value before scanning, including an empty table.
  Value value = update.value;
  if (value.attr_type() != field_meta->type()) {
    RC rc = Value::cast_to(update.value, field_meta->type(), value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to cast update value. table=%s, field=%s, rc=%s", table->name(), field_meta->name(), strrc(rc));
      return rc;
    }
  }
  if (field_meta->type() == AttrType::VECTORS &&
      (value.get_vector().empty() || static_cast<int>(value.get_vector().size()) > field_meta->vector_dim())) {
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  }

  unordered_map<string, Table *> table_map;
  table_map.emplace(update.relation_name, table);
  FilterStmt *filter_stmt = nullptr;
  RC rc = FilterStmt::create(
      db, table, &table_map, update.conditions.data(), static_cast<int>(update.conditions.size()), filter_stmt);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to create update filter statement. rc=%s", strrc(rc));
    return rc;
  }

  stmt = new UpdateStmt(table, field_meta, value, filter_stmt);
  return RC::SUCCESS;
}
