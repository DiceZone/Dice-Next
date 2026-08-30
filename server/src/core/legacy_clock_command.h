#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <string>

namespace dice::legacy_clock {

enum class Operation { kInvalid, kList, kAdd, kRemove };

struct Request {
    Operation operation = Operation::kInvalid;
    std::string name;
    std::string time;
    std::string command;
};

inline std::string trim(const std::string& input) {
    size_t begin = 0, end = input.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(input[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1]))) --end;
    return input.substr(begin, end - begin);
}

inline std::string lowerAscii(std::string input) {
    std::transform(input.begin(), input.end(), input.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return input;
}

inline Request parse(const std::string& input) {
    Request request;
    std::string text = trim(input);
    if (text.empty() || lowerAscii(text) == "list") {
        request.operation = Operation::kList;
        return request;
    }

    char sign = 0;
    if (text.front() == '+' || text.front() == '-') {
        sign = text.front();
        text = trim(text.substr(1));
    } else {
        std::istringstream words(text);
        std::string operation;
        words >> operation;
        const std::string lowered = lowerAscii(operation);
        if (lowered == "add") sign = '+';
        else if (lowered == "del" || lowered == "delete" || lowered == "remove") sign = '-';
        else return request;
        std::getline(words, text);
        text = trim(text);
    }

    if (sign == '-') {
        if (text.empty()) return request;
        request.operation = Operation::kRemove;
        request.name = text;
        return request;
    }

    // Original Dice! documentation commonly used compact syntax:
    //   .admin clock+task=5:00
    // The optional tail is a Dice!Next extension for a per-window plugin command:
    //   .admin clock+morning=7:30 .dailynews
    const size_t equals = text.find('=');
    if (equals != std::string::npos) {
        request.name = trim(text.substr(0, equals));
        text = trim(text.substr(equals + 1));
        std::istringstream tail(text);
        tail >> request.time;
        std::getline(tail, request.command);
        request.command = trim(request.command);
    } else {
        std::istringstream tail(text);
        tail >> request.name >> request.time;
        std::getline(tail, request.command);
        request.command = trim(request.command);
    }
    if (request.name.empty() || request.time.empty()) return Request{};
    request.operation = Operation::kAdd;
    return request;
}

inline std::optional<std::string> normalizeDailyTime(const std::string& value) {
    const size_t colon = value.find(':');
    if (colon == std::string::npos || colon < 1 || colon > 2 || value.size() - colon - 1 != 2)
        return std::nullopt;
    for (size_t i = 0; i < value.size(); ++i)
        if (i != colon && !std::isdigit(static_cast<unsigned char>(value[i])))
            return std::nullopt;
    int hour = 0;
    for (size_t i = 0; i < colon; ++i) hour = hour * 10 + value[i] - '0';
    const int minute = (value[colon + 1] - '0') * 10 + value[colon + 2] - '0';
    if (hour >= 24 || minute >= 60) return std::nullopt;
    return (hour < 10 ? "0" : "") + std::to_string(hour) + ":" + value.substr(colon + 1);
}

}  // namespace dice::legacy_clock