# RF-DETR → ONNX → RKNN: Feasibility for argus-rtsp-watcher

Status: research / pre-spike, revised after an independent adversarial review
(§10). Nothing here has been run on a player. Every performance number is
either quoted from a cited source or explicitly marked as an estimate;
anything not confirmed is marked **UNVERIFIED**.

## 1. Bottom line

| Question | Answer |
|---|---|
| Does the stock RF-DETR ONNX convert with the toolkit this repo pins (v2.3.0)? | **Not as-is.** Deformable cross-attention emits ONNX `GridSample` (2 nodes for Nano), which v2.3.0 has no built-in kernel for. Two-stage query selection emits `TopK` + `GatherElements`, which are CPU-only at best (TopK support in 2.3.0 is **UNVERIFIED** — the op-list `.md` says *Not Supported*, the compiler PDF lists a CPU kernel). |
| Can it be made to convert? | **Yes — several routes.** (1) register `GridSample` as a **custom CPU op** inside the `.rknn` (v2.3.0 has `reg_custom_op` / `rknn_register_custom_ops`); (2) rewrite it to rank-1 `Gather` using RF-DETR's own TFLite ONNX rewrite; (3) upgrade to **toolkit v2.5.0**, which adds `GridSample` (GPU) and `TopK` (CPU); (4) split backbone (NPU) / decoder (CPU). |
| Will it be fast enough? | **Unknown — and the engine's current NPU scheduling makes it worse.** The only RK3588 measurement (Nano, 198 ms/frame) is an unprofiled Python FP16 upper bound. Separately, this engine runs every model with `RKNN_NPU_CORE_0_1_2`, under which all non-conv ops (MatMul, attention, LayerNorm, GELU, Softmax…) **fall back to Core0** — so every stream's transformer work would queue on one core. Realistic expectation on XT5 even after optimization: a handful of streams, not 8. |
| Will INT8 hold accuracy? | **Plain INT8 PTQ: no** (RT-DETR-R18 on RK3588: 21.0 → 9.5 AP). **Auto-hybrid INT8: probably yes** (same source: 20.8 vs 21.0 FP16). LayerNorm is FP16-only on the NPU regardless. |
| Is it worth it versus alternatives? | Questionable. **YOLOX-M** (Apache 2.0, already in the zoo, drop-in for our post-processing) is 46.9 AP at ~62 ms/core — RF-DETR Nano is only ~1.5 AP higher on COCO. That is the comparison the spike must beat. |
| Recommendation | Do **not** plan RF-DETR as a drop-in YOLOX replacement. If the accuracy gain matters, first evaluate YOLOX-M (cheap); then, only if needed, run the RF-DETR spike in §8 with hard go/no-go gates on XT5 and XS6. LS5/HS5 (RK3568) are out of scope. |

## 2. What RF-DETR is

Source: [roboflow/rf-detr](https://github.com/roboflow/rf-detr) (cloned at
`5b22dfc`, 2026-10-16, package `1.12.0.dev`, Python ≥ 3.10).

- **Backbone:** DINOv2 ViT-S with windowed attention (`dinov2_windowed_small`)
  for Nano–Large; one window partition at the embedding, windows folded into
  the batch dim (B×4 for Nano), full-attention layers merge windows with
  `.view`.
- **Decoder:** 2–4 layers of self-attention + multi-scale deformable
  cross-attention (`ms_deform_attn_core_pytorch` → `F.grid_sample`). Nano: 1
  feature level, 16 heads, 2 points, 300 queries, 2 layers.
- **Two-stage query selection:** `torch.topk` over encoder proposals + gather.
- **NMS-free outputs:** `dets [1,300,4]` (normalized cx,cy,w,h) and
  `labels [1,300,91]` (sigmoid logits, COCO 91-index — `person` = **1**).
- **Preprocessing:** **stretch** resize (antialiased bilinear) to a square,
  ImageNet mean/std, RGB. No letterbox.
- **Export tooling worth reusing:** built-in `backbone_only` export
  (`export/prepare.py`); ONNX-level `GridSample` → rank-1 `Gather` rewrite
  (`export/_tflite/exporter.py`, `_replace_gridsample_for_tflite`); TopK
  replacement for Apple ANE (`export/_neural_engine.py`, `one_hot_top_rows` —
  **not** RKNN-friendly, see §4).

