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
inline bool officialMaterialOk(Kind kind) {
    return kind == Kind::jpeg || kind == Kind::png || kind == Kind::gif || kind == Kind::webp;
}
const char* kindName(Kind kind);

struct Prepared {
    std::filesystem::path path;
    Kind kind = Kind::unknown;
    bool transcoded = false;
};

/// 已是 png/jpeg 则原样返回；否则在 Windows 上用系统解码器转成 JPEG。
/// 读不到或转不成返回 nullopt（不要把 AVIF 原样交给 QQ）。
std::optional<Prepared> prepareForQq(const std::filesystem::path& source);
/// 素材管理还收 GIF/WebP。其余格式仍转 JPEG。
std::optional<Prepared> prepareForOfficialMaterial(const std::filesystem::path& source);

struct MaterialBytes {
    std::string bytes;
    std::string mime;
    std::string fileName;
    Kind kind = Kind::unknown;
    bool transcoded = false;
};

/// 官机图床：扩展名与真实格式对齐。已是 png/jpeg/gif/webp 时只改名；
/// 其余格式在 Windows 上转成无损 PNG。
std::optional<MaterialBytes> prepareMaterialBytes(std::string bytes, std::string fileName);

}  // namespace dice::image_transcode
