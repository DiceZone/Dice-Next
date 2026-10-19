#include "card_deck.h"
#include "../../common/logger.h"
#include "../../common/message_break.h"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <vector>
#include <limits>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace dice {

CardDeck::CardDeck() {
    std::random_device rd;
    rng_.seed(rd() ^ static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    seedBuiltins();
}

// Minimal emergency fallbacks. Complete bundled decks are loaded from the
// release decks/ directory, followed by user overrides from data/decks/.
// Caller holds mutex_ (or single-thread construction). Reload re-seeds these.
void CardDeck::seedBuiltins() {
    (*decks_)[lower("数字")]     = {"0","1","2","3","4","5","6","7","8","9"};
    (*decks_)[lower("大写字母")] = {"A","B","C","D","E","F","G","H","I","J","K","L","M",
                                 "N","O","P","Q","R","S","T","U","V","W","X","Y","Z"};
    (*decks_)[lower("小写字母")] = {"a","b","c","d","e","f","g","h","i","j","k","l","m",
                                 "n","o","p","q","r","s","t","u","v","w","x","y","z"};
    (*decks_)[lower("天干")]     = {"甲","乙","丙","丁","戊","己","庚","辛","壬","癸"};
    (*decks_)[lower("地支")]     = {"子","丑","寅","卯","辰","巳","午","未","申","酉","戌","亥"};
    (*decks_)[lower("八卦")]     = {"乾","坤","震","巽","坎","离","艮","兑"};
}

std::string CardDeck::lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return r;
}

const CardDeck::Deck* CardDeck::find(const std::string& name) const {
    auto it = decks_->find(lower(name));
    return it != decks_->end() ? &it->second : nullptr;
}

bool CardDeck::has(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mutex_);
    return find(name) != nullptr;
}

std::optional<std::vector<std::string>> CardDeck::entries(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mutex_);
    const auto* deck = find(name);
    return deck ? std::optional<std::vector<std::string>>(*deck) : std::nullopt;
}

size_t CardDeck::deckCount() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return decks_->size();
}

std::vector<std::string> CardDeck::deckNames() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<std::string> names;
    names.reserve(decks_->size());
    for (const auto& [k, _] : *decks_) names.push_back(k);
    std::sort(names.begin(), names.end());
    return names;
}

static bool isBundledDeckDir(const std::string& dir) {
    const fs::path path = fs::path(std::u8string(dir.begin(), dir.end())).lexically_normal();
    auto text = [](const fs::path& part) {
        auto value = part.u8string();
        std::string result(value.begin(), value.end());
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return result;
    };
    return text(path.filename()) == "decks" && text(path.parent_path().filename()) != "data";
}

int CardDeck::loadDir(const std::string& dir) {
    std::lock_guard<std::mutex> lk(mutex_);
    return loadDirLocked(dir, isBundledDeckDir(dir));
}

void CardDeck::reload(const std::vector<std::string>& dirs) {
    std::lock_guard<std::mutex> lk(mutex_);
    decks_ = std::make_shared<DeckMap>();
    sourceFiles_.clear();
    seedBuiltins();                       // 重新铺内置牌堆
    for (const auto& d : dirs) loadDirLocked(d, isBundledDeckDir(d));
}

int CardDeck::loadDirLocked(const std::string& dir, bool bundled) {   // caller holds mutex_
    if (decks_.use_count() != 1) decks_ = std::make_shared<DeckMap>(*decks_);
    std::string d = dir;
    if (!fs::exists(d) && fs::exists("../" + dir)) d = "../" + dir;
    if (!fs::exists(d)) {
        DICE_LOG_DEBUG("牌堆目录「{}」不存在，本次跳过该目录。", dir);
        return 0;
    }
    // 文件名转 UTF-8：Windows 上 path::string() 返回系统 ANSI 码页(中文系统=GBK)，
    // 直接进 JSON/日志会变方块问号；u8string() 才是 UTF-8（测试机 Win2016 复现）。
    auto u8name = [](const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); };
    std::vector<fs::directory_entry> entries;
    for (const auto& entry : fs::directory_iterator(d)) entries.push_back(entry);
    std::sort(entries.begin(), entries.end(), [&](const auto& lhs, const auto& rhs) {
        return lower(u8name(lhs.path().filename())) < lower(u8name(rhs.path().filename()));
    });
    int count = 0;
    for (const auto& entry : entries) {
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
                    (*decks_)[key] = std::move(cards);
                    sourceFiles_[key] = {u8name(entry.path().filename()), bundled};
                    ++count;
                }
            }
            DICE_LOG_DEBUG("已读取牌堆文件「{}」。", u8name(entry.path().filename()));
        } catch (const std::exception& e) {
            DICE_LOG_WARN("牌堆文件「{}」解析失败，已跳过：{}", u8name(entry.path().filename()), e.what());
        }
    }
    return count;
}

std::string CardDeck::getSourceFile(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = sourceFiles_.find(lower(name));
    return (it != sourceFiles_.end()) ? it->second.filename : "";
}

std::optional<CardDeck::SourceInfo> CardDeck::getSourceInfo(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = sourceFiles_.find(lower(name));
    if (it == sourceFiles_.end()) return std::nullopt;
    return it->second;
}

std::optional<std::string> CardDeck::drawFromDeck(const std::string& name) {
    ReferenceContext context; prepare(context);
    const auto it = context.catalog->find(lower(name));
    if (it == context.catalog->end()) return std::nullopt;
    Deck working = it->second;
    const auto text = drawCard(working, false, context, 0);
    return safe_template::materialize(text, {}, context.budget);
}

