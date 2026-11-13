#pragma once
#include <filesystem>
#include <openssl/rand.h>
#include <stdexcept>

namespace dice::ai {
// Atomically claim a private directory for each request, including across
// independent processes. Only the owner removes it; no shared numbered files.
class RequestFiles {
    std::filesystem::path root_;
public:
    RequestFiles() {
        for (int attempt=0; attempt<8; ++attempt) {
            unsigned char random[16];
            if (RAND_bytes(random,sizeof(random)) != 1) throw std::runtime_error("无法生成 AI 请求临时目录");
            std::string name="dnai-";
            for (auto v : random) { name += "0123456789abcdef"[v>>4]; name += "0123456789abcdef"[v&15]; }
            const auto candidate=std::filesystem::temp_directory_path()/name;
            std::error_code ec;
            if (!std::filesystem::create_directory(candidate,ec)) continue;
            root_=candidate;
            std::filesystem::permissions(root_,std::filesystem::perms::owner_all,std::filesystem::perm_options::replace,ec);
            if (ec) { std::filesystem::remove(root_,ec); root_.clear(); throw std::runtime_error("无法保护 AI 请求临时目录"); }
            return;
        }
        throw std::runtime_error("无法创建 AI 请求临时目录");
    }
    RequestFiles(const RequestFiles&)=delete;
    RequestFiles& operator=(const RequestFiles&)=delete;
    ~RequestFiles() { if (!root_.empty()) { std::error_code ec; std::filesystem::remove_all(root_,ec); } }
    std::filesystem::path file(const char* name) const { return root_/name; }
};
} // namespace dice::ai
