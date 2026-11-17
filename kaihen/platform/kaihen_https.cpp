// A small HTTPS GET (kaihen_https.h).
#include "kaihen_https.h"

#include "runtime_config.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#if defined(MBEDTLS_PSA_CRYPTO_C)
#include <psa/crypto.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace kaihen_https {
namespace {

// The certificates and the random generator, set up once.
struct Globals {
    mbedtls_x509_crt ca;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    bool ready = false;
    std::string error;
};

Globals& Shared() {
    static Globals globals;
    static std::once_flag once;
    std::call_once(once, [] {
        mbedtls_x509_crt_init(&globals.ca);
        mbedtls_entropy_init(&globals.entropy);
        mbedtls_ctr_drbg_init(&globals.drbg);
#if defined(MBEDTLS_PSA_CRYPTO_C)
        psa_crypto_init();
#endif
        static const char kSeed[] = "kaihen_setup";
        if (mbedtls_ctr_drbg_seed(&globals.drbg, mbedtls_entropy_func, &globals.entropy,
                                  reinterpret_cast<const unsigned char*>(kSeed), sizeof kSeed - 1) != 0) {
            globals.error = "the random number generator didn't start";
            return;
        }
        const std::string bundle = std::string(RuntimeConfigFile::kaihen_root_directory()) + "/cacert.pem";
        if (mbedtls_x509_crt_parse_file(&globals.ca, bundle.c_str()) < 0) {
            globals.error = "couldn't read " + bundle;
            return;
        }
        globals.ready = true;
    });
    return globals;
}

std::string TlsError(int code) {
    char text[160];
    mbedtls_strerror(code, text, sizeof text);
    return text;
}

bool ParseUrl(const std::string& url, std::string& host, std::string& path) {
    constexpr char kScheme[] = "https://";
    if (url.compare(0, sizeof kScheme - 1, kScheme) != 0) {
        return false;
    }
    const size_t start = sizeof kScheme - 1;
    const size_t slash = url.find('/', start);
    host = url.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    path = slash == std::string::npos ? "/" : url.substr(slash);
    return !host.empty() && host.find(':') == std::string::npos;
}

std::string HeaderValue(const std::string& headers, const char* name) {
    std::string lower = headers;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    const std::string key = std::string("\r\n") + name + ":";
    const size_t at = lower.find(key);
    if (at == std::string::npos) {
        return {};
    }
    size_t begin = at + key.size();
    const size_t end = headers.find("\r\n", begin);
    while (begin < end && headers[begin] == ' ') ++begin;
    return headers.substr(begin, end - begin);
}

}  // namespace

struct Get::Impl {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    bool open = false;
    std::string pending;  // body bytes read along with the headers
    uint64_t length = UINT64_MAX;
    uint64_t remaining = UINT64_MAX;
    uint64_t total = UINT64_MAX;

    Impl() {
        mbedtls_net_init(&net);
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&config);
    }
    ~Impl() { Close(); }

    void Close() {
        if (open) {
            mbedtls_ssl_close_notify(&ssl);
        }
        open = false;
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&config);
        mbedtls_net_free(&net);
        mbedtls_net_init(&net);
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&config);
        pending.clear();
    }

    bool Connect(const std::string& host, std::string& error) {
        Globals& globals = Shared();
        if (!globals.ready) {
            error = globals.error;
            return false;
        }
        int code = mbedtls_net_connect(&net, host.c_str(), "443", MBEDTLS_NET_PROTO_TCP);
        if (code != 0) {
            error = "couldn't connect to " + host + " (" + TlsError(code) + ")";
            return false;
        }
        if ((code = mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                                MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
            error = TlsError(code);
            return false;
        }
        mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&config, &globals.ca, nullptr);
        mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &globals.drbg);
        mbedtls_ssl_conf_read_timeout(&config, 30000);
        if ((code = mbedtls_ssl_setup(&ssl, &config)) != 0 ||
            (code = mbedtls_ssl_set_hostname(&ssl, host.c_str())) != 0) {
            error = TlsError(code);
            return false;
        }
        mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, nullptr, mbedtls_net_recv_timeout);
        while ((code = mbedtls_ssl_handshake(&ssl)) != 0) {
            if (code != MBEDTLS_ERR_SSL_WANT_READ && code != MBEDTLS_ERR_SSL_WANT_WRITE) {
                error = "secure connection to " + host + " failed (" + TlsError(code) + ")";
                return false;
            }
        }
        open = true;
        return true;
    }

    bool WriteAll(const std::string& text, std::string& error) {
        size_t done = 0;
        while (done < text.size()) {
            const int n = mbedtls_ssl_write(&ssl, reinterpret_cast<const unsigned char*>(text.data()) + done,
                                            text.size() - done);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (n <= 0) {
                error = "sending the request failed (" + TlsError(n) + ")";
                return false;
            }
            done += size_t(n);
        }
        return true;
    }

    // Raw TLS read: count, 0 at the end of the connection, -1 on an error.
    int RawRead(unsigned char* buffer, size_t size, std::string& error) {
        for (;;) {
            const int n = mbedtls_ssl_read(&ssl, buffer, size);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == 0) return 0;
            if (n < 0) {
                error = "the download stopped (" + TlsError(n) + ")";
                return -1;
            }
            return n;
        }
    }
};

