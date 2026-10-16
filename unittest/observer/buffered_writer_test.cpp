/* Copyright (c) 2026 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2. */

#include <cstdio>
#include <memory>
#include <string>
#include <unistd.h>

#include "gtest/gtest.h"
#include "event/session_event.h"
#include "net/buffered_writer.h"
#include "net/plain_communicator.h"
#include "session/session.h"

using namespace std;

namespace {

string read_contents(int fd)
{
  if (lseek(fd, 0, SEEK_SET) == -1) {
    return {};
  }
  string result;
  char buffer[16384];
  ssize_t count;
  while ((count = ::read(fd, buffer, sizeof(buffer))) > 0) {
    result.append(buffer, count);
  }
  return result;
}

using TemporaryFile = unique_ptr<FILE, decltype(&fclose)>;

}  // namespace

TEST(BufferedWriterTest, stage_small_response_until_commit)
{
  TemporaryFile file(tmpfile(), &fclose);
  ASSERT_NE(file, nullptr);
  int fd = fileno(file.get());
  BufferedWriter writer(fd, 8);
  const string response("id | value\n1 | result\n\0", 23);

  ASSERT_EQ(writer.begin_staging(), RC::SUCCESS);
  ASSERT_EQ(writer.writen(response.data(), response.size()), RC::SUCCESS);
  ASSERT_EQ(writer.flush(), RC::SUCCESS);
  EXPECT_EQ(lseek(fd, 0, SEEK_CUR), 0);
  ASSERT_EQ(writer.commit_staging(), RC::SUCCESS);
  ASSERT_EQ(writer.flush(), RC::SUCCESS);
  EXPECT_EQ(read_contents(fd), response);
}

TEST(BufferedWriterTest, discard_partial_response_before_failure)
{
  TemporaryFile file(tmpfile(), &fclose);
  ASSERT_NE(file, nullptr);
  int fd = fileno(file.get());
  BufferedWriter writer(fd, 8);

  ASSERT_EQ(writer.begin_staging(), RC::SUCCESS);
  ASSERT_EQ(writer.writen("header\n1\n2 | ", 13), RC::SUCCESS);
  writer.discard_staging();
  ASSERT_EQ(writer.begin_staging(), RC::SUCCESS);
  const string failure("FAILURE\n\0", 9);
  ASSERT_EQ(writer.writen(failure.data(), failure.size()), RC::SUCCESS);
  ASSERT_EQ(writer.commit_staging(), RC::SUCCESS);
  ASSERT_EQ(writer.flush(), RC::SUCCESS);
  EXPECT_EQ(read_contents(fd), failure);
}

TEST(BufferedWriterTest, large_response_spills_and_commits_in_order)
{
  TemporaryFile file(tmpfile(), &fclose);
  ASSERT_NE(file, nullptr);
  int fd = fileno(file.get());
  BufferedWriter writer(fd, 4096);
  string first(700000, 'a');
  string second(1500000, 'b');
  second[1024000] = '\0';

  ASSERT_EQ(writer.begin_staging(), RC::SUCCESS);
  ASSERT_EQ(writer.writen(first.data(), first.size()), RC::SUCCESS);
  ASSERT_EQ(writer.writen(second.data(), second.size()), RC::SUCCESS);
  ASSERT_EQ(writer.flush(), RC::SUCCESS);
  EXPECT_EQ(lseek(fd, 0, SEEK_CUR), 0);
  ASSERT_EQ(writer.commit_staging(), RC::SUCCESS);
  ASSERT_EQ(writer.flush(), RC::SUCCESS);
  EXPECT_EQ(read_contents(fd), first + second);
}

TEST(BufferedWriterTest, large_response_can_be_discarded_and_writer_reused)
{
  TemporaryFile file(tmpfile(), &fclose);
  ASSERT_NE(file, nullptr);
  int fd = fileno(file.get());
  BufferedWriter writer(fd, 16);
  string large(2 * 1024 * 1024, 'x');

  ASSERT_EQ(writer.begin_staging(), RC::SUCCESS);
  ASSERT_EQ(writer.writen(large.data(), large.size()), RC::SUCCESS);
  writer.discard_staging();
  EXPECT_EQ(lseek(fd, 0, SEEK_CUR), 0);
  ASSERT_EQ(writer.writen("next result", 11), RC::SUCCESS);
  ASSERT_EQ(writer.flush(), RC::SUCCESS);
  EXPECT_EQ(read_contents(fd), "next result");
}

TEST(BufferedWriterTest, staging_keeps_previous_output_and_validates_lifecycle)
{
  TemporaryFile file(tmpfile(), &fclose);
  ASSERT_NE(file, nullptr);
  int fd = fileno(file.get());
  BufferedWriter writer(fd, 32);

  EXPECT_EQ(writer.commit_staging(), RC::INVALID_ARGUMENT);
  ASSERT_EQ(writer.writen("previous\n", 9), RC::SUCCESS);
  ASSERT_EQ(writer.begin_staging(), RC::SUCCESS);
  EXPECT_EQ(lseek(fd, 0, SEEK_CUR), 9);
  EXPECT_EQ(writer.begin_staging(), RC::INVALID_ARGUMENT);
  ASSERT_EQ(writer.writen("discarded", 9), RC::SUCCESS);
  ASSERT_EQ(writer.close(), RC::SUCCESS);
  EXPECT_EQ(read_contents(fd), "previous\n");
  EXPECT_EQ(writer.begin_staging(), RC::INVALID_ARGUMENT);
}

TEST(PlainCommunicatorTest, reads_requests_across_initial_buffer_boundary)
{
  for (size_t length : {8191U, 8192U, 65536U}) {
    SCOPED_TRACE(length);
    TemporaryFile file(tmpfile(), &fclose);
    ASSERT_NE(file, nullptr);
    string query(length, 'a');
    ASSERT_EQ(fwrite(query.c_str(), 1, query.size() + 1, file.get()), query.size() + 1);
    ASSERT_EQ(fflush(file.get()), 0);
    ASSERT_EQ(fseek(file.get(), 0, SEEK_SET), 0);

    int fd = dup(fileno(file.get()));
    ASSERT_GE(fd, 0);
    PlainCommunicator communicator;
    ASSERT_EQ(communicator.init(fd, unique_ptr<Session>(), "unit test"), RC::SUCCESS);
    SessionEvent *raw_event = nullptr;
    ASSERT_EQ(communicator.read_event(raw_event), RC::SUCCESS);
    unique_ptr<SessionEvent> event(raw_event);
    ASSERT_NE(event, nullptr);
    EXPECT_EQ(event->query(), query);
  }
}

TEST(PlainCommunicatorTest, rejects_requests_larger_than_limit)
{
  TemporaryFile file(tmpfile(), &fclose);
  ASSERT_NE(file, nullptr);
  string query(16 * 1024 * 1024, ' ');
  ASSERT_EQ(fwrite(query.data(), 1, query.size(), file.get()), query.size());
  ASSERT_EQ(fflush(file.get()), 0);
  ASSERT_EQ(fseek(file.get(), 0, SEEK_SET), 0);

  int fd = dup(fileno(file.get()));
  ASSERT_GE(fd, 0);
  PlainCommunicator communicator;
  ASSERT_EQ(communicator.init(fd, unique_ptr<Session>(), "unit test"), RC::SUCCESS);
  SessionEvent *event = nullptr;
  EXPECT_EQ(communicator.read_event(event), RC::IOERR_TOO_LONG);
  EXPECT_EQ(event, nullptr);
}

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
