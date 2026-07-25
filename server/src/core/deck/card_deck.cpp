#include "card_deck.h"
#include "../../common/logger.h"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <chrono>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace dice {

CardDeck::CardDeck() {
    std::random_device rd;
    rng_.seed(rd() ^ static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    seedBuiltins();
}

// A small built-in set (the original ships many more; users add JSON files).
// Caller holds mutex_ (or single-thread construction). Reload re-seeds these.
void CardDeck::seedBuiltins() {
    decks_[lower("数字")]     = {"0","1","2","3","4","5","6","7","8","9"};
    decks_[lower("大写字母")] = {"A","B","C","D","E","F","G","H","I","J","K","L","M",
                                 "N","O","P","Q","R","S","T","U","V","W","X","Y","Z"};
    decks_[lower("小写字母")] = {"a","b","c","d","e","f","g","h","i","j","k","l","m",
                                 "n","o","p","q","r","s","t","u","v","w","x","y","z"};
    decks_[lower("天干")]     = {"甲","乙","丙","丁","戊","己","庚","辛","壬","癸"};
    decks_[lower("地支")]     = {"子","丑","寅","卯","辰","巳","午","未","申","酉","戌","亥"};
    decks_[lower("八卦")]     = {"乾","坤","震","巽","坎","离","艮","兑"};
}

std::string CardDeck::lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return r;
}

const CardDeck::Deck* CardDeck::find(const std::string& name) const {
    auto it = decks_.find(lower(name));
    return it != decks_.end() ? &it->second : nullptr;
}

bool CardDeck::has(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mutex_);
    return find(name) != nullptr;
}

size_t CardDeck::deckCount() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return decks_.size();
}

std::vector<std::string> CardDeck::deckNames() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<std::string> names;
    names.reserve(decks_.size());
    for (const auto& [k, _] : decks_) names.push_back(k);
    std::sort(names.begin(), names.end());
    return names;
}

int CardDeck::loadDir(const std::string& dir) {
    std::lock_guard<std::mutex> lk(mutex_);
    return loadDirLocked(dir);
}

void CardDeck::reload(const std::vector<std::string>& dirs) {
    std::lock_guard<std::mutex> lk(mutex_);
    decks_.clear();
    sourceFiles_.clear();
    seedBuiltins();                       // 重新铺内置牌堆
    for (const auto& d : dirs) loadDirLocked(d);   // 重新扫描文件夹（新增/修改/删除都反映）
}

int CardDeck::loadDirLocked(const std::string& dir) {   // caller holds mutex_
    std::string d = dir;
    if (!fs::exists(d) && fs::exists("../" + dir)) d = "../" + dir;
    if (!fs::exists(d)) {
        DICE_LOG_INFO("CardDeck: no deck dir '{}' (skipping)", dir);
        return 0;
    }
    // 文件名转 UTF-8：Windows 上 path::string() 返回系统 ANSI 码页(中文系统=GBK)，
    // 直接进 JSON/日志会变方块问号；u8string() 才是 UTF-8（C#35 测试机 Win2016 复现）。
    auto u8name = [](const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); };
    int count = 0;
    for (const auto& entry : fs::directory_iterator(d)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
        try {
            std::ifstream in(entry.path());
            json j; in >> j;
            if (!j.is_object()) continue;
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (!it.value().is_array()) continue;
                Deck cards;
                for (const auto& c : it.value())
                    if (c.is_string()) cards.push_back(c.get<std::string>());
                if (!cards.empty()) {
                    auto key = lower(it.key());
                    decks_[key] = std::move(cards);
                    sourceFiles_[key] = u8name(entry.path().filename());
                    ++count;
                }
            }
            DICE_LOG_INFO("CardDeck: loaded decks from {}", u8name(entry.path().filename()));
        } catch (const std::exception& e) {
            DICE_LOG_WARN("CardDeck: failed to parse {}: {}", u8name(entry.path().filename()), e.what());
        }
    }
    return count;
}

std::string CardDeck::getSourceFile(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = sourceFiles_.find(lower(name));
    return (it != sourceFiles_.end()) ? it->second : "";
}

std::optional<std::string> CardDeck::drawFromDeck(const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    const Deck* d = find(name);
    if (!d) return std::nullopt;
    TempMap temp;
    Deck working = *d;   // a depleting copy for this draw
    return drawCard(working, /*back=*/false, temp, 0);
}

