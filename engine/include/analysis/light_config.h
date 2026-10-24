#ifndef LIGHT_CONFIG_H
#define LIGHT_CONFIG_H

/// Defaults are the spec section 4.5 values (docs/superpowers/specs/
/// 2026-10-24-light-dark-detection-design.md), the source of truth. They are
/// intentionally duplicated in the Go config; keep both identical.
struct LightCfg {
    bool enabled{true};
    int interval_seconds{10};
    bool on_change{true};
    float dark_luma{40.f};
    float light_luma{70.f};
    float chroma_threshold{10.f};
    float ema_alpha{0.1f};
    int min_dwell_seconds{5};
    int warmup_seconds{3};
    float roi_frac{0.6f};
    int sample_stride{8};  ///< minimum stride; the analyzer may sample more sparsely at high resolution
    bool monochrome_camera{false};
};

#endif  // LIGHT_CONFIG_H
