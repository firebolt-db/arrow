#pragma once

#include <gtest/gtest.h>
#include <map>
#include <span>
#include "arrow/buffer.h"
#include "arrow/corded_buffer.h"
#include "arrow/io/interfaces.h"
#include "arrow/io/memory.h"
#include "parquet/file_reader.h"
#include "parquet/properties.h"

namespace parquet {

class SimpleCordedRandomAccessFile : public ::arrow::io::CordedRandomAccessFile {
  struct Slice {
    int64_t offset;
    std::vector<std::byte> data;
  };

 public:
  ~SimpleCordedRandomAccessFile() override;
  explicit SimpleCordedRandomAccessFile(std::shared_ptr<::arrow::Buffer> file_contents,
                                        int64_t slice_size);

  static std::shared_ptr<SimpleCordedRandomAccessFile> FromFile(std::string path,
                                                                int64_t slice_size);

  bool closed() const override { return false; }
  ::arrow::Result<int64_t> GetSize() override { return file_size_; }

  ::arrow::Result<::arrow::CordedBuffer> ReadCordedAt(int64_t position,
                                                      int64_t nbytes) override;

  // NOT IMPLEMENTED:
  ::arrow::Result<::arrow::CordedBuffer> ReadCorded(int64_t nbytes) override {
    return ::arrow::Status::NotImplemented("ReadCorded");
  }
  ::arrow::Result<int64_t> Tell() const override {
    return ::arrow::Status::NotImplemented("Tell");
  }
  ::arrow::Status Seek(int64_t position) override {
    return ::arrow::Status::NotImplemented("Seek");
  }
  ::arrow::Status Close() override { return ::arrow::Status::NotImplemented("Close"); }

 private:
  const int64_t file_size_;
  std::vector<Slice> data_;
  std::vector<std::span<const std::byte>> slices_;
  std::map<int64_t /* file offset */, int32_t /* slice id */> lookup_;

  ARROW_DISALLOW_COPY_AND_ASSIGN(SimpleCordedRandomAccessFile);
};

// Opens a ParquetFileReader that is corded if slice_size is > 0 and non-corded for slice
// size 0.  Remaining params are forwarded to the ParquetFileReader c'tor. The
// `memory_map` arg is ignored for corded readers.
std::unique_ptr<ParquetFileReader> OpenFileReader(
    const std::string& path, int64_t slice_size, bool memory_map = false,
    ReaderProperties props = default_reader_properties(),
    std::shared_ptr<FileMetaData> metadata = NULLPTR);

// Opens a ParquetFileReader from a buffer. The reader is corded if slice_size
// is > 0 and non-corded for slice size 0.
std::unique_ptr<ParquetFileReader> MakeBufferReader(
    std::shared_ptr<::arrow::Buffer> file_contents, int64_t slice_size,
    ReaderProperties props = default_reader_properties());

}  // namespace parquet
