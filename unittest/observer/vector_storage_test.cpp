/* Copyright (c) 2026 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2. */

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <utility>

#include "common/log/log.h"
#include "common/type/vector_type.h"
#include "common/value.h"
#include "gtest/gtest.h"
#include "storage/common/meta_util.h"
#include "storage/db/db.h"
#include "storage/index/index.h"
#include "storage/record/record_scanner.h"
#include "storage/table/table.h"
#include "storage/trx/trx.h"

namespace {

using TrxPtr = std::unique_ptr<Trx, std::function<void(Trx *)>>;

class VectorStorageTest : public testing::TestWithParam<const char *>
{
protected:
  void SetUp() override
  {
    char directory_template[] = "/tmp/miniob_vector_XXXXXX";
    const char *directory = mkdtemp(directory_template);
    ASSERT_NE(nullptr, directory);
    path_ = directory;
    ASSERT_EQ(RC::SUCCESS, reopen());
    AttrInfoSqlNode id;
    id.name = "id";
    id.type = AttrType::INTS;
    id.length = sizeof(int);
    AttrInfoSqlNode embedding;
    embedding.name = "embedding";
    embedding.type = AttrType::VECTORS;
    embedding.length = VectorType::MAX_DIMENSIONS;
    AttrInfoSqlNode tail = id;
    tail.name = "tail";
    AttrInfoSqlNode auxiliary = embedding;
    auxiliary.name = "auxiliary";
    auxiliary.length = 3;
    const vector<AttrInfoSqlNode> fields = {id, embedding, tail, auxiliary};
    ASSERT_EQ(RC::SUCCESS, db_->create_table("vectors", fields, {}));
  }

  void TearDown() override
  {
    db_.reset();
    if (!path_.empty()) {
      std::filesystem::remove_all(path_);
    }
  }

  Table *table() { return db_->find_table("vectors"); }

  RC reopen()
  {
    db_.reset();
    db_ = std::make_unique<Db>();
    return db_->init("vector_storage", path_.c_str(), GetParam(), "disk");
  }

  TrxPtr begin()
  {
    TrxKit *kit = &db_->trx_kit();
    TrxPtr trx(kit->create_trx(db_->log_handler()), [kit](Trx *trx) { kit->destroy_trx(trx); });
    EXPECT_NE(nullptr, trx);
    EXPECT_EQ(RC::SUCCESS, trx->start_if_need());
    return trx;
  }

  vector<float> payload(float shift = 0)
  {
    vector<float> result(VectorType::MAX_DIMENSIONS);
    for (size_t i = 0; i < result.size(); ++i) {
      result[i] = static_cast<float>(i) / 8 - 1000 + shift;
    }
    return result;
  }

  RC insert()
  {
    Value large;
    large.set_vector(payload());
    Value small;
    small.set_vector({0.0F, 1.25F, -2.5F});
    const Value values[] = {Value(7), large, Value(19), small};
    Record record;
    RC rc = table()->make_record(4, values, record);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    auto trx = begin();
    rc = trx->insert_record(table(), record);
    return rc == RC::SUCCESS ? trx->commit() : rc;
  }

  vector<Record> records(Trx *trx)
  {
    RecordScanner *raw = nullptr;
    EXPECT_EQ(RC::SUCCESS, table()->get_record_scanner(raw, trx, ReadWriteMode::READ_ONLY));
    std::unique_ptr<RecordScanner> scanner(raw);
    vector<Record> rows;
    if (!scanner) {
      return rows;
    }
    Record record;
    RC rc;
    while ((rc = scanner->next(record)) == RC::SUCCESS) {
      Record copy;
      EXPECT_EQ(RC::SUCCESS, copy.copy_data(record.data(), record.len()));
      copy.set_rid(record.rid());
      rows.push_back(std::move(copy));
    }
    EXPECT_EQ(RC::RECORD_EOF, rc);
    return rows;
  }