### Variants relevant to the edge

| Variant | COCO AP50:95 | Params | Input | Backbone tokens | Decoder layers | License |
|---|---|---|---|---|---|---|
| Nano | 48.4 | 30.5 M | 384² | 576 (4 windows × 144) | 2 | Apache 2.0 |
| Small | 53.0 | 32.1 M | 512² | 1024 | 3 | Apache 2.0 |
| Medium | 54.7 | 33.7 M | 576² | 1296 | 4 | Apache 2.0 |
| Pico / Femto / Atto | 41.6 / 37.8 / 30.5 | ~8 M | 560² / 384² / 380² | — | — | **PML 1.0** (legal review) |
| *(current)* YOLOX-S | 40.5 | 9.0 M | 640² | n/a | n/a | Apache 2.0 |
| *(alternative)* YOLOX-M | 46.9 | 25.3 M | 640² | n/a | n/a | Apache 2.0 |

RF-DETR README latencies are NVIDIA T4 TensorRT FP16 — irrelevant to Rockchip.
Nano runs at 384² vs our 640², which works against small/distant people.
Resolution is configurable in multiples of 32 for Nano, but 640² costs ~2.8×
the tokens.

## 3. What this project needs from a detector

- `scripts/build-models.sh`: rknn-toolkit2 / rknn_model_zoo **v2.3.0**,
  `convert.py <onnx> <soc> i8`, per-SoC `.rknn` for RK3588, RK3576, RK3568.
- `engine/src/yolox.cc`: C RKNN API; `rknn_set_core_mask(select_core_mask(npu_core))`.
  `configs/config.json` sets `npu_core: -1` → **`RKNN_NPU_CORE_0_1_2`** for
  every engine process.
- `librknnrt.so` **2.3.0** bundled per SoC in `staging/<SOC>/lib/` and preferred
  via `LD_LIBRARY_PATH` — runtime version is under our control; the kernel
  `rknpu` driver is not.
- **Capacity targets are inconsistent in-repo:** README and
  `internal/platform/platform.go` (`rk3588StreamCapacity = 8`) say XT5 = 8
  streams; `docs/DESIGN.md` §9 says 3 streams (~60 inf/s incl. RetinaFace).
  RetinaFace mobile320 is minor (~4.4 ms/core per the zoo).
- No CPU inference engine (ONNX Runtime etc.) exists in the SDK/extension.

## 4. Operator-level conversion analysis

Sources: `RKNNToolKit2_OP_Support-2.3.0.md`,
`05_RKNN_Compiler_Support_Operator_List_V2.3.0.pdf` (§3.2 RK3588 NPU, §3.5
RK3576 NPU, §4 CPU), `04_..._RKNNRT_V2.3.0_EN.pdf`, all in the shared build
cache.