std::string CardDeck::drawCard(Deck& deck, bool back, TempMap& temp, int depth) {
    if (deck.empty() || depth > 50) return "";

    // Parse weights: "::N::card" → weight N; otherwise weight 1.
    std::vector<std::string> cards;
    std::vector<long long> cumulative;   // prefix sums of weights
    long long total = 0;
    std::vector<long long> weights;
    for (const auto& s : deck) {
        long long w = 1;
        std::string card = s;
        if (s.rfind("::", 0) == 0) {
            size_t r = s.find("::", 2);
            if (r != std::string::npos) {
                try { long long n = std::stoll(s.substr(2, r - 2)); if (n > 0) { w = n; card = s.substr(r + 2); } }
                catch (...) {}
            }
        }
        cards.push_back(card);
        weights.push_back(w);
        total += w;
        cumulative.push_back(total);
    }
    if (total <= 0) return "";

    std::uniform_int_distribution<long long> dist(0, total - 1);
    long long roll = dist(rng_);
    size_t idx = static_cast<size_t>(
        std::upper_bound(cumulative.begin(), cumulative.end(), roll) - cumulative.begin());
    if (idx >= cards.size()) idx = cards.size() - 1;

    std::string chosen = cards[idx];
    if (!back) {
        if (weights[idx] <= 1) deck.erase(deck.begin() + idx);
        else deck[idx] = "::" + std::to_string(weights[idx] - 1) + "::" + chosen;
    }
    return expand(chosen, temp, depth + 1);
}

// 「像骰子/算式」：含数字，且只由 0-9 d/D + - * / ( ) 空格 组成（k/h/l/x 等也放行）。
static bool looksDice(const std::string& s) {
    if (s.empty()) return false;
    bool hasDigit = false;
    for (unsigned char c : s) {
        if (std::isdigit(c)) { hasDigit = true; continue; }
        if (c=='d'||c=='D'||c=='+'||c=='-'||c=='*'||c=='/'||c=='('||c==')'||c==' '||
            c=='k'||c=='K'||c=='h'||c=='H'||c=='l'||c=='L'||c=='x'||c=='X') continue;
        return false;
    }
    return hasDigit;
}

std::string CardDeck::expand(std::string expr, TempMap& temp, int depth) {
    if (depth > 50) return expr;

    // {牌堆} / {%牌堆} references
    size_t pos = 0;
    while (true) {
        size_t lq = expr.find('{', pos);
        if (lq == std::string::npos) break;
        if (lq > 0 && expr[lq - 1] == '\\') { expr.erase(lq - 1, 1); pos = lq; continue; }
        size_t rq = expr.find('}', lq);
        if (rq == std::string::npos) break;

        std::string name = expr.substr(lq + 1, rq - lq - 1);
        bool back = false;
        if (!name.empty() && name[0] == '%') { back = true; name = name.substr(1); }

        const Deck* master = find(name);
        if (!master) {
            // 溯洄/OneDice 引用：{名} 不是牌堆 → 先试帮助词条(展开其内容)；再试 {%_1D3} 这类
            // 内联骰子/算式(去掉前导 % 和 _ 后求值)。都不行就原样保留。
            std::string res; bool handled = false;
            if (helpLookup_) if (auto h = helpLookup_(name)) { res = expand(*h, temp, depth + 1); handled = true; }
            if (!handled) {
                std::string dx = name;
                while (!dx.empty() && (dx.front() == '%' || dx.front() == '_')) dx.erase(dx.begin());
                if (diceEval_ && looksDice(dx)) { res = std::to_string(diceEval_(dx)); handled = true; }
            }
            if (handled) { expr.replace(lq, rq - lq + 1, res); pos = lq + res.size(); }
            else pos = rq + 1;
            continue;
        }

        std::string res;
        if (back) {
            Deck copy = *master;
            res = drawCard(copy, true, temp, depth);
        } else {
            std::string key = lower(name);
            auto it = temp.find(key);
            if (it == temp.end() || it->second.empty()) temp[key] = *master;
            res = drawCard(temp[key], false, temp, depth);
        }
        expr.replace(lq, rq - lq + 1, res);
        pos = lq + res.size();
    }

    // [骰子表达式]
    pos = 0;
    while (true) {
        size_t lb = expr.find('[', pos);
        if (lb == std::string::npos) break;
        if (lb > 0 && expr[lb - 1] == '\\') { expr.erase(lb - 1, 1); pos = lb; continue; }
        size_t rb = expr.find(']', lb);
        if (rb == std::string::npos) break;
        std::string inner = expr.substr(lb + 1, rb - lb - 1);
        // 图片/CQ 等媒体码（[图片:url]/[图:url]/[CQ:image,..]）含 ':' 或非 ASCII 字符，
        // 不是骰子表达式 → 保留原样交给适配器渲染（修复牌堆网络图片被求值成 "0" 的 bug）。
        bool isMedia = inner.find(':') != std::string::npos;
        if (!isMedia) for (unsigned char c : inner) if (c >= 0x80) { isMedia = true; break; }
        if (diceEval_ && !isMedia) {
            std::string val = std::to_string(diceEval_(inner));
            expr.replace(lb, rb - lb + 1, val);
            pos = lb + val.size();
        } else {
            pos = rb + 1;   // 非骰子表达式：保留方括号内容原样
        }
    }
    return expr;
}

}  // namespace dice
