#pragma once

// Minimal WSS client used by the QQ Official Bot Gateway.  Drogon's hostname
// resolver is c-ares based; it can reject valid CNAME records on some VPN DNS
// configurations.  This client resolves through the operating system, while
// retaining the original hostname for TLS SNI and certificate validation.

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <drogon/utils/Utilities.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace dice {

class QQGatewaySocket final : public std::enable_shared_from_this<QQGatewaySocket> {
public:
    using TextHandler = std::function<void(std::string)>;
    using ErrorHandler = std::function<void(const std::string&)>;
    using CloseHandler = std::function<void()>;

    QQGatewaySocket(std::string host, uint16_t port, std::string path,
                    TextHandler onText, ErrorHandler onError, CloseHandler onClose)
        : host_(std::move(host)), port_(port), path_(std::move(path)),
          onText_(std::move(onText)), onError_(std::move(onError)), onClose_(std::move(onClose)) {}

    ~QQGatewaySocket() { stop(); }

    void start() {
        std::thread([self = shared_from_this()] { self->run(); }).detach();
    }

    void stop() {
        stopping_ = true;
        const auto socket = socket_.load();
        if (socket != kInvalidSocket) shutdownSocket(static_cast<Socket>(socket));
    }

    bool sendText(const std::string& text) { return sendFrame(0x1, text); }

private:
#ifdef _WIN32
    using Socket = SOCKET;
    static constexpr Socket kNativeInvalidSocket = INVALID_SOCKET;
#else
    using Socket = int;
    static constexpr Socket kNativeInvalidSocket = -1;
#endif
    static constexpr uintptr_t kInvalidSocket = (std::numeric_limits<uintptr_t>::max)();

    static void closeSocket(Socket socket) {
#ifdef _WIN32
        closesocket(socket);
#else
        close(socket);
#endif
    }
    static void shutdownSocket(Socket socket) {
#ifdef _WIN32
        shutdown(socket, SD_BOTH);
#else
        shutdown(socket, SHUT_RDWR);
#endif
    }

    static std::string sslError(const char* prefix) {
        const auto code = ERR_get_error();
        char text[256]{};
        if (code != 0) ERR_error_string_n(code, text, sizeof(text));
        return std::string(prefix) + (code != 0 ? ": " + std::string(text) : "");
    }

#ifdef _WIN32
    // OpenSSL on Windows does not automatically use the Windows root store.
    // Validate the peer certificate with CryptoAPI instead, including hostname
    // validation, before allowing any Gateway application data to be exchanged.
    bool verifyWithWindowsTrustStore() const {
        X509* peer = SSL_get1_peer_certificate(ssl_);
        if (!peer) return false;
        const int encodedSize = i2d_X509(peer, nullptr);
        std::vector<unsigned char> encoded(encodedSize > 0 ? static_cast<size_t>(encodedSize) : 0);
        unsigned char* cursor = encoded.data();
        const bool encodedOk = encodedSize > 0 && i2d_X509(peer, &cursor) == encodedSize;
        X509_free(peer);
        if (!encodedOk) return false;
        PCCERT_CONTEXT certificate = CertCreateCertificateContext(
            X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, encoded.data(), static_cast<DWORD>(encoded.size()));
        if (!certificate) return false;
        CERT_CHAIN_PARA chainParameters{};
        chainParameters.cbSize = sizeof(chainParameters);
        PCCERT_CHAIN_CONTEXT chain = nullptr;
        const bool chained = CertGetCertificateChain(nullptr, certificate, nullptr, nullptr,
            &chainParameters, 0, nullptr, &chain) == TRUE;
        bool verified = false;
        if (chained) {
            std::wstring hostname(host_.begin(), host_.end());
            SSL_EXTRA_CERT_CHAIN_POLICY_PARA sslParameters{};
            sslParameters.cbSize = sizeof(sslParameters);
            sslParameters.dwAuthType = AUTHTYPE_SERVER;
            sslParameters.pwszServerName = hostname.data();
            CERT_CHAIN_POLICY_PARA policyParameters{};
            policyParameters.cbSize = sizeof(policyParameters);
            policyParameters.pvExtraPolicyPara = &sslParameters;
            CERT_CHAIN_POLICY_STATUS policyStatus{};
            policyStatus.cbSize = sizeof(policyStatus);
            verified = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain,
                &policyParameters, &policyStatus) == TRUE && policyStatus.dwError == 0;
            CertFreeCertificateChain(chain);
        }
        CertFreeCertificateContext(certificate);
        return verified;
    }
#endif

    Socket connectTcp() {
        addrinfo hints{};
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family = AF_UNSPEC;
        addrinfo* results = nullptr;
        const auto service = std::to_string(port_);
        if (getaddrinfo(host_.c_str(), service.c_str(), &hints, &results) != 0 || !results) return kNativeInvalidSocket;
        Socket connected = kNativeInvalidSocket;
        for (auto* entry = results; entry; entry = entry->ai_next) {
            const auto socket = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
            if (socket == kNativeInvalidSocket) continue;
            if (::connect(socket, entry->ai_addr, static_cast<int>(entry->ai_addrlen)) == 0) {
                connected = socket;
                break;
            }
            closeSocket(socket);
        }
        freeaddrinfo(results);
        return connected;
    }

    bool readExact(void* destination, size_t size) {
        auto* out = static_cast<unsigned char*>(destination);
        size_t offset = 0;
        while (offset < size && !stopping_) {
            const int received = SSL_read(ssl_, out + offset, static_cast<int>(size - offset));
            if (received <= 0) return false;
            offset += static_cast<size_t>(received);
        }
        return offset == size;
    }

    bool writeAll(const void* source, size_t size) {
        const auto* data = static_cast<const unsigned char*>(source);
        size_t offset = 0;
        while (offset < size && !stopping_) {
            const int written = SSL_write(ssl_, data + offset, static_cast<int>(size - offset));
            if (written <= 0) return false;
            offset += static_cast<size_t>(written);
        }
        return offset == size;
    }

    bool sendFrame(uint8_t opcode, const std::string& payload) {
        std::lock_guard lock(writeMutex_);
        if (!ssl_ || stopping_) return false;
        std::vector<unsigned char> frame;
        frame.reserve(payload.size() + 14);
        frame.push_back(static_cast<unsigned char>(0x80 | opcode));
        const auto size = payload.size();
        if (size <= 125) frame.push_back(static_cast<unsigned char>(0x80 | size));
        else if (size <= 0xffff) {
            frame.push_back(0x80 | 126);
            frame.push_back(static_cast<unsigned char>((size >> 8) & 0xff));
            frame.push_back(static_cast<unsigned char>(size & 0xff));
        } else {
            frame.push_back(0x80 | 127);
            for (int i = 7; i >= 0; --i) frame.push_back(static_cast<unsigned char>((static_cast<uint64_t>(size) >> (i * 8)) & 0xff));
        }
        unsigned char mask[4]{};
        if (RAND_bytes(mask, sizeof(mask)) != 1) return false;
        frame.insert(frame.end(), std::begin(mask), std::end(mask));
        for (size_t i = 0; i < size; ++i) frame.push_back(static_cast<unsigned char>(payload[i]) ^ mask[i % 4]);
        return writeAll(frame.data(), frame.size());
    }

    bool performHandshake() {
        std::string random(16, '\0');
        if (RAND_bytes(reinterpret_cast<unsigned char*>(random.data()), static_cast<int>(random.size())) != 1) return false;
        const auto key = drogon::utils::base64Encode(random);
        const std::string request = "GET " + path_ + " HTTP/1.1\r\nHost: " + host_ +
            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + key +
            "\r\nUser-Agent: DiceNext/3\r\n\r\n";
        if (!writeAll(request.data(), request.size())) return false;
        std::string response;
        char byte{};
        while (response.size() < 16384 && readExact(&byte, 1)) {
            response.push_back(byte);
            if (response.size() >= 4 && response.compare(response.size() - 4, 4, "\r\n\r\n") == 0) break;
        }
        return response.rfind("HTTP/1.1 101", 0) == 0 || response.rfind("HTTP/1.0 101", 0) == 0;
    }

    void readFrames() {
        while (!stopping_) {
            unsigned char head[2]{};
            if (!readExact(head, sizeof(head))) return;
            const uint8_t opcode = head[0] & 0x0f;
            uint64_t size = head[1] & 0x7f;
            if (size == 126) {
                unsigned char extra[2]{};
                if (!readExact(extra, sizeof(extra))) return;
                size = (static_cast<uint64_t>(extra[0]) << 8) | extra[1];
            } else if (size == 127) {
                unsigned char extra[8]{};
                if (!readExact(extra, sizeof(extra))) return;
                size = 0;
                for (const auto value : extra) size = (size << 8) | value;
            }
            if (size > 8 * 1024 * 1024) { if (onError_) onError_("QQ Gateway 返回了过大的数据帧"); return; }
            if (head[1] & 0x80) {
                unsigned char mask[4]{};
                if (!readExact(mask, sizeof(mask))) return;
                // Gateway frames are server-to-client and normally unmasked.
            }
            std::string payload(static_cast<size_t>(size), '\0');
            if (size && !readExact(payload.data(), payload.size())) return;
            if (opcode == 0x1 && onText_) onText_(std::move(payload));
            else if (opcode == 0x8) return;
            else if (opcode == 0x9) sendFrame(0xA, payload);
        }
    }

    void run() {
        const auto socket = connectTcp();
        if (socket == kNativeInvalidSocket) { if (!stopping_ && onError_) onError_("无法通过系统 DNS 连接 QQ Gateway"); return; }
        socket_.store(static_cast<uintptr_t>(socket));
        SSL_CTX* context = SSL_CTX_new(TLS_client_method());
        if (context) {
            // Keep the same certificate-verification guarantee as Drogon's
            // normal wss:// client; resolving through the OS must not weaken
            // the trust boundary for a credential-bearing Gateway session.
#ifdef _WIN32
            // OpenSSL's bundled Windows build has no access to the OS root
            // store.  We verify with CryptoAPI after the TLS handshake below.
            SSL_CTX_set_verify(context, SSL_VERIFY_NONE, nullptr);
#else
            SSL_CTX_set_verify(context, SSL_VERIFY_PEER, nullptr);
            SSL_CTX_set_default_verify_paths(context);
#endif
        }
        ssl_ = context ? SSL_new(context) : nullptr;
        if (!ssl_ || SSL_set_tlsext_host_name(ssl_, host_.c_str()) != 1 || SSL_set_fd(ssl_, static_cast<int>(socket)) != 1 || SSL_connect(ssl_) != 1) {
            if (!stopping_ && onError_) onError_(sslError("QQ Gateway TLS 握手失败"));
        }
#ifdef _WIN32
        else if (!verifyWithWindowsTrustStore()) {
            if (!stopping_ && onError_) onError_("QQ Gateway TLS 证书未通过 Windows 系统证书库校验");
        }
#endif
        else if (!performHandshake()) {
            if (!stopping_ && onError_) onError_("QQ Gateway WebSocket 升级被拒绝");
        } else {
            readFrames();
        }
        if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); ssl_ = nullptr; }
        if (context) SSL_CTX_free(context);
        closeSocket(socket);
        socket_.store(kInvalidSocket);
        if (!stopping_ && onClose_) onClose_();
    }

    std::string host_;
    uint16_t port_;
    std::string path_;
    TextHandler onText_;
    ErrorHandler onError_;
    CloseHandler onClose_;
    std::atomic<bool> stopping_{false};
    std::atomic<uintptr_t> socket_{kInvalidSocket};
    std::mutex writeMutex_;
    SSL* ssl_{nullptr};
};

}  // namespace dice
