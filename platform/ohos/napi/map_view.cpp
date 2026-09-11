#include "map_view.hpp"
#include "watch_render_policy.hpp"

#if MLN_RENDER_BACKEND_VULKAN
#include "vulkan_window_backend.hpp"
#else
#include "egl_window_backend.hpp"
#endif

#include <mbgl/math/angles.hpp>
#include <mbgl/map/map_options.hpp>
#include <mbgl/style/expression/image.hpp>
#include <mbgl/style/image.hpp>
#include <mbgl/style/layers/circle_layer.hpp>
#include <mbgl/style/layers/line_layer.hpp>
#include <mbgl/style/layers/symbol_layer.hpp>
#include <mbgl/style/source.hpp>
#include <mbgl/style/sources/geojson_source.hpp>
#include <mbgl/style/style.hpp>
#include <mbgl/style/types.hpp>
#include <mbgl/util/async_task.hpp>
#include <mbgl/util/color.hpp>
#include <mbgl/util/geojson.hpp>
#include <mbgl/util/client_options.hpp>
#include <mbgl/util/image.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/run_loop.hpp>
#include <mbgl/util/string.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <numbers>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace mbgl {
namespace ohos {
namespace {

std::uint32_t logicalDimensionForFramebufferDimension(const std::uint32_t dimension, const float pixelRatio) {
    return std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::ceil(dimension / pixelRatio)));
}

Size logicalSizeForFramebufferSize(const Size size, const float pixelRatio) {
    return {logicalDimensionForFramebufferDimension(size.width, pixelRatio),
            logicalDimensionForFramebufferDimension(size.height, pixelRatio)};
}

double logicalCoordinateForFramebufferCoordinate(const double coordinate, const float pixelRatio) {
    return coordinate / pixelRatio;
}

ScreenCoordinate logicalCoordinateForFramebufferCoordinate(const double x, const double y, const float pixelRatio) {
    return {logicalCoordinateForFramebufferCoordinate(x, pixelRatio),
            logicalCoordinateForFramebufferCoordinate(y, pixelRatio)};
}

constexpr auto ZoomSessionIdle = std::chrono::milliseconds(WatchRenderPolicy::idleMilliseconds);

constexpr const char* kUserLocationSource = "user_location_source";
constexpr const char* kUserLocationPuck = "user_location_puck";
constexpr const char* kUserLocationHalo = "user_location_halo";
constexpr const char* kUserLocationBeam = "user_location_beam";
constexpr const char* kUserLocationBeamImage = "user_location_beam_image";
constexpr float kBeamIconSize = 0.56f;
// Tuyến dẫn đường: 2 source (đã đi / còn lại) + 3 layer (casing, line, travelled).
// Dùng 2 source thay vì filter để tránh phụ thuộc API expression của mbgl.
constexpr const char* kRouteRemainingSource = "nav_route_remaining_source";
constexpr const char* kRouteTravelledSource = "nav_route_travelled_source";
constexpr const char* kRouteCasingLayer = "nav_route_casing";
constexpr const char* kRouteLineLayer = "nav_route_line";
constexpr const char* kRouteTravelledLayer = "nav_route_travelled";

double normalizeHeadingDegrees(double heading) {
    double wrapped = std::fmod(heading, 360.0);
    if (wrapped < 0.0) {
        wrapped += 360.0;
    }
    return wrapped;
}

PremultipliedImage makeHeadingBeamImage() {
    constexpr std::uint32_t kSize = 256;
    constexpr float kPi = std::numbers::pi_v<float>;
    constexpr float kHalfAngle = 26.0f * kPi / 180.0f;
    constexpr float kFeather = 7.0f * kPi / 180.0f;
    PremultipliedImage image({kSize, kSize});
    std::uint8_t* dst = image.data.get();
    const float cx = (kSize - 1) * 0.5f;
    const float cy = (kSize - 1) * 0.5f;
    const float maxR = kSize * 0.5f;
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const float dx = static_cast<float>(x) - cx;
            const float dy = static_cast<float>(y) - cy;
            const float dist = std::sqrt(dx * dx + dy * dy);
            const float r = dist / maxR;
            const float absAng = std::fabs(std::atan2(dx, -dy));
            float angT = (absAng - kHalfAngle) / kFeather;
            angT = std::clamp(angT, 0.0f, 1.0f);
            angT = angT * angT * (3.0f - 2.0f * angT);
            const float angMask = 1.0f - angT;
            const float rad = std::clamp((r - 0.08f) / 0.92f, 0.0f, 1.0f);
            const float radMask = (1.0f - rad) * (1.0f - rad);
            const float a = angMask * radMask * 0.48f;
            const std::size_t i = (static_cast<std::size_t>(y) * kSize + x) * 4;
            dst[i + 0] = static_cast<std::uint8_t>(std::lround(0.10f * a * 255.0f));
            dst[i + 1] = static_cast<std::uint8_t>(std::lround(0.45f * a * 255.0f));
            dst[i + 2] = static_cast<std::uint8_t>(std::lround(0.91f * a * 255.0f));
            dst[i + 3] = static_cast<std::uint8_t>(std::lround(a * 255.0f));
        }
    }
    return image;
}

} // namespace

MapView::MapView(const float pixelRatio_)
    : pixelRatio(pixelRatio_) {
    if (!std::isfinite(pixelRatio) || pixelRatio <= 0.0f) {
        throw std::invalid_argument("Pixel ratio must be a finite positive value");
    }
    asyncInvalidate = std::make_unique<util::AsyncTask>([this] { onInvalidate(); });
}

MapView::~MapView() {
    repaintCallback = nullptr;
    if (frontend) {
        frontend->setInvalidateCallback(nullptr);
    }
    asyncInvalidate.reset();
    clearSurface();
}

