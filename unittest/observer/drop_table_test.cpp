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
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "catalog/catalog.h"
#include "common/log/log.h"
#include "storage/common/meta_util.h"
#include "storage/db/db.h"
#include "storage/index/index.h"
#include "storage/record/record_scanner.h"
#include "storage/table/table.h"
#include "storage/trx/trx.h"

using namespace std;
using namespace common;

namespace {

vector<AttrInfoSqlNode> int_fields()
{
  AttrInfoSqlNode id;
  id.name   = "id";
  id.type   = AttrType::INTS;
  id.length = sizeof(int);
  AttrInfoSqlNode payload = id;
  payload.name           = "payload";
  return {id, payload};
}

class DropTableTest : public testing::TestWithParam<const char *>
{
protected:
  void SetUp() override
  {
    char directory_template[] = "/tmp/miniob_drop_table_XXXXXX";
    const char *directory = mkdtemp(directory_template);
    ASSERT_NE(nullptr, directory);
    path_ = directory;
    db_   = make_unique<Db>();
    ASSERT_EQ(RC::SUCCESS, db_->init("drop_test", path_.c_str(), GetParam(), "disk"));
  }

  void TearDown() override
  {
    db_.reset();
    if (!path_.empty()) {
      filesystem::remove_all(path_);
    }
  }

  RC reopen()
  {
    db_.reset();
    db_ = make_unique<Db>();
    return db_->init("drop_test", path_.c_str(), GetParam(), "disk");
  }

  RC insert(Table *table, const vector<Value> &values)
  {
    TrxKit &kit = db_->trx_kit();
    unique_ptr<Trx, function<void(Trx *)>> trx(
        kit.create_trx(db_->log_handler()), [&kit](Trx *trx) { kit.destroy_trx(trx); });
    if (!trx) {
      return RC::INTERNAL;
    }
    RC rc = trx->start_if_need();
    Record record;
    if (rc == RC::SUCCESS) {
      rc = table->make_record(values.size(), values.data(), record);
    }
    if (rc == RC::SUCCESS) {
      rc = trx->insert_record(table, record);
    }
    if (rc == RC::SUCCESS) {
      return trx->commit();
    }
    trx->rollback();
    return rc;
  }

  vector<int> record_ids(Table *table)
  {
    vector<int> ids;
    EXPECT_NE(nullptr, table);
    if (table == nullptr) {
      return ids;
    }
    const FieldMeta *field = table->table_meta().field("id");
    EXPECT_NE(nullptr, field);
    if (field == nullptr) {
      return ids;
    }
    TrxKit &kit = db_->trx_kit();
    unique_ptr<Trx, function<void(Trx *)>> trx(
        kit.create_trx(db_->log_handler()), [&kit](Trx *trx) { kit.destroy_trx(trx); });
    EXPECT_NE(nullptr, trx);
    if (!trx) {
      return ids;
    }
    RC rc = trx->start_if_need();
    EXPECT_EQ(RC::SUCCESS, rc);
    if (rc != RC::SUCCESS) {
      return ids;
    }
    RecordScanner *raw_scanner = nullptr;
    rc = table->get_record_scanner(raw_scanner, trx.get(), ReadWriteMode::READ_ONLY);
    EXPECT_EQ(RC::SUCCESS, rc);
    unique_ptr<RecordScanner> scanner(raw_scanner);
    if (rc != RC::SUCCESS || !scanner) {
      EXPECT_EQ(RC::SUCCESS, trx->rollback());
      return ids;
    }
    Record record;
    while ((rc = scanner->next(record)) == RC::SUCCESS) {
      int id = 0;
      memcpy(&id, record.data() + field->offset(), sizeof(id));
      ids.push_back(id);
    }
    EXPECT_EQ(RC::RECORD_EOF, rc);
    scanner.reset();
    EXPECT_EQ(RC::SUCCESS, trx->commit());
    sort(ids.begin(), ids.end());
    return ids;
  }

