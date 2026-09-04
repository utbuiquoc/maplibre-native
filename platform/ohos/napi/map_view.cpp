#include "map_view.hpp"
#include "watch_render_policy.hpp"

#if MLN_RENDER_BACKEND_VULKAN
#include "vulkan_window_backend.hpp"
#else
#include "egl_window_backend.hpp"
#endif

#include <mbgl/math/angles.hpp>
#include <mbgl/map/map_options.hpp>
#include <mbgl/style/source.hpp>
#include <mbgl/style/style.hpp>
#include <mbgl/util/async_task.hpp>
#include <mbgl/util/client_options.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/run_loop.hpp>
#include <mbgl/util/string.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
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
    tickZoomSession();
    runLoopOnce();
    // Fling / flyTo keep isPanning|isScaling after the finger is up. Re-assert
    // MapLibre's gesture flag so symbol placement stays deferred until idle.
    syncGestureFlag();
}

bool MapView::renderFrame() {
    pumpEvents();

    bool rendered = false;
    auto renderIfNeeded = [&] {
        if (frontend && frontend->hasPendingRender()) {
            if (frontend->renderFrame()) {
                ++renderedFrameCount;
                rendered = true;
            }
        }
    };

    renderIfNeeded();
    runLoopOnce();
    // Second GPU pass is for draining HTTP during load. During pan it doubles
    // a ~30–50ms GL frame on the UI thread and batches the next Moves.
    if (!isInteractive()) {
        renderIfNeeded();
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

void MapView::moveBy(double x, double y, AnimationOptions animationOptions) {
    if (!map) {
        return;
    }

    map->moveBy(logicalCoordinateForFramebufferCoordinate(x, y, pixelRatio), std::move(animationOptions));
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
    const double nextZoom = currentZoom + std::log2(scale);
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
    // Giữ coveringShift để giới hạn trần zoom phủ (maxCoveringZoom = 16.0),
    // không đóng băng mức tile cũ khi zoom in để tránh triệt tiêu layer đường nhỏ.
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

    zoomSession = {};
    applyCoveringZoomCap();
    syncGestureFlag();
}

void MapView::syncGestureFlag() {
    if (!map) {
        return;
    }
    map->setGestureInProgress(touchGestureActive || zoomSession.active || map->isPanning() || map->isScaling() ||
                              map->isRotating());
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

    if (!desiredBounds || !desiredBounds->maxZoom) {
        BoundOptions bounds = desiredBounds.value_or(BoundOptions());
        bounds.withMaxZoom(WatchRenderPolicy::maxZoom).withMinZoom(WatchRenderPolicy::minZoom);
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
    if (!map) {
        return;
    }
    WatchRenderPolicy::applyStyle(map->getStyle());
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
    lastMapLoadError.clear();
    lastRenderError.clear();
    lastStyleImageMissing.clear();
    lastGlyphsError.clear();
    lastSpritesError.clear();
}

} // namespace ohos
} // namespace mbgl
