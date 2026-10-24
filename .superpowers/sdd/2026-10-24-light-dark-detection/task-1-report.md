# Task 1 report: LightAnalyzer core

## Implemented
- `light_state.h`, `light_config.h` (spec 4.5 defaults, comment pointing at spec), `light_analyzer.h`, `light_analyzer.cpp`.
- BT.601 luma via integer weights (x1000), chroma = max-min, uint64_t accumulators, central ROI, resolution-relative stride (`kTargetSampleCount` = 2048; config `sample_stride` is the floor), at least one sample guaranteed.
- Constructor clamps `sample_stride>=1`, `roi_frac` to [0.01,1].
- Mutex (`lock_guard`) around all state; `update()` ignores empty/undersized buffers.
- State machine per spec 4.3: warmup from first sample, IR rule (skipped for monochrome cameras), hysteresis dead-band, dwell gate on (state, ir_mode) pair; first commit out of Unknown bypasses dwell.

## TDD
- RED: with `light_analyzer.cpp` absent, `make build-engine-tests` failed at CMake generate ("No SOURCES given to target: run_engine_tests", Error 1).
- GREEN first run: 156 passed, 2 failed (DarkColorIsDark, DwellSuppressesRapidFlip): my test colour (10,40,60) had luma 42.5 > dark_luma. Fixed test data to (5,30,50) (luma 33, chroma 45), not the implementation.
- GREEN: `make run-engine-tests` -> `[  PASSED  ] 158 tests.` (10 LightAnalyzer tests).

## Files
engine/include/analysis/{light_state,light_config,light_analyzer}.h, engine/src/analysis/light_analyzer.cpp, engine/tests/test_light_analyzer.cpp, engine/tests/CMakeLists.txt, engine/CMakeLists.txt.

## Notes / concerns
- Tests use ema_alpha=1.0 for determinism.
- In the dead-band with a colour frame after IR mode, candidate is (current state, ir_mode=false); the dwell gate applies to that ir_mode flip.
- `argus-engine` cross-compile target not built here (host tests only).

## Fix round 1
Changed (no candidate-logic change):
- Added `IrExitIntoDeadBandHoldsDarkInColourMode` (DARK stays, ir_mode false, level>0).
- `kMaxLevel` replaces literal 100.0 in level computation.
- 64-bit sum comment now states the real WHY (defensive CWE-190 guard; ~2048 samples would fit in 32 bits).
- Constructor: NaN/too-small roi_frac handled before clamping (NaN -> default 0.6, <min -> min, >1 -> 1); ema_alpha outside (0,1] falls back to the default.
- Added tests: zero stride, zero/oversized roi_frac, NaN roi_frac + bad alpha, zero-width frame.

Command: `make run-engine-tests`
Output: `[  PASSED  ] 164 tests.` (no warnings or errors)
