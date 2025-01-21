#include <memory>
#include "arrow/buffer.h"
#include "arrow/corded_buffer.h"
#include "arrow/util/compression_internal.h"

namespace arrow::util::internal {
namespace {

// A fake corded codec for uncompressed pages: need to copy to the contiguous output
// buffer
class UncompressedCordedCodec final : public CordedCodec {
 public:
  Result<int64_t> DecompressCorded(int64_t input_len, CordedBuffer input,
                                   int64_t output_buffer_len,
                                   uint8_t* output_buffer) override {
    if (output_buffer_len < input_len) {
      return Status::IOError("Output buffer too short");
    }

    return MemcpyFromCorded(output_buffer, input, input_len);
  }

  int minimum_compression_level() const override { return 0; }
  int maximum_compression_level() const override { return 0; }
  int default_compression_level() const override { return 0; }
  Compression::type compression_type() const override {
    return Compression::UNCOMPRESSED;
  }
  int64_t MaxCompressedLen(int64_t input_len, const uint8_t* input) override {
    return input_len;
  }
};

// A wrapper that makes any codec a corded codec by first copying the corded buffer to
// a contiguous buffer, and then calling the wrapped codec on the contiguous buffer
class CordedCodecWrapper final : public CordedCodec {
 public:
  void SetCodec(std::unique_ptr<Codec> wrapped_codec) {
    wrapped_codec_ = std::move(wrapped_codec);
  }
  void SetDecompressionBuffer(std::unique_ptr<ResizableBuffer> buffer) {
    decompression_buffer_ = std::move(buffer);
  }

  Result<int64_t> DecompressCorded(int64_t input_len, CordedBuffer input,
                                   int64_t output_buffer_len,
                                   uint8_t* output_buffer) override {
    if (input.RemainingBytesInCurrentSlice() >= input_len) {
      // Fast path: the entire region to copy is contained in a single slice of the input
      // buffer, so no need for an intermediate buffer
      const auto* raw_input = reinterpret_cast<const uint8_t*>(
          input.slice(input.slice_idx()).data() + input.slice_offset());
      return wrapped_codec_->Decompress(input_len, raw_input, output_buffer_len,
                                        output_buffer);
    }

    if (!decompression_buffer_) {
      return Status::IOError("CordedCodecWrapper requires external decompression buffer");
    }
    if (!decompression_buffer_->Resize(input_len, /*shrink_to_fit=*/false).ok()) {
      return Status::OutOfMemory("Could not allocate memory for corded codec wrapper");
    }

    auto bytes_copied =
        MemcpyFromCorded(decompression_buffer_->mutable_data(), input, input_len);
    if (bytes_copied != input_len) {
      return Status::IOError(
          "CordedCodecWrapper prematurely encountered end of CordedBuffer: expected ",
          input_len, " bytes but exhausted buffer after ", bytes_copied, " bytes");
    }
    return wrapped_codec_->Decompress(input_len, decompression_buffer_->data(),
                                      output_buffer_len, output_buffer);
  }

  int compression_level() const override { return wrapped_codec_->compression_level(); }
  int minimum_compression_level() const override {
    return wrapped_codec_->minimum_compression_level();
  }
  int maximum_compression_level() const override {
    return wrapped_codec_->maximum_compression_level();
  }
  int default_compression_level() const override {
    return wrapped_codec_->default_compression_level();
  }
  Compression::type compression_type() const override {
    return wrapped_codec_->compression_type();
  }
  int64_t MaxCompressedLen(int64_t input_len, const uint8_t* input) override {
    return wrapped_codec_->MaxCompressedLen(input_len, input);
  }

 private:
  std::unique_ptr<Codec> wrapped_codec_;
  std::unique_ptr<ResizableBuffer> decompression_buffer_;
};

}  // namespace

std::unique_ptr<CordedCodec> MakeUncompressedCordedCodec() {
  return std::make_unique<UncompressedCordedCodec>();
}

std::unique_ptr<CordedCodec> MakeCordedCodecWrapper(
    std::unique_ptr<Codec> wrapped_codec, std::unique_ptr<ResizableBuffer> buffer) {
  auto wrapper = std::make_unique<CordedCodecWrapper>();
  wrapper->SetCodec(std::move(wrapped_codec));
  wrapper->SetDecompressionBuffer(std::move(buffer));
  return wrapper;
}

}  // namespace arrow::util::internal
