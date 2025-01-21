// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "arrow/util/compression_internal.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <snappy-sinksource.h>
#include <snappy.h>

#include "arrow/corded_buffer.h"
#include "arrow/result.h"
#include "arrow/status.h"
#include "arrow/util/logging.h"
#include "arrow/util/macros.h"

using std::size_t;

namespace arrow {
namespace util {
namespace internal {

namespace {

// ----------------------------------------------------------------------
// Snappy implementation

class SnappyCodec : public Codec {
 public:
  Result<int64_t> Decompress(int64_t input_len, const uint8_t* input,
                             int64_t output_buffer_len, uint8_t* output_buffer) override {
    size_t decompressed_size;
    if (!snappy::GetUncompressedLength(reinterpret_cast<const char*>(input),
                                       static_cast<size_t>(input_len),
                                       &decompressed_size)) {
      return Status::IOError("Corrupt snappy compressed data.");
    }
    if (output_buffer_len < static_cast<int64_t>(decompressed_size)) {
      return Status::Invalid("Output buffer size (", output_buffer_len, ") must be ",
                             decompressed_size, " or larger.");
    }
    if (!snappy::RawUncompress(reinterpret_cast<const char*>(input),
                               static_cast<size_t>(input_len),
                               reinterpret_cast<char*>(output_buffer))) {
      return Status::IOError("Corrupt snappy compressed data.");
    }
    return static_cast<int64_t>(decompressed_size);
  }

  int64_t MaxCompressedLen(int64_t input_len,
                           const uint8_t* ARROW_ARG_UNUSED(input)) override {
    DCHECK_GE(input_len, 0);
    return snappy::MaxCompressedLength(static_cast<size_t>(input_len));
  }

  Result<int64_t> Compress(int64_t input_len, const uint8_t* input,
                           int64_t ARROW_ARG_UNUSED(output_buffer_len),
                           uint8_t* output_buffer) override {
    size_t output_size;
    snappy::RawCompress(reinterpret_cast<const char*>(input),
                        static_cast<size_t>(input_len),
                        reinterpret_cast<char*>(output_buffer), &output_size);
    return static_cast<int64_t>(output_size);
  }

  Result<std::shared_ptr<Compressor>> MakeCompressor() override {
    return Status::NotImplemented("Streaming compression unsupported with Snappy");
  }

  Result<std::shared_ptr<Decompressor>> MakeDecompressor() override {
    return Status::NotImplemented("Streaming decompression unsupported with Snappy");
  }

  Compression::type compression_type() const override { return Compression::SNAPPY; }
  int minimum_compression_level() const override { return kUseDefaultCompressionLevel; }
  int maximum_compression_level() const override { return kUseDefaultCompressionLevel; }
  int default_compression_level() const override { return kUseDefaultCompressionLevel; }
};

class SnappyCordedCodec : public CordedCodec {
 private:
  // Adaptation of SnappyIOVecReader from snappy.cc to CordedBuffer sources.
  // `total_size` is the total number of bytes to be read from `source`.
  class SnappyCordedReader : public snappy::Source {
   public:
    SnappyCordedReader(const CordedBuffer& source, int64_t total_size)
        : source_(source),
          curr_slice_idx_(source_.slice_idx()),
          curr_offset_(source_.slice_offset()),
          curr_size_remaining_(
              std::min(total_size, source_.RemainingBytesInCurrentSlice())),
          total_size_remaining_(total_size) {
      // Skip empty leading `iovec`s.
      if (total_size > 0 && curr_size_remaining_ == 0) Advance();
    }

    ~SnappyCordedReader() override = default;

    size_t Available() const override { return total_size_remaining_; }

    const char* Peek(size_t* len) override {
      *len = curr_size_remaining_;
      return reinterpret_cast<const char*>(source_.slice(curr_slice_idx_).data()) +
             curr_offset_;
    }

    void Skip(size_t n) override {
      while (static_cast<int64_t>(n) >= curr_size_remaining_ && n > 0) {
        n -= curr_size_remaining_;
        Advance();
      }
      curr_size_remaining_ -= n;
      total_size_remaining_ -= n;
      curr_offset_ += n;
    }

   private:
    // Advances to the next nonempty slice and updates related variables.
    void Advance() {
      do {
        assert(total_size_remaining_ >= curr_size_remaining_);
        total_size_remaining_ -= curr_size_remaining_;
        if (total_size_remaining_ == 0) {
          curr_offset_ = 0;
          curr_size_remaining_ = 0;
          return;
        }
        ++curr_slice_idx_;
        curr_offset_ = 0;
        curr_size_remaining_ = std::min<int64_t>(source_.slice(curr_slice_idx_).size(),
                                                 total_size_remaining_);
      } while (curr_size_remaining_ == 0);
    }

    // The CordedBuffer to read from
    CordedBuffer source_;
    // The index of the slice currently being read
    int32_t curr_slice_idx_;
    // The location in `curr_slice_` currently being read.
    int32_t curr_offset_;
    // The amount of unread data in `curr_iov_`.
    int64_t curr_size_remaining_;
    // The amount of unread data in the entire input array.
    int64_t total_size_remaining_;
  };

 public:
  Result<int64_t> DecompressCorded(int64_t input_len, CordedBuffer input,
                                   int64_t output_buffer_len,
                                   uint8_t* output_buffer) override {
    // Try reading up to 16 bytes, that's definitely enough to figure out the uncompressed
    // size. A lower value might also work.
    auto header = input.PeekBuffer(16);
    if (header->size() == 0) {
      return Status::IOError("Invalid corded input buffer for Snappy");
    }

    size_t decompressed_size;
    if (!snappy::GetUncompressedLength(header->data_as<char>(),
                                       static_cast<size_t>(input_len),
                                       &decompressed_size)) {
      return Status::IOError("Corrupt snappy compressed data (could not read length)");
    }
    if (output_buffer_len < static_cast<int64_t>(decompressed_size)) {
      return Status::Invalid("Output buffer size (", output_buffer_len, ") must be ",
                             decompressed_size, " or larger.");
    }

    SnappyCordedReader source(input, input_len);
    if (!snappy::RawUncompress(&source, reinterpret_cast<char*>(output_buffer))) {
      return Status::IOError("Corrupt snappy compressed data.");
    }

    return static_cast<int64_t>(decompressed_size);
  }

  int64_t MaxCompressedLen(int64_t input_len,
                           const uint8_t* ARROW_ARG_UNUSED(input)) override {
    DCHECK_GE(input_len, 0);
    return snappy::MaxCompressedLength(static_cast<size_t>(input_len));
  }

  Compression::type compression_type() const override { return Compression::SNAPPY; }
  int minimum_compression_level() const override { return kUseDefaultCompressionLevel; }
  int maximum_compression_level() const override { return kUseDefaultCompressionLevel; }
  int default_compression_level() const override { return kUseDefaultCompressionLevel; }
};

}  // namespace

std::unique_ptr<Codec> MakeSnappyCodec() { return std::make_unique<SnappyCodec>(); }

std::unique_ptr<CordedCodec> MakeSnappyCordedCodec() {
  return std::make_unique<SnappyCordedCodec>();
}

}  // namespace internal
}  // namespace util
}  // namespace arrow