  vector<int> indexed_ids(Table *table, int key)
  {
    vector<int> ids;
    EXPECT_NE(nullptr, table);
    if (table == nullptr) {
      return ids;
    }
    Index *index = table->find_index("by_id");
    EXPECT_NE(nullptr, index);
    if (index == nullptr) {
      return ids;
    }
    TrxKit &kit = db_->trx_kit();
    unique_ptr<Trx, function<void(Trx *)>> trx(
        kit.create_trx(db_->log_handler()), [&kit](Trx *trx) { kit.destroy_trx(trx); });
    EXPECT_NE(nullptr, trx);
    if (!trx) {
      return ids;
    }
    RC rc = trx->start_if_need();
    EXPECT_EQ(RC::SUCCESS, rc);
    if (rc != RC::SUCCESS) {
      return ids;
    }
    const char *key_data = reinterpret_cast<const char *>(&key);
    unique_ptr<IndexScanner> scanner(index->create_scanner(key_data, sizeof(key), true, key_data, sizeof(key), true));
    EXPECT_NE(nullptr, scanner);
    if (!scanner) {
      EXPECT_EQ(RC::SUCCESS, trx->rollback());
      return ids;
    }
    RID rid;
    while ((rc = scanner->next_entry(&rid)) == RC::SUCCESS) {
      Record record;
      RC record_rc = table->get_record(rid, record);
      EXPECT_EQ(RC::SUCCESS, record_rc);
      if (record_rc != RC::SUCCESS) {
        break;
      }
      record_rc = trx->visit_record(table, record, ReadWriteMode::READ_ONLY);
      if (record_rc == RC::RECORD_INVISIBLE) {
        continue;
      }
      EXPECT_EQ(RC::SUCCESS, record_rc);
      if (record_rc != RC::SUCCESS) {
        break;
      }
      int id = 0;
      memcpy(&id, record.data() + table->table_meta().field("id")->offset(), sizeof(id));
      ids.push_back(id);
    }
    EXPECT_EQ(RC::RECORD_EOF, rc);
    scanner.reset();
    EXPECT_EQ(RC::SUCCESS, trx->commit());
    sort(ids.begin(), ids.end());
    return ids;
  }

  int32_t buffer_pool_id(const string &filename)
  {
    ifstream file(filename, ios::binary);
    Page page{};
    file.read(reinterpret_cast<char *>(&page), sizeof(page));
    EXPECT_EQ(static_cast<streamsize>(sizeof(page)), file.gcount()) << filename;
    int32_t id = -1;
    memcpy(&id, page.data, sizeof(id));
    return id;
  }

  void check_restart(bool create_index_last)
  {
    const auto attributes = int_fields();
    ASSERT_EQ(RC::SUCCESS, db_->create_table("kept", attributes, {}));
    Table *kept = db_->find_table("kept");
    ASSERT_NE(nullptr, kept);
    ASSERT_EQ(RC::SUCCESS, insert(kept, {Value(1), Value(10)}));
    ASSERT_EQ(RC::SUCCESS, db_->create_table("removed", attributes, {}));
    Table *removed = db_->find_table("removed");
    ASSERT_NE(nullptr, removed);
    const int32_t removed_id = removed->table_id();
    ASSERT_EQ(RC::SUCCESS, insert(removed, {Value(17), Value(170)}));
    if (create_index_last) {
      ASSERT_EQ(RC::SUCCESS, removed->create_index(nullptr, removed->table_meta().field("id"), "by_id"));
    }

    // With vacuous transactions, the final WAL entry references either the removed
    // record file or its new B+ tree. Recovery must exclude the checkpoint itself.
    const LSN before_drop = db_->log_handler().current_lsn();
    ASSERT_GT(before_drop, 0);
    ASSERT_EQ(RC::SUCCESS, db_->drop_table("removed"));
    ASSERT_EQ(RC::SUCCESS, reopen());
    EXPECT_EQ(nullptr, db_->find_table("removed"));
    EXPECT_EQ(nullptr, db_->find_table(removed_id));
    EXPECT_EQ(vector<int>({1}), record_ids(db_->find_table("kept")));
    EXPECT_GE(db_->log_handler().current_lsn(), before_drop);

    // New work after recovery must use later LSNs, including when a deleted name
    // and index name are reused. A second recovery must retain only fresh rows.
    ASSERT_EQ(RC::SUCCESS, db_->create_table("removed", attributes, {}));
    removed = db_->find_table("removed");
    ASSERT_NE(nullptr, removed);
    ASSERT_EQ(RC::SUCCESS, removed->create_index(nullptr, removed->table_meta().field("id"), "by_id"));
    ASSERT_EQ(RC::SUCCESS, insert(removed, {Value(91), Value(910)}));
    ASSERT_EQ(RC::SUCCESS, insert(db_->find_table("kept"), {Value(2), Value(20)}));
    EXPECT_GT(db_->log_handler().current_lsn(), before_drop);
    ASSERT_EQ(RC::SUCCESS, reopen());
    EXPECT_EQ(vector<int>({1, 2}), record_ids(db_->find_table("kept")));
    removed = db_->find_table("removed");
    EXPECT_EQ(vector<int>({91}), record_ids(removed));
    EXPECT_EQ(vector<int>({91}), indexed_ids(removed, 91));
    EXPECT_TRUE(indexed_ids(removed, 17).empty());
    ASSERT_EQ(RC::SUCCESS, db_->drop_table("removed"));
    ASSERT_EQ(RC::SUCCESS, reopen());
    EXPECT_EQ(nullptr, db_->find_table("removed"));
    EXPECT_EQ(vector<int>({1, 2}), record_ids(db_->find_table("kept")));
  }

