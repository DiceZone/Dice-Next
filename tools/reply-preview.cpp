// Credential-free local preview driver. Uses the production serializers.
#include "common/template_preview.h"
#include <iostream>
#include <iterator>
#include <stdexcept>

int main() {
    using namespace dice;
    try {
        const std::string input{std::istreambuf_iterator<char>(std::cin), {}};
        if (input.size() > 524288) return 1;
        const auto request = outbound::Json::parse(input);
        std::cout << outbound::Json{{"code", 0}, {"data", outbound::templatePreview(request)}}.dump();
        return 0;
    } catch (...) {
        std::cout << R"({"code":1,"message":"preview request failed"})";
        return 1;
    }
}
