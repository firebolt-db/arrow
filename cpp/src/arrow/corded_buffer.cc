#include "corded_buffer.h"

#include <stdexcept>

namespace arrow {

std::shared_ptr<Buffer> arrow::CordedBuffer::PeekBuffer(int64_t nbytes) const {
  const auto available = RemainingBytesInCurrentSlice();
  if (available >= nbytes || slice_idx_ + 1 == static_cast<int64_t>(slices_.size())) {
    // We are lucky: the entire read range fits into a single buffer
    auto range = Peek(nbytes);
    return std::make_shared<Buffer>(reinterpret_cast<const uint8_t*>(range.data()),
                                    range.size());
  }

  std::string data;
  data.resize(nbytes);
  auto actual_size = util::MemcpyFromCorded(data.data(), *this, nbytes);
  if (actual_size < nbytes) {
    data.resize(actual_size);
  }
  return Buffer::FromString(std::move(data));
}

CordedBuffer::Slice CordedBuffer::slice(int32_t slice_idx) const {
  if (slice_idx < 0 || static_cast<size_t>(slice_idx) >= slices_.size()) {
    throw std::runtime_error("out-of-bounds slice access");
  }
  return slices_[slice_idx];
}

namespace util {

int64_t MemcpyFromCorded(void* dest, const CordedBuffer& src, int64_t nbytes) noexcept {
  if (nbytes <= 0) return 0;
  char* curr_dest = reinterpret_cast<char*>(dest);
  int64_t copied = 0;
  auto offset = src.slice_offset();  // need to skip this many bytes from current slice
  for (auto idx = src.slice_idx(); idx < src.num_slices(); ++idx) {
    const auto& slice = src.slice(idx);
    int64_t to_copy =
        std::min(/* available */ static_cast<int64_t>(slice.size()) - offset,
                 /* required */ nbytes - copied);
    memcpy(curr_dest, reinterpret_cast<const char*>(slice.data()) + offset, to_copy);
    copied += to_copy;
    if (copied == nbytes) {
      break;
    }
    curr_dest += to_copy;
    offset = 0;  // offset only applies to initial slice
  }
  return copied;
}

}  // namespace util
}  // namespace arrow
