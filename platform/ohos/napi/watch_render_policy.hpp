#pragma once

#include <optional>

namespace mbgl {

namespace style {
class Style;
}

namespace ohos {

struct WatchRenderPolicy {
    static constexpr double minZoom = 0.0;
    static constexpr double maxZoom = 18.0;
    // Trần zoom phủ (tile LOD cap). 09/2026: basemap chính là VietMap Tilemap
    // vector (style "SEA Map Dark", source openmaptiles maxzoom 15) khi có
    // VIETMAP_TILE_KEY; fallback OpenFreeMap khi key rỗng (source maxzoom 14,
    // tile z15+ server trả 200 rỗng — đã verify). Cap 15 ≥ cả hai source-max
    // → vô điều kiện; chỉ cần tăng (17.0, plan S1: ~4-8× tile bytes) khi có
    // server phục vụ tile z16+.
    static constexpr double maxCoveringZoom = 15.0;
    static constexpr double maxZoomStepPerFrame = 2.0;
    static constexpr double minZoomDelta = 1.0e-6;
    // Digital Crown zoom session: apply pending delta, then idle this long
    // before unfreezing tile LOD. Not the GPU fallback interval.
    static constexpr int idleMilliseconds = 180;
    // GPU interactive cadence: 16ms target (~60fps) cho trải nghiệm vuốt chạm mượt mà.
    static constexpr int gpuInteractiveMilliseconds = 16;
    static constexpr int gpuIdleMilliseconds = 250;
    // Pacing theo nhịp vsync (P2: gate 12ms → render 83fps × 12-17ms ≈ 100%
    // duty của thread JS → input 120Hz bị trễ dưới fling liên hoàn → "quán
    // tính cũ lấn át tay" + freeze. 16ms = trần ~62fps khớp panel 60Hz, duty
    // ~72-100% tuỳ frame, chừa headroom cho input/framework).
    static constexpr int gpuInteractiveMinIntervalMilliseconds = 16;
    static constexpr int gpuIdleMinIntervalMilliseconds = 150;
    // Watch touch streams often insert Up/Down between Moves (~100–400ms).
    // Dropping to the idle pump in those gaps is the "cụt bộ" hitch.
    static constexpr int gpuInteractiveHoldMilliseconds = 280;
    static constexpr int gpuHitchMilliseconds = 45;
    // Epsilon đẩy covering ra khỏi biên floor(): coveringZoomLevel dùng
    // floor(zoom) nên zoom đúng số nguyên (GPS jump 15.0, default 14.0) nằm
    // đúng điểm gián đoạn — jitter float là flip tập tile 14↔15 liên tục,
    // gây fetch/abort churn + đường nhấp nháy/mất. +1e-3 giữ nguyên mọi
    // mức zoom khác, chỉ ổn định vùng biên.
    static constexpr double coveringEpsilon = 1e-3;

    static void applyStyle(style::Style&);
    static double coveringShift(double cameraZoom);
};

} // namespace ohos
} // namespace mbgl
