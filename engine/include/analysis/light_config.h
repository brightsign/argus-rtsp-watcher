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

    /// Optional operator-drawn light-measurement region, normalized 0..1 of the
    /// frame. When region_enabled, the analyzer measures mean luminance inside
    /// this rectangle and decides LIGHT/DARK purely on region_dark_luma --
    /// bypassing the central-ROI chroma/luma state machine. The operator draws
    /// it on the config-server dashboard, the same way as a detection zone, so
    /// a lit-but-nighttime scene reads DARK when the region covers a dark area.
    /// EMA smoothing and min_dwell still apply so the state cannot flap.
    bool region_enabled{false};
    float region_x0{0.f}, region_y0{0.f}, region_x1{0.f}, region_y1{0.f};
    float region_dark_luma{40.f};  ///< region mean luma below this => DARK
};

#endif  // LIGHT_CONFIG_H