void MapView::setSurface(OHNativeWindow* newWindow, Size size) {
    if (newWindow == nullptr || size.isEmpty()) {
        clearSurface();
        return;
    }

    if (backend && window == newWindow) {
        setSize(size);
        return;
    }

    createMap(newWindow, size);
}

void MapView::clearSurface() {
    zoomSession = {};
    touchGestureActive = false;
    if (frontend) {
        frontend->setInvalidateCallback(nullptr);
    }
    map.reset();
    frontend.reset();
    backend.reset();
    window = nullptr;
    surfaceSize = {};
    resetRuntimeState();
}

void MapView::setRepaintCallback(std::function<void()> callback) {
    repaintCallback = std::move(callback);
}

void MapView::onInvalidate() {
    if (repaintCallback) {
        repaintCallback();
    }
}

void MapView::pumpEvents() {
    const auto start = std::chrono::steady_clock::now();
    tickZoomSession();
    runLoopOnce();
    // Apply coalesced touch pan once per tick: a 120Hz digitizer delivers
    // ~2 Moves per 60Hz frame; one moveBy per tick halves onUpdate churn.
    flushPendingPan();
    // Fling / flyTo keep isPanning|isScaling after the finger is up. Re-assert
    // MapLibre's gesture flag so symbol placement stays deferred until idle.
    syncGestureFlag();
    lastPumpMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

bool MapView::renderFrame() {
    pumpEvents();

    bool rendered = false;
    double glMs = 0.0;
    auto renderIfNeeded = [&] {
        if (frontend && frontend->hasPendingRender()) {
            const auto glStart = std::chrono::steady_clock::now();
            if (frontend->renderFrame()) {
                ++renderedFrameCount;
                rendered = true;
            }
            glMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - glStart).count();
        }
    };

    renderIfNeeded();
    runLoopOnce();
    // Second GPU pass is for draining HTTP during load. Skipped while
    // interactive so pan/fling never pays two GL frames on the UI thread.
    if (!isInteractive()) {
        renderIfNeeded();
    }
    lastGlMs = glMs;
    // Attribution correction: on glFinish-sampled frames the wall time above
    // contains the full GPU drain (pipelined work included). Subtract it so the
    // ring measures comparable CPU-side cost on every frame. Raw lastGlMs and
    // hitch logic are untouched.
    lastGpuWaitMs = 0.0;
    lastGlCpuMs = glMs;
    lastGpuWaitSampled = backend && backend->consumeGpuWaitSampled();
    if (lastGpuWaitSampled) {
        lastGpuWaitMs = backend->getLastGpuWaitMs();
        lastGlCpuMs = std::max(0.0, glMs - lastGpuWaitMs);
    }

    return rendered;
}

void MapView::runLoopOnce() {
    util::RunLoop::Get()->runOnce();
}

void MapView::reduceMemoryUse() {
    if (frontend) {
        frontend->reduceMemoryUse();
    }
}

void MapView::setStyleURL(const std::string& url) {
    if (!map) {
        return;
    }

    resetRuntimeState();
    map->getStyle().loadURL(url);
}

void MapView::setStyleJSON(const std::string& json) {
    if (map) {
        resetRuntimeState();
        map->getStyle().loadJSON(json);
    }
}

void MapView::jumpTo(CameraOptions cameraOptions) {
    finishZoomSession();
    desiredCameraBounds.reset();
    desiredFreeCamera.reset();
    desiredCamera = mergeCameraOptions(cameraOptions);
    if (map) {
        map->jumpTo(std::move(cameraOptions));
        applyCoveringZoomCap();
        // Log mỗi jumpTo: chẩn đoán covering sau di chuyển tức thời (GPS).
        mbgl::Log::Info(mbgl::Event::General,
                        "[DIAG] jumpTo done zoom=" + std::to_string(currentZoom()) +
                            " active=" + (zoomSession.active ? "true" : "false"));
    }
}

void MapView::easeTo(CameraOptions cameraOptions, AnimationOptions animationOptions) {
    finishZoomSession();
    desiredCameraBounds.reset();
    desiredFreeCamera.reset();
    desiredCamera = mergeCameraOptions(cameraOptions);
    if (map) {
        map->easeTo(std::move(cameraOptions), std::move(animationOptions));
        applyCoveringZoomCap();
    }
}

void MapView::flyTo(CameraOptions cameraOptions, AnimationOptions animationOptions) {
    finishZoomSession();
    desiredCameraBounds.reset();
    desiredFreeCamera.reset();
    desiredCamera = mergeCameraOptions(cameraOptions);
    if (map) {
        map->flyTo(std::move(cameraOptions), std::move(animationOptions));
        applyCoveringZoomCap();
    }
}

void MapView::fitBounds(CameraBoundsOptions options) {
    finishZoomSession();
    desiredFreeCamera.reset();
    desiredCameraBounds = options;
    if (map) {
        desiredCamera = cameraForBounds(options);
        map->jumpTo(*desiredCamera);
        applyCoveringZoomCap();
    }
}

void MapView::setFreeCameraOptions(FreeCameraOptions cameraOptions) {
    if (!cameraOptions.position && !cameraOptions.orientation) {
        return;
    }

    finishZoomSession();
    desiredCamera.reset();
    desiredCameraBounds.reset();
    if (map) {
        map->setFreeCameraOptions(cameraOptions);
        desiredFreeCamera = map->getFreeCameraOptions();
    } else {
        desiredFreeCamera = mergeFreeCameraOptions(cameraOptions);
    }
}

void MapView::setGestureInProgress(bool inProgress) {
    touchGestureActive = inProgress;
    syncGestureFlag();
}

bool MapView::isGestureInProgress() const {
    return map && map->isGestureInProgress();
}

bool MapView::isPanning() const {
    return map && map->isPanning();
}

bool MapView::isScaling() const {
    return map && map->isScaling();
}

bool MapView::isRotating() const {
    return map && map->isRotating();
}

bool MapView::isInteractive() const {
    if (touchGestureActive || zoomSession.active) {
        return true;
    }
    return isPanning() || isScaling() || isRotating() || isGestureInProgress();
}

