#pragma once
// 把本地图转成 QQ 官方 / OpenOPQ highway 收得下的 png/jpeg。
// 牌堆里常见把 AVIF/HEIF 存成 .jpg：官方会 850019「富媒体文件格式不支持」，
// 内置登录 highway 则 921。扩展名不可信，按文件头判断。

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace dice::image_transcode {

enum class Kind { jpeg, png, gif, webp, bmp, avif, heic, ico, unknown };

Kind sniff(std::string_view bytes);
inline bool qqImageOk(Kind kind) { return kind == Kind::jpeg || kind == Kind::png; }
const char* kindName(Kind kind);

struct Prepared {
    std::filesystem::path path;
    Kind kind = Kind::unknown;
    bool transcoded = false;
};

/// 已是 png/jpeg 则原样返回；否则在 Windows 上用系统解码器转成 JPEG。
/// 读不到或转不成返回 nullopt（不要把 AVIF 原样交给 QQ）。
std::optional<Prepared> prepareForQq(const std::filesystem::path& source);

}  // namespace dice::image_transcode
