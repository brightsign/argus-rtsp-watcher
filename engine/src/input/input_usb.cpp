#include "input/input_usb.h"
#include "metrics/log_global.h"
#include <opencv2/opencv.hpp>
#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>
#include <chrono>
#include <thread>
#include <cstring>
#include <cstdio>
#include <cerrno>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/videodev2.h>
#include <dirent.h>
#include <algorithm>

// Probe /dev/video* for a real V4L2 capture node and return its path (or "").
//
// We identify the camera with VIDIOC_QUERYCAP (V4L2_CAP_VIDEO_CAPTURE +
// V4L2_CAP_STREAMING), NOT cv::VideoCapture. OpenCV's V4L2 open is unreliable
// for detection: on a Logitech Brio it failed to open the valid capture node
// (video1) at all. QUERYCAP is deterministic -- it selects the true capture
// node, skips the paired metadata node (V4L2_CAP_META_CAPTURE only), and skips
// the RK3588 codec m2m nodes (/dev/video0..2, /dev/video-dec0), which report
// M2M/MPLANE caps rather than single-planar VIDEO_CAPTURE. Actual capture then
// uses raw V4L2 in UsbInputSource (which orders S_PARM before STREAMON so the
// requested fps is honoured).
std::string find_working_usb_camera() {
  std::vector<std::string> devices;
  if (DIR* dir = ::opendir("/dev")) {
    for (dirent* e = ::readdir(dir); e != nullptr; e = ::readdir(dir)) {
      std::string name = e->d_name;
      if (name.rfind("video", 0) == 0) devices.emplace_back("/dev/" + name);
    }
    ::closedir(dir);
  }
  std::sort(devices.begin(), devices.end());
  if (devices.empty()) {
    LG_WARN("usb: no /dev/video* nodes found");
    return "";
  }
  for (const auto& dev : devices) {
    int fd = ::open(dev.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
      LG_INFO("usb: probe %s - cannot open (%s)", dev.c_str(), strerror(errno));
      continue;
    }
    v4l2_capability cap;
    std::memset(&cap, 0, sizeof(cap));
    int rc = ioctl(fd, VIDIOC_QUERYCAP, &cap);
    ::close(fd);
    if (rc < 0) {
      LG_INFO("usb: probe %s - VIDIOC_QUERYCAP failed (%s)", dev.c_str(), strerror(errno));
      continue;
    }
    const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                          ? cap.device_caps : cap.capabilities;
    if ((caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING)) {
      LG_INFO("usb: probe %s - capture device '%s' SELECTED", dev.c_str(),
              reinterpret_cast<const char*>(cap.card));
      return dev;
    }
    LG_INFO("usb: probe %s - not a video-capture node (caps=0x%08x)", dev.c_str(), caps);
  }
  LG_WARN("usb: no working /dev/video* capture device found");
  return "";
}



using SteadyClock = std::chrono::steady_clock; // was clock_t alias conflicting with system clock_t
static inline int64_t to_ns(SteadyClock::time_point t) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

// Enumerate and log V4L2 capabilities (pixel formats, frame sizes, frame rates)
// directly via ioctls. Useful on devices where v4l2-ctl is unavailable.
static void log_v4l2_capabilities(const std::string& dev) {
  int fd = ::open(dev.c_str(), O_RDWR);
  if (fd < 0) {
    LG_WARN("v4l2 probe: cannot open %s (errno=%d)", dev.c_str(), errno);
    return;
  }
  LG_INFO("v4l2 probe: supported modes for %s:", dev.c_str());
  for (__u32 fi = 0; ; ++fi) {
    struct v4l2_fmtdesc fmt;
    std::memset(&fmt, 0, sizeof(fmt));
    fmt.index = fi;
    fmt.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) < 0) break;
    const __u32 pf = fmt.pixelformat;
    char fourcc[5] = {
      (char)(pf & 0xff), (char)((pf >> 8) & 0xff),
      (char)((pf >> 16) & 0xff), (char)((pf >> 24) & 0xff), 0
    };
    for (__u32 si = 0; ; ++si) {
      struct v4l2_frmsizeenum fs;
      std::memset(&fs, 0, sizeof(fs));
      fs.index = si;
      fs.pixel_format = pf;
      if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) < 0) break;
      if (fs.type != V4L2_FRMSIZE_TYPE_DISCRETE) continue;
      const __u32 w = fs.discrete.width, h = fs.discrete.height;
      char rates[256]; int n = 0; rates[0] = 0;
      for (__u32 ii = 0; ; ++ii) {
        struct v4l2_frmivalenum fiv;
        std::memset(&fiv, 0, sizeof(fiv));
        fiv.index = ii;
        fiv.pixel_format = pf;
        fiv.width = w; fiv.height = h;
        if (ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fiv) < 0) break;
        if (fiv.type != V4L2_FRMIVAL_TYPE_DISCRETE) continue;
        double fps = fiv.discrete.denominator /
                     (double)(fiv.discrete.numerator ? fiv.discrete.numerator : 1);
        n += snprintf(rates + n, sizeof(rates) - n, "%s%.0f",
                      n ? "," : "", fps);
        if (n >= (int)sizeof(rates) - 8) break;
      }
      LG_INFO("v4l2 probe:   %s %ux%u fps=[%s]", fourcc, w, h, rates);
    }
  }
  ::close(fd);
}

