
#include <gtest/gtest.h>
#include <algorithm>
#include <cstddef>
#include <memory>

#include "arrow/util/corded_buffer.h"

namespace arrow::internal {

// Empty buffer is well-behaved
TEST(CordedBuffer, EmptyBuffer) {
  std::vector<std::span<std::byte>> data;

  CordedBuffer buffer(std::span{data.begin(), data.size()});
  EXPECT_TRUE(buffer.Exhausted());
  EXPECT_EQ(buffer.RemainingBytesInCurrentSlice(), 0);
  EXPECT_EQ(buffer.RemainingBytes(), 0);
  EXPECT_EQ(buffer.Peek(42), "");
  EXPECT_EQ(buffer.PeekBuffer(42)->ToString(), "");
  EXPECT_NO_THROW(buffer.Advance(42));
  EXPECT_EQ(buffer.num_slices(), 0);
  EXPECT_EQ(buffer.slice_idx(), 0);
  EXPECT_EQ(buffer.slice_offset(), 0);
}

TEST(CordedBuffer, Simple) {
  std::vector<std::byte> bytes(10000);
  // Fill vector with 0 1 2 ... 254 255 0 1 2 ...
  std::generate(bytes.begin(), bytes.end(),
                [pos = 0]() mutable { return static_cast<std::byte>(pos++ & 0xFF); });

  std::vector<std::span<std::byte>> slices(91);
  // Create overlapping slices of 1024 bytes each with offsets of 100 bytes, except that
  // the final buffer is only 1000 bytes
  std::generate(slices.begin(), slices.end(), [pos = 0, &bytes]() mutable {
    auto start = 100 * pos++;
    auto len = std::min(static_cast<size_t>(1024), bytes.size() - start);
    EXPECT_GE(len, 1000);  // final buffer is 1000, rest are 1024 bytes
    return std::span{bytes.data() + start, len};
  });

  CordedBuffer buffer(std::span{slices.begin(), slices.size()});
  EXPECT_EQ(buffer.num_slices(), slices.size());
  EXPECT_EQ(90 * 1024 + 1000, buffer.RemainingBytes());
  EXPECT_EQ(1024, buffer.RemainingBytesInCurrentSlice());
  // Peek stays within the current slice
  EXPECT_EQ(buffer.Peek(10000),
            std::string_view(reinterpret_cast<char*>(bytes.data()), 1024));
  buffer.Advance(100);
  EXPECT_EQ(buffer.Peek(10000),
            std::string_view(reinterpret_cast<char*>(bytes.data() + 100), 924));
  EXPECT_EQ(buffer.slice_idx(), 0);
  EXPECT_EQ(buffer.slice_offset(), 100);

  // PeekBuffer works across slices
  buffer.Advance(900);
  EXPECT_EQ(buffer.PeekBuffer(50)->ToString(),
            std::string(reinterpret_cast<char*>(slices[0].data() + 1000), 24) +
                std::string(reinterpret_cast<char*>(slices[1].data()), 26));
  // Advancing to the end of a slice advances to the next slice
  buffer.Advance(24);
  EXPECT_EQ(1, buffer.slice_idx());
  EXPECT_EQ(0, buffer.slice_offset());
  buffer.Advance(10799);
  EXPECT_EQ(11, buffer.slice_idx());
  EXPECT_EQ(559, buffer.slice_offset());
  EXPECT_EQ(465, buffer.RemainingBytesInCurrentSlice());
  EXPECT_EQ(81337, buffer.RemainingBytes());

  buffer.Advance(1337);
  EXPECT_EQ(12, buffer.slice_idx());
  EXPECT_EQ(872, buffer.slice_offset());

  std::string dest;
  dest.resize(100'000);
  EXPECT_EQ(80'000, util::TryMemcpyFromCorded(dest.data(), buffer, 100'000));
  EXPECT_EQ(std::string_view(dest.data(), 1024 + 1024 - 872),
            std::string(reinterpret_cast<char*>(slices[12].data() + 872), 1024 - 872) +
                std::string(reinterpret_cast<char*>(slices[13].data()), 1024));
}

}  // namespace arrow::internal
