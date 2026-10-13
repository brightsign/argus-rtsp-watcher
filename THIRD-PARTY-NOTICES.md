# Third-Party Notices

Argus RTSP Watcher is licensed under the Apache License, Version 2.0 (see
[`LICENSE`](LICENSE) and [`NOTICE`](NOTICE)).

**The Apache-2.0 grant covers only the original work in this repository** — the
Go supervisor (`cmd/`, `internal/`), the build/packaging scripts (`scripts/`,
`packaging/`, `Makefile`), the tools (`tools/`), and the documentation.

**It does not relicense any of the third-party components listed below.** This
project *aggregates* and *packages* those components; each one remains under its
own copyright and license, which continue to govern it both in source form and
as redistributed inside the packaged player extension (`*.zip`). Where a
component ships its own license text, that text is authoritative and travels
with the component (for example in `build/*/LICENSE.txt` and
`build/*/NOTICE.md`).

If anything here is ambiguous for your use, consult the upstream project's
canonical license — the summaries below are for convenience, not a substitute
for the component's own terms.

## Derived component

| Component | Origin | License |
|-----------|--------|---------|
| Analytics engine (`engine/`) | Derived from the BrightSign **Argus Audience Measurement Extension** | Carries its upstream Argus license; **not** relicensed by this repository. |

## Rockchip (proprietary / vendor terms)

Fetched by the build (`make prep`) and/or bundled into the extension. These are
**not** open source; they are redistributed under Rockchip's own terms.

| Component | Files | Terms |
|-----------|-------|-------|
| RKNN runtime | `librknnrt.so`, `rknn_api.h`, `rknn_matmul_api.h`, `rknn_box_priors.h`, related headers | Rockchip proprietary. Redistribution governed by Rockchip's license. |
| RGA / MPP | `librga.so*`, `librockchip_mpp.so*` | Rockchip. See the respective upstream project for its license (Rockchip's `linux-rga` / `mpp`). |

## Bundled runtime components (packaged into the extension)

Each is redistributed under its own license. SPDX identifiers are given as a
convenience; the authoritative terms are the upstream project's own license.

| Component | Typical files | License (confirm against the bundled version) |
|-----------|---------------|------------------------------------------------|
| Eclipse Mosquitto | `libmosquitto.so*`, `mosquitto` broker | EPL-2.0 / EDL-1.0 (dual) — see `build/mosquitto-build/LICENSE.txt` and `NOTICE.md` |
| MediaMTX | `mediamtx` | MIT |
| FFmpeg | `libavcodec`, `libavformat`, `libavfilter`, `libavutil`, `libswscale`, `libswresample` | LGPL-2.1-or-later (GPL if any GPL-only components are enabled) |
| GStreamer + plugins | `libgst*-1.0.so*`, `gstreamer-1.0/` | LGPL-2.1-or-later |
| OpenCV (incl. contrib) | `libopencv_*.so*` | Apache-2.0 (some contrib algorithms may be patent-encumbered in some jurisdictions) |
| OpenSSL 1.1 | `libssl.so.1.1`, `libcrypto.so.1.1` | OpenSSL License / original SSLeay License (dual) |
| GLib / GObject / GIO | `libglib-2.0`, `libgobject-2.0`, `libgio-2.0`, `libgmodule-2.0` | LGPL-2.1-or-later |
| libjpeg-turbo | `libjpeg.so*`, `libturbojpeg.so*` | IJG / BSD-3-Clause / zlib |
| libpng | `libpng16.so*` | PNG Reference Library License (libpng) |
| zlib | `libz.so*` | Zlib |
| libwebp / sharpyuv | `libwebp.so*`, `libsharpyuv.so*` | BSD-3-Clause |
| libtiff | `libtiff.so*` | libtiff (BSD-like) |
| libexif | `libexif.so*` | LGPL-2.1 |
| libgphoto2 | `libgphoto2.so*`, `libgphoto2_port.so*` | LGPL-2.1-or-later |
| libxml2 | `libxml2.so*` | MIT |
| PCRE | `libpcre.so*` | BSD-3-Clause |
| xz / liblzma | `liblzma.so*` | 0BSD / public domain |
| libwebsockets | `libwebsockets.so*` | MIT |
| libsrt | `libsrt.so*` | MPL-2.0 |
| libuv | `libuv.so*` | MIT |
| libdrm | `libdrm.so*` | MIT |
| oneTBB | `libtbb*.so*` | Apache-2.0 |
| glog | `libglog.so*` | BSD-3-Clause |
| libunwind | `libunwind.so*` | MIT |
| ORC | `liborc-0.4.so*` | BSD-2-Clause |
| libffi | `libffi.so*` | MIT |
| util-linux libs | `libblkid.so*`, `libmount.so*` | LGPL-2.1-or-later |
| libltdl | `libltdl.so*` | LGPL-2.1 |

The packaged extension may bundle additional shared libraries pulled in as
transitive dependencies by the BrightSign cross-compile SDK. Any such library
is likewise redistributed under its own upstream license.

## Models

Any neural-network model files (`*.rknn`) packaged with this extension are
derived from their respective upstream model weights and are subject to those
models' own licenses and usage terms. Confirm the license of each model before
redistribution.
