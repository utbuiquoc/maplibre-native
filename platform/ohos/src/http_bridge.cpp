#include "http_bridge.hpp"

#include <mutex>
#include <unordered_map>

namespace mbgl {
namespace ohos {

namespace {

static std::mutex g_bridgeMutex;
static HttpBridgeSendFn g_sendFn;
static HttpBridgeCancelFn g_cancelFn;

} // namespace

void setHttpBridge(HttpBridgeSendFn sendFn, HttpBridgeCancelFn cancelFn) {
    std::lock_guard<std::mutex> lock(g_bridgeMutex);
    g_sendFn = std::move(sendFn);
    g_cancelFn = std::move(cancelFn);
}

bool hasHttpBridge() {
    std::lock_guard<std::mutex> lock(g_bridgeMutex);
    return static_cast<bool>(g_sendFn);
}

void sendHttpBridgeRequest(const HttpBridgeRequest& request) {
    HttpBridgeSendFn fn;
    {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        fn = g_sendFn;
    }
    if (fn) {
        fn(request);
    }
}

void cancelHttpBridgeRequest(uint64_t id) {
    HttpBridgeCancelFn fn;
    {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        fn = g_cancelFn;
    }
    if (fn) {
        fn(id);
    }
}

} // namespace ohos
} // namespace mbgl