void MapView::addZoomDelta(double deltaZoom) {
    if (!map || !std::isfinite(deltaZoom) || std::abs(deltaZoom) < WatchRenderPolicy::minZoomDelta) {
        return;
    }
    if (zoomSession.owner == ZoomSessionOwner::Pinch) {
        return;
    }

    startZoomSession(ZoomSessionOwner::Wheel);
    zoomSession.pendingDelta += deltaZoom;
    zoomSession.lastEvent = std::chrono::steady_clock::now();
}

void MapView::beginInteractionZoom() {
    if (!map) {
        return;
    }
    if (zoomSession.owner == ZoomSessionOwner::Wheel) {
        applyPendingZoomDelta();
        zoomSession.owner = ZoomSessionOwner::Pinch;
        zoomSession.pendingDelta = 0.0;
        zoomSession.lastEvent = std::chrono::steady_clock::now();
        return;
    }
    startZoomSession(ZoomSessionOwner::Pinch);
}

void MapView::prepareInteractionZoom(double nextZoom) {
    if (!zoomSession.active || !std::isfinite(nextZoom)) {
        return;
    }
    applyTileLodShift(nextZoom);
}

void MapView::endInteractionZoom() {
    if (zoomSession.owner == ZoomSessionOwner::Pinch) {
        finishZoomSession();
    }
}

void MapView::cancelTransitions() {
    if (map) {
        map->cancelTransitions();
    }
    hasPendingPan = false;
    pendingPanX = 0.0;
    pendingPanY = 0.0;
}

void MapView::moveBy(double x, double y, AnimationOptions animationOptions) {
    if (!map) {
        return;
    }

    map->moveBy(logicalCoordinateForFramebufferCoordinate(x, y, pixelRatio), std::move(animationOptions));
    desiredCameraBounds.reset();
    desiredFreeCamera.reset();
    desiredCamera = map->getCameraOptions();
}

void MapView::accumulatePan(double x, double y) {
    if (!map || !std::isfinite(x) || !std::isfinite(y) || (x == 0.0 && y == 0.0)) {
        return;
    }
    pendingPanX += x;
    pendingPanY += y;
    hasPendingPan = true;
}

void MapView::flushPendingPan() {
    if (!hasPendingPan) {
        return;
    }
    hasPendingPan = false;
    const double dx = pendingPanX;
    const double dy = pendingPanY;
    pendingPanX = 0.0;
    pendingPanY = 0.0;
    if (!map || (dx == 0.0 && dy == 0.0)) {
        return;
    }
    map->moveBy(logicalCoordinateForFramebufferCoordinate(dx, dy, pixelRatio), AnimationOptions{});
    desiredCameraBounds.reset();
    desiredFreeCamera.reset();
    desiredCamera = map->getCameraOptions();
}

void MapView::pitchBy(double deltaPitch) {
    if (!map || !std::isfinite(deltaPitch)) {
        return;
    }

    const auto currentCamera = map->getCameraOptions();
    const auto bounds = map->getBounds();
    const double minPitch = bounds.minPitch.value_or(0.0);
    const double maxPitch = bounds.maxPitch.value_or(60.0);
    const double lowerPitch = std::min(minPitch, maxPitch);
    const double upperPitch = std::max(minPitch, maxPitch);
    const double currentPitch = currentCamera.pitch.value_or(0.0);
    const double nextPitch = std::clamp(currentPitch + deltaPitch, lowerPitch, upperPitch);
    map->easeTo(CameraOptions().withPitch(nextPitch), AnimationOptions{});
    desiredCameraBounds.reset();
    desiredFreeCamera.reset();
    desiredCamera = map->getCameraOptions();
}

void MapView::scaleBy(double scale, double anchorX, double anchorY) {
    if (!map || !std::isfinite(scale) || scale <= 0.0) {
        return;
    }

    map->scaleBy(scale, logicalCoordinateForFramebufferCoordinate(anchorX, anchorY, pixelRatio));
    applyCoveringZoomCap();
    desiredCameraBounds.reset();
    desiredFreeCamera.reset();
    desiredCamera = map->getCameraOptions();
}

void MapView::flyBy(double scale, double anchorX, double anchorY, AnimationOptions animationOptions) {
    if (!map || !std::isfinite(scale) || scale <= 0.0 || !std::isfinite(anchorX) || !std::isfinite(anchorY)) {
        return;
    }

    finishZoomSession();
    const auto currentCamera = map->getCameraOptions();
    const double currentZoom = currentCamera.zoom.value_or(0.0);
    // Clamp cùng policy với applyPendingZoomDelta: mọi đường zoom chung một trần/sàn.
    const double nextZoom = std::clamp(currentZoom + std::log2(scale),
                                       WatchRenderPolicy::minZoom, WatchRenderPolicy::maxZoom);
    map->flyTo(CameraOptions().withZoom(nextZoom).withAnchor(
                   logicalCoordinateForFramebufferCoordinate(anchorX, anchorY, pixelRatio)),
               std::move(animationOptions));
    applyCoveringZoomCap();
    desiredCameraBounds.reset();
    desiredFreeCamera.reset();
    desiredCamera = map->getCameraOptions();
}

void MapView::rotateBy(double previousAngle, double currentAngle, double anchorX, double anchorY) {
    if (!map || !std::isfinite(previousAngle) || !std::isfinite(currentAngle) || !std::isfinite(anchorX) ||
        !std::isfinite(anchorY)) {
        return;
    }

    const auto currentCamera = map->getCameraOptions();
    const double currentBearing = currentCamera.bearing.value_or(0.0);
    const double bearingDelta = util::rad2deg(currentAngle - previousAngle);
    map->easeTo(CameraOptions()
                    .withBearing(currentBearing - bearingDelta)
                    .withAnchor(logicalCoordinateForFramebufferCoordinate(anchorX, anchorY, pixelRatio)),
                AnimationOptions{});
    desiredCameraBounds.reset();
    desiredFreeCamera.reset();
    desiredCamera = map->getCameraOptions();
}

