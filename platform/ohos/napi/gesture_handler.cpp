#include "gesture_handler.hpp"

#include "map_view.hpp"

#include <mbgl/map/camera.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace mbgl {
namespace ohos {
namespace {

struct PinchState {
    float centerX = 0.0f;
    float centerY = 0.0f;
    double distance = 0.0;
    double angle = 0.0;
};

constexpr auto MaxTapDuration = std::chrono::milliseconds(300);
constexpr auto MaxDoubleTapInterval = std::chrono::milliseconds(300);
constexpr double MaxTapMovement = 24.0;
constexpr double MaxDoubleTapDistance = 48.0;
constexpr double DoubleTapZoomScale = 2.0;
constexpr auto DoubleTapAnimationDuration = std::chrono::milliseconds(300);
constexpr double FlingAnimationBaseMilliseconds = 180.0;
constexpr double MinFlingVelocity = 400.0;
constexpr double FlingVelocityDivisor = 7.0;
constexpr double FlingTiltBaseFactor = 1.5;
constexpr double FlingTiltScale = 10.0;
constexpr double FlingOffsetFactor = 0.42;
constexpr double MaxFlingDistance = 1200.0;
constexpr double FlingViewportMargin = 1.0;
constexpr double FlingVerticalPitchLimitScale = 90.0;
constexpr double Pi = 3.14159265358979323846;
constexpr double TwoPi = 2.0 * Pi;
constexpr double ShoveStartMovement = 16.0;
constexpr double ShoveVerticalRatio = 1.5;
constexpr double ShoveMaxDistanceScaleDelta = 0.06;
constexpr double ShoveMaxAngleDelta = Pi / 18.0;
constexpr double ShovePixelChangeFactor = 0.1;

double distanceBetween(float firstX, float firstY, float secondX, float secondY) {
    return std::hypot(static_cast<double>(secondX - firstX), static_cast<double>(secondY - firstY));
}

double normalizedAngleDelta(double firstAngle, double secondAngle) {
    const double delta = std::remainder(secondAngle - firstAngle, TwoPi);
    return std::abs(delta);
}

double normalizedFlingVelocity(double velocityX, double velocityY, float pixelRatio) {
    if (!std::isfinite(pixelRatio) || pixelRatio <= 0.0f) {
        pixelRatio = 1.0f;
    }
    return std::hypot(velocityX / pixelRatio, velocityY / pixelRatio);
}

std::chrono::milliseconds flingAnimationDuration(double normalizedVelocity, double pitch) {
    if (!std::isfinite(pitch)) {
        pitch = 0.0;
    }
    const double tiltFactor = std::max(0.1, FlingTiltBaseFactor + (pitch != 0.0 ? pitch / FlingTiltScale : 0.0));
    const double duration = normalizedVelocity / FlingVelocityDivisor / tiltFactor + FlingAnimationBaseMilliseconds;
    return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(duration));
}

double flingLimitForExtent(std::uint32_t extent) {
    if (extent <= 2) {
        return MaxFlingDistance;
    }
    return std::max(0.0, std::min(MaxFlingDistance, static_cast<double>(extent) * 0.5 - FlingViewportMargin));
}

double clampHorizontalFlingOffset(double offset, Size surfaceSize) {
    if (!std::isfinite(offset)) {
        return 0.0;
    }
    const double limit = flingLimitForExtent(surfaceSize.width);
    return std::clamp(offset, -limit, limit);
}

double clampVerticalFlingOffset(double offset, Size surfaceSize, double pitch) {
    if (!std::isfinite(offset)) {
        return 0.0;
    }
    double positiveLimit = flingLimitForExtent(surfaceSize.height);
    if (std::isfinite(pitch) && pitch > 0.0) {
        positiveLimit /= 1.0 + pitch / FlingVerticalPitchLimitScale;
    }
    const double negativeLimit = flingLimitForExtent(surfaceSize.height);
    return std::clamp(offset, -negativeLimit, positiveLimit);
}

std::optional<TouchPoint> touchPointForEvent(const TouchEvent& event, std::optional<int32_t> activeId) {
    if (!activeId) {
        return TouchPoint{event.id, event.x, event.y};
    }

    for (const auto& point : event.points) {
        if (point.id == *activeId) {
            return point;
        }
    }

    // Resilient fallback: if active pointer ID is not found (e.g. multi-finger transition,
    // pointer reassignment), adopt the primary available point rather than dropping gesture events.
    if (!event.points.empty()) {
        return event.points.front();
    }

    return std::nullopt;
}

std::optional<std::array<TouchPoint, 2>> touchPairForEvent(const TouchEvent& event) {
    if (event.points.size() < 2) {
        return std::nullopt;
    }

    return std::array<TouchPoint, 2>{
        event.points[0],
        event.points[1],
    };
}

std::optional<PinchState> pinchStateForPoints(const std::optional<std::array<TouchPoint, 2>>& points) {
    if (!points) {
        return std::nullopt;
    }

    const auto& first = (*points)[0];
    const auto& second = (*points)[1];
    const auto distance = distanceBetween(first.x, first.y, second.x, second.y);
    if (!std::isfinite(distance) || distance <= 0.0) {
        return std::nullopt;
    }

    return PinchState{
        (first.x + second.x) * 0.5f,
        (first.y + second.y) * 0.5f,
        distance,
        std::atan2(static_cast<double>(second.y - first.y), static_cast<double>(second.x - first.x)),
    };
}

bool handleTouchAction(GestureState& state,
                       MapView* mapView,
                       TouchAction action,
                       std::optional<TouchPoint> point,
                       std::optional<PinchState> pinch,
                       bool hasMultiplePoints) {
    bool needsRender = false;

    auto handleTap = [&](const TouchPoint& tap) {
        if (!state.tapCandidate) {
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto tapDuration = now - state.tapStartTime;
        state.tapCandidate = false;
        if (tapDuration > MaxTapDuration ||
            distanceBetween(tap.x, tap.y, state.tapStartX, state.tapStartY) > MaxTapMovement) {
            state.lastTapActive = false;
            return false;
        }

        const bool doubleTap = state.lastTapActive && now - state.lastTapTime <= MaxDoubleTapInterval &&
                               distanceBetween(tap.x, tap.y, state.lastTapX, state.lastTapY) <= MaxDoubleTapDistance;
        if (doubleTap) {
            state.lastTapActive = false;
            if (mapView) {
                mapView->flyBy(DoubleTapZoomScale, tap.x, tap.y, mbgl::AnimationOptions{DoubleTapAnimationDuration});
                needsRender = true;
            }
            return true;
        }

        state.lastTapActive = true;
        state.lastTapX = tap.x;
        state.lastTapY = tap.y;
        state.lastTapTime = now;
        return false;
    };

    auto beginPinch = [&] {
        if (!pinch) {
            return;
        }
        state.touchActive = false;
        state.tapCandidate = false;
        state.lastTapActive = false;
        state.touchVelocityX = 0.0;
        state.touchVelocityY = 0.0;
        state.pinchActive = true;
        state.shoveActive = false;
        state.pinchStartCenterX = pinch->centerX;
        state.pinchStartCenterY = pinch->centerY;
        state.pinchStartDistance = pinch->distance;
        state.pinchStartAngle = pinch->angle;
        state.pinchCenterX = pinch->centerX;
        state.pinchCenterY = pinch->centerY;
        state.pinchDistance = pinch->distance;
        state.pinchAngle = pinch->angle;
        if (mapView) {
            mapView->flushPendingPan();
            mapView->setGestureInProgress(true);
            mapView->beginInteractionZoom();
        }
    };

    switch (action) {
        case TouchAction::Down: {
            if (mapView) {
                mapView->cancelTransitions();
                mapView->setGestureInProgress(true);
            }
            if (pinch) {
                beginPinch();
                return true;
            }
            if (!point) {
                return false;
            }
            state.touchActive = true;
            state.pinchActive = false;
            state.shoveActive = false;
            state.touchId = point->id;
            state.touchX = point->x;
            state.touchY = point->y;
            state.tapCandidate = true;
            state.tapStartX = point->x;
            state.tapStartY = point->y;
            state.tapStartTime = std::chrono::steady_clock::now();
            state.touchSampleTime = state.tapStartTime;
            state.touchVelocityX = 0.0;
            state.touchVelocityY = 0.0;
            needsRender = true;
            break;
        }
        case TouchAction::Move: {
            if (pinch) {
                if (!state.pinchActive) {
                    beginPinch();
                    return false;
                }
                if (!mapView) {
                    return false;
                }
                const double totalDx = pinch->centerX - state.pinchStartCenterX;
                const double totalDy = pinch->centerY - state.pinchStartCenterY;
                const double totalVerticalMovement = std::abs(totalDy);
                const double totalHorizontalMovement = std::abs(totalDx);
                const double distanceScaleDelta = state.pinchStartDistance > 0.0
                                                      ? std::abs((pinch->distance / state.pinchStartDistance) - 1.0)
                                                      : 0.0;
                const double angleDelta = normalizedAngleDelta(state.pinchStartAngle, pinch->angle);
                if (!state.shoveActive && totalVerticalMovement >= ShoveStartMovement &&
                    totalVerticalMovement >= totalHorizontalMovement * ShoveVerticalRatio &&
                    distanceScaleDelta <= ShoveMaxDistanceScaleDelta && angleDelta <= ShoveMaxAngleDelta) {
                    state.shoveActive = true;
                }

                const double scale = pinch->distance / state.pinchDistance;
                const double dx = pinch->centerX - state.pinchCenterX;
                const double dy = pinch->centerY - state.pinchCenterY;
                const double previousAngle = state.pinchAngle;
                state.pinchCenterX = pinch->centerX;
                state.pinchCenterY = pinch->centerY;
                state.pinchDistance = pinch->distance;
                state.pinchAngle = pinch->angle;
                if (state.shoveActive) {
                    mapView->pitchBy(-dy * ShovePixelChangeFactor);
                    return true;
                }

                const double currentZoom = mapView->getCameraOptions().zoom.value_or(0.0);
                mapView->prepareInteractionZoom(currentZoom + std::log2(scale));
                mapView->scaleBy(scale, pinch->centerX, pinch->centerY);
                mapView->rotateBy(previousAngle, pinch->angle, pinch->centerX, pinch->centerY);
                mapView->moveBy(dx, dy);
                return true;
            }

            // Pinch ended because second finger lifted: recover gracefully to single-finger pan
            if (state.pinchActive) {
                state.pinchActive = false;
                state.shoveActive = false;
                state.touchActive = true;
                if (point) {
                    state.touchId = point->id;
                    state.touchX = point->x;
                    state.touchY = point->y;
                    state.touchSampleTime = std::chrono::steady_clock::now();
                    state.touchVelocityX = 0.0;
                    state.touchVelocityY = 0.0;
                }
                return false;
            }

            if (hasMultiplePoints) {
                return false;
            }

            // Recover touchActive if Down event was missed
            if (!state.touchActive) {
                if (!point) {
                    return false;
                }
                state.touchActive = true;
                state.touchId = point->id;
                state.touchX = point->x;
                state.touchY = point->y;
                state.tapCandidate = false;
                state.touchSampleTime = std::chrono::steady_clock::now();
                state.touchVelocityX = 0.0;
                state.touchVelocityY = 0.0;
                return false;
            }

            if (!point || !mapView) {
                return false;
            }

            state.touchId = point->id;

            const auto now = std::chrono::steady_clock::now();
            const double dx = point->x - state.touchX;
            const double dy = point->y - state.touchY;
            state.touchX = point->x;
            state.touchY = point->y;
            const auto elapsed = std::chrono::duration<double>(now - state.touchSampleTime).count();
            // Reject micro-intervals (< 8ms) to prevent velocity explosion during batch touch events / frame hitches
            if (elapsed >= 0.008) {
                const double instantVx = dx / elapsed;
                const double instantVy = dy / elapsed;
                state.touchVelocityX = state.touchVelocityX * 0.35 + instantVx * 0.65;
                state.touchVelocityY = state.touchVelocityY * 0.35 + instantVy * 0.65;
                state.touchSampleTime = now;
            }
            if (distanceBetween(point->x, point->y, state.tapStartX, state.tapStartY) > MaxTapMovement) {
                state.tapCandidate = false;
            }
            // Coalesce: accumulate here, apply once per pump tick in
            // MapView::pumpEvents (driven by DisplaySync/watchdog, not by input).
            mapView->accumulatePan(dx, dy);
            needsRender = true;
            break;
        }
        case TouchAction::Up:
        case TouchAction::Cancel: {
            if (action == TouchAction::Cancel) {
                state.touchActive = false;
                state.pinchActive = false;
                state.shoveActive = false;
                state.tapCandidate = false;
                state.touchVelocityX = 0.0;
                state.touchVelocityY = 0.0;
                if (mapView) {
                    mapView->cancelTransitions();
                    mapView->setGestureInProgress(false);
                    mapView->endInteractionZoom();
                }
                needsRender = true;
                break;
            }

            const bool canHandleTap = !state.pinchActive && point;
            double velocity = mapView ? normalizedFlingVelocity(
                                            state.touchVelocityX, state.touchVelocityY, mapView->getPixelRatio())
                                      : 0.0;
            // Cap fling velocity to avoid launching camera into astronomical unrendered space
            constexpr double MaxFlingVelocity = 2400.0;
            if (velocity > MaxFlingVelocity) {
                const double scale = MaxFlingVelocity / velocity;
                state.touchVelocityX *= scale;
                state.touchVelocityY *= scale;
                velocity = MaxFlingVelocity;
            }

            const bool shouldFling = !state.tapCandidate && !state.pinchActive &&
                                     std::isfinite(velocity) && velocity >= MinFlingVelocity;
            state.touchActive = false;
            state.pinchActive = false;
            state.shoveActive = false;
            if (mapView) {
                // Apply any coalesced pan first so the fling animation starts
                // from the true finger position, not one tick behind.
                mapView->flushPendingPan();
                mapView->endInteractionZoom();
                if (!canHandleTap || !handleTap(*point)) {
                    if (shouldFling) {
                        const double pitch = mapView->getCameraOptions().pitch.value_or(0.0);
                        auto duration = flingAnimationDuration(velocity, pitch);
                        // Cap fling animation duration to wearable maximum (350ms)
                        constexpr std::chrono::milliseconds MaxFlingDuration{350};
                        if (duration > MaxFlingDuration) {
                            duration = MaxFlingDuration;
                        }
                        const double durationSeconds = std::chrono::duration<double>(duration).count();
                        const auto surfaceSize = mapView->getSurfaceSize();
                        const double offsetX = state.touchVelocityX * durationSeconds * FlingOffsetFactor;
                        const double offsetY = state.touchVelocityY * durationSeconds * FlingOffsetFactor;
                        // Keep gestureInProgress true during fling invocation so Transform::easeTo
                        // does not convert the moveBy animation into a flyTo parabolic flight curve!
                        mapView->moveBy(clampHorizontalFlingOffset(offsetX, surfaceSize),
                                        clampVerticalFlingOffset(offsetY, surfaceSize, pitch),
                                        mbgl::AnimationOptions{duration});
                    } else {
                        mapView->cancelTransitions();
                    }
                    mapView->setGestureInProgress(false);
                    needsRender = true;
                } else {
                    mapView->setGestureInProgress(false);
                }
            }
            state.tapCandidate = false;
            state.touchVelocityX = 0.0;
            state.touchVelocityY = 0.0;
            break;
        }
        case TouchAction::Unknown:
            break;
    }

    return needsRender;
}

} // namespace

bool hasActiveGesture(const GestureState& state) {
    return state.touchActive || state.pinchActive;
}

void resetGestureState(GestureState& state) {
    state = GestureState();
}

bool handleTouchEvent(GestureState& state, MapView* mapView, const TouchEvent& event) {
    const auto action = event.action;
    const auto point = action == TouchAction::Move ? touchPointForEvent(event, state.touchId)
                                                   : touchPointForEvent(event, std::nullopt);
    const auto pinch = pinchStateForPoints(touchPairForEvent(event));
    return handleTouchAction(state, mapView, action, point, pinch, event.points.size() > 1);
}

} // namespace ohos
} // namespace mbgl
