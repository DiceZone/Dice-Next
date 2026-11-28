#pragma once

// In-process HTTP(S)/SMTP via libcurl. Desktop still ships a curl CLI for
// packaging compatibility; every application download/API/mail path goes
// through this so Android (no curl binary) and desktop behave the same.

#include "subprocess.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace dice::http {

struct FormPart {
    std::string name;
    std::string value;
    std::filesystem::path file;
    std::string filename;
    std::string contentType;
};

struct Fetch {
    std::string url;
    std::string method = "GET";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::vector<FormPart> form;
    std::filesystem::path output;
    std::filesystem::path cookieFile;
    std::filesystem::path uploadFile;
    std::string userAgent = "DiceNext/3";
    std::string mailFrom;
    std::string mailRcpt;
    std::string username;
    std::string password;
    int connectTimeoutSec = 8;
    int timeoutSec = 20;
    std::uint64_t maxBytes = 0;
    bool allowHttp = false;
    bool noProxy = false;
    bool ipv4Only = false;
    bool followRedirects = true;
    bool minTls12 = true;
    bool smtpStartTls = false;
    dice::proc::CancellationCheck cancelled;
};

struct FetchResult {
    int http = 0;
    int curlCode = 0;
    bool cancelled = false;
    std::string error;
    std::string body;

    bool transportOk() const { return curlCode == 0 && !cancelled; }
    bool ok() const {
        return transportOk() && (http == 0 || (http >= 200 && http < 300));
    }
};

FetchResult fetch(const Fetch& request);
inline FetchResult fetchToFile(const Fetch& request) { return fetch(request); }

} // namespace dice::http