void MapView::tickZoomSession() {
    if (!zoomSession.active || zoomSession.owner != ZoomSessionOwner::Wheel) {
        return;
    }

    applyPendingZoomDelta();

    if (std::abs(zoomSession.pendingDelta) >= WatchRenderPolicy::minZoomDelta) {
        return;
    }

    if (std::chrono::steady_clock::now() - zoomSession.lastEvent >= ZoomSessionIdle) {
        finishZoomSession();
    }
}

void MapView::startZoomSession(ZoomSessionOwner owner) {
    if (!map) {
        return;
    }
    if (zoomSession.active) {
        zoomSession.owner = owner;
        return;
    }

    zoomSession.active = true;
    zoomSession.owner = owner;
    zoomSession.pendingDelta = 0.0;
    zoomSession.startZoom = currentZoom();
    zoomSession.lastEvent = std::chrono::steady_clock::now();
    syncGestureFlag();
}

void MapView::applyPendingZoomDelta() {
    if (!map || std::abs(zoomSession.pendingDelta) < WatchRenderPolicy::minZoomDelta) {
        return;
    }

    const double zoom = currentZoom();
    const double step = std::clamp(zoomSession.pendingDelta, -WatchRenderPolicy::maxZoomStepPerFrame,
                                   WatchRenderPolicy::maxZoomStepPerFrame);
    const double nextZoom = std::clamp(zoom + step, WatchRenderPolicy::minZoom, WatchRenderPolicy::maxZoom);
    const double applied = nextZoom - zoom;
    if (std::abs(applied) < WatchRenderPolicy::minZoomDelta) {
        zoomSession.pendingDelta = 0.0;
        return;
    }

    applyTileLodShift(nextZoom);
    scaleBy(std::exp2(applied), static_cast<double>(surfaceSize.width) * 0.5,
            static_cast<double>(surfaceSize.height) * 0.5);
    zoomSession.pendingDelta -= applied;
}

void MapView::applyTileLodShift(double nextZoom) {
    if (!map) {
        return;
    }
    // Giữ coveringShift để giới hạn trần zoom phủ (maxCoveringZoom = 15.0;
    // giới hạn chi tiết thật nằm ở source maxzoom=14 của OpenFreeMap — xem
    // comment watch_render_policy.hpp), không đóng băng mức tile cũ khi
    // zoom in để tránh triệt tiêu layer đường nhỏ.
    double shift = WatchRenderPolicy::coveringShift(nextZoom);
    map->setTileLodZoomShift(shift + WatchRenderPolicy::coveringEpsilon);
}

void MapView::applyCoveringZoomCap() {
    applyTileLodShift(currentZoom());
}

void MapView::finishZoomSession() {
    if (!zoomSession.active && zoomSession.owner == ZoomSessionOwner::None) {
        return;
    }

    const double startZoom = zoomSession.startZoom;
    const double endZoom = currentZoom();
    zoomSession = {};
    applyCoveringZoomCap();
    syncGestureFlag();
    // Zoom out ≥ 2 mức: đổ cache RAM của tile z cao (đô thị) trước khi
    // parse tile z thấp (cả nước). Không gọi khi zoom in — tile gần còn dùng.
    if (startZoom - endZoom >= 2.0) {
        reduceMemoryUse();
    }
}

void MapView::syncGestureFlag() {
    if (!map) {
        return;
    }
    const bool want = touchGestureActive || zoomSession.active || map->isPanning() || map->isScaling() ||
                      map->isRotating();
    // Idempotency guard: Map::setGestureInProgress() calls onUpdate()
    // unconditionally, which marks needsRender. Calling it on every pump
    // self-dirties and defeats the hasPendingRender() early-out, forcing a
    // full GL frame every tick even with the finger held still.
    if (map->isGestureInProgress() != want) {
        map->setGestureInProgress(want);
    }
    applyTileLodShift(currentZoom());
}

double MapView::currentZoom() const {
    if (map) {
        return map->getCameraOptions().zoom.value_or(0.0);
    }
    return desiredCamera && desiredCamera->zoom ? *desiredCamera->zoom : 0.0;
}

CameraOptions MapView::getCameraOptions() const {
    return map ? map->getCameraOptions() : desiredCamera.value_or(CameraOptions());
}

std::vector<std::string> MapView::getStyleAttributions() const {
    if (!map) {
        return {};
    }

    std::set<std::string> seen;
    std::vector<std::string> attributions;
    for (const auto* source : map->getStyle().getSources()) {
        if (source == nullptr) {
            continue;
        }

        auto attribution = source->getAttribution();
        if (!attribution) {
            continue;
        }

        const auto first = attribution->find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            continue;
        }

        const auto last = attribution->find_last_not_of(" \t\r\n");
        auto trimmed = attribution->substr(first, last - first + 1);
        if (seen.insert(trimmed).second) {
            attributions.push_back(std::move(trimmed));
        }
    }
    return attributions;
}

FreeCameraOptions MapView::getFreeCameraOptions() const {
    return map ? map->getFreeCameraOptions() : desiredFreeCamera.value_or(FreeCameraOptions());
}

CameraOptions MapView::cameraForBounds(const CameraBoundsOptions& options) const {
    if (!map) {
        throw std::runtime_error("Camera for bounds requires an active map surface");
    }
    return map->cameraForLatLngBounds(options.bounds, options.padding, options.bearing, options.pitch);
}

void MapView::setDebugOptions(MapDebugOptions debugOptions_) {
    debugOptions = debugOptions_;
    if (map) {
        map->setDebug(debugOptions);
    }
}

void MapView::setBounds(BoundOptions boundOptions) {
    desiredBounds = mergeBoundOptions(boundOptions);
    if (map) {
        map->setBounds(std::move(boundOptions));
    }
}

