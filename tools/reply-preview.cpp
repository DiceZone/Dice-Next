// Credential-free local preview driver. Uses the production serializers.
#include "common/template_preview.h"
#include "common/logger.h"
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <filesystem>
#include <spdlog/sinks/null_sink.h>

// Standalone previews must neither mix logs into JSON stdout nor create a
// production data/logs directory in the caller's working directory.
namespace dice {
std::shared_ptr<spdlog::logger> getLogger() {
    static const auto logger = [] {
        auto value = std::make_shared<spdlog::logger>("preview", std::make_shared<spdlog::sinks::null_sink_mt>());
        value->set_level(spdlog::level::off);
        return value;
    }();
    return logger;
}
}

int main() {
    using namespace dice;
    try {
        const std::string input{std::istreambuf_iterator<char>(std::cin), {}};
        if (input.size() > 524288) return 1;
        const auto request = outbound::Json::parse(input);
        const auto source = (std::filesystem::path(__FILE__).parent_path().parent_path() / "server" / "i18n").u8string();
        I18n i18n(std::string(source.begin(), source.end()));
        if (!i18n.load()) return 1;
        if (request.contains("referenceOverrides")) {
            const auto& overrides = request["referenceOverrides"];
            if (!overrides.is_object() || overrides.size() > 256) return 1;
            for (const auto& [key, value] : overrides.items()) {
                if (!value.is_object() || !value.contains("value") || !value["value"].is_string()) return 1;
                i18n.setOverride(localeFromString(request.value("locale", std::string("zh-Hans"))), key,
                    value["value"].get<std::string>(), contentFormatFromString(value.value("format", std::string("plain"))));
            }
        }
        std::cout << outbound::Json{{"code", 0}, {"data", outbound::templatePreview(request, &i18n)}}.dump();
        return 0;
    } catch (...) {
        std::cout << R"({"code":1,"message":"preview request failed"})";
        return 1;
    }
}
