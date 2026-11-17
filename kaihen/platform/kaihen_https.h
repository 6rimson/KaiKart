#pragma once

// A small HTTPS GET for the first-launch setup (kaihen_setup.cpp): one request per object, an
// optional byte range, redirects followed, the body read in pieces. mbed TLS (the runtime's own
// copy) checks the server against the card's cacert.pem.
#include <cstdint>
#include <memory>
#include <string>

namespace kaihen_https {

class Get {
public:
    Get();
    ~Get();
    Get(const Get&) = delete;
    Get& operator=(const Get&) = delete;

    // Requests `url` (https only), from byte `from` to byte `to` inclusive (to == UINT64_MAX: to the
    // end). False with `error` set when it fails or the server answers with anything but the data.
    bool Open(const std::string& url, uint64_t from, uint64_t to, std::string& error);
    // Reads up to `size` bytes of the body: the count, 0 at its end, or -1 with `error` set.
    int64_t Read(void* buffer, size_t size, std::string& error);
    // The body's length (the range's, for a range), or UINT64_MAX when the server didn't say.
    uint64_t Length() const;
    // For a range: the whole object's size (Content-Range), or UINT64_MAX when the server didn't say.
    uint64_t Total() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace kaihen_https
