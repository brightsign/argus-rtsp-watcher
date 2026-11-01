#pragma once

#include <cassert>
#include <cstdint>

// Which processed frames the preview writer saves: one in every `stride`, the
// first included. The writer used to hard-code one in three, so a stream
// processed at 12 fps updated its preview about 4 times a second and every
// preview frame was on average 125 ms older than it needed to be. Header-only and
// dependency-free so it is unit tested on the host.
namespace output {

class FrameStride {
 public:
  // stride 1 saves every frame, 2 every other frame, and so on.
  explicit FrameStride(int stride) : stride_(stride) { assert(stride >= 1); }

  // Call once per processed frame; true when this frame should be saved.
  bool admit() noexcept {
    const bool save = (seen_ % static_cast<uint64_t>(stride_)) == 0;
    ++seen_;
    return save;
  }

 private:
  int stride_;
  uint64_t seen_{0};
};

}  // namespace output
