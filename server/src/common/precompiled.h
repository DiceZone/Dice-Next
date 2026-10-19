// 测试程序的预编译头：只收标准库与不依赖 Windows 头的大型第三方库。
// 不要加入 drogon 或 spdlog 的 sink：它们会提前引入 windows.h，使源文件里
// 先定义 NOMINMAX / WIN32_LEAN_AND_MEAN 再引入 windows.h 的写法失效。
// 也不要加入项目自己的头文件：项目头改动频繁，会让预编译头反复重建。
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <sqlite_orm/sqlite_orm.h>