Get::Get() : impl_(std::make_unique<Impl>()) {}
Get::~Get() = default;

bool Get::Open(const std::string& start_url, uint64_t from, uint64_t to, std::string& error) {
    std::string url = start_url;
    for (int redirects = 0; redirects < 5; ++redirects) {
        impl_->Close();
        std::string host, path;
        if (!ParseUrl(url, host, path)) {
            error = "unsupported address " + url;
            return false;
        }
        if (!impl_->Connect(host, error)) {
            return false;
        }
        std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + host +
                              "\r\nUser-Agent: WiiCompiled-kaiHEN\r\nAccept-Encoding: identity\r\nConnection: close\r\n";
        if (from != 0 || to != UINT64_MAX) {
            request += "Range: bytes=" + std::to_string(from) + "-" + (to == UINT64_MAX ? "" : std::to_string(to)) + "\r\n";
        }
        request += "\r\n";
        if (!impl_->WriteAll(request, error)) {
            return false;
        }
        // The status line and headers.
        std::string head;
        unsigned char buffer[4096];
        size_t end;
        while ((end = head.find("\r\n\r\n")) == std::string::npos) {
            const int n = impl_->RawRead(buffer, sizeof buffer, error);
            if (n <= 0) {
                if (n == 0) error = "the server closed the connection";
                return false;
            }
            head.append(reinterpret_cast<char*>(buffer), size_t(n));
            if (head.size() > 65536) {
                error = "the server's answer was too long";
                return false;
            }
        }
        impl_->pending = head.substr(end + 4);
        const std::string headers = "\r\n" + head.substr(0, end + 2);
        int status = 0;
        if (std::sscanf(head.c_str(), "HTTP/%*s %d", &status) != 1) {
            error = "the server's answer wasn't HTTP";
            return false;
        }
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            url = HeaderValue(headers, "location");
            continue;
        }
        const bool ranged = from != 0 || to != UINT64_MAX;
        if (status != (ranged ? 206 : 200)) {
            error = "the server answered " + std::to_string(status);
            return false;
        }
        if (!HeaderValue(headers, "transfer-encoding").empty()) {
            error = "the server sent the data in a form this app can't read";
            return false;
        }
        const std::string length = HeaderValue(headers, "content-length");
        impl_->length = length.empty() ? UINT64_MAX : std::strtoull(length.c_str(), nullptr, 10);
        impl_->remaining = impl_->length;
        const std::string range = HeaderValue(headers, "content-range");  // "bytes 0-0/1234"
        const size_t slash = range.rfind('/');
        impl_->total = slash == std::string::npos || range.compare(slash + 1, 1, "*") == 0
                           ? UINT64_MAX : std::strtoull(range.c_str() + slash + 1, nullptr, 10);
        return true;
    }
    error = "too many redirects";
    return false;
}

int64_t Get::Read(void* buffer, size_t size, std::string& error) {
    if (impl_->remaining == 0) {
        return 0;
    }
    size = size_t(std::min<uint64_t>(size, impl_->remaining));
    int64_t n;
    if (!impl_->pending.empty()) {
        n = int64_t(std::min(size, impl_->pending.size()));
        std::memcpy(buffer, impl_->pending.data(), size_t(n));
        impl_->pending.erase(0, size_t(n));
    } else {
        n = impl_->RawRead(static_cast<unsigned char*>(buffer), size, error);
        if (n < 0) return -1;
        if (n == 0) {
            if (impl_->remaining != UINT64_MAX) {
                error = "the download ended early";
                return -1;
            }
            return 0;
        }
    }
    if (impl_->remaining != UINT64_MAX) impl_->remaining -= uint64_t(n);
    return n;
}

uint64_t Get::Length() const { return impl_->length; }
uint64_t Get::Total() const { return impl_->total; }

}  // namespace kaihen_https