UsbInputSource::UsbInputSource(const std::string& device)
: device_(device) {}

UsbInputSource::UsbInputSource(const std::string& device, const UsbOptions& opts)
: device_(device) {
  if (opts.width > 0) pref_w_ = opts.width;
  if (opts.height > 0) pref_h_ = opts.height;
  if (opts.fps > 0) pref_fps_ = opts.fps;
}

UsbInputSource::~UsbInputSource() { close(); }

bool UsbInputSource::open() noexcept {
  close();
  broken_.store(false);
  stopping_.store(false);
  frames_ok_.store(0);
  connected_.store(false);
  last_ok_ns_.store(to_ns(SteadyClock::now()));

  // Resolve the device. For the auto-detect sentinel (or an empty token) scan
  // for a real V4L2 capture node instead of assuming /dev/video0: on RK3588
  // /dev/video0..2 are the hardware codec (rkvdec/rkvenc/rga) m2m nodes, NOT
  // cameras, so the old hardcoded fallback opened a decoder and failed with
  // "not a streaming capture node", crash-looping the engine. If no camera is
  // found, fail cleanly so the supervisor retries (the camera may be a hotplug
  // or a USB brownout under load) rather than erroring into a codec node.
  std::string dev = device_;
  if (dev.empty() || dev == "usb_camera") {
    dev = find_working_usb_camera();
    if (dev.empty()) {
      LG_ERROR("usb: no working USB capture device found (only codec nodes "
               "present?); not opening a decoder node");
      broken_.store(true);
      return false;
    }
  }

  // Log what the camera actually supports (formats / sizes / fps).
  log_v4l2_capabilities(dev);

  const int want_w   = pref_w_   > 0 ? pref_w_   : 640;
  const int want_h   = pref_h_   > 0 ? pref_h_   : 480;
  const int want_fps = pref_fps_ > 0 ? (int)(pref_fps_ + 0.5) : 30;
  LG_INFO("pref_w_=%d pref_h_=%d pref_fps_=%.1f", pref_w_, pref_h_, pref_fps_);

  // Raw V4L2 capture. We deliberately DO NOT use cv::VideoCapture here: its
  // open() issues VIDIOC_STREAMON internally, which locks the UVC frame rate
  // before any CAP_PROP_FPS / VIDIOC_S_PARM takes effect (camera ends up stuck
  // at its default low rate, e.g. 5 fps). By driving V4L2 directly we guarantee
  // the order: S_FMT -> S_PARM(fps) -> REQBUFS -> mmap/QBUF -> STREAMON, so the
  // requested frame rate is honoured.
  // Acquire the device and negotiate the format, retrying on EBUSY. A UVC
  // camera tears its streaming interface down ASYNCHRONOUSLY after close(); on
  // an idle box that is instant, but under load (7 other engines) the teardown
  // lags. Because this engine opens the node a few times in quick succession
  // (auto-detect scan, capability probe, then here), the real open()+S_FMT can
  // race the previous close() and get EBUSY -- which used to kill the engine and
  // crash-loop it forever. Retry with a short backoff so the device settles.
  v4l2_format fmt{};
  bool acquired = false;
  constexpr int kMaxOpenAttempts = 25;   // ~5s at 200ms between tries
  for (int attempt = 0; attempt < kMaxOpenAttempts && !acquired; ++attempt) {
    if (attempt > 0) {
      cleanup();  // close fd_ from the failed try before reopening
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    fd_ = ::open(dev.c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ < 0) {
      if (errno == EBUSY) { LG_INFO("usb: open '%s' busy, retry %d", dev.c_str(), attempt); continue; }
      LG_ERROR("usb: cannot open '%s': %s", dev.c_str(), strerror(errno));
      broken_.store(true);
      return false;
    }

    // 0. Verify this node is a streaming video-capture device.
    v4l2_capability cap;
    std::memset(&cap, 0, sizeof(cap));
    if (ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
      LG_ERROR("usb: VIDIOC_QUERYCAP failed on '%s': %s", dev.c_str(), strerror(errno));
      cleanup();
      broken_.store(true);
      return false;
    }
    const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                          ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
      LG_ERROR("usb: '%s' is not a streaming capture node", dev.c_str());
      cleanup();
      broken_.store(true);
      return false;
    }

    // 1. Set format. Prefer MJPEG (compressed, higher fps on capable cameras);
    // fall back to YUYV (raw) for simple/cheap cameras that only offer YUYV.
    // Whatever the driver negotiates is recorded and used to pick the decoder.
    std::memset(&fmt, 0, sizeof(fmt));
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = (unsigned)want_w;
    fmt.fmt.pix.height      = (unsigned)want_h;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field       = V4L2_FIELD_ANY;
    if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
      if (errno == EBUSY) { LG_INFO("usb: S_FMT '%s' busy, retry %d", dev.c_str(), attempt); continue; }
      LG_ERROR("usb: VIDIOC_S_FMT(MJPEG) failed: %s", strerror(errno));
      cleanup();
      broken_.store(true);
      return false;
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
      // Driver downgraded MJPEG (camera doesn't support it). Explicitly request
      // YUYV so we know exactly what we're decoding.
      LG_INFO("usb: camera did not accept MJPEG; requesting YUYV instead");
      v4l2_format yfmt;
      std::memset(&yfmt, 0, sizeof(yfmt));
      yfmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      yfmt.fmt.pix.width       = (unsigned)want_w;
      yfmt.fmt.pix.height      = (unsigned)want_h;
      yfmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
      yfmt.fmt.pix.field       = V4L2_FIELD_ANY;
      if (ioctl(fd_, VIDIOC_S_FMT, &yfmt) == 0) {
        fmt = yfmt;
      }
    }
    acquired = true;
  }
  if (!acquired) {
    LG_ERROR("usb: '%s' stayed busy after %d attempts, giving up this cycle",
             dev.c_str(), kMaxOpenAttempts);
    broken_.store(true);
    return false;
  }
  pixfmt_ = fmt.fmt.pix.pixelformat;
  cap_w_  = (int)fmt.fmt.pix.width;
  cap_h_  = (int)fmt.fmt.pix.height;
  {
    char fourcc[5] = {
      (char)(pixfmt_ & 0xff), (char)((pixfmt_ >> 8) & 0xff),
      (char)((pixfmt_ >> 16) & 0xff), (char)((pixfmt_ >> 24) & 0xff), 0
    };
    if (pixfmt_ != V4L2_PIX_FMT_MJPEG && pixfmt_ != V4L2_PIX_FMT_YUYV) {
      LG_WARN("usb: negotiated unsupported pixel format '%s' (0x%08x); "
              "decode will likely fail", fourcc, pixfmt_);
    } else {
      LG_INFO("usb: negotiated pixel format '%s' %dx%d", fourcc, cap_w_, cap_h_);
    }
  }

  // 2. Set frame rate BEFORE STREAMON. This is the critical step.
  v4l2_streamparm parm;
  std::memset(&parm, 0, sizeof(parm));
  parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  parm.parm.capture.timeperframe.numerator   = 1;
  parm.parm.capture.timeperframe.denominator = (unsigned)want_fps;
  if (ioctl(fd_, VIDIOC_S_PARM, &parm) < 0) {
    LG_WARN("usb: VIDIOC_S_PARM(%d fps) failed: %s", want_fps, strerror(errno));
  }

  // Read back the rate the driver actually agreed to (authoritative).
  v4l2_streamparm parm_actual;
  std::memset(&parm_actual, 0, sizeof(parm_actual));
  parm_actual.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ioctl(fd_, VIDIOC_G_PARM, &parm_actual);
  const unsigned den = parm_actual.parm.capture.timeperframe.denominator;
  const unsigned num = parm_actual.parm.capture.timeperframe.numerator ?
                       parm_actual.parm.capture.timeperframe.numerator : 1;
  const double actual_fps = (double)den / (double)num;
  LG_INFO("usb opened %s (w=%u h=%u fps=%.1f)  [requested %d fps]",
          dev.c_str(), fmt.fmt.pix.width, fmt.fmt.pix.height, actual_fps, want_fps);

  // 3. Request mmap buffers.
  v4l2_requestbuffers req;
  std::memset(&req, 0, sizeof(req));
  req.count  = 4;
  req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0 || req.count == 0) {
    LG_ERROR("usb: VIDIOC_REQBUFS failed: %s", strerror(errno));
    cleanup();
    broken_.store(true);
    return false;
  }

  // 4. mmap each buffer and queue it.
  bufs_.assign(req.count, nullptr);
  buf_lens_.assign(req.count, 0);
  for (unsigned i = 0; i < req.count; ++i) {
    v4l2_buffer buf;
    std::memset(&buf, 0, sizeof(buf));
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = i;
    if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
      LG_ERROR("usb: VIDIOC_QUERYBUF[%u] failed: %s", i, strerror(errno));
      cleanup(); broken_.store(true); return false;
    }
    buf_lens_[i] = buf.length;
    bufs_[i]     = ::mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd_, buf.m.offset);
    if (bufs_[i] == MAP_FAILED) {
      bufs_[i] = nullptr;
      LG_ERROR("usb: mmap buf[%u] failed: %s", i, strerror(errno));
      cleanup(); broken_.store(true); return false;
    }
    if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
      LG_ERROR("usb: VIDIOC_QBUF[%u] failed: %s", i, strerror(errno));
      cleanup(); broken_.store(true); return false;
    }
  }

  // 5. Start streaming (now that fps is locked in).
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
    LG_ERROR("usb: VIDIOC_STREAMON failed: %s", strerror(errno));
    cleanup(); broken_.store(true); return false;
  }

  streaming_ = true;
  connected_.store(true);
  return true;
}