BoundOptions MapView::getBounds() const {
    return map ? map->getBounds() : desiredBounds.value_or(BoundOptions());
}

bool MapView::hasPendingRender() const {
    return frontend && frontend->hasPendingRender();
}

bool MapView::isFullyLoaded() const {
    return map && map->isFullyLoaded();
}

std::int32_t MapView::getGlesContextClientVersion() const {
    return backend ? backend->getGlesContextClientVersion() : 0;
}

const std::string& MapView::getRendererDiagnostic() const {
    static const std::string empty;
    return backend ? backend->getRendererDiagnostic() : empty;
}

void MapView::setClientOptions(std::string name, std::string version) {
    if (clientName == name && clientVersion == version) {
        return;
    }

    clientName = std::move(name);
    clientVersion = std::move(version);
    if (window != nullptr && !surfaceSize.isEmpty()) {
        createMap(window, surfaceSize);
    }
}

void MapView::setResourceOptions(ResourceOptions options) {
    resourceOptions = std::move(options);
    if (window != nullptr && !surfaceSize.isEmpty()) {
        createMap(window, surfaceSize);
    }
}

void MapView::setTileCacheEnabled(bool enabled) {
    tileCacheEnabled = enabled;
    if (frontend) {
        frontend->setTileCacheEnabled(tileCacheEnabled);
    }
}

void MapView::setPixelRatio(float pixelRatio_) {
    if (!std::isfinite(pixelRatio_) || pixelRatio_ <= 0.0f) {
        throw std::invalid_argument("Pixel ratio must be a finite positive value");
    }

    if (pixelRatio == pixelRatio_) {
        return;
    }

    pixelRatio = pixelRatio_;
    if (window != nullptr && !surfaceSize.isEmpty()) {
        createMap(window, surfaceSize);
    }
}

void MapView::setSize(Size size) {
    surfaceSize = size;
    if (backend) {
        backend->setSize(size);
    }
    if (map) {
        map->setSize(logicalSizeForFramebufferSize(size, pixelRatio));
    }
}

void MapView::createMap(OHNativeWindow* newWindow, Size size) {
    clearSurface();

#if MLN_RENDER_BACKEND_VULKAN
    backend = std::make_unique<VulkanWindowBackend>(newWindow, size);
#else
    backend = std::make_unique<EGLWindowBackend>(newWindow, size);
#endif
    frontend = std::make_unique<RendererFrontend>(backend->getRendererBackend(), pixelRatio);
    frontend->setTileCacheEnabled(tileCacheEnabled);
    if (asyncInvalidate) {
        frontend->setInvalidateCallback([this] { asyncInvalidate->send(); });
    }

    MapOptions mapOptions;
    mapOptions.withSize(logicalSizeForFramebufferSize(size, pixelRatio))
        .withPixelRatio(pixelRatio)
        .withMapMode(MapMode::Continuous);

    ClientOptions clientOptions;
    clientOptions.withName(clientName).withVersion(clientVersion);

    map = std::make_unique<Map>(*frontend, *this, mapOptions, resourceOptions, clientOptions);
    window = newWindow;
    surfaceSize = size;
    map->setDebug(debugOptions);
    // Default prefetch is 4 extra zoom levels. Trên smartwatch với CPU/radio giới hạn,
    // prefetch tải thừa tile cha (zoom Z-1) làm nghẽn mạng, tốn CPU parse và gây spike
    // upload GPU kép (25-30ms). Tắt hoàn toàn prefetch (delta = 0) theo quy chuẩn AGENTS.md.
    map->setPrefetchZoomDelta(0);
    map->setTileLodMinRadius(WatchRenderPolicy::tileLodMinRadius);

    {
        BoundOptions bounds = desiredBounds.value_or(BoundOptions());
        if (!bounds.maxZoom) {
            bounds.withMaxZoom(WatchRenderPolicy::maxZoom);
        }
        const double minZ = bounds.minZoom ? std::max(*bounds.minZoom, WatchRenderPolicy::minZoom)
                                           : WatchRenderPolicy::minZoom;
        bounds.withMinZoom(minZ);
        desiredBounds = bounds;
    }

    applyDesiredBounds();
    applyDesiredCamera();
    applyCoveringZoomCap();
}

CameraOptions MapView::mergeCameraOptions(const CameraOptions& cameraOptions) const {
    CameraOptions merged = map ? map->getCameraOptions() : desiredCamera.value_or(CameraOptions());
    if (cameraOptions.center) {
        merged.center = cameraOptions.center;
    }
    if (cameraOptions.centerAltitude) {
        merged.centerAltitude = cameraOptions.centerAltitude;
    }
    if (cameraOptions.padding) {
        merged.padding = cameraOptions.padding;
    }
    if (cameraOptions.anchor) {
        merged.anchor = cameraOptions.anchor;
    }
    if (cameraOptions.zoom) {
        merged.zoom = cameraOptions.zoom;
    }
    if (cameraOptions.bearing) {
        merged.bearing = cameraOptions.bearing;
    }
    if (cameraOptions.pitch) {
        merged.pitch = cameraOptions.pitch;
    }
    if (cameraOptions.roll) {
        merged.roll = cameraOptions.roll;
    }
    if (cameraOptions.fov) {
        merged.fov = cameraOptions.fov;
    }
    return merged;
}

FreeCameraOptions MapView::mergeFreeCameraOptions(const FreeCameraOptions& cameraOptions) const {
    FreeCameraOptions merged = map ? map->getFreeCameraOptions() : desiredFreeCamera.value_or(FreeCameraOptions());
    if (cameraOptions.position) {
        merged.position = cameraOptions.position;
    }
    if (cameraOptions.orientation) {
        merged.orientation = cameraOptions.orientation;
    }
    return merged;
}

