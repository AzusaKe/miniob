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
// Created by Wangyunlai on 2023/06/16.
//

#ifdef __MUSL__
#include <errno.h>
#else
#include <sys/errno.h>
#endif
#include <unistd.h>

#include "net/buffered_writer.h"

BufferedWriter::BufferedWriter(int fd) : fd_(fd), buffer_() {}

BufferedWriter::BufferedWriter(int fd, int32_t size) : fd_(fd), buffer_(size) {}

BufferedWriter::~BufferedWriter() { close(); }

RC BufferedWriter::close()
{
  discard_staging();
  if (fd_ < 0) {
    return RC::SUCCESS;
  }

  RC rc = flush();
  if (OB_FAIL(rc)) {
    return rc;
  }

  fd_ = -1;

  return RC::SUCCESS;
}

RC BufferedWriter::write(const char *data, int32_t size, int32_t &write_size)
{
  write_size = 0;
  if (fd_ < 0 || size < 0 || (size > 0 && data == nullptr)) {
    return RC::INVALID_ARGUMENT;
  }
  if (size == 0) {
    return RC::SUCCESS;
  }
  if (staging_) {
    RC rc = write_staged(data, size);
    if (OB_SUCC(rc)) {
      write_size = size;
    }
    return rc;
  }

  if (buffer_.remain() == 0) {
    RC rc = flush_internal(size);
    if (OB_FAIL(rc)) {
      return rc;
    }
  }

  return buffer_.write(data, size, write_size);
}

RC BufferedWriter::writen(const char *data, int32_t size)
{
  if (fd_ < 0 || size < 0 || (size > 0 && data == nullptr)) {
    return RC::INVALID_ARGUMENT;
  }

  int32_t write_size = 0;
  while (write_size < size) {
    int32_t tmp_write_size = 0;

    RC rc = write(data + write_size, size - write_size, tmp_write_size);
    if (OB_FAIL(rc)) {
      return rc;
    }

    write_size += tmp_write_size;
  }

  return RC::SUCCESS;
}

RC BufferedWriter::flush()
{
  if (fd_ < 0) {
    return RC::INVALID_ARGUMENT;
  }
  if (staging_) {
    return RC::SUCCESS;
  }

  RC rc = RC::SUCCESS;
  while (OB_SUCC(rc) && buffer_.size() > 0) {
    rc = flush_internal(buffer_.size());
  }
  return rc;
}

RC BufferedWriter::begin_staging()
{
  if (fd_ < 0 || staging_) {
    return RC::INVALID_ARGUMENT;
  }
  RC rc = flush();
  if (OB_FAIL(rc)) {
    return rc;
  }
  staging_ = true;
  return RC::SUCCESS;
}

RC BufferedWriter::write_staged(const char *data, int32_t size)
{
  constexpr size_t memory_limit = 1024 * 1024;
  if (staged_file_ == nullptr && staged_data_.size() + static_cast<size_t>(size) <= memory_limit) {
    staged_data_.insert(staged_data_.end(), data, data + size);
    return RC::SUCCESS;
  }

  if (staged_file_ == nullptr) {
    staged_file_ = std::tmpfile();
    if (staged_file_ == nullptr) {
      return RC::IOERR_WRITE;
    }
    if (!staged_data_.empty()
        && std::fwrite(staged_data_.data(), 1, staged_data_.size(), staged_file_) != staged_data_.size()) {
      return RC::IOERR_WRITE;
    }
    staged_data_.clear();
  }
  if (std::fwrite(data, 1, size, staged_file_) != static_cast<size_t>(size)) {
    return RC::IOERR_WRITE;
  }
  return RC::SUCCESS;
}

RC BufferedWriter::commit_staging()
{
  if (!staging_) {
    return RC::INVALID_ARGUMENT;
  }
  RC rc = RC::SUCCESS;
  if (staged_file_ != nullptr) {
    if (std::fflush(staged_file_) != 0 || std::fseek(staged_file_, 0, SEEK_SET) != 0) {
      discard_staging();
      return RC::IOERR_READ;
    }
    staging_ = false;
    char replay_buffer[16384];
    size_t read_size;
    while ((read_size = std::fread(replay_buffer, 1, sizeof(replay_buffer), staged_file_)) != 0) {
      rc = writen(replay_buffer, static_cast<int32_t>(read_size));
      if (OB_FAIL(rc)) {
        break;
      }
    }
    if (OB_SUCC(rc) && std::ferror(staged_file_)) {
      rc = RC::IOERR_READ;
    }
  } else {
    staging_ = false;
    if (!staged_data_.empty()) {
      rc = writen(staged_data_.data(), static_cast<int32_t>(staged_data_.size()));
    }
  }
  discard_staging();
  return rc;
}

void BufferedWriter::discard_staging()
{
  staging_ = false;
  staged_data_.clear();
  if (staged_file_ != nullptr) {
    std::fclose(staged_file_);
    staged_file_ = nullptr;
  }
}

RC BufferedWriter::flush_internal(int32_t size)
{
  if (fd_ < 0) {
    return RC::INVALID_ARGUMENT;
  }

  RC rc = RC::SUCCESS;

  int32_t write_size = 0;
  while (OB_SUCC(rc) && buffer_.size() > 0 && size > write_size) {
    const char *buf       = nullptr;
    int32_t     read_size = 0;
    rc                    = buffer_.buffer(buf, read_size);
    if (OB_FAIL(rc)) {
      return rc;
    }

    ssize_t tmp_write_size = 0;
    while (tmp_write_size == 0) {
      tmp_write_size = ::write(fd_, buf, read_size);
      if (tmp_write_size < 0) {
        if (errno == EAGAIN || errno == EINTR) {
          tmp_write_size = 0;
          continue;
        } else {
          return RC::IOERR_WRITE;
        }
      }
    }

    write_size += tmp_write_size;
    buffer_.forward(tmp_write_size);
  }

  return rc;
}