void UsbInputSource::cleanup() noexcept {
  if (streaming_ && fd_ >= 0) {
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd_, VIDIOC_STREAMOFF, &type);
  }
  streaming_ = false;
  for (size_t i = 0; i < bufs_.size(); ++i) {
    if (bufs_[i] && bufs_[i] != MAP_FAILED) ::munmap(bufs_[i], buf_lens_[i]);
  }
  bufs_.clear();
  buf_lens_.clear();
  pixfmt_ = 0;
  cap_w_ = 0;
  cap_h_ = 0;
  if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

bool UsbInputSource::start() noexcept {
  // open() already negotiated format and started streaming.
  return fd_ >= 0 && streaming_ && !broken_.load();
}

void UsbInputSource::stop() noexcept {
  // Streaming is torn down in close(); nothing to do here.
}

void UsbInputSource::close() noexcept {
  cleanup();
  connected_.store(false);
}

FetchStatus UsbInputSource::tryFetch(FrameView& out) noexcept {
  // If we're being told to stop, exit immediately
  if (stopping_.load(std::memory_order_relaxed)) {
    return FetchStatus::Broken;
  }

  if (fd_ < 0 || !streaming_ || broken_.load(std::memory_order_relaxed)) {
    return FetchStatus::Broken;
  }

  // Wait up to ~200ms for a frame. A bounded timeout lets the worker loop
  // re-check the stopping_ flag promptly (request_stop just sets the flag).
  fd_set fds; FD_ZERO(&fds); FD_SET(fd_, &fds);
  timeval tv{0, 200000};
  int ready = ::select(fd_ + 1, &fds, nullptr, nullptr, &tv);
  if (ready == 0) {
    return FetchStatus::Timeout;
  }
  if (ready < 0) {
    if (errno == EINTR || errno == EAGAIN) return FetchStatus::Timeout;
    LG_WARN("usb: select failed: %s", strerror(errno));
    broken_.store(true, std::memory_order_relaxed);
    return FetchStatus::Broken;
  }

  // Dequeue a filled buffer.
  v4l2_buffer buf;
  std::memset(&buf, 0, sizeof(buf));
  buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;
  if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
    if (errno == EAGAIN || errno == EINTR) return FetchStatus::Timeout;
    if (errno == EIO) {
      // Transient: buffer was never dequeued, do not requeue.
      LG_WARN("usb: VIDIOC_DQBUF transient EIO — skipping frame");
      return FetchStatus::Timeout;
    }
    LG_WARN("usb: VIDIOC_DQBUF error: %s", strerror(errno));
    broken_.store(true, std::memory_order_relaxed);
    return FetchStatus::Broken;
  }

  if (buf.index >= bufs_.size() || buf.bytesused == 0) {
    LG_WARN("usb: bad DQBUF index=%u bytesused=%u — skipping", buf.index, buf.bytesused);
    ioctl(fd_, VIDIOC_QBUF, &buf);
    return FetchStatus::Timeout;
  }

  // Decode the captured buffer into BGR based on the negotiated pixel format.
  const auto* data = static_cast<uint8_t*>(bufs_[buf.index]);
  cv::Mat bgr;
  if (pixfmt_ == V4L2_PIX_FMT_MJPEG) {
    cv::Mat compressed(1, static_cast<int>(buf.bytesused), CV_8UC1,
                       const_cast<uint8_t*>(data));
    bgr = cv::imdecode(compressed, cv::IMREAD_COLOR);
  } else if (pixfmt_ == V4L2_PIX_FMT_YUYV) {
    // Raw YUYV (YUY2): 2 bytes per pixel. Convert to BGR.
    const size_t need = (size_t)cap_w_ * (size_t)cap_h_ * 2;
    if (buf.bytesused >= need && cap_w_ > 0 && cap_h_ > 0) {
      cv::Mat yuyv(cap_h_, cap_w_, CV_8UC2, const_cast<uint8_t*>(data));
      cv::cvtColor(yuyv, bgr, cv::COLOR_YUV2BGR_YUYV);
    }
  } else {
    // Unsupported format negotiated; nothing we can decode.
    ioctl(fd_, VIDIOC_QBUF, &buf);
    broken_.store(true, std::memory_order_relaxed);
    return FetchStatus::Broken;
  }

  // Requeue the buffer once decoding is done. Both decoders (imdecode for
  // MJPEG, cvtColor for YUYV) read directly from the mmap'd buffer, so it must
  // stay dequeued until decode completes. With 4 ring buffers this brief hold
  // does not starve capture. The heavier work (FrameView copy, downstream
  // inference) happens only after this QBUF returns the buffer to the driver.
  if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
    LG_WARN("usb: VIDIOC_QBUF after decode failed: %s", strerror(errno));
  }

  if (bgr.empty()) {
    return FetchStatus::Timeout;
  }
  if (!bgr.isContinuous()) bgr = bgr.clone();

  // Package it into the FrameView (copy into scratch so it stays valid).
  const size_t need_sz = size_t(bgr.cols) * size_t(bgr.rows) * 3;
  if (scratch_bgr_.size() != need_sz) {
    scratch_bgr_.resize(need_sz);
  }
  std::memcpy(scratch_bgr_.data(), bgr.data, need_sz);

  const int64_t now_ns_val =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()
      ).count();

  // Return full resolution frame without pre-scaling
  // Let each inference worker (face/yolo) resize as needed
  out.fmt     = PixelFormat::BGR24;
  out.width   = bgr.cols;
  out.height  = bgr.rows;
  out.orig_width  = bgr.cols;   // V7.1: Store original camera dimensions for visualization
  out.orig_height = bgr.rows;   // V7.1: Same as width/height for USB (no preprocessing)
  out.stride0 = bgr.cols * 3;
  out.stride1 = 0;
  out.plane0  = scratch_bgr_.data();
  out.plane1  = nullptr;
  out.pts_ns  = now_ns_val;

  frames_ok_.fetch_add(1, std::memory_order_relaxed);
  last_ok_ns_.store(now_ns_val, std::memory_order_relaxed);
  connected_.store(true, std::memory_order_relaxed);

  return FetchStatus::Ok;
}


FetchStatus UsbInputSource::fetch(FrameView& out, int timeout_ms) noexcept {
  auto start = SteadyClock::now();
  while (true) {
    auto st = tryFetch(out);
    if (st == FetchStatus::Ok || st == FetchStatus::Error) return st;
    if (timeout_ms <= 0) return st;
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - start).count();
    if (elapsed_ms >= timeout_ms) return FetchStatus::Timeout;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void UsbInputSource::setPreferredResolution(int w, int h) {
  pref_w_ = w; pref_h_ = h;
}
void UsbInputSource::setPreferredFps(double fps) {
  pref_fps_ = fps;
}

HealthInfo UsbInputSource::getHealth() const noexcept {
  HealthInfo h{};
  h.frames_ok = frames_ok_.load(std::memory_order_relaxed);
  h.last_ok_ns = last_ok_ns_.load(std::memory_order_relaxed);
  h.connected = (fd_ >= 0 && streaming_);
  if (broken_.load()) h.status = HealthStatus::Error;
  else if (!h.connected) h.status = HealthStatus::Disconnected;
  else h.status = HealthStatus::Ok;
  return h;
}
