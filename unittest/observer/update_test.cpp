/* Copyright (c) 2026 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "common/log/log.h"
#include "sql/expr/expression.h"
#include "sql/operator/logical_operator.h"
#include "sql/optimizer/logical_plan_generator.h"
#include "sql/parser/parse.h"
#include "sql/stmt/update_stmt.h"
#include "storage/db/db.h"
#include "storage/index/index.h"
#include "storage/record/record_scanner.h"
#include "storage/table/table.h"
#include "storage/trx/trx.h"

using namespace std;
using namespace common;

namespace {

using Row = pair<int, int>;
using TrxPtr = unique_ptr<Trx, function<void(Trx *)>>;

class UpdateTest : public testing::TestWithParam<const char *>
{
protected:
  void SetUp() override
  {
    char directory_template[] = "/tmp/miniob_update_XXXXXX";
    const char *directory = mkdtemp(directory_template);
    ASSERT_NE(nullptr, directory);
    path_ = directory;
    db_ = make_unique<Db>();
    ASSERT_EQ(RC::SUCCESS, db_->init("update_test", path_.c_str(), GetParam(), "disk"));

    AttrInfoSqlNode id;
    id.name = "id";
    id.type = AttrType::INTS;
    id.length = sizeof(int);
    AttrInfoSqlNode payload = id;
    payload.name = "payload";
    const vector<AttrInfoSqlNode> fields = {id, payload};
    ASSERT_EQ(RC::SUCCESS, db_->create_table("updated", fields, {}));
    ASSERT_EQ(RC::SUCCESS, table()->create_index(nullptr, table()->table_meta().field("id"), "by_id"));
    ASSERT_EQ(RC::SUCCESS, table()->create_index(nullptr, table()->table_meta().field("payload"), "by_payload"));
  }

  void TearDown() override
  {
    db_.reset();
    if (!path_.empty()) {
      filesystem::remove_all(path_);
    }
  }

  Table *table() { return db_->find_table("updated"); }

  TrxPtr begin()
  {
    TrxKit *kit = &db_->trx_kit();
    TrxPtr trx(kit->create_trx(db_->log_handler()), [kit](Trx *trx) { kit->destroy_trx(trx); });
    EXPECT_NE(nullptr, trx);
    if (trx) {
      EXPECT_EQ(RC::SUCCESS, trx->start_if_need());
    }
    return trx;
  }

  RC reopen()
  {
    db_.reset();
    db_ = make_unique<Db>();
    return db_->init("update_test", path_.c_str(), GetParam(), "disk");
  }

  RC insert(int id, int payload, Trx *trx)
  {
    const Value values[] = {Value(id), Value(payload)};
    Record record;
    RC rc = table()->make_record(2, values, record);
    return rc == RC::SUCCESS ? trx->insert_record(table(), record) : rc;
  }

  RC insert_committed(int id, int payload)
  {
    auto trx = begin();
    if (!trx) {
      return RC::INTERNAL;
    }
    RC rc = insert(id, payload, trx.get());
    if (rc == RC::SUCCESS) {
      return trx->commit();
    }
    trx->rollback();
    return rc;
  }

  Row row_value(const Record &record)
  {
    Row row;
    memcpy(&row.first, record.data() + table()->table_meta().field("id")->offset(), sizeof(int));
    memcpy(&row.second, record.data() + table()->table_meta().field("payload")->offset(), sizeof(int));
    return row;
  }

  vector<Record> records(Trx *trx)
  {
    vector<Record> rows;
    RecordScanner *raw_scanner = nullptr;
    RC rc = table()->get_record_scanner(raw_scanner, trx, ReadWriteMode::READ_ONLY);
    EXPECT_EQ(RC::SUCCESS, rc);
    unique_ptr<RecordScanner> scanner(raw_scanner);
    if (rc != RC::SUCCESS || !scanner) {
      return rows;
    }

    Record record;
    while ((rc = scanner->next(record)) == RC::SUCCESS) {
      Record copy;
      EXPECT_EQ(RC::SUCCESS, copy.copy_data(record.data(), table()->table_meta().record_size()));
      copy.set_rid(record.rid());
      rows.push_back(std::move(copy));
    }
    EXPECT_EQ(RC::RECORD_EOF, rc);
    return rows;
  }

  vector<Row> visible_rows(Trx *trx)
  {
    vector<Row> rows;
    for (const Record &record : records(trx)) {
      rows.push_back(row_value(record));
    }
    sort(rows.begin(), rows.end());
    return rows;
  }

  vector<Row> indexed_rows(const char *index_name, int key, Trx *trx)
  {
    vector<Row> rows;
    Index *index = table()->find_index(index_name);
    EXPECT_NE(nullptr, index);
    if (!index) {
      return rows;
    }
    const char *key_data = reinterpret_cast<const char *>(&key);
    unique_ptr<IndexScanner> scanner(index->create_scanner(key_data, sizeof(key), true, key_data, sizeof(key), true));
    EXPECT_NE(nullptr, scanner);
    if (!scanner) {
      return rows;
    }

    RID rid;
    RC rc;
    while ((rc = scanner->next_entry(&rid)) == RC::SUCCESS) {
      Record record;
      RC record_rc = table()->get_record(rid, record);
      EXPECT_EQ(RC::SUCCESS, record_rc);
      if (record_rc != RC::SUCCESS) {
        break;
      }
      record_rc = trx->visit_record(table(), record, ReadWriteMode::READ_ONLY);
      if (record_rc == RC::RECORD_INVISIBLE) {
        continue;
      }
      EXPECT_EQ(RC::SUCCESS, record_rc);
      if (record_rc != RC::SUCCESS) {
        break;
      }
      rows.push_back(row_value(record));
    }
    EXPECT_EQ(RC::RECORD_EOF, rc);
    sort(rows.begin(), rows.end());
    return rows;
  }

  RC change_id(Trx *trx, int old_id, int new_id)
  {
    // Close the scanner and release its page latches before writing.
    vector<Record> selected = records(trx);
    for (Record &old_record : selected) {
      if (row_value(old_record).first != old_id) {
        continue;
      }
      Record replacement = old_record;
      memcpy(replacement.data() + table()->table_meta().field("id")->offset(), &new_id, sizeof(new_id));
      return trx->update_record(table(), old_record, replacement);
    }
    return RC::RECORD_NOT_EXIST;
  }

  filesystem::path path_;
  unique_ptr<Db> db_;
};

TEST_P(UpdateTest, ChangedAndUnchangedIndexesReferToCurrentRows)
{
  ASSERT_EQ(RC::SUCCESS, insert_committed(1, 10));
  ASSERT_EQ(RC::SUCCESS, insert_committed(2, 20));
  {
    auto trx = begin();
    ASSERT_EQ(RC::SUCCESS, change_id(trx.get(), 1, 7));
    ASSERT_EQ(RC::SUCCESS, trx->commit());
  }
  {
    auto trx = begin();
    EXPECT_EQ(vector<Row>({{2, 20}, {7, 10}}), visible_rows(trx.get()));
    EXPECT_TRUE(indexed_rows("by_id", 1, trx.get()).empty());
    EXPECT_EQ(vector<Row>({{7, 10}}), indexed_rows("by_id", 7, trx.get()));
    EXPECT_EQ(vector<Row>({{7, 10}}), indexed_rows("by_payload", 10, trx.get()));
    EXPECT_EQ(vector<Row>({{2, 20}}), indexed_rows("by_payload", 20, trx.get()));
    ASSERT_EQ(RC::SUCCESS, trx->commit());
  }

  // Updating an indexed key to its current value must not add a visible copy.
  {
    auto trx = begin();
    ASSERT_EQ(RC::SUCCESS, change_id(trx.get(), 7, 7));
    ASSERT_EQ(RC::SUCCESS, trx->commit());
  }
  auto reader = begin();
  EXPECT_EQ(vector<Row>({{2, 20}, {7, 10}}), visible_rows(reader.get()));
  EXPECT_EQ(vector<Row>({{7, 10}}), indexed_rows("by_id", 7, reader.get()));
  EXPECT_EQ(vector<Row>({{7, 10}}), indexed_rows("by_payload", 10, reader.get()));
  EXPECT_EQ(RC::SUCCESS, reader->commit());
}

TEST_P(UpdateTest, RepeatedUpdatesCommitAndReplayAfterCheckpoint)
{
  ASSERT_EQ(RC::SUCCESS, insert_committed(1, 10));
  ASSERT_EQ(RC::SUCCESS, db_->sync());
  const LSN checkpoint = db_->log_handler().current_lsn();
  {
    auto trx = begin();
    ASSERT_EQ(RC::SUCCESS, change_id(trx.get(), 1, 2));
    ASSERT_EQ(RC::SUCCESS, change_id(trx.get(), 2, 3));
    EXPECT_EQ(vector<Row>({{3, 10}}), visible_rows(trx.get()));
    EXPECT_TRUE(indexed_rows("by_id", 1, trx.get()).empty());
    EXPECT_TRUE(indexed_rows("by_id", 2, trx.get()).empty());
    EXPECT_EQ(vector<Row>({{3, 10}}), indexed_rows("by_id", 3, trx.get()));
    ASSERT_EQ(RC::SUCCESS, trx->commit());
  }
  ASSERT_GT(db_->log_handler().current_lsn(), checkpoint);

  // Do not checkpoint the updates: reopening must replay their later WAL.
  ASSERT_EQ(RC::SUCCESS, reopen());
  {
    auto reader = begin();
    EXPECT_EQ(vector<Row>({{3, 10}}), visible_rows(reader.get()));
    EXPECT_TRUE(indexed_rows("by_id", 1, reader.get()).empty());
    EXPECT_TRUE(indexed_rows("by_id", 2, reader.get()).empty());
    EXPECT_EQ(vector<Row>({{3, 10}}), indexed_rows("by_id", 3, reader.get()));
    EXPECT_EQ(vector<Row>({{3, 10}}), indexed_rows("by_payload", 10, reader.get()));
    ASSERT_EQ(RC::SUCCESS, reader->commit());
  }
  ASSERT_EQ(RC::SUCCESS, insert_committed(4, 40));
  ASSERT_EQ(RC::SUCCESS, reopen());
  auto reader = begin();
  EXPECT_EQ(vector<Row>({{3, 10}, {4, 40}}), visible_rows(reader.get()));
  EXPECT_EQ(vector<Row>({{4, 40}}), indexed_rows("by_id", 4, reader.get()));
  EXPECT_EQ(RC::SUCCESS, reader->commit());
}

TEST_P(UpdateTest, MvccRepeatedUpdatesRollbackBothIndexes)
{
  if (strcmp(GetParam(), "mvcc") != 0) {
    GTEST_SKIP() << "Vacuous transactions do not implement rollback.";
  }
  ASSERT_EQ(RC::SUCCESS, insert_committed(1, 10));
  ASSERT_EQ(RC::SUCCESS, db_->sync());
  {
    auto observer = begin();
    auto writer = begin();
    ASSERT_EQ(RC::SUCCESS, change_id(writer.get(), 1, 2));
    ASSERT_EQ(RC::SUCCESS, change_id(writer.get(), 2, 3));
    ASSERT_EQ(RC::SUCCESS, insert(9, 90, writer.get()));
    ASSERT_EQ(RC::SUCCESS, change_id(writer.get(), 9, 10));
    EXPECT_EQ(vector<Row>({{3, 10}, {10, 90}}), visible_rows(writer.get()));
    EXPECT_EQ(vector<Row>({{3, 10}}), indexed_rows("by_payload", 10, writer.get()));
    EXPECT_EQ(vector<Row>({{1, 10}}), visible_rows(observer.get()));
    EXPECT_EQ(vector<Row>({{1, 10}}), indexed_rows("by_id", 1, observer.get()));
    EXPECT_TRUE(indexed_rows("by_id", 3, observer.get()).empty());
    ASSERT_EQ(RC::SUCCESS, writer->rollback());
    EXPECT_EQ(vector<Row>({{1, 10}}), visible_rows(observer.get()));
    ASSERT_EQ(RC::SUCCESS, observer->commit());
  }

  ASSERT_EQ(RC::SUCCESS, reopen());
  auto reader = begin();
  EXPECT_EQ(vector<Row>({{1, 10}}), visible_rows(reader.get()));
  EXPECT_EQ(vector<Row>({{1, 10}}), indexed_rows("by_id", 1, reader.get()));
  EXPECT_EQ(vector<Row>({{1, 10}}), indexed_rows("by_payload", 10, reader.get()));
  EXPECT_TRUE(indexed_rows("by_payload", 90, reader.get()).empty());
  for (int key : {2, 3, 9, 10}) {
    EXPECT_TRUE(indexed_rows("by_id", key, reader.get()).empty()) << key;
  }
  EXPECT_EQ(RC::SUCCESS, reader->commit());
}

TEST_P(UpdateTest, ValidatesAndOwnsAssignmentsBeforeScanningEmptyTable)
{
  vector<AttrInfoSqlNode> fields(3);
  fields[0].name = "born";
  fields[0].type = AttrType::DATES;
  fields[0].length = sizeof(int);
  fields[1].name = "score";
  fields[1].type = AttrType::FLOATS;
  fields[1].length = sizeof(float);
  fields[2].name = "label";
  fields[2].type = AttrType::CHARS;
  fields[2].length = 16;
  ASSERT_EQ(RC::SUCCESS, db_->create_table("empty_values", fields, {}));

  for (const char *sql : {
      "update absent set born = '2000-2-29';",
      "update empty_values set absent = 1;",
      "update empty_values set born = '1900-2-29';",
      "update empty_values set born = '2100-2-29' where score = 999;",
      "update empty_values set score = 'bad';",
      "update empty_values set label = 1;",
      "update empty_values set born = '2000-2-29' where absent = 1;",
      "update empty_values set score = 2 where born = '2000-2-30';",
  }) {
    SCOPED_TRACE(sql);
    ParsedSqlResult parsed;
    ASSERT_EQ(RC::SUCCESS, parse(sql, &parsed));
    ASSERT_EQ(1U, parsed.sql_nodes().size());
    Stmt *stmt = nullptr;
    RC rc = Stmt::create_stmt(db_.get(), *parsed.sql_nodes().front(), stmt);
    if (rc == RC::SUCCESS) {
      LogicalPlanGenerator generator;
      unique_ptr<LogicalOperator> plan;
      rc = generator.create(stmt, plan);
    }
    EXPECT_NE(RC::SUCCESS, rc);
    delete stmt;
  }

  unique_ptr<Stmt> owned;
  {
    ParsedSqlResult parsed;
    ASSERT_EQ(RC::SUCCESS, parse("update empty_values set born = '2400-2-29';", &parsed));
    ASSERT_EQ(1U, parsed.sql_nodes().size());
    Stmt *stmt = nullptr;
    ASSERT_EQ(RC::SUCCESS, Stmt::create_stmt(db_.get(), *parsed.sql_nodes().front(), stmt));
    owned.reset(stmt);
  }
  auto *date_update = static_cast<UpdateStmt *>(owned.get());
  ASSERT_EQ(AttrType::DATES, date_update->value().attr_type());
  EXPECT_EQ("2400-02-29", date_update->value().to_string());

  UpdateSqlNode numeric;
  numeric.relation_name = "empty_values";
  numeric.attribute_name = "score";
  numeric.value = Value(7);
  Stmt *stmt = nullptr;
  ASSERT_EQ(RC::SUCCESS, UpdateStmt::create(db_.get(), numeric, stmt));
  owned.reset(stmt);
  auto *float_update = static_cast<UpdateStmt *>(owned.get());
  EXPECT_EQ(AttrType::FLOATS, float_update->value().attr_type());
  EXPECT_FLOAT_EQ(7.0F, float_update->value().get_float());
}

INSTANTIATE_TEST_SUITE_P(TransactionModels, UpdateTest, testing::Values("vacuous", "mvcc"));

}  // namespace

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  LoggerFactory::init_default("update_test.log", LOG_LEVEL_TRACE);
  return RUN_ALL_TESTS();
}
