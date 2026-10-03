// Credential-free local preview driver. Uses the production serializers.
#include "common/reply_preview.h"
#include <iostream>
#include <iterator>
#include <stdexcept>

int main() {
    using namespace dice;
    try {
        const std::string input{std::istreambuf_iterator<char>(std::cin), {}};
        if (input.size() > 131072) return 1;
        const auto request = outbound::Json::parse(input);
        const auto text = request.value("text", std::string());
        if (text.size() > 65536) return 1;
        const auto formatName = request.value("format", std::string("plain"));
        const auto style = request.value("style", std::string("visual"));
        if ((formatName != "plain" && formatName != "markdown") ||
            (style != "traditional" && style != "standard" && style != "visual"))
            throw std::invalid_argument("invalid preview options");
        const auto format = formatName == "markdown" ? ContentFormat::kMarkdown : ContentFormat::kPlainText;
        const auto preview = outbound::replyPreview(text, format, request.value("platform", std::string("qq_group")),
            presentationStyleFromString(style), request.value("forcePlain", false));
        std::cout << outbound::Json{{"code", 0}, {"data", {{"preview", preview}}}}.dump();
        return 0;
    } catch (...) {
        std::cout << R"({"code":1,"message":"preview request failed"})";
        return 1;
    }
}
