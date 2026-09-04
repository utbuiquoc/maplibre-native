#include "watch_render_policy.hpp"

#include <mbgl/style/layer.hpp>
#include <mbgl/style/style.hpp>
#include <mbgl/style/types.hpp>

#include <algorithm>
#include <cmath>

namespace mbgl {
namespace ohos {

void WatchRenderPolicy::applyStyle(style::Style& style) {
    if (auto* extrusion = style.getLayer("building-3d")) {
        extrusion->setVisibility(style::VisibilityType::None);
    }
    if (auto* building = style.getLayer("building")) {
        building->setMinZoom(15.5f);
        building->setMaxZoom(24.0f);
    }

    // Mức A (mặt tròn 466px): ẩn các chi tiết gây rối, giữ hình học đường,
    // nhà, park, POI lớn. getLayer() null-safe nếu style đổi ID.
    constexpr const char* hiddenLayers[] = {
        "poi_r20",
        "poi_r7", // quán nhỏ z16+ — rối nhất ở zoom phố
        "road_one_way_arrow",
        "road_one_way_arrow_opposite",
        // Biển đường kiểu US, VN không dùng.
        "highway-shield-non-us",
        "highway-shield-us-interstate",
        "road_shield_us",
        // Biên hành chính — nhiễu nét đứt ở zoom phố.
        "boundary_2",
        "boundary_3",
        "boundary_disputed",
        // Vạch trang trí đường ray.
        "tunnel_major_rail_hatching",
        "tunnel_transit_rail_hatching",
        "road_major_rail_hatching",
        "road_transit_rail_hatching",
        "bridge_major_rail_hatching",
        "bridge_transit_rail_hatching",
        // Viền (casing) đường hẻm, đường phụ, cầu hầm:
        // Cắt giảm 20+ draw call và giảm tải fragment shader cho Mali-G310 trên watch.
        "tunnel_motorway_link_casing",
        "tunnel_service_track_casing",
        "tunnel_link_casing",
        "tunnel_street_casing",
        "tunnel_secondary_tertiary_casing",
        "tunnel_trunk_primary_casing",
        "tunnel_motorway_casing",
        "bridge_motorway_link_casing",
        "bridge_service_track_casing",
        "bridge_link_casing",
        "bridge_street_casing",
        "bridge_path_pedestrian_casing",
        "bridge_secondary_tertiary_casing",
        "road_motorway_link_casing",
        "road_service_track_casing",
        "road_link_casing",
        "road_minor_casing",
        "road_secondary_tertiary_casing",
        "road_trunk_primary_casing",
        "road_motorway_casing",
        "bridge_trunk_primary_casing",
        "bridge_motorway_casing",
        // Tên đường đi bộ nhỏ (tránh placement text khi vuốt chạm)
        "highway-name-path",
        // Đường ray xe lửa (giảm draw calls trên đồng hồ)
        "road_major_rail",
        "tunnel_major_rail",
        "bridge_major_rail",
        "road_transit_rail",
        "tunnel_transit_rail",
        "bridge_transit_rail",
        // Ẩn POI nhỏ (r1 gồm quán nhỏ, barrier, gate...) tránh missing image và giảm tải GPU
        "poi_r1",
        // Ẩn đường đi bộ/vỉa hè/hẻm nhỏ bậc thang (chiếm hàng ngàn đoạn line gây nghẽn Mali-G310)
        "road_path_pedestrian",
        "tunnel_path_pedestrian",
        "bridge_path_pedestrian",
        "road_area_pattern",
        "aeroway_runway",
        "aeroway_taxiway",
    };
    for (const char* id : hiddenLayers) {
        if (auto* layer = style.getLayer(id)) {
            layer->setVisibility(style::VisibilityType::None);
        }
    }

    // Nhãn sông suối nhỏ: chỉ hiện khi zoom xa (giữ water fill + tên sông lớn).
    constexpr const char* farOnlyLabels[] = {
        "water_name_line_label",
        "waterway_line_label",
    };
    for (const char* id : farOnlyLabels) {
        if (auto* layer = style.getLayer(id)) {
            layer->setMinZoom(12.0f);
        }
    }

    // Petal-like road ladder on 466px. Motorway/trunk/primary keep style
    // minzoom (visible when zoomed out). Raising minzoom only skips GPU
    // line work — vector tiles still download the same geometry.
    struct MinZoomLayer {
        const char* id;
        float minZoom;
    };
    constexpr MinZoomLayer roadMinZoom[] = {
        // Secondary / tertiary: hide the yellow mesh below city zoom.
        {"road_secondary_tertiary", 12.0f},
        {"tunnel_secondary_tertiary", 12.0f},
        {"bridge_secondary_tertiary", 12.0f},
        {"highway-name-major", 13.0f},
        // Minor streets: đường nhánh hiện từ z14.5
        {"road_minor", 14.5f},
        {"tunnel_minor", 14.5f},
        {"bridge_street", 14.5f},
        // Service tracks: chỉ hiện khi zoom sâu z16+
        {"road_service_track", 16.0f},
        {"tunnel_service_track", 16.0f},
        {"bridge_service_track", 16.0f},
        // Nhãn tên đường hẻm / đường nhỏ: chỉ hiện khi zoom gần z16.0+
        {"highway-name-minor", 16.0f},
    };
    for (const auto& entry : roadMinZoom) {
        if (auto* layer = style.getLayer(entry.id)) {
            layer->setMinZoom(entry.minZoom);
        }
    }
}

double WatchRenderPolicy::coveringShift(double cameraZoom) {
    if (!std::isfinite(cameraZoom)) {
        return 0.0;
    }
    double target = std::min(cameraZoom, maxCoveringZoom);
    return std::min(0.0, target - cameraZoom);
}

} // namespace ohos
} // namespace mbgl
