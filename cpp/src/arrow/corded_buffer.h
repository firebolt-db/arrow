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
  using Slice = std::span<std::byte>;

  explicit CordedBuffer(std::span<Slice> slices, int32_t slice_idx = 0,
                        int32_t slice_offset = 0)
      : slices_(slices), slice_idx_(slice_idx), slice_offset_(slice_offset) {}

  /// How many bytes remain in the current slice
  int64_t RemainingBytesInCurrentSlice() const {
    if (slices_.empty()) return 0;
    return static_cast<int64_t>(slices_[slice_idx_].size()) - slice_offset_;
  }

  /// How many bytes remain in the corded buffer
  int64_t RemainingBytes() const {
    int64_t res = RemainingBytesInCurrentSlice();
    for (size_t idx = slice_idx_ + 1; idx < slices_.size(); ++idx) {
      res += slices_[idx].size();
    }
    return res;
  }

  /// Zero-copy peek into *the current slice* only without advancing the position.  Will
  /// return at most `RemainingBytesInCurrentSlice` bytes.
  std::string_view Peek(int64_t nbytes) const {
    if (slices_.empty()) return {};
    return {reinterpret_cast<char*>(slices_[slice_idx_].data()) + slice_offset_,
            static_cast<size_t>(std::min(nbytes, RemainingBytesInCurrentSlice()))};
  }

  /// Peek into the corded buffer without advancing the position.  Zero-copy if the entire
  /// read range is contained in a single slice, copying if it does not.
  std::shared_ptr<Buffer> PeekBuffer(int64_t nbytes) const;

  /// Advance the current position by `n` bytes.
  void Advance(size_t n) {
    size_t skipped = 0;
    for (; slice_idx_ < static_cast<int32_t>(slices_.size()); ++slice_idx_) {
      const auto& slice = slices_[slice_idx_];
      const size_t remaining_to_skip = n - skipped;
      const size_t this_slice_offset = std::exchange(slice_offset_, 0);
      const size_t available_in_slice = slice.size() - this_slice_offset;
      if (remaining_to_skip == available_in_slice) {
        // We advanced just to the page boundary, need to advance to the next page
        ++slice_idx_;
        return;
      }
      if (remaining_to_skip < available_in_slice) {
        slice_offset_ = static_cast<int32_t>(remaining_to_skip + this_slice_offset);
        return;
      }
      skipped += available_in_slice;
    }
  }

  /// Whether the position is at the end of the buffer
  bool Exhausted() const {
    return slices_.empty() ||
           (slice_idx_ == static_cast<int32_t>(slices_.size()) &&
            slice_offset_ == static_cast<int32_t>(slices_.back().size()));
  }

  int32_t num_slices() const { return slices_.size(); }

  /// Raw access to a particular slice.
  Slice slice(int32_t slice_idx) const;

  int32_t slice_idx() const { return slice_idx_; }

  int32_t slice_offset() const { return slice_offset_; }

 private:
  // The slices
  std::span<Slice> slices_;
  // Current buffer position, encoded as slice index and offset within that slice
  int32_t slice_idx_;
  int32_t slice_offset_;
};

namespace util {

// Copies *up to* `nbytes` bytes the corded buffer `src`, starting at its current
// position, to the contiguous buffer `dest` and returns how many bytes were copied.  This
// may be less than `nbytes` if `src` has fewer than `nbytes` bytes remaining.
[[nodiscard]] int64_t TryMemcpyFromCorded(void* dest, const CordedBuffer& src,
                                          int64_t nbytes);

// Like memcpy, however, we stop once `src` is exhausted. No error is returned.
void* MemcpyFromCorded(void* dest, const CordedBuffer& src, size_t n);

}  // namespace util
}  // namespace arrow
