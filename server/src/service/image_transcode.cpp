#include "image_transcode.h"
#include "image_resource_service.h"
#include "../common/logger.h"

#include <openssl/evp.h>

#include <fstream>
#include <vector>

#ifdef _WIN32
#include <objbase.h>
#include <wincodec.h>
#endif

namespace dice::image_transcode {
namespace fs = std::filesystem;

namespace {
std::string readAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string md5Hex(std::string_view bytes) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return {};
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    const bool ok = EVP_DigestInit_ex(ctx, EVP_md5(), nullptr) == 1
        && EVP_DigestUpdate(ctx, bytes.data(), bytes.size()) == 1
        && EVP_DigestFinal_ex(ctx, digest, &len) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok) return {};
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i) {
        out += kHex[digest[i] >> 4];
        out += kHex[digest[i] & 0xF];
    }
    return out;
}

fs::path cacheDir() {
    std::error_code ec;
    if (image_resources::current()) return image_resources::cacheDirectory("send");
    const auto dir = fs::temp_directory_path() / "dice-next-qq-send";
    fs::create_directories(dir, ec);
    return dir;
}

#ifdef _WIN32
struct ComInit {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool owned = SUCCEEDED(hr);
    ComInit() { if (hr == RPC_E_CHANGED_MODE) hr = S_OK; }
    ~ComInit() { if (owned) CoUninitialize(); }
    explicit operator bool() const { return SUCCEEDED(hr); }
};

template <class T>
struct ComPtr {
    T* p = nullptr;
    ComPtr() = default;
    ~ComPtr() { if (p) p->Release(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T** reset() { if (p) { p->Release(); p = nullptr; } return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

bool encodeJpegWic(std::string& bytes, const fs::path& dest) {
    ComInit com;
    if (!com || bytes.size() > 0xffffffffull) return false;
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    const fs::path tmp{dest.wstring() + L".tmp"};
    fs::remove(tmp, ec);
    bool ok = false;
    {
        ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(factory.reset()))))
            return false;
        ComPtr<IWICStream> input;
        if (FAILED(factory->CreateStream(input.reset()))) return false;
        if (FAILED(input->InitializeFromMemory(
                reinterpret_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()))))
            return false;
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(factory->CreateDecoderFromStream(input.p, nullptr,
                WICDecodeMetadataCacheOnDemand, decoder.reset())))
            return false;
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, frame.reset()))) return false;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(factory->CreateFormatConverter(converter.reset()))) return false;
        if (FAILED(converter->Initialize(frame.p, GUID_WICPixelFormat24bppBGR,
                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
            return false;
        ComPtr<IWICStream> output;
        if (FAILED(factory->CreateStream(output.reset()))) return false;
        if (FAILED(output->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE))) return false;
        ComPtr<IWICBitmapEncoder> encoder;
        if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, encoder.reset())))
            return false;
        if (FAILED(encoder->Initialize(output.p, WICBitmapEncoderNoCache))) return false;
        ComPtr<IWICBitmapFrameEncode> encoded;
        if (FAILED(encoder->CreateNewFrame(encoded.reset(), nullptr))) return false;
        if (FAILED(encoded->Initialize(nullptr))) return false;
        UINT width = 0, height = 0;
        if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0) return false;
        if (FAILED(encoded->SetSize(width, height))) return false;
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        if (FAILED(encoded->SetPixelFormat(&format))) return false;
        if (FAILED(encoded->WriteSource(converter.p, nullptr))) return false;
        ok = SUCCEEDED(encoded->Commit()) && SUCCEEDED(encoder->Commit());
    }
    if (!ok) { fs::remove(tmp, ec); return false; }
    fs::remove(dest, ec);
    fs::rename(tmp, dest, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return fs::is_regular_file(dest, ec) && !ec && fs::file_size(dest, ec) > 0 && !ec;
}
#endif
}  // namespace

Kind sniff(std::string_view b) {
    auto starts = [&](std::string_view s) {
        return b.size() >= s.size() && b.substr(0, s.size()) == s;
    };
    if (starts(std::string_view("\x89PNG\r\n\x1a\n", 8))) return Kind::png;
    if (starts(std::string_view("\xff\xd8\xff", 3))) return Kind::jpeg;
    if (starts("GIF87a") || starts("GIF89a")) return Kind::gif;
    if (b.size() >= 12 && b.substr(0, 4) == "RIFF" && b.substr(8, 4) == "WEBP") return Kind::webp;
    if (starts("BM")) return Kind::bmp;
    if (starts(std::string_view("\0\0\1\0", 4))) return Kind::ico;
    if (b.size() >= 12 && b.substr(4, 4) == "ftyp") {
        const auto brand = b.substr(8, 4);
        if (brand == "avif" || brand == "avis" || brand == "miaf") return Kind::avif;
        if (brand == "heic" || brand == "heif" || brand == "mif1" || brand == "msf1") return Kind::heic;
    }
    return Kind::unknown;
}

const char* kindName(Kind kind) {
    switch (kind) {
        case Kind::jpeg: return "JPEG";
        case Kind::png: return "PNG";
        case Kind::gif: return "GIF";
        case Kind::webp: return "WebP";
        case Kind::bmp: return "BMP";
        case Kind::avif: return "AVIF";
        case Kind::heic: return "HEIC";
        case Kind::ico: return "ICO";
        default: return "未知格式";
    }
}

std::optional<Prepared> prepareForQq(const fs::path& source) {
    std::error_code ec;
    if (!fs::is_regular_file(source, ec) || ec) return std::nullopt;
    auto bytes = readAll(source);
    if (bytes.empty()) return std::nullopt;
    const auto kind = sniff(bytes);
    if (qqImageOk(kind)) return Prepared{source, kind, false};
#ifdef _WIN32
    const auto hash = md5Hex(bytes);
    if (hash.empty()) return std::nullopt;
    const auto cached = cacheDir() / image_resources::fromUtf8(hash + ".jpg");
    if (fs::is_regular_file(cached, ec) && !ec && fs::file_size(cached, ec) > 0 && !ec) {
        DICE_LOG_INFO("本地图片「{}」为 {}，使用已转换的 JPEG 缓存再发送",
            image_resources::utf8(source), kindName(kind));
        return Prepared{cached, kind, true};
    }
    if (!encodeJpegWic(bytes, cached)) {
        DICE_LOG_WARN("本地图片「{}」为 {}，无法转成 JPEG；QQ 官方/内置登录发不出去",
            image_resources::utf8(source), kindName(kind));
        return std::nullopt;
    }
    DICE_LOG_INFO("本地图片「{}」为 {}，已转成 JPEG 再发送",
        image_resources::utf8(source), kindName(kind));
    return Prepared{cached, kind, true};
#else
    DICE_LOG_WARN("本地图片「{}」为 {}，当前平台无法转成 JPEG",
        image_resources::utf8(source), kindName(kind));
    return std::nullopt;
#endif
}

}  // namespace dice::image_transcode
