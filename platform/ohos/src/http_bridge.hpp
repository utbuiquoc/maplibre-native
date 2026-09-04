#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mbgl {
namespace ohos {

struct HttpBridgeRequest {
    uint64_t id;
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
};

using HttpBridgeSendFn = std::function<void(const HttpBridgeRequest&)>;
using HttpBridgeCancelFn = std::function<void(uint64_t)>;

void setHttpBridge(HttpBridgeSendFn sendFn, HttpBridgeCancelFn cancelFn);
bool hasHttpBridge();
void sendHttpBridgeRequest(const HttpBridgeRequest& request);
void cancelHttpBridgeRequest(uint64_t id);

void dispatchHttpResponse(uint64_t id,
                          int statusCode,
                          const char* data,
                          size_t size,
                          const std::map<std::string, std::string>& headers,
                          const std::string& errorMsg);

} // namespace ohos
} // namespace mbgl
