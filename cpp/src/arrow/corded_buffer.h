#pragma once

#include <span>

#include "arrow/buffer.h"

namespace arrow {

/// A non-contiguous non-owning buffer that references multiple slices of data with
/// potentially different sizes.  Maintains a position.
class CordedBuffer {
 public:
  /// A corded buffer consists of a number of slices, which are contiguous memory regions.
  /// The lengths of the slices may differ.
  using Slice = std::span<const std::byte>;

  explicit CordedBuffer(std::span<const Slice> slices, int32_t slice_idx = 0,
                        int32_t slice_offset = 0)
      : slices_(slices), slice_idx_(slice_idx), slice_offset_(slice_offset) {}

  /// How many bytes remain in the current slice
  int64_t RemainingBytesInCurrentSlice() const noexcept {
    if (Exhausted()) return 0;
    return static_cast<int64_t>(slices_[slice_idx_].size()) - slice_offset_;
  }

  /// How many bytes remain in the corded buffer
  int64_t RemainingBytes() const noexcept {
    int64_t res = RemainingBytesInCurrentSlice();
    for (size_t idx = slice_idx_ + 1; idx < slices_.size(); ++idx) {
      res += slices_[idx].size();
    }
    return res;
  }

  /// Zero-copy peek into *the current slice* only without advancing the position.  Will
  /// return at most `RemainingBytesInCurrentSlice` bytes.
  std::span<const std::byte> Peek(int64_t nbytes) const noexcept {
    if (Exhausted()) return {};
    return {slices_[slice_idx_].data() + slice_offset_,
            static_cast<size_t>(std::min(nbytes, RemainingBytesInCurrentSlice()))};
  }

  /// Peek into the corded buffer without advancing the position.  Zero-copy if the entire
  /// read range is contained in a single slice, copying if it does not.
  std::shared_ptr<Buffer> PeekBuffer(int64_t nbytes) const noexcept;

  /// Advance the current position by `n` bytes.
  void Advance(size_t n) noexcept;

  /// Whether the position is at the end of the buffer
  bool Exhausted() const noexcept {
    return slices_.empty() || slice_idx_ >= static_cast<int32_t>(slices_.size());
  }

  int32_t num_slices() const noexcept { return static_cast<int32_t>(slices_.size()); }

  /// Raw access to a particular slice.  Returns an empty slice for out-of-bounds accesses.
  const Slice& slice(int32_t slice_idx) const noexcept;

  int32_t slice_idx() const noexcept { return slice_idx_; }

  int32_t slice_offset() const noexcept { return slice_offset_; }

 private:
  // The slices
  std::span<const Slice> slices_;
  // Current buffer position, encoded as slice index and offset within that slice
  int32_t slice_idx_;
  int32_t slice_offset_;
};

namespace util {

// Copies *up to* `nbytes` bytes the corded buffer `src`, starting at its current
// position, to the contiguous buffer `dest` and returns how many bytes were copied.  This
// may be less than `nbytes` if `src` has fewer than `nbytes` bytes remaining.  The source
// buffer's position remains unchanged.
[[nodiscard]] int64_t MemcpyFromCorded(void* dest, const CordedBuffer& src,
                                       int64_t nbytes) noexcept;

}  // namespace util
}  // namespace arrow