BoundOptions MapView::mergeBoundOptions(const BoundOptions& boundOptions) const {
    BoundOptions merged = getBounds();
    if (boundOptions.bounds) {
        merged.bounds = boundOptions.bounds;
    }
    if (boundOptions.minZoom) {
        merged.minZoom = boundOptions.minZoom;
    }
    if (boundOptions.maxZoom) {
        merged.maxZoom = boundOptions.maxZoom;
    }
    if (boundOptions.minPitch) {
        merged.minPitch = boundOptions.minPitch;
    }
    if (boundOptions.maxPitch) {
        merged.maxPitch = boundOptions.maxPitch;
    }
    return merged;
}

void MapView::applyDesiredBounds() {
    if (map && desiredBounds) {
        map->setBounds(*desiredBounds);
    }
}

void MapView::applyDesiredCamera() {
    if (!map) {
        return;
    }
    if (desiredFreeCamera) {
        map->setFreeCameraOptions(*desiredFreeCamera);
        return;
    }
    if (desiredCameraBounds) {
        desiredCamera = cameraForBounds(*desiredCameraBounds);
    }
    if (desiredCamera) {
        map->jumpTo(*desiredCamera);
    }
}

void MapView::onDidFailLoadingMap(MapLoadError, const std::string& message) {
    mapLoaded = false;
    styleLoaded = false;
    lastMapLoadError = message;
    Log::Error(Event::Style, lastMapLoadError);
}

void MapView::onDidFinishLoadingMap() {
    mapLoaded = true;
}

void MapView::onDidFinishLoadingStyle() {
    styleLoaded = true;
    userLocationBeamImageReady = false;
    if (!map) {
        return;
    }
    WatchRenderPolicy::applyStyle(map->getStyle());
    if (currentUserLocation.has_value()) {
        updateUserLocationPuck();
    }
    // Style reload xoá sạch source/layer ⇒ phải vẽ lại tuyến đang dẫn đường (gate A6:
    // reloadStyle giữa phiên vẫn phải thấy tuyến).
    if (!routeCoords.empty()) {
        try {
            ensureRouteLayers(map->getStyle());
            applyRouteGeoJSON(map->getStyle());
            if (repaintCallback) {
                repaintCallback();
            }
        } catch (const std::exception& exception) {
            Log::Warning(Event::General, std::string("route re-apply after style failed: ") + exception.what());
        } catch (...) {
            Log::Warning(Event::General, "route re-apply after style failed");
        }
    }
    try {
        const auto defaultCamera = map->getStyle().getDefaultCamera();
        if (!desiredCamera && !desiredCameraBounds && !desiredFreeCamera && defaultCamera.center &&
            defaultCamera.zoom) {
            desiredCameraBounds.reset();
            desiredCamera = defaultCamera;
            map->jumpTo(defaultCamera);
            applyCoveringZoomCap();
            return;
        }
    } catch (const std::exception& exception) {
        Log::Error(Event::Style, std::string("Style camera jump failed: ") + exception.what());
    } catch (...) {
        Log::Error(Event::Style, "Style camera jump failed");
    }
    applyDesiredCamera();
    applyCoveringZoomCap();
}

void MapView::onStyleImageMissing(const std::string& id) {
    lastStyleImageMissing = id;
}

void MapView::onGlyphsError(const FontStack&, const GlyphRange&, std::exception_ptr error) {
    lastGlyphsError = util::toString(error);
}

void MapView::onSpriteError(const std::optional<style::Sprite>&, std::exception_ptr error) {
    lastSpritesError = util::toString(error);
}

void MapView::onRenderError(std::exception_ptr error) {
    lastRenderError = util::toString(error);
    Log::Error(Event::Render, lastRenderError);
}

void MapView::resetRuntimeState() {
    styleLoaded = false;
    mapLoaded = false;
    renderedFrameCount = 0;
    lastPumpMs = 0.0;
    lastGlMs = 0.0;
    lastGlCpuMs = 0.0;
    lastGpuWaitMs = 0.0;
    lastGpuWaitSampled = false;
    pendingPanX = 0.0;
    pendingPanY = 0.0;
    hasPendingPan = false;
    lastMapLoadError.clear();
    lastRenderError.clear();
    lastStyleImageMissing.clear();
    lastGlyphsError.clear();
    lastSpritesError.clear();
    userLocationBeamImageReady = false;
}

void MapView::setUserLocation(double latitude, double longitude, std::optional<double> heading) {
    currentUserLocation = {latitude, longitude};
    if (heading.has_value()) {
        if (std::isfinite(*heading)) {
            currentUserHeading = normalizeHeadingDegrees(*heading);
        } else {
            currentUserHeading.reset();
        }
    }
    updateUserLocationPuck();
}

void MapView::clearUserLocation() {
    currentUserLocation.reset();
    currentUserHeading.reset();
    if (map && styleLoaded) {
        try {
            auto& style = map->getStyle();
            if (style.getLayer(kUserLocationPuck)) {
                style.removeLayer(kUserLocationPuck);
            }
            if (style.getLayer(kUserLocationHalo)) {
                style.removeLayer(kUserLocationHalo);
            }
            if (style.getLayer(kUserLocationBeam)) {
                style.removeLayer(kUserLocationBeam);
            }
            if (style.getSource(kUserLocationSource)) {
                style.removeSource(kUserLocationSource);
            }
            if (repaintCallback) {
                repaintCallback();
            }
        } catch (...) {
            // best-effort cleanup
        }
    }
}

