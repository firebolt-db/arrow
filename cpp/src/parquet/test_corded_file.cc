#include "test_corded_file.h"
#include <memory>
#include "arrow/io/file.h"
#include "arrow/io/interfaces.h"
#include "arrow/type_fwd.h"
#include "parquet/exception.h"
#include "parquet/file_reader.h"
#include "parquet/properties.h"

namespace parquet {

SimpleCordedRandomAccessFile::~SimpleCordedRandomAccessFile() = default;

SimpleCordedRandomAccessFile::SimpleCordedRandomAccessFile(
    std::shared_ptr<::arrow::Buffer> file_contents, int64_t slice_size)
    : file_size_(file_contents->size()) {
  const int64_t num_slices = (file_size_ + slice_size - 1) / slice_size;
  data_.resize(num_slices);
  slices_.reserve(num_slices);

  int64_t offset = 0;
  for (int32_t i = 0; i < num_slices; ++i) {
    data_[i].offset = offset;
    auto slice_len = std::min(slice_size, file_size_ - offset);
    data_[i].data.resize(slice_len);
    memcpy(data_[i].data.data(), file_contents->data() + offset, slice_len);
    slices_.emplace_back(data_[i].data.data(), slice_len);
    lookup_[offset] = i;
    offset += slice_len;
  }
}

::arrow::Result<::arrow::CordedBuffer> SimpleCordedRandomAccessFile::ReadCordedAt(
    int64_t position, int64_t nbytes) {
  // Find first slice
  auto first = lookup_.lower_bound(position);
  if (first != lookup_.begin() && (first == lookup_.end() || first->first > position)) {
    --first;
  }

  auto last = lookup_.upper_bound(position + nbytes);
  size_t num_slices = std::distance(first, last);

  auto first_slice_idx = first->second;
  int32_t offset = static_cast<int32_t>(position - data_.at(first_slice_idx).offset);
  EXPECT_GE(offset, 0);
  EXPECT_LT(offset, data_.at(first_slice_idx).data.size());
  ::arrow::CordedBuffer buffer{std::span{slices_.data() + first_slice_idx, num_slices},
                               /* slice index */ 0,
                               /* slice offset */ offset};

  // CordedBuffers may always be too long since they're based on pre-existing underlying
  // slices
  EXPECT_LE(nbytes, buffer.RemainingBytes());
  return buffer;
}

/*static*/ std::shared_ptr<SimpleCordedRandomAccessFile>
SimpleCordedRandomAccessFile::FromFile(std::string path, int64_t slice_size) {
  PARQUET_ASSIGN_OR_THROW(
      auto file, ::arrow::io::ReadableFile::Open(path, ::arrow::default_memory_pool()));
  PARQUET_ASSIGN_OR_THROW(auto file_size, file->GetSize());
  PARQUET_ASSIGN_OR_THROW(auto buffer, file->Read(file_size));
  return std::make_shared<SimpleCordedRandomAccessFile>(std::move(buffer), slice_size);
}

std::unique_ptr<ParquetFileReader> OpenFileReader(
    const std::string& path, int64_t slice_size, bool memory_map, ReaderProperties props,
    std::shared_ptr<FileMetaData> metadata) {
  if (slice_size == 0) {
    return ParquetFileReader::OpenFile(path, memory_map, props, std::move(metadata));
  } else {
    auto source = SimpleCordedRandomAccessFile::FromFile(path, slice_size);
    props.use_firebolt_corded_buffers();
    return ParquetFileReader::Open(std::move(source), props, std::move(metadata));
  }
}

std::unique_ptr<ParquetFileReader> MakeBufferReader(
    std::shared_ptr<::arrow::Buffer> file_contents, int64_t slice_size,
    ReaderProperties props) {
  std::shared_ptr<::arrow::io::RandomAccessFile> source;
  if (slice_size == 0) {
    source = std::make_shared<::arrow::io::BufferReader>(std::move(file_contents));
  } else {
    props.use_firebolt_corded_buffers();
    source = std::make_shared<SimpleCordedRandomAccessFile>(std::move(file_contents),
                                                            slice_size);
  }
  return ParquetFileReader::Open(std::move(source), props);
}
}  // namespace parquet
