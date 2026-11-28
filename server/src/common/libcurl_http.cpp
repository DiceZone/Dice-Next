#include "libcurl_http.h"

#include <curl/curl.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>

namespace dice::http {
namespace fs = std::filesystem;

namespace {

void ensureCurl() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct FileCloser {
    void operator()(FILE* file) const {
        if (file) std::fclose(file);
    }
};

struct CurlHandle {
    CURL* easy = nullptr;
    CurlHandle() : easy(curl_easy_init()) {}
    ~CurlHandle() {
        if (easy) curl_easy_cleanup(easy);
    }
    CurlHandle(const CurlHandle&) = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;
};

struct HeaderList {
    curl_slist* list = nullptr;
    ~HeaderList() {
        if (list) curl_slist_free_all(list);
    }
    void append(const std::string& line) { list = curl_slist_append(list, line.c_str()); }
};

struct Mime {
    curl_mime* mime = nullptr;
    explicit Mime(CURL* easy) : mime(curl_mime_init(easy)) {}
    ~Mime() {
        if (mime) curl_mime_free(mime);
    }
};

std::string pathUtf8(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

FILE* openFile(const fs::path& path, bool write) {
#if defined(_WIN32)
    return _wfopen(path.wstring().c_str(), write ? L"wb" : L"rb");
#else
    return std::fopen(path.string().c_str(), write ? "wb" : "rb");
#endif
}

struct Sink {
    FILE* file = nullptr;
    std::string* body = nullptr;
    curl_off_t maxBytes = 0;
    curl_off_t written = 0;
    bool tooLarge = false;
};

size_t onWrite(char* ptr, size_t size, size_t nmemb, void* user) {
    auto* sink = static_cast<Sink*>(user);
    const size_t count = size * nmemb;
    if (sink->maxBytes > 0 &&
        sink->written + static_cast<curl_off_t>(count) > sink->maxBytes) {
        sink->tooLarge = true;
        return 0;
    }
    if (sink->file) {
        const size_t wrote = std::fwrite(ptr, 1, count, sink->file);
        sink->written += static_cast<curl_off_t>(wrote);
        return wrote;
    }
    if (sink->body) {
        sink->body->append(ptr, count);
        sink->written += static_cast<curl_off_t>(count);
        return count;
    }
    return 0;
}

struct TransferState {
    const dice::proc::CancellationCheck* cancelled = nullptr;
    curl_off_t maxBytes = 0;
    bool tooLarge = false;
};

int onProgress(void* client, curl_off_t, curl_off_t now, curl_off_t, curl_off_t) {
    auto* state = static_cast<TransferState*>(client);
    if (state->maxBytes > 0 && now > state->maxBytes) {
        state->tooLarge = true;
        return 1;
    }
    if (state->cancelled && (*state->cancelled)()) return 1;
    return 0;
}

bool isSmtp(const std::string& url) {
    return url.rfind("smtp://", 0) == 0 || url.rfind("smtps://", 0) == 0;
}

} // namespace

FetchResult fetch(const Fetch& request) {
    FetchResult result;
    if (request.url.empty()) {
        result.error = "下载地址为空";
        result.curlCode = CURLE_URL_MALFORMAT;
        return result;
    }
    ensureCurl();
    CurlHandle curl;
    if (!curl.easy) {
        result.error = "无法初始化 libcurl";
        result.curlCode = CURLE_FAILED_INIT;
        return result;
    }

    std::unique_ptr<FILE, FileCloser> outFile;
    std::unique_ptr<FILE, FileCloser> inFile;
    Sink sink;
    if (!request.output.empty()) {
        outFile.reset(openFile(request.output, true));
        if (!outFile) {
            result.error = "无法写入下载文件";
            result.curlCode = CURLE_WRITE_ERROR;
            return result;
        }
        sink.file = outFile.get();
    } else {
        sink.body = &result.body;
    }
    sink.maxBytes = static_cast<curl_off_t>(request.maxBytes);

    HeaderList headers;
    for (const auto& [name, value] : request.headers) {
        if (name.empty() || name.find_first_of(":\r\n") != std::string::npos ||
            value.find_first_of("\r\n") != std::string::npos) {
            result.error = "无效的 HTTP 请求头";
            result.curlCode = CURLE_BAD_FUNCTION_ARGUMENT;
            return result;
        }
        headers.append(name + ": " + value);
    }

    TransferState state;
    state.cancelled = request.cancelled ? &request.cancelled : nullptr;
    state.maxBytes = static_cast<curl_off_t>(request.maxBytes);

    const bool smtp = !request.mailRcpt.empty() && !request.uploadFile.empty() && isSmtp(request.url);
    const char* protocols = smtp ? "smtp,smtps" : (request.allowHttp ? "http,https" : "https");
    curl_easy_setopt(curl.easy, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl.easy, CURLOPT_PROTOCOLS_STR, protocols);
    curl_easy_setopt(curl.easy, CURLOPT_REDIR_PROTOCOLS_STR, protocols);
    curl_easy_setopt(curl.easy, CURLOPT_FOLLOWLOCATION, request.followRedirects ? 1L : 0L);
    curl_easy_setopt(curl.easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.easy, CURLOPT_CONNECTTIMEOUT, static_cast<long>(request.connectTimeoutSec));
    curl_easy_setopt(curl.easy, CURLOPT_TIMEOUT, static_cast<long>(request.timeoutSec));
    if (!request.userAgent.empty())
        curl_easy_setopt(curl.easy, CURLOPT_USERAGENT, request.userAgent.c_str());
    curl_easy_setopt(curl.easy, CURLOPT_WRITEFUNCTION, onWrite);
    curl_easy_setopt(curl.easy, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl.easy, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.easy, CURLOPT_XFERINFOFUNCTION, onProgress);
    curl_easy_setopt(curl.easy, CURLOPT_XFERINFODATA, &state);
    if (request.maxBytes > 0)
        curl_easy_setopt(curl.easy, CURLOPT_MAXFILESIZE_LARGE, state.maxBytes);
    if (headers.list) curl_easy_setopt(curl.easy, CURLOPT_HTTPHEADER, headers.list);
    if (!request.cookieFile.empty()) {
        const std::string cookie = pathUtf8(request.cookieFile);
        curl_easy_setopt(curl.easy, CURLOPT_COOKIEFILE, cookie.c_str());
        curl_easy_setopt(curl.easy, CURLOPT_COOKIEJAR, cookie.c_str());
    }
    if (request.noProxy) curl_easy_setopt(curl.easy, CURLOPT_NOPROXY, "*");
    if (request.ipv4Only) curl_easy_setopt(curl.easy, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    if (request.minTls12) curl_easy_setopt(curl.easy, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    if (const char* ca = std::getenv("CURL_CA_BUNDLE"); ca && *ca)
        curl_easy_setopt(curl.easy, CURLOPT_CAINFO, ca);
    else if (const char* ssl = std::getenv("SSL_CERT_FILE"); ssl && *ssl)
        curl_easy_setopt(curl.easy, CURLOPT_CAINFO, ssl);

    std::unique_ptr<Mime> mime;
    if (!request.form.empty()) {
        mime = std::make_unique<Mime>(curl.easy);
        if (!mime->mime) {
            result.error = "无法初始化表单上传";
            result.curlCode = CURLE_FAILED_INIT;
            return result;
        }
        for (const auto& part : request.form) {
            curl_mimepart* item = curl_mime_addpart(mime->mime);
            curl_mime_name(item, part.name.c_str());
            if (!part.file.empty()) {
                const std::string file = pathUtf8(part.file);
                curl_mime_filedata(item, file.c_str());
                if (!part.filename.empty()) curl_mime_filename(item, part.filename.c_str());
            } else {
                curl_mime_data(item, part.value.data(), part.value.size());
            }
            if (!part.contentType.empty()) curl_mime_type(item, part.contentType.c_str());
        }
        curl_easy_setopt(curl.easy, CURLOPT_MIMEPOST, mime->mime);
    }

    const std::string method = request.method.empty() ? "GET" : request.method;
    HeaderList recipients;
    if (smtp) {
        if (!request.mailFrom.empty())
            curl_easy_setopt(curl.easy, CURLOPT_MAIL_FROM, request.mailFrom.c_str());
        if (!request.mailRcpt.empty()) recipients.append(request.mailRcpt);
        if (recipients.list) curl_easy_setopt(curl.easy, CURLOPT_MAIL_RCPT, recipients.list);
        if (!request.username.empty())
            curl_easy_setopt(curl.easy, CURLOPT_USERNAME, request.username.c_str());
        if (!request.password.empty())
            curl_easy_setopt(curl.easy, CURLOPT_PASSWORD, request.password.c_str());
        if (request.smtpStartTls)
            curl_easy_setopt(curl.easy, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
        if (!request.uploadFile.empty()) {
            inFile.reset(openFile(request.uploadFile, false));
            if (!inFile) {
                result.error = "无法读取邮件正文";
                result.curlCode = CURLE_READ_ERROR;
                return result;
            }
            std::error_code ec;
            const auto size = fs::file_size(request.uploadFile, ec);
            curl_easy_setopt(curl.easy, CURLOPT_UPLOAD, 1L);
            curl_easy_setopt(curl.easy, CURLOPT_READDATA, inFile.get());
            if (!ec) curl_easy_setopt(curl.easy, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(size));
        }
    } else if (mime) {
        if (method != "POST" && method != "GET")
            curl_easy_setopt(curl.easy, CURLOPT_CUSTOMREQUEST, method.c_str());
    } else if (method == "POST") {
        curl_easy_setopt(curl.easy, CURLOPT_POST, 1L);
        curl_easy_setopt(curl.easy, CURLOPT_POSTFIELDS, request.body.c_str());
        curl_easy_setopt(curl.easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
    } else if (method != "GET") {
        curl_easy_setopt(curl.easy, CURLOPT_CUSTOMREQUEST, method.c_str());
        if (!request.body.empty()) {
            curl_easy_setopt(curl.easy, CURLOPT_POSTFIELDS, request.body.c_str());
            curl_easy_setopt(curl.easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
        }
    }

    const CURLcode code = curl_easy_perform(curl.easy);
    outFile.reset();
    inFile.reset();
    result.curlCode = static_cast<int>(code);
    long http = 0;
    curl_easy_getinfo(curl.easy, CURLINFO_RESPONSE_CODE, &http);
    result.http = static_cast<int>(http);
    result.cancelled = code == CURLE_ABORTED_BY_CALLBACK &&
        !(state.tooLarge) && !(sink.tooLarge) && request.cancelled && request.cancelled();
    if (code != CURLE_OK) {
        if (result.cancelled) result.error = "已取消";
        else if (state.tooLarge || sink.tooLarge) result.error = "下载超过大小限制";
        else result.error = curl_easy_strerror(code);
        return result;
    }
    if (!smtp && (result.http < 200 || result.http >= 300))
        result.error = "HTTP " + std::to_string(result.http);
    return result;
}

} // namespace dice::http