void MapView::setRoute(const std::vector<double>& lonLat) {
    routeCoords.clear();
    const std::size_t count = lonLat.size() / 2;
    routeCoords.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double lng = lonLat[i * 2];
        const double lat = lonLat[i * 2 + 1];
        if (std::isfinite(lng) && std::isfinite(lat)) {
            routeCoords.emplace_back(lng, lat);
        }
    }
    routeTravelledIndex = 0;
    if (!map || !styleLoaded || routeCoords.size() < 2) {
        // Chưa có style ⇒ để onDidFinishLoadingStyle vẽ; tuyến < 2 điểm không phải LineString.
        return;
    }
    try {
        auto& style = map->getStyle();
        ensureRouteLayers(style);
        applyRouteGeoJSON(style);
        if (repaintCallback) {
            repaintCallback();
        }
    } catch (const std::exception& exception) {
        Log::Warning(Event::General, std::string("setRoute failed: ") + exception.what());
    } catch (...) {
        Log::Warning(Event::General, "setRoute failed");
    }
}

void MapView::setRouteProgress(std::size_t travelledIndex) {
    if (routeCoords.empty()) {
        return;
    }
    const std::size_t maxIndex = routeCoords.size() - 1;
    const std::size_t clamped = travelledIndex > maxIndex ? maxIndex : travelledIndex;
    if (clamped == routeTravelledIndex) {
        return;
    }
    routeTravelledIndex = clamped;
    if (!map || !styleLoaded) {
        return;
    }
    try {
        applyRouteGeoJSON(map->getStyle());
        if (repaintCallback) {
            repaintCallback();
        }
    } catch (const std::exception& exception) {
        Log::Warning(Event::General, std::string("setRouteProgress failed: ") + exception.what());
    } catch (...) {
        Log::Warning(Event::General, "setRouteProgress failed");
    }
}

void MapView::clearRoute() {
    routeCoords.clear();
    routeTravelledIndex = 0;
    if (!map || !styleLoaded) {
        return;
    }
    try {
        auto& style = map->getStyle();
        if (style.getLayer(kRouteCasingLayer)) {
            style.removeLayer(kRouteCasingLayer);
        }
        if (style.getLayer(kRouteLineLayer)) {
            style.removeLayer(kRouteLineLayer);
        }
        if (style.getLayer(kRouteTravelledLayer)) {
            style.removeLayer(kRouteTravelledLayer);
        }
        if (style.getSource(kRouteRemainingSource)) {
            style.removeSource(kRouteRemainingSource);
        }
        if (style.getSource(kRouteTravelledSource)) {
            style.removeSource(kRouteTravelledSource);
        }
        if (repaintCallback) {
            repaintCallback();
        }
    } catch (const std::exception& exception) {
        Log::Warning(Event::General, std::string("clearRoute failed: ") + exception.what());
    } catch (...) {
        Log::Warning(Event::General, "clearRoute failed");
    }
}

/**
 * Chèn layer trước lớp symbol ĐẦU TIÊN để nhãn đường luôn nằm trên tuyến. Style chưa có
 * symbol layer ⇒ trả nullopt và layer được thêm lên trên cùng (không còn cách nào tốt hơn).
 */
std::optional<std::string> MapView::firstSymbolLayerId(mbgl::style::Style& style) const {
    for (auto* layer : style.getLayers()) {
        if (!layer || !layer->getTypeInfo()) {
            continue;
        }
        if (std::strcmp(layer->getTypeInfo()->type, "symbol") == 0) {
            return layer->getID();
        }
    }
    return std::nullopt;
}

void MapView::ensureRouteLayers(mbgl::style::Style& style) {
    if (!style.getSource(kRouteRemainingSource)) {
        style.addSource(std::make_unique<mbgl::style::GeoJSONSource>(kRouteRemainingSource));
    }
    if (!style.getSource(kRouteTravelledSource)) {
        style.addSource(std::make_unique<mbgl::style::GeoJSONSource>(kRouteTravelledSource));
    }
    const std::optional<std::string> before = firstSymbolLayerId(style);

    if (!style.getLayer(kRouteCasingLayer)) {
        auto casing = std::make_unique<mbgl::style::LineLayer>(kRouteCasingLayer, kRouteRemainingSource);
        casing->setLineCap(mbgl::style::LineCapType::Round);
        casing->setLineJoin(mbgl::style::LineJoinType::Round);
        casing->setLineWidth(WatchRenderPolicy::routeCasingWidth);
        casing->setLineColor(mbgl::Color(WatchRenderPolicy::routeCasingR, WatchRenderPolicy::routeCasingG,
            WatchRenderPolicy::routeCasingB, WatchRenderPolicy::routeCasingA));
        style.addLayer(std::move(casing), before);
    }
    if (!style.getLayer(kRouteLineLayer)) {
        auto line = std::make_unique<mbgl::style::LineLayer>(kRouteLineLayer, kRouteRemainingSource);
        line->setLineCap(mbgl::style::LineCapType::Round);
        line->setLineJoin(mbgl::style::LineJoinType::Round);
        line->setLineWidth(WatchRenderPolicy::routeLineWidth);
        line->setLineColor(mbgl::Color(WatchRenderPolicy::routeLineR, WatchRenderPolicy::routeLineG,
            WatchRenderPolicy::routeLineB, WatchRenderPolicy::routeLineA));
        style.addLayer(std::move(line), before);
    }
    if (!style.getLayer(kRouteTravelledLayer)) {
        auto travelled = std::make_unique<mbgl::style::LineLayer>(kRouteTravelledLayer, kRouteTravelledSource);
        travelled->setLineCap(mbgl::style::LineCapType::Round);
        travelled->setLineJoin(mbgl::style::LineJoinType::Round);
        travelled->setLineWidth(WatchRenderPolicy::routeLineWidth);
        travelled->setLineColor(mbgl::Color(WatchRenderPolicy::routeTravelledR, WatchRenderPolicy::routeTravelledG,
            WatchRenderPolicy::routeTravelledB, WatchRenderPolicy::routeTravelledA));
        style.addLayer(std::move(travelled), before);
    }
}