  void expect_row(const Record &record, float shift = 0)
  {
    Value value;
    ASSERT_EQ(RC::SUCCESS, table()->get_value_from_record(record.data(), *table()->table_meta().field("id"), value));
    EXPECT_EQ(7, value.get_int());
    ASSERT_EQ(RC::SUCCESS,
        table()->get_value_from_record(record.data(), *table()->table_meta().field("embedding"), value));
    EXPECT_EQ(payload(shift), value.get_vector());
    ASSERT_EQ(RC::SUCCESS, table()->get_value_from_record(record.data(), *table()->table_meta().field("tail"), value));
    EXPECT_EQ(19, value.get_int());
    ASSERT_EQ(RC::SUCCESS,
        table()->get_value_from_record(record.data(), *table()->table_meta().field("auxiliary"), value));
    EXPECT_EQ(vector<float>({0.0F, 1.25F, -2.5F}), value.get_vector());
  }

  std::filesystem::path path_;
  std::unique_ptr<Db> db_;
};

TEST_P(VectorStorageTest, LargeVectorsSurviveRecordLogReplayAndDropReclaimsLob)
{
  EXPECT_LT(table()->table_meta().record_size(), 128);
  ASSERT_EQ(RC::SUCCESS, insert());
  // Reopen without a checkpoint: committed record logs reference durable LOB bytes.
  ASSERT_EQ(RC::SUCCESS, reopen());
  {
    auto reader = begin();
    auto rows = records(reader.get());
    ASSERT_EQ(1U, rows.size());
    expect_row(rows.front());
    ASSERT_EQ(RC::SUCCESS, reader->commit());
  }
  const string lob_file = table_lob_file(path_.c_str(), "vectors");
  EXPECT_TRUE(std::filesystem::exists(lob_file));
  EXPECT_EQ((VectorType::MAX_DIMENSIONS + 3) * sizeof(float), std::filesystem::file_size(lob_file));
  ASSERT_EQ(RC::SUCCESS, db_->drop_table("vectors"));
  EXPECT_FALSE(std::filesystem::exists(lob_file));
  ASSERT_EQ(RC::SUCCESS, reopen());
  EXPECT_EQ(nullptr, table());
}

TEST_P(VectorStorageTest, UpdatesOwnNewPayloadAndKeepOldPayloadForRollback)
{
  ASSERT_EQ(RC::SUCCESS, insert());
  ASSERT_EQ(RC::SUCCESS, db_->sync());
  {
    auto writer = begin();
    auto rows = records(writer.get());
    ASSERT_EQ(1U, rows.size());
    Record replacement = rows.front();
    Value changed;
    changed.set_vector(payload(8));
    ASSERT_EQ(RC::SUCCESS,
        table()->set_value_to_record(replacement.data(), changed, table()->table_meta().field("embedding")));
    ASSERT_EQ(RC::SUCCESS, writer->update_record(table(), rows.front(), replacement));
    auto visible = records(writer.get());
    ASSERT_EQ(1U, visible.size());
    expect_row(visible.front(), 8);
    if (string(GetParam()) == "mvcc") {
      ASSERT_EQ(RC::SUCCESS, writer->rollback());
    } else {
      ASSERT_EQ(RC::SUCCESS, writer->commit());
    }
  }
  ASSERT_EQ(RC::SUCCESS, reopen());
  auto reader = begin();
  auto rows = records(reader.get());
  ASSERT_EQ(1U, rows.size());
  expect_row(rows.front(), string(GetParam()) == "mvcc" ? 0 : 8);
  EXPECT_EQ(RC::SUCCESS, reader->commit());
}

TEST_P(VectorStorageTest, ExcessDimensionsAndEmptyVectorsWriteNoPayload)
{
  Value oversized;
  oversized.set_vector(vector<float>(VectorType::MAX_DIMENSIONS + 1, 1.0F));
  Value auxiliary;
  auxiliary.set_vector({1.0F, 2.0F, 3.0F});
  const Value values[] = {Value(1), oversized, Value(2), auxiliary};
  Record record;
  EXPECT_NE(RC::SUCCESS, table()->make_record(4, values, record));
  Value empty;
  empty.set_vector({});
  const Value empty_values[] = {Value(1), empty, Value(2), auxiliary};
  EXPECT_NE(RC::SUCCESS, table()->make_record(4, empty_values, record));
  EXPECT_EQ(0U, std::filesystem::file_size(table_lob_file(path_.c_str(), "vectors")));
}

TEST_P(VectorStorageTest, ShorterVectorsPersistAndCanBeUpdatedToAnotherLength)
{
  Value embedding;
  embedding.set_vector({1.25F, -4.0F});
  Value auxiliary;
  auxiliary.set_vector({3.0F});
  const Value values[] = {Value(7), embedding, Value(19), auxiliary};
  Record record;
  ASSERT_EQ(RC::SUCCESS, table()->make_record(4, values, record));
  {
    auto writer = begin();
    ASSERT_EQ(RC::SUCCESS, writer->insert_record(table(), record));
    ASSERT_EQ(RC::SUCCESS, writer->commit());
  }
  ASSERT_EQ(RC::SUCCESS, reopen());
  {
    auto writer = begin();
    auto rows = records(writer.get());
    ASSERT_EQ(1U, rows.size());
    Value actual;
    ASSERT_EQ(RC::SUCCESS,
        table()->get_value_from_record(rows.front().data(), *table()->table_meta().field("embedding"), actual));
    EXPECT_EQ(vector<float>({1.25F, -4.0F}), actual.get_vector());
    ASSERT_EQ(RC::SUCCESS,
        table()->get_value_from_record(rows.front().data(), *table()->table_meta().field("auxiliary"), actual));
    EXPECT_EQ(vector<float>({3.0F}), actual.get_vector());

    Record replacement = rows.front();
    Value shorter;
    shorter.set_vector({8.0F});
    ASSERT_EQ(RC::SUCCESS,
        table()->set_value_to_record(replacement.data(), shorter, table()->table_meta().field("embedding")));
    ASSERT_EQ(RC::SUCCESS, writer->update_record(table(), rows.front(), replacement));
    ASSERT_EQ(RC::SUCCESS, writer->commit());
  }
  ASSERT_EQ(RC::SUCCESS, reopen());
  auto reader = begin();
  auto rows = records(reader.get());
  ASSERT_EQ(1U, rows.size());
  Value actual;
  ASSERT_EQ(RC::SUCCESS,
      table()->get_value_from_record(rows.front().data(), *table()->table_meta().field("embedding"), actual));
  EXPECT_EQ(vector<float>({8.0F}), actual.get_vector());
  EXPECT_EQ(RC::SUCCESS, reader->commit());
}

TEST_P(VectorStorageTest, IndexBuildStartsAnUnstartedTransactionBeforeScanning)
{
  ASSERT_EQ(RC::SUCCESS, insert());
  TrxKit *kit = &db_->trx_kit();
  // Match CREATE INDEX's command-executor path: create_trx alone, no start.
  TrxPtr builder(kit->create_trx(db_->log_handler()), [kit](Trx *trx) { kit->destroy_trx(trx); });
  ASSERT_NE(nullptr, builder);
  ASSERT_EQ(RC::SUCCESS, table()->create_index(builder.get(), table()->table_meta().field("id"), "ordinary_id"));
  ASSERT_EQ(RC::SUCCESS, builder->commit());

  Index *index = table()->find_index("ordinary_id");
  ASSERT_NE(nullptr, index);
  const int key = 7;
  const char *key_data = reinterpret_cast<const char *>(&key);
  std::unique_ptr<IndexScanner> scanner(index->create_scanner(key_data, sizeof(key), true, key_data, sizeof(key), true));
  ASSERT_NE(nullptr, scanner);
  RID rid;
  ASSERT_EQ(RC::SUCCESS, scanner->next_entry(&rid));
  Record indexed;
  ASSERT_EQ(RC::SUCCESS, table()->get_record(rid, indexed));
  expect_row(indexed);
  EXPECT_EQ(RC::RECORD_EOF, scanner->next_entry(&rid));
}

INSTANTIATE_TEST_SUITE_P(TransactionModels, VectorStorageTest, testing::Values("vacuous", "mvcc"));

}  // namespace

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  common::LoggerFactory::init_default("vector_storage_test.log", common::LOG_LEVEL_TRACE);
  return RUN_ALL_TESTS();
}