  filesystem::path path_;
  unique_ptr<Db> db_;
};

TEST_P(DropTableTest, RemovesFilesBufferPoolsAndCatalogEntry)
{
  const auto attributes = int_fields();
  ASSERT_EQ(RC::SUCCESS, db_->create_table("removed", attributes, {}));
  ASSERT_EQ(RC::SUCCESS, db_->create_table("removed_extra", attributes, {}));
  Table *removed = db_->find_table("removed");
  Table *kept    = db_->find_table("removed_extra");
  ASSERT_NE(nullptr, removed);
  ASSERT_NE(nullptr, kept);
  const int32_t removed_id = removed->table_id();
  ASSERT_EQ(RC::SUCCESS, insert(removed, {Value(1), Value(11)}));
  ASSERT_EQ(RC::SUCCESS, insert(kept, {Value(7), Value(77)}));
  ASSERT_EQ(RC::SUCCESS, removed->create_index(nullptr, removed->table_meta().field("id"), "by_id"));
  ASSERT_EQ(RC::SUCCESS, removed->create_index(nullptr, removed->table_meta().field("payload"), "by_payload"));
  ASSERT_EQ(RC::SUCCESS, kept->create_index(nullptr, kept->table_meta().field("id"), "by_id"));
  ASSERT_EQ(RC::SUCCESS, db_->sync());

  const string meta = table_meta_file(path_.c_str(), "removed");
  vector<string> buffer_files = {
      table_data_file(path_.c_str(), "removed"),
      table_index_file(path_.c_str(), "removed", "by_id"),
      table_index_file(path_.c_str(), "removed", "by_payload"),
  };
  vector<int32_t> pool_ids;
  for (const string &filename : buffer_files) {
    ASSERT_TRUE(filesystem::exists(filename));
    int32_t id = buffer_pool_id(filename);
    DiskBufferPool *pool = nullptr;
    ASSERT_EQ(RC::SUCCESS, db_->buffer_pool_manager().get_buffer_pool(id, pool));
    ASSERT_NE(nullptr, pool);
    EXPECT_EQ(filename, pool->filename());
    pool_ids.push_back(id);
  }
  const string lob = table_lob_file(path_.c_str(), "removed");
  // These optional resources can remain after TEXT use or an interrupted index
  // metadata rewrite; they are still owned by the dropped table.
  ofstream(lob) << "large object fixture";
  ofstream(meta + ".tmp") << "temporary metadata fixture";
  Catalog::get_instance().update_table_stats(removed_id, TableStats(123));
  ASSERT_EQ(123, Catalog::get_instance().get_table_stats(removed_id).row_nums);

  ASSERT_EQ(RC::SUCCESS, db_->drop_table("removed"));
  EXPECT_EQ(nullptr, db_->find_table("removed"));
  EXPECT_EQ(nullptr, db_->find_table(removed_id));
  vector<string> names;
  db_->all_tables(names);
  EXPECT_EQ(vector<string>({"removed_extra"}), names);
  EXPECT_FALSE(filesystem::exists(meta));
  EXPECT_FALSE(filesystem::exists(meta + ".tmp"));
  EXPECT_FALSE(filesystem::exists(lob));
  for (const string &filename : buffer_files) {
    EXPECT_FALSE(filesystem::exists(filename)) << filename;
  }
  for (int32_t id : pool_ids) {
    DiskBufferPool *pool = nullptr;
    EXPECT_NE(RC::SUCCESS, db_->buffer_pool_manager().get_buffer_pool(id, pool));
    EXPECT_EQ(nullptr, pool);
  }
  EXPECT_EQ(0, Catalog::get_instance().get_table_stats(removed_id).row_nums);
  EXPECT_EQ(kept, db_->find_table("removed_extra"));
  EXPECT_EQ(vector<int>({7}), indexed_ids(kept, 7));
  ASSERT_EQ(RC::SUCCESS, insert(kept, {Value(8), Value(88)}));
  EXPECT_EQ(vector<int>({7, 8}), record_ids(kept));
  EXPECT_EQ(RC::SCHEMA_TABLE_NOT_EXIST, db_->drop_table("removed"));
  EXPECT_EQ(RC::SCHEMA_TABLE_NOT_EXIST, db_->drop_table("missing"));
}

TEST_P(DropTableTest, RecreatesFreshSchemaAndIndex)
{
  ASSERT_EQ(RC::SUCCESS, db_->create_table("reused", int_fields(), {}));
  Table *table = db_->find_table("reused");
  ASSERT_NE(nullptr, table);
  const int32_t old_id = table->table_id();
  ASSERT_EQ(RC::SUCCESS, insert(table, {Value(1), Value(100)}));
  ASSERT_EQ(RC::SUCCESS, table->create_index(nullptr, table->table_meta().field("id"), "by_id"));
  ASSERT_EQ(RC::SUCCESS, db_->drop_table("reused"));

  auto attributes       = int_fields();
  attributes[0].name    = "label";
  attributes[0].type    = AttrType::CHARS;
  attributes[0].length  = 16;
  attributes[1].name    = "id";
  ASSERT_EQ(RC::SUCCESS, db_->create_table("reused", attributes, {}));
  table = db_->find_table("reused");
  ASSERT_NE(nullptr, table);
  EXPECT_NE(old_id, table->table_id());
  EXPECT_EQ(nullptr, db_->find_table(old_id));
  EXPECT_EQ(nullptr, table->table_meta().field("payload"));
  ASSERT_NE(nullptr, table->table_meta().field("label"));
  EXPECT_EQ(AttrType::CHARS, table->table_meta().field("label")->type());
  EXPECT_EQ(16, table->table_meta().field("label")->len());
  EXPECT_TRUE(record_ids(table).empty());
  ASSERT_EQ(RC::SUCCESS, table->create_index(nullptr, table->table_meta().field("id"), "by_id"));
  ASSERT_EQ(RC::SUCCESS, insert(table, {Value("fresh"), Value(9)}));
  EXPECT_EQ(vector<int>({9}), indexed_ids(table, 9));
  EXPECT_TRUE(indexed_ids(table, 1).empty());
}

TEST_P(DropTableTest, RestartsAfterDroppingLastInsertedTable) { check_restart(false); }

TEST_P(DropTableTest, RestartsAfterDroppingLastIndexedTable) { check_restart(true); }

TEST_P(DropTableTest, RecoversLegacyCheckpointMetadata)
{
  const auto attributes = int_fields();
  ASSERT_EQ(RC::SUCCESS, db_->create_table("keeper", attributes, {}));
  Table *keeper = db_->find_table("keeper");
  ASSERT_NE(nullptr, keeper);
  ASSERT_EQ(RC::SUCCESS, insert(keeper, {Value(7), Value(70)}));
  ASSERT_EQ(RC::SUCCESS, keeper->create_index(nullptr, keeper->table_meta().field("id"), "by_id"));
  ASSERT_EQ(RC::SUCCESS, db_->create_table("removed", attributes, {}));
  Table *removed = db_->find_table("removed");
  ASSERT_NE(nullptr, removed);
  ASSERT_EQ(RC::SUCCESS, insert(removed, {Value(17), Value(170)}));
  ASSERT_EQ(RC::SUCCESS, removed->create_index(nullptr, removed->table_meta().field("id"), "by_id"));
  ASSERT_EQ(RC::SUCCESS, db_->drop_table("removed"));
  db_.reset();

  // Existing databases stored only the checkpoint. Transaction IDs must be
  // recovered without replaying historical operations on the dropped files.
  const string metadata_file = db_meta_file(path_.c_str(), "drop_test");
  LSN checkpoint = 0;
  int32_t trx_id = 0;
  {
    ifstream metadata(metadata_file);
    ASSERT_TRUE(static_cast<bool>(metadata >> checkpoint >> trx_id));
    ASSERT_GT(checkpoint, 0);
  }
  {
    ofstream metadata(metadata_file, ios::trunc);
    metadata << checkpoint;
    metadata.close();
    ASSERT_FALSE(metadata.fail());
  }

  ASSERT_EQ(RC::SUCCESS, reopen());
  EXPECT_EQ(nullptr, db_->find_table("removed"));
  EXPECT_EQ(vector<int>({7}), record_ids(db_->find_table("keeper")));
  EXPECT_EQ(vector<int>({7}), indexed_ids(db_->find_table("keeper"), 7));
  ASSERT_EQ(RC::SUCCESS, insert(db_->find_table("keeper"), {Value(8), Value(80)}));
  ASSERT_EQ(RC::SUCCESS, reopen());
  EXPECT_EQ(nullptr, db_->find_table("removed"));
  EXPECT_EQ(vector<int>({7, 8}), record_ids(db_->find_table("keeper")));
  EXPECT_EQ(vector<int>({8}), indexed_ids(db_->find_table("keeper"), 8));
}

INSTANTIATE_TEST_SUITE_P(TransactionModels, DropTableTest, testing::Values("vacuous", "mvcc"));

}  // namespace

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  LoggerFactory::init_default("drop_table_test.log", LOG_LEVEL_TRACE);
  return RUN_ALL_TESTS();
}
