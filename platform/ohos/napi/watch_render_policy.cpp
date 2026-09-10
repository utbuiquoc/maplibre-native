#include "watch_render_policy.hpp"

#include <mbgl/style/layer.hpp>
#include <mbgl/style/layers/symbol_layer.hpp>
#include <mbgl/style/style.hpp>
#include <mbgl/style/types.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace mbgl {
namespace ohos {
namespace {

void raiseMinZoom(style::Layer* layer, float minZoom) {
    if (!layer) {
        return;
    }
    if (layer->getMinZoom() < minZoom) {
        layer->setMinZoom(minZoom);
    }
}

// Ladder đường/ngõ: được HẠ minzoom style (VietMap `road_path` = 16 → 14.8).
void setLayerMinZoom(style::Layer* layer, float minZoom) {
    if (!layer) {
        return;
    }
    if (layer->getMinZoom() != minZoom) {
        layer->setMinZoom(minZoom);
    }
}

bool idStartsWith(const std::string& id, const char* prefix) {
    return id.rfind(prefix, 0) == 0;
}

void collapseSymbolFonts(style::Style& style) {
    // Six font stacks (Noto + Roboto Regular/Medium/Bold/Italic) × 3 Vietnamese
    // PBF ranges ≈ 18 HTTP fetches on a cold jump. Collapse to Noto Sans so
    // the glyph atlas and radio only pay for one Regular + one Bold stack.
    for (style::Layer* layer : style.getLayers()) {
        if (!layer || !layer->getTypeInfo() || std::strcmp(layer->getTypeInfo()->type, "symbol") != 0) {
            continue;
        }
        auto* symbol = static_cast<style::SymbolLayer*>(layer);
        const auto& font = symbol->getTextFont();
        if (!font.isConstant()) {
            continue;
        }
        bool bold = false;
        for (const auto& name : font.asConstant()) {
            if (name.find("Bold") != std::string::npos) {
                bold = true;
                break;
            }
        }
        symbol->setTextFont(std::vector<std::string>{bold ? "Noto Sans Bold" : "Noto Sans Regular"});
    }
}

} // namespace

void WatchRenderPolicy::applyStyle(style::Style& style) {
    if (auto* extrusion = style.getLayer("building-3d")) {
        extrusion->setVisibility(style::VisibilityType::None);
    }
    if (auto* building = style.getLayer("building")) {
        if (building->getMinZoom() < 16.0f) {
            building->setMinZoom(16.0f);
        }
        building->setMaxZoom(24.0f);
    }

    // Mức A: Chỉ ẩn vĩnh viễn các layer 3D không tương thích hoặc vạch trang trí vô nghĩa.
    // Toàn bộ các layer casing, POI, ranh giới, nhãn được chuyển sang bậc thang minZoom
    // để hiển thị đầy đủ khi ở mức zoom lớn theo yêu cầu của user.
    constexpr const char* permanentlyHiddenLayers[] = {
        // 3D extrusions: depth buffer / shader không tương thích Mali-G310
        "building-3d",
        // Biển số kiểu US, VN không dùng
        "highway-shield-non-us",
        "highway-shield-us-interstate",
        "road_shield_us",
        // Vạch trang trí đường ray không cần thiết
        "tunnel_major_rail_hatching",
        "tunnel_transit_rail_hatching",
        "road_major_rail_hatching",
        "road_transit_rail_hatching",
        "bridge_major_rail_hatching",
        "bridge_transit_rail_hatching",
        // Mũi tên một chiều OpenFreeMap
        "road_one_way_arrow",
        "road_one_way_arrow_opposite",
        // Biên hành chính OpenFreeMap gây nhiễu nét đứt
        "boundary_2",
        "boundary_3",
        "boundary_disputed",
        // VietMap: layer trùng / branding / nhãn hành chính dày trên 466px
        "poiz18_golf_1",
        "poiz18_vietmap",
        "boundary_2_label_left",
        "boundary_2_label_right",
        "boundary_2_label_left_TQ",
        "boundary_2_label_right_vn",
        "boundary_district_left",
        "boundary_district_right",
        "boundary_province_left",
        "boundary_province_right",
    };
    for (const char* id : permanentlyHiddenLayers) {
        if (auto* layer = style.getLayer(id)) {
            layer->setVisibility(style::VisibilityType::None);
        }
    }

    // Nhãn sông suối nhỏ: chỉ hiện khi zoom gần (giữ water fill + tên sông lớn).
    // SEA Map Dark dùng `waterway_name` / `water_point`; OpenFreeMap dùng `*_line_label`.
    constexpr const char* farOnlyLabels[] = {
        "water_name_line_label",
        "waterway_line_label",
        "waterway_name",
        "water_point",
    };
    for (const char* id : farOnlyLabels) {
        raiseMinZoom(style.getLayer(id), 12.0f);
    }

    // Prefix ladder cho toàn bộ POI VietMap (poiz18_cinema minzoom style = 13,
    // poiz16_education = 13, poiz18_vietmap = 11 — không nằm trong danh sách
    // ID cứng bên dưới). Chỉ nâng, không hạ minzoom style.
    for (style::Layer* layer : style.getLayers()) {
        if (!layer) {
            continue;
        }
        const std::string id = layer->getID();
        if (idStartsWith(id, "poiz18_")) {
            raiseMinZoom(layer, 16.5f);
        } else if (idStartsWith(id, "poiz16_")) {
            raiseMinZoom(layer, 16.0f);
        } else if (idStartsWith(id, "poiz15_")) {
            raiseMinZoom(layer, 15.5f);
        } else if (idStartsWith(id, "poiz14_")) {
            raiseMinZoom(layer, 15.2f);
        } else if (idStartsWith(id, "poiz13_")) {
            raiseMinZoom(layer, 15.0f);
        } else if (idStartsWith(id, "poiz12_")) {
            raiseMinZoom(layer, 14.0f);
        } else if (idStartsWith(id, "poiz11_")) {
            raiseMinZoom(layer, 13.5f);
        } else if (idStartsWith(id, "poiz9_")) {
            raiseMinZoom(layer, 12.0f);
        }
    }

    // BẬC THANG HIỂN THỊ THEO MỨC ZOOM (ZOOM LADDER):
    // Ở zoom xa/trung bình: ẩn để đạt 60 FPS mượt mà.
    // Ở MỨC ZOOM LỚN: hiển thị đầy đủ toàn bộ casing, trạm bus, bãi đỗ, quán ăn, cafe, atm, ranh giới...
    struct MinZoomLayer {
        const char* id;
        float minZoom;
    };
    constexpr MinZoomLayer zoomLadder[] = {
        // --- ZOOM 10.0 - 13.0: Mạng lưới đường chính & nhãn đường ---
        {"road_shield", 10.0f},
        {"road_secondary", 11.0f},
        {"tunnel_secondary", 11.0f},
        {"bridge_secondary", 11.0f},
        {"road_secondary_label", 12.5f},
        {"road_secondary_tertiary", 12.0f},
        {"tunnel_secondary_tertiary", 12.0f},
        {"bridge_secondary_tertiary", 12.0f},
        {"road_tertiary", 12.0f},
        {"tunnel_tertiary", 12.0f},
        {"bridge_tertiary", 12.0f},
        {"road_tertiary_label", 13.0f},
        {"road_primary_label", 12.8f},
        {"road_trunk_label", 12.5f},
        {"road_motorway_label", 12.0f},
        {"road_ferry_label", 12.0f},
        {"highway-name-major", 13.0f},

        // --- ZOOM 14.0: Đường sắt ---
        {"road_major_rail", 14.0f},
        {"tunnel_major_rail", 14.0f},
        {"bridge_major_rail", 14.0f},
        {"road_transit_rail", 14.0f},
        {"tunnel_transit_rail", 14.0f},
        {"bridge_transit_rail", 14.0f},

        // --- ZOOM 14.0: Mạng lưới đường nhánh, đường dân sinh (minor) ---
        {"road_minor", 14.0f},
        {"tunnel_minor", 14.0f},
        {"bridge_minor", 14.0f},
        {"bridge_street", 14.0f},
        {"tunnel_minor_construction", 14.0f},

        // --- ZOOM 14.5: Nhãn tên đường nhánh (minor label) ---
        {"road_minor_label", 14.5f},
        {"highway-name-minor", 14.5f},

        // --- ZOOM 14.8: Casing đường nhánh & Bắt đầu hiển thị toàn bộ ngõ, ngách, hẻm nhỏ, đường đi bộ, đường nội bộ ---
        // (Ẩn hoàn toàn ở zoom < 14.8 để tránh vẽ hàng vạn polyline li ti gây tụt FPS / lag ở tầm nhìn trên cao)
        {"road_minor_casing", 14.8f},
        {"tunnel_minor_casing", 14.8f},
        {"bridge_minor_casing", 14.8f},
        {"tunnel_street_casing", 14.8f},
        {"bridge_street_casing", 14.8f},
        {"road_path", 14.8f},
        {"bridge_path", 14.8f},
        {"tunnel_path", 14.8f},
        {"tunnel_path_construction", 14.8f},
        {"road_path_pedestrian", 14.8f},
        {"tunnel_path_pedestrian", 14.8f},
        {"bridge_path_pedestrian", 14.8f},
        {"road_service_track", 14.8f},
        {"tunnel_service_track", 14.8f},
        {"bridge_service_track", 14.8f},

        // --- ZOOM 15.2: Tên & Casing cho ngõ, ngách nhỏ và đường nội bộ ---
        {"road_path_label", 15.2f},
        {"highway-name-path", 15.2f},
        {"road_path_casing", 15.2f},
        {"tunnel_path_casing", 15.2f},
        {"bridge_path_casing", 15.2f},
        {"bridge_path_pedestrian_casing", 15.2f},
        {"road_service_track_casing", 15.2f},
        {"tunnel_service_track_casing", 15.2f},
        {"bridge_service_track_casing", 15.2f},

        // ===============================================================
        // MỨC ZOOM LỚN - TIER 1 (>= 15.0):
        // Casing đường thứ cấp, nhánh nối cao tốc, ranh giới xã, nhãn tỉnh
        // ===============================================================
        {"boundary_commune", 15.0f},
        {"place_province_en", 15.0f},
        {"place_province_en_extra", 15.0f},
        {"place_capital_en", 15.0f},
        {"road_secondary_casing", 15.0f},
        {"tunnel_secondary_casing", 15.0f},
        {"bridge_secondary_casing", 15.0f},
        {"road_tertiary_casing", 15.0f},
        {"tunnel_tertiary_casing", 15.0f},
        {"bridge_tertiary_casing", 15.0f},
        {"road_secondary_link_casing", 15.0f},
        {"road_primary_link_casing", 15.0f},
        {"road_trunk_link_casing", 15.0f},
        {"road_motorway_link_casing", 15.0f},
        {"bridge_secondary_link_casing", 15.0f},
        {"bridge_primary_link_casing", 15.0f},
        {"bridge_trunk_link_casing", 15.0f},
        {"bridge_motorway_link_casing", 15.0f},
        {"road_rail_casing", 15.0f},
        {"road_rail_casing_MT", 15.0f},

        // ===============================================================
        // MỨC ZOOM LỚN - TIER 2 (>= 15.5):
        // Casing đường nối cao tốc, bãi đỗ xe, ngân hàng, nhãn ranh giới huyện/tỉnh
        // ===============================================================
        {"tunnel_secondary_tertiary_casing", 15.5f},
        {"bridge_secondary_tertiary_casing", 15.5f},
        {"tunnel_motorway_link_casing", 15.5f},
        {"tunnel_link_casing", 15.5f},
        {"tunnel_trunk_primary_casing", 15.5f},
        {"tunnel_motorway_casing", 15.5f},
        {"bridge_link_casing", 15.5f},
        {"road_link_casing", 15.5f},
        {"poiz13_bank", 15.5f},
        {"poiz14_parking", 15.5f},
        {"poiz14_reststop", 15.5f},
        {"poiz15_resort", 15.5f},
        {"poiz15_chargingstation", 15.5f},
        {"boundary_district_right", 15.5f},
        {"boundary_district_left", 15.5f},
        {"boundary_province_right", 15.5f},
        {"boundary_province_left", 15.5f},
        {"boundary_2_label_left_TQ", 15.5f},
        {"boundary_2_label_right_vn", 15.5f},
        {"boundary_2_label_left", 15.5f},
        {"boundary_2_label_right", 15.5f},

        // ===============================================================
        // MỨC ZOOM LỚN - TIER 3 (>= 16.0):
        // Trạm xe bus, siêu thị, khách sạn, công ty, tiện ích công cộng
        // ===============================================================
        {"road_area_pattern", 16.0f},
        {"aeroway_runway", 16.0f},
        {"aeroway_taxiway", 16.0f},
        {"poiz14_bus", 16.0f},
        {"poiz16_supermarket", 16.0f},
        {"poiz16_hotel", 16.0f},
        {"poiz16_arena", 16.0f},
        {"poiz16_industrial", 16.0f},
        {"poiz16_cemetery", 16.0f},
        {"poiz16_zoo", 16.0f},
        {"poiz16_amusement", 16.0f},
        {"poiz16_committee", 16.0f},
        {"poiz16_attraction", 16.0f},

        // ===============================================================
        // MỨC ZOOM LỚN - TIER 4 (>= 16.5):
        // Toàn bộ quán xá, cafe, nhà hàng, ATM, tiệm ăn, cửa hàng, giải trí
        // ===============================================================
        {"poiz18_other", 16.5f},
        {"poiz18_store", 16.5f},
        {"poiz18_restaurant", 16.5f},
        {"poiz18_fastfood", 16.5f},
        {"poiz18_vietnamfood", 16.5f},
        {"poiz18_cafe", 16.5f},
        {"poiz18_beer", 16.5f},
        {"poiz18_bbq", 16.5f},
        {"poiz18_barclub", 16.5f},
        {"poiz18_gym", 16.5f},
        {"poiz18_tennis", 16.5f},
        {"poiz18_swimming", 16.5f},
        {"poiz18_studio", 16.5f},
        {"poiz18_golf", 16.5f},
        {"poiz18_golf_1", 16.5f},
        {"poiz18_football", 16.5f},
        {"poiz18_casino", 16.5f},
        {"poiz18_company", 16.5f},
        {"poiz18_clinic", 16.5f},
        {"poiz18_book", 16.5f},
        {"poiz18_atm", 16.5f},
        {"poiz18_theatre", 16.5f},
        {"poiz18_building", 16.5f},
        {"poi_r1", 16.5f},
        {"poi_r7", 16.5f},
        {"poi_r20", 16.5f},
    };
    for (const auto& entry : zoomLadder) {
        setLayerMinZoom(style.getLayer(entry.id), entry.minZoom);
    }

    collapseSymbolFonts(style);
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