std::string CardDeck::expandReference(const std::string& token, ReferenceContext& context) {
    prepare(context);
    return safe_template::materialize(expand("{" + token + "}", context, 0), {}, context.budget);
}

void CardDeck::prepare(ReferenceContext& context) const {
    if (context.catalog) return;
    { std::lock_guard<std::mutex> lock(mutex_); context.catalog = decks_; }
    context.budget.read = safe_template::contextRead;
    context.budget.length = safe_template::contextLength;
    context.budget.delay = safe_template::contextDelay;
}

std::string CardDeck::expandText(const std::string& text) {
    ReferenceContext context; prepare(context);
    return safe_template::materialize(expand(text, context, 0), {}, context.budget);
}

std::string CardDeck::drawCard(Deck& deck, bool back, ReferenceContext& context, unsigned depth) {
    if (deck.empty() || depth >= 32 || !context.budget.nodes || !context.budget.bytes) return "";

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
        if (w > (std::numeric_limits<long long>::max)() - total) return "";
        total += w;
        cumulative.push_back(total);
    }
    if (total <= 0) return "";

    std::uniform_int_distribution<long long> dist(0, total - 1);
    long long roll;
    { std::lock_guard<std::mutex> lock(mutex_); roll = dist(rng_); }
    size_t idx = static_cast<size_t>(
        std::upper_bound(cumulative.begin(), cumulative.end(), roll) - cumulative.begin());
    if (idx >= cards.size()) idx = cards.size() - 1;

    std::string chosen = cards[idx];
    if (!back) {
        if (weights[idx] <= 1) deck.erase(deck.begin() + idx);
        else deck[idx] = "::" + std::to_string(weights[idx] - 1) + "::" + chosen;
    }
    return expand(chosen, context, depth + 1);
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

// Convert only authored square-bracket dice blocks into an internal reference.
// This happens BEFORE interpolation; a nickname containing [1d100] is just data.
static std::string deckDiceSyntax(const std::string& text) {
    std::string out;
    for (size_t pos = 0; pos < text.size();) {
        if (text[pos] != '[' || sample_template::escaped(text, pos)) {
            out += text[pos++]; continue;
        }
        const auto end = text.find(']', pos + 1);
        if (end == std::string::npos) { out += text.substr(pos); break; }
        const auto inner = text.substr(pos + 1, end - pos - 1);
        size_t braces = 0; bool media = false;
        for (size_t i = 0; i < inner.size(); ++i) {
            if (sample_template::escaped(inner, i)) continue;
            if (inner[i] == '{') ++braces;
            else if (inner[i] == '}' && braces) --braces;
            else if (!braces && (inner[i] == ':' || static_cast<unsigned char>(inner[i]) >= 128)) media = true;
        }
        out += media ? text.substr(pos, end - pos + 1) : "{" + std::string("\x1e") + "deck-roll:" + inner + "}";
        pos = end + 1;
    }
    return out;
}

std::string CardDeck::expand(const std::string& expr, ReferenceContext& context, unsigned depth) {
    auto& budget = context.budget;
    if (depth >= 32 || expr.size() > 4 * 1024 * 1024 || !budget.nodes || !budget.bytes) return "";
    auto program = safe_template::compile(deckDiceSyntax(expr));
    const auto deckName = [](std::string name) {
        if (!name.empty() && name.front() == '%') name.erase(0, 1);
        return lower(name);
    };
    const auto references = [&](auto&& self, std::vector<safe_template::Node>& nodes) -> void {
        for (auto& node : nodes) {
            if (node.kind != safe_template::Kind::literal && node.text.size() >= 2) {
                const auto authored = node.text.substr(1, node.text.size() - 2);
                // Keep existing decks named like variables/control macros usable.
                if (context.catalog->contains(deckName(authored))) {
                    node.kind = safe_template::Kind::reference; node.field = authored; node.branches.clear();
                } else if (node.kind == safe_template::Kind::reference) node.field = "text:" + node.field;
            }
            for (auto& branch : node.branches) self(self, branch);
        }
    };
    references(references, program.nodes);
    const safe_template::Resolve resolve = [&](const std::string& token, unsigned nested, safe_template::Budget& remaining) -> std::optional<std::string> {
        if (token.rfind(std::string("\x1e") + "deck-roll:", 0) == 0) {
            if (!diceEval_) return "[" + token.substr(11) + "]";
            const auto expression = safe_template::materialize(expand(token.substr(11), context, nested), {}, remaining);
            if (!remaining.nodes || !remaining.bytes) return "";
            return safe_template::hold(std::to_string(diceEval_(expression)), remaining);
        }
        const bool back = !token.empty() && token.front() == '%';
        const auto name = back ? token.substr(1) : token;
        if (const auto found = context.catalog->find(lower(name)); found != context.catalog->end()) {
            if (back) { Deck copy = found->second; return drawCard(copy, true, context, nested); }
            auto& cards = context.decks[lower(name)];
            if (cards.empty()) cards = found->second;
            return drawCard(cards, false, context, nested);
        }
        if (helpLookup_) if (const auto help = helpLookup_(name)) return expand(*help, context, nested);
        auto dice = name;
        while (!dice.empty() && (dice.front() == '%' || dice.front() == '_')) dice.erase(0, 1);
        if (diceEval_ && looksDice(dice)) return safe_template::hold(std::to_string(diceEval_(dice)), remaining);
        return std::nullopt;
    };
    const safe_template::Literal literal = [](std::string_view text) {
        std::string out;
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\\' && i + 1 < text.size() && (text[i + 1] == '[' || text[i + 1] == '{')) ++i;
            out += text[i];
        }
        return out;
    };
    return safe_template::expand(program.nodes, {}, resolve, depth, budget, literal, resolve);
}

}  // namespace dice
