/* Copyright (c) 2026 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2. */

#include <cstring>
#include <sstream>
#include <utility>

#include "common/type/vector_type.h"
#include "common/value.h"
#include "gtest/gtest.h"
#include "storage/table/table_meta.h"

TEST(VectorTypeTest, ParsesDecimalAndScientificComponents)
{
  VectorType type;
  Value value;
  ASSERT_EQ(RC::SUCCESS, type.set_value_from_str(value, " \t[1, -2.5, +3e2, .125, -0]\n"));
  EXPECT_EQ(vector<float>({1.0F, -2.5F, 300.0F, 0.125F, -0.0F}), value.get_vector());
  EXPECT_EQ(5 * sizeof(float), static_cast<size_t>(value.length()));
  EXPECT_EQ("[1.00000e+00,-2.50000e+00,3.00000e+02,1.25000e-01,-0.00000e+00]", value.to_string());
}

TEST(VectorTypeTest, RejectsMalformedNonfiniteAndOutOfRangeInput)
{
  VectorType type;
  for (const char *text : {"", "[]", "[ ]", "1,2", "[1,]", "[,1]", "[1,,2]", "[1 2]", "[1;2]",
           "[1]x", "x[1]", "[1", "[nan]", "[NaN]", "[inf]", "[-Infinity]", "[1e99]", "[0x1p2]",
           "[1.2.3]", "[1e]", "[--1]", "[+ 1]"}) {
    SCOPED_TRACE(text);
    Value value(19);
    EXPECT_NE(RC::SUCCESS, type.set_value_from_str(value, text));
    EXPECT_EQ(AttrType::INTS, value.attr_type());
    EXPECT_EQ(19, value.get_int());
  }
}

TEST(VectorTypeTest, EnforcesDimensionLimit)
{
  VectorType type;
  string text = "[0";
  for (int i = 1; i < VectorType::MAX_DIMENSIONS; ++i) {
    text += ",0";
  }
  Value value;
  ASSERT_EQ(RC::SUCCESS, type.set_value_from_str(value, text + "]"));
  EXPECT_EQ(VectorType::MAX_DIMENSIONS, value.get_vector().size());
  EXPECT_NE(RC::SUCCESS, type.set_value_from_str(value, text + ",0]"));
}

TEST(VectorTypeTest, OwnsCopiesMovesAndBinaryPayload)
{
  Value original;
  original.set_vector({0.0F, -4.5F, 32.0F});
  Value copied(original);
  Value assigned("old string");
  assigned = original;
  Value moved(std::move(copied));
  Value move_assigned("old string");
  move_assigned = std::move(assigned);
  Value restored(AttrType::VECTORS, original.data(), original.length());
  original.set_vector(original.get_vector());

  for (const Value *value : {&moved, &move_assigned, &restored, &original}) {
    EXPECT_EQ(AttrType::VECTORS, value->attr_type());
    EXPECT_EQ(vector<float>({0.0F, -4.5F, 32.0F}), value->get_vector());
    EXPECT_EQ(0, std::memcmp(value->data(), original.data(), original.length()));
  }
  original.set_vector({9.0F});
  EXPECT_EQ(vector<float>({0.0F, -4.5F, 32.0F}), restored.get_vector());
  Value set;
  set.set_value(restored);
  restored.reset();
  EXPECT_EQ(vector<float>({0.0F, -4.5F, 32.0F}), set.get_vector());
}

TEST(VectorTypeTest, ComparesOnlyExactlyEqualComponents)
{
  Value a;
  a.set_vector({1.0F, 2.0F});
  Value b;
  b.set_vector({1.0F, 2.0F});
  EXPECT_EQ(0, a.compare(b));
  b.set_vector({1.0F, 2.000001F});
  EXPECT_NE(0, a.compare(b));
  b.set_vector({1.0F});
  EXPECT_NE(0, a.compare(b));
  EXPECT_EQ(INT32_MAX, a.compare(Value(1)));
}

TEST(VectorTypeTest, MetadataPersistsDimensionsSeparatelyFromRecordLength)
{
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
  const vector<AttrInfoSqlNode> attrs = {id, embedding, tail};
  TableMeta meta;
  ASSERT_EQ(RC::SUCCESS,
      meta.init(1, "test", nullptr, attrs, {}, StorageFormat::ROW_FORMAT, StorageEngine::HEAP));
  EXPECT_EQ(VectorType::MAX_DIMENSIONS, meta.field("embedding")->vector_dim());
  EXPECT_EQ(VectorType::STORAGE_SIZE, meta.field("embedding")->len());
  EXPECT_EQ(sizeof(int) + VectorType::STORAGE_SIZE, meta.field("tail")->offset());
  EXPECT_EQ(2 * sizeof(int) + VectorType::STORAGE_SIZE, meta.record_size());

  std::stringstream serialized;
  ASSERT_GT(meta.serialize(serialized), 0);
  TableMeta restored;
  ASSERT_GT(restored.deserialize(serialized), 0);
  EXPECT_EQ(VectorType::MAX_DIMENSIONS, restored.field("embedding")->vector_dim());
  EXPECT_EQ(VectorType::STORAGE_SIZE, restored.field("embedding")->len());
  EXPECT_EQ(meta.record_size(), restored.record_size());

  for (int invalid : {0, -1, VectorType::MAX_DIMENSIONS + 1}) {
    embedding.length = invalid;
    const vector<AttrInfoSqlNode> invalid_attrs = {embedding};
    TableMeta invalid_meta;
    EXPECT_NE(RC::SUCCESS,
        invalid_meta.init(2, "bad", nullptr, invalid_attrs, {}, StorageFormat::ROW_FORMAT, StorageEngine::HEAP));
  }
}
