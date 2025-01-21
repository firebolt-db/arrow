
#include <gtest/gtest.h>
#include <algorithm>
#include <cstddef>
#include <string_view>

#include "arrow/corded_buffer.h"

namespace std {
inline bool operator==(const span<const byte>& lhs, const span<const byte>& rhs) {
  return string_view{reinterpret_cast<const char*>(lhs.data()), lhs.size()} ==
         string_view{reinterpret_cast<const char*>(rhs.data()), rhs.size()};
}
}  // namespace std

namespace arrow::internal {

// Empty buffer is well-behaved
TEST(CordedBuffer, EmptyBuffer) {
  std::vector<std::span<const std::byte>> data;

  CordedBuffer buffer(std::span{data.begin(), data.size()});
  EXPECT_TRUE(buffer.Exhausted());
  EXPECT_EQ(buffer.RemainingBytesInCurrentSlice(), 0);
  EXPECT_EQ(buffer.RemainingBytes(), 0);
  EXPECT_TRUE(buffer.Peek(42).empty());
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

  std::vector<std::span<const std::byte>> slices(91);
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
  EXPECT_EQ(buffer.Peek(10000), std::span(bytes.data(), 1024));
  buffer.Advance(100);
  EXPECT_EQ(buffer.Peek(10000), std::span(bytes.data() + 100, 924));
  EXPECT_EQ(buffer.slice_idx(), 0);
  EXPECT_EQ(buffer.slice_offset(), 100);

  // PeekBuffer works across slices
  buffer.Advance(900);
  EXPECT_EQ(buffer.PeekBuffer(50)->ToString(),
            std::string(reinterpret_cast<const char*>(slices[0].data() + 1000), 24) +
                std::string(reinterpret_cast<const char*>(slices[1].data()), 26));
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
  EXPECT_EQ(80'000, util::MemcpyFromCorded(dest.data(), buffer, 100'000));
  EXPECT_EQ(
      std::string_view(dest.data(), 1024 + 1024 - 872),
      std::string(reinterpret_cast<const char*>(slices[12].data() + 872), 1024 - 872) +
          std::string(reinterpret_cast<const char*>(slices[13].data()), 1024));
  // Position is unchanged
  EXPECT_EQ(12, buffer.slice_idx());
  EXPECT_EQ(872, buffer.slice_offset());
  // Peeking at the end is fine: we're past the final slice but it's ok
  buffer.Advance(buffer.RemainingBytes());
  EXPECT_EQ(buffer.num_slices(), buffer.slice_idx());
  EXPECT_EQ(0, buffer.slice_offset());
  EXPECT_EQ(buffer.Peek(10), std::span<const std::byte>{});
}

TEST(CordedBuffer, Memcpy) {
  std::vector<std::span<const std::byte>> slices;
  CordedBuffer empty(std::span{slices.begin(), slices.size()});

  // Empty & negative memcpy works just fine and doesn't access the destination memory
  EXPECT_EQ(0, util::MemcpyFromCorded(nullptr, empty, 0));
  EXPECT_EQ(0, util::MemcpyFromCorded(nullptr, empty, -10));

  // Trying to copy from an empty buffer works and doesn't access any pointers
  EXPECT_EQ(0, util::MemcpyFromCorded(nullptr, empty, 10));

  std::string data = "A bunch of test data to put into a corded buffer";
  EXPECT_EQ(48, data.size());
  const std::byte* data_ptr = reinterpret_cast<const std::byte*>(data.data());
  slices = {{data_ptr, 10},
            {data_ptr + 10, 10},
            {data_ptr + 20, 10},
            {data_ptr + 30, 10},
            {data_ptr + 40, 8}};
  CordedBuffer buffer(std::span{slices.begin(), slices.size()});
  EXPECT_EQ(data.size(), buffer.RemainingBytes());

  {
    std::string dest(data.size(), '\0');
    EXPECT_EQ(data.size(), util::MemcpyFromCorded(dest.data(), buffer, data.size()));
    EXPECT_EQ(dest, data);
  }

  // Position is unchanged
  EXPECT_EQ(0, buffer.slice_idx());
  EXPECT_EQ(0, buffer.slice_offset());
  EXPECT_EQ(data.size(), buffer.RemainingBytes());

  buffer.Advance(15);
  EXPECT_EQ(33, buffer.RemainingBytes());

  // Only as much data as is available is copied.
  {
    std::string dest(data.size(), '\0');
    EXPECT_EQ(33, util::MemcpyFromCorded(dest.data(), buffer, 100));
    // EXPECT_STREQ ignores trailing nullbytes, unlike EXPECT_EQ(data.substr(15), dest)
    EXPECT_STREQ(data.data() + 15, dest.data());
  }

  // Also here: 0 and negative nbytes are safe
  EXPECT_EQ(0, util::MemcpyFromCorded(nullptr, buffer, 0));
  EXPECT_EQ(0, util::MemcpyFromCorded(nullptr, buffer, -10));
}

}  // namespace arrow::internal
