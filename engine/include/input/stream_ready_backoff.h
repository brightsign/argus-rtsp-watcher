#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

// When the engine opens its RTSP stream, the server may answer "404 Not Found"
// because the stream has not been published yet: MediaMTX starts at the same
// moment as the engines and needs time to connect to each camera. That is not a
// reason to abandon the hardware decoder. It used to be treated as one: the
// engine waited out a full 15 s first-frame timeout, then tried a pipeline for
// the wrong codec (another 15 s), and ended on software decode for good, though
// the hardware pipeline works once the stream is up. These pure rules let it
// wait for the stream instead. Header-only and dependency-free so they are unit
// tested on the host.
namespace rtsp_open {

constexpr int kNotReadyInitialDelayMs = 500;
constexpr int kNotReadyMaxDelayMs = 2000;
// Total time to wait for a stream to appear before treating its absence as a
// real failure. MediaMTX has been seen to take about 25 s to publish a camera.
constexpr int kNotReadyBudgetMs = 60000;

// How long to wait before retrying the same pipeline after a "not found", given
// how many retries have already happened and how long they have waited in
// total. The delay doubles from kNotReadyInitialDelayMs up to kNotReadyMaxDelayMs.
// Returns nullopt once waiting that long would overrun the budget; the caller
// then stops retrying and reports the failure.
inline std::optional<int> not_ready_retry_delay_ms(int retries_so_far, int waited_ms) {
  int delay_ms = kNotReadyInitialDelayMs;
  for (int retry = 0; retry < retries_so_far && delay_ms < kNotReadyMaxDelayMs; ++retry) {
    delay_ms = std::min(delay_ms * 2, kNotReadyMaxDelayMs);
  }
  if (waited_ms + delay_ms > kNotReadyBudgetMs) return std::nullopt;
  return delay_ms;
}

// Whether a GStreamer error means "the stream is not there yet" as opposed to a
// real failure (a wrong codec, a refused connection). rtspsrc reports a 404 as a
// resource-not-found error, which is the dependable signal; the message text is
// accepted too, because the code mapping is a detail of rtspsrc that has varied
// between versions and a missed match would silently lose the retry.
inline bool is_stream_not_ready(bool is_resource_not_found, const std::string& message) {
  if (is_resource_not_found) return true;
  std::string lowered;
  lowered.reserve(message.size());
  for (const char c : message) lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return lowered.find("not found") != std::string::npos;
}

}  // namespace rtsp_open