| RF-DETR construct | ONNX op(s) | RKNN v2.3.0 placement | Risk | Mitigation |
|---|---|---|---|---|
| Patch embed | `Conv` | NPU | low | — |
| Pos-embed interpolation | bicubic `Resize` on a constant | unclear | low | Constant-fold (onnxsim) at fixed resolution |
| Window partition | 5-D `Reshape`/`Transpose` | NPU `exWindow` fusion exists for exactly this pattern | low–medium | Verify fusion; **check batch=4 window folding** against `Softmax`/`Slice`/`Tile` "batchsize: 1" limits |
| ViT attention | `MatMul`, `Softmax` / SDPA | NPU (`exMatMul`, `exSDPAttention`, int8/fp16); **RK3588: no multi-core** | **high under 0_1_2** | See §5 core-mask problem |
| GELU | exact GELU emits `Erf` | `exGelu` is NPU; bare `Erf` is **CPU-only** (§4.70) | **high** | Confirm fusion with `perf_debug`; else export tanh-GELU (retrain/finetune check) — plausible cause of the slow measured backbone (**UNVERIFIED**) |
| LayerNorm | `LayerNormalization` | NPU `exNorm`, **FP16 only** on RK3588/RK3576/RK3568 | medium | Accept FP16 islands (quant/dequant cost) |
| Proposal ranking | `TopK` + `GatherElements` | Both CPU-only; TopK in 2.3.0 **UNVERIFIED**; v2.5.0 adds TopK (CPU) | medium | Keep as CPU ops (small: 576 scores → 300). RF-DETR's `one_hot_top_rows` is **not** a fix: uses `Abs` (*Not Supported*) and `Greater`/`Equal`/`ReduceSum`, which are not in the RK3588 NPU list |
| Deformable cross-attn | **`GridSample`** ×2 (Nano) | **No kernel in v2.3.0**; v2.5.0: GPU only | **blocker as-is** | (a) custom CPU op via `reg_custom_op` + `rknn_register_custom_ops` (see `rknpu2/examples/rknn_custom_op_demo`) — ~9.6k samples × 16 ch per node, trivial compute, cost is NPU↔CPU switches; (b) RF-DETR TFLite rewrite → rank-1 `Gather` (matches RK3588 NPU Gather spec) but it emits `Floor`/`Cast` (CPU) and index math must stay FP16 (exact for Nano's 26×26 padded map) — never INT8; (c) v2.5.0 GPU GridSample, gated on Mali/OpenCL on BrightSign OS (**UNVERIFIED**); (d) split model |
| Final scoring | 300×91 sigmoid + threshold | CPU, our C code | low | No NMS needed |

**Toolkit versions:**

| Version | Relevant changes |
|---|---|
| 2.3.0 (pinned) | Custom CPU/GPU op API; hybrid quantization (`hybrid_quantization_step1/2`); `op_target`; `quantized_algorithm=mmse/kl_divergence` |
| 2.3.2 (Apr 2025) | "Improved einsum and Norm", **automatic mixed precision** (`auto_hybrid`, `cos_threshold`) |
| 2.5.0 ("20 Oct", year **UNVERIFIED**) | **GridSample (GPU)**, **Pow/TopK (CPU)**, exNorm, Transpose/Reshape improvements, fusion-rule updates |

Any upgrade means: rebuild YOLOX + RetinaFace on the new toolkit, bump the
bundled `librknnrt.so`, and verify the player's `rknpu` kernel driver version
on all three SoCs.

## 5. Performance evidence

| Model | Device | Precision | Latency | Source |
|---|---|---|---|---|
| YOLOX-S 640² | RK3588, 1 core | INT8 | 37.1 fps (≈27 ms) | rknn_model_zoo v2.3.0 README (inference only) |
| YOLOX-M 640² | RK3588, 1 core | INT8 | 16.0 fps (≈62 ms) | same |
| YOLOX-S 640² | RK3576, 1 core | INT8 | 41.5 fps | same |
| YOLOX-S 640² | RK3568 | INT8 | 15.2 fps | same |
| CLIP ViT-B/32 224² | RK3588 / RK3568 | FP16 | 6.5 / 2.3 fps | same |
| MobileSAM encoder 448² | RK3588 / RK3568 | FP16 | 10.0 / 1.0 fps | same |
| RF-DETR Nano 384², full ONNX Runtime | RK3588 A76×4 | FP32 | 329 ms | [rfdetr-on-rockchip-npu](https://github.com/AlexanderDhoore/rfdetr-on-rockchip-npu) |
| RF-DETR Nano 384², RKNN backbone + ORT decoder | RK3588, 1 core (`NPU_CORE_AUTO` default) | backbone FP16, toolkit **2.3.2**, Python lite2, fp32 NCHW input | 198 ms (177 + 21) | same, `benchmarks/rock5b_benchmarks.json` |
| RT-DETR-R18 (2 dec layers, 150 queries) | RK3588 | auto-hybrid INT8, toolkit 2.5.0 | 235 ms single-context; **25 FPS aggregate** with 10 worker threads, `npu-cores auto` | [Dmsj-zktd/rk3588_-rt-dert](https://github.com/Dmsj-zktd/rk3588_-rt-dert) (single author, unverified) |

How to read these:

- **The 177 ms backbone is an upper bound, not an estimate.** It is only 1.7×
  faster than ORT FP32 on four A76 cores, which points to heavy CPU fallback
  inside the "NPU" backbone (likely `Erf`, possibly batch-folded windows) —
  **UNVERIFIED**; nobody ran `perf_debug`. A cleaned-up C-API build could be
  much faster. The true cost is **unknown**.
- **Compute is not the problem.** Nano backbone ≈ 12.2 GMAC linear + ≤3 GMAC
  attention; YOLOX-S ≈ 13.4 GMAC. The gap is NPU efficiency on transformer
  graphs (FP16 islands, layout traffic, CPU fallbacks, no multi-core).
- **Latency ≠ throughput.** The RT-DETR data shows ~9× more aggregate
  throughput than single-context latency implies, via concurrent contexts on
  separate cores. That is the mode a multi-stream system needs — and this
  engine does not currently use it (below).

### The core-mask problem (specific to this engine)

`RKNNRT_V2.3.0` API reference: in `RKNN_NPU_CORE_0_1` / `0_1_2` mode only
Conv, DepthwiseConv, Add, Concat, Relu, Clip, Relu6, ThresholdedRelu, PRelu
and LeakyRelu are multi-core accelerated; *"Other type of op will fallback to
Core0"*. For a ViT that is nearly everything. With N engine processes all at
`npu_core: -1`, every stream's transformer ops serialize on **Core0** while
cores 1–2 idle. `docs/DESIGN.md` §12 already lists all-core contention as an
open risk.

Getting multi-core throughput for RF-DETR requires per-process core pinning
or `RKNN_NPU_CORE_AUTO` — a scheduling change that also affects YOLOX and
RetinaFace and must be re-validated.

### Stream budget (XT5, person detector only — illustrative)

| Detector | Per-inference (1 core) | Streams @10 fps, `0_1_2` (current) | Streams @10 fps, pinned/AUTO |
|---|---|---|---|
| YOLOX-S INT8 | ~27 ms | conv-heavy, scales across cores | ~8–11 |
| YOLOX-M INT8 | ~62 ms | conv-heavy | ~4–5 |
| RF-DETR Nano, measured (FP16, unprofiled) | ~198 ms | <1 (Core0-bound) | ~1–1.5 |
| RF-DETR Nano, hypothetical 60 ms after cleanup | ~60 ms (**guess**) | ~1.5 (Core0-bound) | ~4–5 |

RetinaFace load and pre/post-processing are not included. Any split design
also costs ~21 ms of A76 CPU per frame (~1.7 big cores at 80 frames/s),
competing with GStreamer decode, RGA, tracking and MQTT.

## 6. Quantization

- **Plain PTQ INT8 fails on DETR:** RT-DETR-R18 on RK3588 — FP16 20.96 AP →
  INT8 9.47 AP.
- **Auto-hybrid INT8 recovers it:** same source — 20.76 AP (toolkit 2.3.2,
  `cos_threshold=0.90`); deployed 2.5.0 model 20.49 AP.
- **LayerNorm (`exNorm`) is FP16-only** on RK3588/RK3576/RK3568 — so every
  transformer block contains INT8↔FP16 transitions regardless of strategy.
- **w8a16 is not available on RK3588** (RK3562 only); w4a16 is RK3576 only.
- Deformable-attention sampling offsets and any `Gather` index math must stay
  FP16 or CPU — INT8 there is direct localization error, which hurts ByteTrack
  and `in_zone`.
- The zoo shipping ViTs FP16-only is weak evidence by itself (it also ships
  some CNNs FP16-only); the RT-DETR numbers are the real evidence.
- **Implication:** plan on 2.3.2+ `auto_hybrid`, not 2.3.0 plain `i8`.

## 7. Integration challenges in this engine

The `IModelRunner` interface fits, but the engine bypasses it in several places:

1. `engine/src/orchestration/inference_worker.cpp:282` —
   `dynamic_cast<RKNNYoloXRunner*>(runner)`; a new runner's detections would be
   silently dropped.
2. `engine/src/main.cpp:252,266` hard-code `ModelFamily::YOLOX` for the
   secondary model.
3. `class_id != 0` person filters in `byte_tracker_lite.cpp:71` and
   `inference_worker.cpp:512,540` (RF-DETR person = 1), plus a hard-coded
   `0.25f` score floor.
4. **Letterbox happens upstream** in `inference_worker.cpp` (OpenCV, ~line 230),
   not just in the runner; RF-DETR needs stretch resize and independent x/y
   de-scaling. Antialiased vs `INTER_LINEAR` resize is a small accuracy risk.
5. `build-models.sh` stages `coco_80_labels_list.txt`; `postprocess.cc` uses
   `OBJ_CLASS_NUM` (80).
6. **Score semantics change:** DETR sigmoid scores ≠ YOLOX obj×cls.
   `streams[].confidence` (default 0.5), the 0.25 floor, and ByteTrack
   high/low thresholds all need re-tuning.
7. **Core scheduling change** (§5) for all models.
8. **Custom op path** (if chosen): C++ `GridSample` kernel registered via
   `rknn_register_custom_ops` in the runner — modest, self-contained.
9. **Split path** (if chosen): needs ONNX Runtime cross-built into the SDK
   via `meta-bs`, or a hand-written decoder — substantially more work than (8).
10. **Build pipeline:** RF-DETR export needs Python ≥ 3.10 + torch; current
    `rknn_tk2` image is Ubuntu 20.04/cp38 → separate export container stage.
11. **Memory:** ~30 M params per engine process vs ~9 M for YOLOX-S.
12. **Per-SoC:** RK3568 lacks `exSDPAttention` and is 3–10× slower than
    RK3588 on ViT-class models — not viable. RK3576's `exSDPAttention`
    supports multi-core (int8 and fp16) and FlashAttentionV2 (fp16, requires
    RKNPU driver ≥ 0.9.7 — player driver version **UNVERIFIED**); it may be
    the best SoC for RF-DETR.
13. **Licensing:** Nano/Small/Medium/Large Apache 2.0; Pico/Femto/Atto PML 1.0.

## 8. Paths forward

| Option | What | Effort | Notes |
|---|---|---|---|
| **C. Better CNN (first)** | Swap to YOLOX-M (zoo, Apache 2.0) or PP-YOLOE-s; optionally fine-tune on person/site data | Low | Zero new ops, same post-process. ~46.9 AP at ~62 ms/core. YOLOv8/YOLO11 are **AGPL-3.0** — avoid without legal review |
| A. Full-graph RKNN | Custom CPU `GridSample` op (2.3.0) or v2.5.0 GPU GridSample; CPU TopK; `auto_hybrid` INT8 on 2.3.2+ | Medium–high | Most promising RF-DETR path; requires toolkit/runtime/driver bump for auto_hybrid |
| B. Split | `backbone_only` export on NPU; decoder in ORT/C++ on CPU | High | ORT cross-build or hand decoder; CPU cost scales with streams |
| D. Non-real-time use | RF-DETR on low-fps / single-stream / audit paths | Medium | Accuracy where it matters without multi-stream cost |

Also worth testing for RF-DETR cost reduction: fewer `num_queries` (load-safe
per `config.py`), and DETR variants designed to avoid bilinear `GridSample`
(RT-DETRv2 `discrete_sample`; D-FINE/DEIM — RKNN behavior **UNVERIFIED**).

### Proposed RF-DETR spike — go/no-go gates

0. **Baseline first:** convert and measure YOLOX-M on XT5 + XS6 (person AP on
   site-like clips, latency). This sets the bar.
1. Read `rknpu` driver version on XT5/XS6/LS5 players; confirm whether a
   2.3.2 / 2.5.0 `librknnrt.so` runs against it. Check for OpenCL/Mali on
   BrightSign OS (needed only for GPU GridSample).
2. Export RF-DETR Nano @384 (opset 17, static batch 1), onnxsim
   constant-fold. Replace `GridSample` with a custom CPU op (or 2.5.0 GPU).
3. Convert for rk3588 and rk3576; run `perf_debug` / `RKNN_LOG_LEVEL=4`.
   **Gate 1:** no unexpected CPU ops in the backbone (`Erf` in particular).
4. Benchmark with the C API: single context, and N concurrent processes under
   `0_1_2`, `AUTO`, and per-process pinning; FP16 / auto-hybrid INT8.
   **Gate 2:** aggregate throughput on XT5 supports the target stream count
   for the SKU (and beats YOLOX-M at equal accuracy, or wins on accuracy).
5. Person AP: ONNX FP32 vs RKNN FP16 vs RKNN hybrid, against shipped YOLOX-S
   **and** YOLOX-M. **Gate 3:** RKNN RF-DETR beats YOLOX-M person AP by a
   margin the product cares about, after quantization.
6. Only after all gates: runner + engine fixes (§7), threshold retuning,
   core-scheduling change, capacity table updates.

## 9. Sources

- [roboflow/rf-detr](https://github.com/roboflow/rf-detr) — `config.py`,
  `models/ops/functions/ms_deform_attn_func.py`, `models/transformer.py`,
  `models/backbone/dinov2_with_windowed_attn.py`, `export/prepare.py`,
  `export/_neural_engine.py`, `export/_litert/sampling.py`,
  `export/_tflite/exporter.py`, `export/_onnx/inference.py`.
- [AlexanderDhoore/rfdetr-on-rockchip-npu](https://github.com/AlexanderDhoore/rfdetr-on-rockchip-npu) —
  `export_onnx.py`, `convert_rknn.py`, `Dockerfile.rknn`, `benchmark_deploy.py`,
  `benchmarks/rock5b_benchmarks.json`.
- rknn-toolkit2 v2.3.0 docs (shared build cache): `RKNNToolKit2_OP_Support-2.3.0.md`,
  `05_RKNN_Compiler_Support_Operator_List_V2.3.0.pdf`,
  `03_..._RKNN_Toolkit2_V2.3.0_EN.pdf`, `04_..._RKNNRT_V2.3.0_EN.pdf`;
  `rknpu2/examples/rknn_custom_op_demo`.
- rknn_model_zoo v2.3.0 `README.md` performance table.
- [rknn-toolkit2 releases](https://github.com/airockchip/rknn-toolkit2/releases),
  [v2.5.0 notes](https://github.com/airockchip/rknn-toolkit2/releases/tag/v2.5.0),
  [changelog mirror (2.3.2)](https://science.ecosyste.ms/projects/96240).
- [Dmsj-zktd/rk3588_-rt-dert](https://github.com/Dmsj-zktd/rk3588_-rt-dert) — RT-DETR on RK3588 INT8/hybrid.
- [RT-DETRv2 paper](https://arxiv.org/pdf/2407.17140) — `discrete_sample`.
- [LibreYOLO RKNN export notes](https://www.libreyolo.com/docs/export/rknn) — toolkit 2.3.2 pinning.

## 10. Adversarial review — what changed

An independent reviewer checked the first draft against the primary sources.
Accepted corrections (key ones spot-verified in code/docs):

| Draft claim | Correction |
|---|---|
| No newer toolkit adds GridSample | v2.5.0 adds GridSample (GPU) and TopK (CPU) |
| GridSample is a hard blocker | v2.3.0 custom-op API makes it a custom CPU op — cheapest fix |
| `one_hot_top_rows` is NPU-friendly | Uses `Abs` (unsupported) and CPU-only compare/reduce ops |
| 3 cores → 3× throughput | Engine's `RKNN_NPU_CORE_0_1_2` sends non-conv ops to Core0 (verified in RKNNRT API ref + `configs/config.json`) |
| INT8 "roughly halves" AP | True for plain PTQ; auto-hybrid INT8 recovers ~FP16 accuracy (same source) |
| Split result proven on our toolkit | Measured on toolkit 2.3.2, Python, unprofiled — upper bound only |
| `IModelRunner` fits, nothing else changes | `dynamic_cast<RKNNYoloXRunner*>`, `class_id != 0`, upstream letterbox, hard-coded YOLOX family (verified) |
| README/DESIGN agree on 8 streams | DESIGN says 3; README/`platform.go` say 8 |
| Alternatives: YOLOv8/YOLO11 | AGPL-3.0; YOLOX-M (Apache, 46.9 AP) is the right comparison |
| Windowing is medium risk | `exWindow` fusion exists; real risk is batch-folded windows vs batch-1 op limits |
