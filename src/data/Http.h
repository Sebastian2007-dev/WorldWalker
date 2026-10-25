#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace net {

struct HttpResult {
    int status = 0;  // 0 = transport error
    std::vector<uint8_t> body;
    bool ok() const { return status == 200 || status == 206; }
    bool notFound() const { return status == 404 || status == 403; }  // S3 answers 403 for missing keys
};

// Blocking GET; thread-safe. rangeEnd is inclusive, pass -1 for no range.
HttpResult get(const std::string& url, int64_t rangeBegin = -1, int64_t rangeEnd = -1);

}  // namespace net
