#pragma once

#include <array>
#include <cstdint>

// Pure encoder for the machine-readable frame stamp burned into annotated
// frames by draw_frame_stamp() (engine/src/orchestration/inference_worker.cpp).
// Kept header-only and dependency-free (no OpenCV) so it can be unit tested
// on the host without pulling in the vision/NPU stack.
namespace inference_worker {

// Encodes a 64-bit frame sequence number into 9 bytes: out[0..7] are the
// seq bytes MSB-first (out[0] = bits 63-56 of seq, out[7] = bits 7-0), and
// out[8] is an 8-bit XOR checksum of out[0..7]. This is the exact byte/bit
// order the machine-readable stamp's cell grid encodes (see the layout
// comment on draw_frame_stamp()), letting a decoder validate a sampled
// stamp before trusting it.
inline std::array<uint8_t, 9> encode_frame_stamp_bytes(uint64_t seq) noexcept {
  std::array<uint8_t, 9> out{};
  for (int i = 0; i < 8; ++i) {
    out[static_cast<size_t>(i)] = static_cast<uint8_t>((seq >> (56 - 8 * i)) & 0xFF);
  }
  uint8_t checksum = 0;
  for (int i = 0; i < 8; ++i) {
    checksum ^= out[static_cast<size_t>(i)];
  }
  out[8] = checksum;
  return out;
}

} // namespace inference_worker
