#pragma once

#include "../common/utils.h"
#include <charconv>
#include <chrono>
#include <stdexcept>

namespace dice::statistics {

struct DateRange {
    std::vector<std::string> dates;
    std::string sampleFrom, sampleUntil;
};

inline DateRange dateRange(const std::string& days, const std::string& period,
                           const std::string& from, const std::string& to,
                           std::chrono::system_clock::time_point now, int offsetMinutes) {
    using namespace std::chrono;
    const auto offset = minutes{offsetMinutes};
    const auto today = floor<std::chrono::days>(now + offset);
    auto first = today, last = today;
    auto iso = [](auto time) { return utils::timestampToIso8601(time).substr(0, 19); };
    auto parse = [&](const std::string& s) {
        if (s.size() != 10 || s < "1970-01-01" || s > iso(today).substr(0, 10))
            throw std::invalid_argument("Invalid statistics date");
        const auto day = floor<std::chrono::days>(system_clock::from_time_t(utils::parseIsoUtcToEpoch(s + "T00:00:00")));
        if (iso(day).substr(0, 10) != s) throw std::invalid_argument("Invalid statistics date");
        return day;
    };
    if (!from.empty() || !to.empty()) {
        first = parse(from);
        last = parse(to);
    } else if (period == "yesterday") {
        first = last = today - std::chrono::days{1};
    } else {
        int count = 30;
        if (!days.empty()) {
            const auto [end, err] = std::from_chars(days.data(), days.data() + days.size(), count);
            if (err != std::errc{} || end != days.data() + days.size() || count < 1 || count > 366)
                throw std::invalid_argument("Statistics range must be 1 to 366 days");
        }
        first = last - std::chrono::days{count - 1};
    }
    if (first > last || (last - first).count() >= 366)
        throw std::invalid_argument("Statistics range must be 1 to 366 days");
    DateRange range;
    for (auto day = first; day <= last; day += std::chrono::days{1})
        range.dates.push_back(iso(day).substr(0, 10));
    range.sampleFrom = iso(first - offset) + "Z";
    range.sampleUntil = iso(last + std::chrono::days{1} - offset) + "Z";
    return range;
}

} // namespace dice::statistics