void MapView::applyRouteGeoJSON(mbgl::style::Style& style) {
    auto* remaining = static_cast<mbgl::style::GeoJSONSource*>(style.getSource(kRouteRemainingSource));
    auto* travelled = static_cast<mbgl::style::GeoJSONSource*>(style.getSource(kRouteTravelledSource));
    if (!remaining || !travelled) {
        return;
    }
    const std::size_t maxIndex = routeCoords.size() > 0 ? routeCoords.size() - 1 : 0;
    const std::size_t split = routeTravelledIndex > maxIndex ? maxIndex : routeTravelledIndex;

    mbgl::LineString<double> travelledLine;
    if (split >= 1) {
        for (std::size_t i = 0; i <= split; ++i) {
            travelledLine.emplace_back(routeCoords[i].first, routeCoords[i].second);
        }
    }
    mbgl::LineString<double> remainingLine;
    if (routeCoords.size() >= 2 && split < routeCoords.size()) {
        for (std::size_t i = split; i < routeCoords.size(); ++i) {
            remainingLine.emplace_back(routeCoords[i].first, routeCoords[i].second);
        }
    }
    travelled->setGeoJSON(mbgl::Geometry<double>{travelledLine});
    remaining->setGeoJSON(mbgl::Geometry<double>{remainingLine});
}

void MapView::ensureUserLocationBeamImage(mbgl::style::Style& style) {
    if (userLocationBeamImageReady) {
        return;
    }
    style.addImage(std::make_unique<mbgl::style::Image>(
        kUserLocationBeamImage, makeHeadingBeamImage(), 1.0f, false));
    userLocationBeamImageReady = true;
}

void MapView::ensureUserLocationBeamLayer(mbgl::style::Style& style) {
    if (style.getLayer(kUserLocationBeam)) {
        return;
    }
    auto beamLayer = std::make_unique<mbgl::style::SymbolLayer>(kUserLocationBeam, kUserLocationSource);
    beamLayer->setIconImage(mbgl::style::expression::Image(kUserLocationBeamImage));
    beamLayer->setIconAnchor(mbgl::style::SymbolAnchorType::Center);
    beamLayer->setIconAllowOverlap(true);
    beamLayer->setIconIgnorePlacement(true);
    beamLayer->setIconKeepUpright(false);
    beamLayer->setIconPitchAlignment(mbgl::style::AlignmentType::Viewport);
    beamLayer->setIconRotationAlignment(mbgl::style::AlignmentType::Map);
    beamLayer->setIconSize(kBeamIconSize);
    beamLayer->setVisibility(mbgl::style::VisibilityType::None);
    if (style.getLayer(kUserLocationHalo)) {
        style.addLayer(std::move(beamLayer), std::string(kUserLocationHalo));
    } else {
        style.addLayer(std::move(beamLayer));
    }
}

void MapView::syncUserLocationBeam(mbgl::style::Style& style) {
    auto* layer = style.getLayer(kUserLocationBeam);
    if (!layer || !layer->getTypeInfo() || std::strcmp(layer->getTypeInfo()->type, "symbol") != 0) {
        return;
    }
    auto* beam = static_cast<mbgl::style::SymbolLayer*>(layer);
    if (currentUserHeading.has_value()) {
        beam->setIconRotate(static_cast<float>(*currentUserHeading));
        beam->setVisibility(mbgl::style::VisibilityType::Visible);
    } else {
        beam->setVisibility(mbgl::style::VisibilityType::None);
    }
}

void MapView::updateUserLocationPuck() {
    if (!map || !styleLoaded || !currentUserLocation.has_value()) {
        return;
    }
    try {
        auto& style = map->getStyle();
        double lat = currentUserLocation->first;
        double lng = currentUserLocation->second;

        auto* source = static_cast<mbgl::style::GeoJSONSource*>(style.getSource(kUserLocationSource));
        if (!source) {
            auto newSource = std::make_unique<mbgl::style::GeoJSONSource>(kUserLocationSource);
            style.addSource(std::move(newSource));
            ensureUserLocationBeamImage(style);

            // Chùm sáng định hướng (dưới halo + puck).
            ensureUserLocationBeamLayer(style);

            // Vòng hào quang xanh nhạt bán trong suốt
            auto haloLayer = std::make_unique<mbgl::style::CircleLayer>(kUserLocationHalo, kUserLocationSource);
            haloLayer->setCircleRadius(16.0f);
            haloLayer->setCircleColor(mbgl::Color(0.10f, 0.45f, 0.91f, 0.22f)); // #1A73E8 22%
            haloLayer->setCircleStrokeWidth(0.0f);
            haloLayer->setCirclePitchAlignment(mbgl::style::AlignmentType::Viewport);
            style.addLayer(std::move(haloLayer));

            // Chấm xanh trung tâm viền trắng tinh khiết
            auto puckLayer = std::make_unique<mbgl::style::CircleLayer>(kUserLocationPuck, kUserLocationSource);
            puckLayer->setCircleRadius(7.0f);
            puckLayer->setCircleColor(mbgl::Color(0.10f, 0.45f, 0.91f, 1.0f)); // #1A73E8
            puckLayer->setCircleStrokeWidth(2.5f);
            puckLayer->setCircleStrokeColor(mbgl::Color::white());
            puckLayer->setCirclePitchAlignment(mbgl::style::AlignmentType::Viewport);
            style.addLayer(std::move(puckLayer));

            source = static_cast<mbgl::style::GeoJSONSource*>(style.getSource(kUserLocationSource));
        } else {
            ensureUserLocationBeamImage(style);
            ensureUserLocationBeamLayer(style);
        }

        if (source) {
            mbgl::Point<double> pt(lng, lat);
            source->setGeoJSON(mbgl::Geometry<double>{pt});
        }
        syncUserLocationBeam(style);
        if (repaintCallback) {
            repaintCallback();
        }
    } catch (const std::exception& e) {
        Log::Warning(Event::General, std::string("updateUserLocationPuck failed: ") + e.what());
    } catch (...) {
        Log::Warning(Event::General, "updateUserLocationPuck failed");
    }
}

} // namespace ohos
} // namespace mbgl
