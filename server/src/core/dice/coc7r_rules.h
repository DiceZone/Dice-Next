#pragma once

// Keeper Companion (CHA23197-H, 2026), printed page references below.
// This is an opt-in profile, not a replacement for the CoC7 core rules.
// No global card aliases/defaults are registered here.
#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

namespace dice::coc7r {
inline constexpr const char* profile = "keeper-companion-2026";
// Introduction p.5: the revised rules are options to adopt one by one. Each
// module below is a separate per-group switch; the defaults follow the pack.
struct Option { const char* key; bool enabled; };
inline const std::array<Option, 7>& options() {
    static const std::array<Option, 7> list = {{
        {"creation", true}, {"skills", true}, {"movement", true}, {"sanity", true},
        {"bases", false}, {"specialties", false}, {"take5", false},
    }};
    return list;
}
inline bool isOption(const std::string& key) {
    for (const auto& o : options()) if (key == o.key) return true;
    return false;
}
inline bool optionDefault(const std::string& key) {
    for (const auto& o : options()) if (key == o.key) return o.enabled;
    return false;
}
using Lookup = std::function<std::optional<int>(const std::string&)>;
inline const std::map<std::string, int>& bases();

inline std::string canonical(std::string name, unsigned depth=0) {
    if (depth>8) return name;
    for (auto& c : name) if (static_cast<unsigned char>(c) < 128) c = static_cast<char>(std::tolower(c));
    static const std::map<std::string, std::string> aliases = {
        {"reassure", "安抚"}, {"安撫", "安抚"}, {"persuade", "说服"}, {"說服", "说服"},
        {"取悦", "说服"}, {"魅惑", "说服"}, {"话术", "说服"}, {"恐吓", "说服"},
        {"charm", "说服"}, {"fasttalk", "说服"}, {"intimidate", "说服"},
        {"lore", "神秘学"}, {"学识", "神秘学"}, {"survival", "生存"},
        {"mov", "移动"}, {"移动力", "移动"}, {"移動", "移动"}, {"age", "年龄"},
        {"dodge", "闪避"}, {"ownlanguage", "母语"}, {"链锯", "电锯"},
        {"机枪", "机关枪"}, {"rifle", "步枪/霰弹枪"}, {"shotgun", "步枪/霰弹枪"},
        {"handgun", "手枪"}, {"smg", "冲锋枪"}, {"brawl", "斗殴"},
        {"pilotaircraft", "飞行器驾驶"}, {"pilotboat", "船驾驶"},
        {"accounting","会计"},{"animalhandling","驯兽"},{"anthropology","人类学"},{"appraise","估价"},
        {"archaeology","考古学"},{"artillery","炮术"},{"climb","攀爬"},{"computeruse","计算机使用"},
        {"creditrating","信用评级"},{"cthulhumythos","克苏鲁神话"},{"demolitions","爆破"},{"disguise","乔装"},
        {"diving","潜水"},{"driveauto","驾驶"},{"electricalrepair","电气维修"},{"electronics","电子学"},
        {"firstaid","急救"},{"history","历史"},{"hypnosis","催眠"},{"jump","跳跃"},{"law","法律"},
        {"libraryuse","图书馆使用"},{"listen","聆听"},{"locksmith","锁匠"},{"mechanicalrepair","机械维修"},
        {"medicine","医学"},{"naturalworld","博物学"},{"navigate","导航"},{"occult","神秘学"},
        {"operateheavymachinery","操作重型机械"},{"psychoanalysis","精神分析"},{"psychology","心理学"},
        {"readlips","读唇"},{"sleightofhand","妙手"},{"spothidden","侦查"},{"stealth","潜行"},
        {"swim","游泳"},{"throw","投掷"},{"track","追踪"},{"axe","斧"},{"chainsaw","电锯"},
        {"flail","连枷"},{"garrote","绞索"},{"spear","矛"},{"sword","剑"},{"whip","鞭子"},
        {"bow","弓术"},{"flamethrower","火焰喷射器"},{"heavyweapons","重武器"},{"machinegun","机关枪"},
    };
    if (const auto it = aliases.find(name); it != aliases.end()) return it->second;
    auto open=name.find('('); size_t closeBytes=1;
    if (open==std::string::npos) { open=name.find("（"); closeBytes=3; }
    if (open!=std::string::npos && name.size()>open+closeBytes*2 &&
        name.substr(name.size()-closeBytes)==(closeBytes==1?")":"）")) {
        const auto category=name.substr(0,open);
        const auto specialty=name.substr(open+closeBytes,name.size()-open-closeBytes*2);
        if (category=="religion" || category=="宗教") return "宗教:"+specialty;
        if (category=="ride" || category=="骑乘") return "骑乘:"+specialty;
        if (category=="language" || category=="外语" || category=="语言") return "外语:"+specialty;
        if (category=="母语" || category=="ownlanguage") return "母语:"+specialty;
        if (category=="science" || category=="科学") return bases().count(canonical(specialty,depth+1))?canonical(specialty,depth+1):"科学:"+specialty;
        if (category=="art" || category=="art&craft" || category=="艺术" || category=="艺术与手艺") return bases().count(canonical(specialty,depth+1))?canonical(specialty,depth+1):"艺术:"+specialty;
        if (category=="fighting" || category=="firearms" || category=="格斗" || category=="射击") return canonical(specialty,depth+1);
    }
    // Specializations remain separate fields; category aliases don't combine
    // stored values. In particular SMG cannot be mapped without a weapon.
    for (const auto* prefix : {"格斗:", "格斗：", "射击:", "射击："})
        if (name.rfind(prefix, 0) == 0) return canonical(name.substr(std::string(prefix).size()),depth+1);
    for (const auto* prefix : {"科学:", "科学：", "艺术:", "艺术："})
        if (name.rfind(prefix,0)==0) {
            auto sub=canonical(name.substr(std::string(prefix).size()),depth+1);
            if (bases().count(sub)) return sub;
        }
    return name;
}

inline const std::vector<std::string>& fighting() {
    static const std::vector<std::string> names = {"斗殴", "斧", "电锯", "连枷", "绞索", "矛", "剑", "鞭子"};
    return names;
}
inline const std::vector<std::string>& firearms() {
    static const std::vector<std::string> names = {"弓术", "火焰喷射器", "手枪", "重武器", "机关枪", "步枪/霰弹枪"};
    return names;
}

// p.15,17: the complete revised defaults, independent of legacy rule packs.
inline const std::map<std::string, int>& bases() {
    static const std::map<std::string, int> values = {
        {"会计",5},{"驯兽",5},{"考古学",1},{"艺术与手艺",5},{"炮术",1},{"攀爬",20},
        {"计算机使用",5},{"信用评级",0},{"克苏鲁神话",0},{"爆破",1},{"潜水",1},
        {"驾驶",20},{"电气维修",10},{"电子学",1},{"急救",30},{"历史",5},{"催眠",1},
        {"跳跃",20},{"外语",1},{"法律",5},{"图书馆使用",20},{"聆听",20},{"锁匠",1},
        {"机械维修",10},{"医学",1},{"博物学",10},{"导航",10},{"神秘学",5},
        {"操作重型机械",1},{"说服",10},{"飞行器驾驶",1},{"船驾驶",1},{"精神分析",1},
        {"心理学",10},{"读唇",1},{"骑乘",5},{"科学",1},{"妙手",10},{"侦查",25},
        {"潜行",20},{"生存",10},{"游泳",20},{"投掷",20},{"追踪",10},
        {"斗殴",25},{"斧",15},{"电锯",15},{"连枷",15},{"绞索",15},{"矛",20},
        {"剑",20},{"鞭子",15},{"弓术",15},{"火焰喷射器",15},{"手枪",20},{"重武器",15},
        {"机关枪",15},{"步枪/霰弹枪",25},{"人类学",1},{"估价",5},{"乔装",5},
        {"表演",5},{"摄影",5},{"伪造",5},{"作画",5},{"文学",5},{"书法",5},{"乐理",5},
        {"厨艺",5},{"裁缝",5},{"理发",5},{"建筑",5},{"舞蹈",5},{"酿酒",5},{"歌唱",5},
        {"制陶",5},{"雕塑",5},{"杂技",5},{"技术制图",5},{"耕作",5},{"打字",5},{"速记",5},
        {"地质学",1},{"化学",1},{"生物学",1},{"数学",1},{"天文学",1},{"物理学",1},
        {"药学",1},{"植物学",1},{"动物学",1},{"密码学",1},{"工程学",1},{"气象学",1},{"司法科学",1},
    };
    return values;
}

inline bool isSkill(const std::string& name) {
    if (bases().count(name) || name=="安抚" || name=="闪避" || name=="母语") return true;
    for (const auto* prefix : {"宗教:","母语:","外语:","科学:","艺术:","骑乘:"}) if (name.rfind(prefix,0)==0) return true;
    return false;
}
inline constexpr const char* reassureFailureCost = "1d4"; // p.22, user-selected resolution of p.94 conflict.

// p.17: skills folded into one. Values merge only on an explicit request
// (.coc7r migrate); SMG has no target until the weapon is known.
inline std::string mergedSkill(const std::string& key) {
    static const std::map<std::string, std::string> merged = {
        {"取悦", "说服"}, {"魅惑", "说服"}, {"话术", "说服"}, {"快速交谈", "说服"}, {"恐吓", "说服"},
        {"学识", "神秘学"},
    };
    if (auto it = merged.find(key); it != merged.end()) return it->second;
    for (const auto* prefix : {"生存(", "生存（", "生存:", "生存："})
        if (key.rfind(prefix, 0) == 0 && key.size() > std::string(prefix).size()) return "生存";
    return {};
}

struct BaseFormula { std::string first, second; int divisor; };
inline const std::map<std::string, BaseFormula>& attributeBases() {
    // p.23–24. A nonempty second attribute means use the higher of the pair.
    static const std::map<std::string, BaseFormula> f = {
        {"会计",{"教育","",10}},{"表演",{"智力","意志",10}},{"驯兽",{"意志","",10}},
        {"估价",{"教育","",10}},{"斧",{"力量","",5}},{"电锯",{"力量","",5}},
        {"弓术",{"敏捷","",5}},{"斗殴",{"敏捷","力量",2}},{"攀爬",{"敏捷","力量",2}},
        {"计算机使用",{"教育","",10}},{"乔装",{"智力","",10}},{"驾驶",{"敏捷","",2}},
        {"电气维修",{"教育","",5}},{"作画",{"敏捷","",10}},{"急救",{"教育","",2}},
        {"连枷",{"敏捷","",5}},{"火焰喷射器",{"敏捷","",5}},{"绞索",{"敏捷","",5}},
        {"伪造",{"敏捷","教育",10}},{"手枪",{"敏捷","",2}},{"重武器",{"力量","",5}},
        {"机关枪",{"力量","",5}},{"历史",{"教育","",10}},{"法律",{"教育","",10}},
        {"神秘学",{"教育","",10}},{"跳跃",{"敏捷","",2}},{"图书馆使用",{"教育","",2}},
        {"聆听",{"智力","",2}},{"机械维修",{"智力","",5}},{"博物学",{"智力","",5}},
        {"导航",{"智力","",5}},{"说服",{"外貌","",5}},{"摄影",{"敏捷","教育",10}},
        {"心理学",{"智力","",5}},{"骑乘",{"敏捷","",10}},{"步枪/霰弹枪",{"敏捷","",2}},
        {"妙手",{"敏捷","",5}},{"矛",{"力量","",2}},{"侦查",{"智力","",2}},
        {"潜行",{"敏捷","",2}},{"剑",{"敏捷","",2}},{"游泳",{"敏捷","",2}},
        {"投掷",{"敏捷","",2}},{"生存",{"教育","",5}},{"追踪",{"智力","",5}},
        {"鞭子",{"敏捷","",5}},
    };
    return f;
}

inline std::optional<int> base(const std::string& name, const Lookup& lookup, bool byAttribute) {
    auto divide = [&](const std::string& key, int by) -> std::optional<int> {
        auto v = lookup(key); if (!v || *v < 0) return {}; return *v / by;
    };
    if (name == "安抚") return divide("外貌", 5);
    if (name == "闪避") return divide("敏捷", 2);
    if (name == "母语" || name.rfind("母语:",0)==0) return divide("教育", 1);
    if (name.rfind("骑乘:",0)==0) return byAttribute?divide("敏捷",10):std::optional<int>(5);
    if (name.rfind("科学:",0)==0) return 1;
    if (name.rfind("艺术:",0)==0) {
        // Known specializations use their own optional formula; unknown ones
        // retain the category base and require Keeper judgment.
        const auto sub=name.substr(std::string("艺术:").size());
        if (bases().count(sub)) return base(sub,lookup,byAttribute);
        return 5;
    }
    if (name.rfind("宗教:",0) == 0 || name.rfind("宗教：",0) == 0)
        return byAttribute ? divide("教育",5) : std::optional<int>(10);
    if (name.rfind("外语:",0) == 0 || name.rfind("语言:",0) == 0) return 1;
    if (byAttribute) if (auto it = attributeBases().find(name); it != attributeBases().end()) {
        auto v = lookup(it->second.first);
        if (!v || *v < 0) return {};
        if (!it->second.second.empty()) {
            auto other = lookup(it->second.second); if (!other || *other < 0) return {};
            *v = (std::max)(*v, *other);
        }
        return *v / it->second.divisor;
    }
    if (auto it = bases().find(name); it != bases().end()) return it->second;
    return {};
}

inline std::optional<int> value(const std::string& name, const Lookup& lookup, bool byAttribute, bool minimum) {
    auto result = lookup(name);
    if (!result) result = base(name, lookup, byAttribute);
    if (minimum) for (const auto* family : {&fighting(), &firearms()}) {
        if (std::find(family->begin(), family->end(), name) == family->end()) continue;
        int highest = 0;
        for (const auto& skill : *family) {
            auto v = lookup(skill); if (!v) v = base(skill, lookup, byAttribute);
            if (v) highest = (std::max)(highest, *v);
        }
        result = (std::max)(result.value_or(0), highest / 2);
    }
    return result;
}

// p.10. Age adjustments are unchanged from the core book (40/50/.../80).
inline int movement(int str, int dex, int siz, int age = 20) {
    const int reduction = age < 40 ? 0 : (std::min)(5, (age - 30) / 10);
    return (std::max)(0, 7 + (str > siz ? 1 : 0) + (dex > siz ? 1 : 0) - reduction);
}
inline const std::array<std::array<int,8>,10>& arrays() {
    static const std::array<std::array<int,8>,10> a = {{
        {{40,50,55,60,60,70,70,75}},{{35,50,50,60,60,75,75,75}},
        {{50,50,55,55,60,60,70,80}},{{40,45,50,50,70,70,75,80}},
        {{30,50,55,60,60,70,75,80}},{{45,45,50,50,60,70,80,80}},
        {{40,40,45,60,60,70,80,85}},{{40,40,50,50,50,80,85,85}},
        {{30,40,50,50,70,70,80,90}},{{30,35,45,50,70,75,85,90}},
    }};
    return a;
}

inline std::optional<int> number(std::string_view text, int max = 1000000) {
    if (text.empty()) return {};
    int value = 0; const auto [end, error] = std::from_chars(text.data(), text.data()+text.size(), value);
    if (error != std::errc{} || end != text.data()+text.size() || value < 0 || value > max) return {};
    return value;
}

// Bounds for SAN costs: sums/differences of integer constants and NdM terms.
// Reject unsupported syntax rather than randomly rolling a fumble's maximum.
struct Cost { int minimum = 0, maximum = 0; };
inline std::optional<Cost> cost(const std::string& expression) {
    if (expression.empty() || expression.size() > 128) return {};
    Cost out; size_t p = 0; int terms = 0;
    while (p < expression.size()) {
        if (++terms > 16) return {};
        int sign = 1;
        if (expression[p] == '+' || expression[p] == '-') { sign = expression[p++] == '-' ? -1 : 1; }
        const auto start = p;
        while (p < expression.size() && std::isdigit(static_cast<unsigned char>(expression[p]))) ++p;
        auto n = number(std::string_view(expression).substr(start,p-start));
        int lo, hi;
        if (p < expression.size() && (expression[p] == 'd' || expression[p] == 'D')) {
            if (p == start) n = 1;
            if (!n || *n < 1 || *n > 100) return {};
            const auto sidesStart = ++p;
            while (p < expression.size() && std::isdigit(static_cast<unsigned char>(expression[p]))) ++p;
            auto sides = number(std::string_view(expression).substr(sidesStart,p-sidesStart),10000);
            if (!sides || *sides < 1) return {};
            lo = *n; hi = *n * *sides;
        } else { if (!n) return {}; lo = hi = *n; }
        out.minimum += sign > 0 ? lo : -hi;
        out.maximum += sign > 0 ? hi : -lo;
        if (p < expression.size() && expression[p] != '+' && expression[p] != '-') return {};
        if (p + 1 == expression.size()) return {};
    }
    if (out.minimum < 0 || out.maximum > 1000000) return {};
    return out;
}

struct SanityState {
    int chapterLoss = 0;
    bool shock = false, aftershock = false, changed = false, reassured = false;
    bool realityProtected = false;
    int mythosPending = 0;
    int shockRounds = 0;
};
inline SanityState sanityState(const nlohmann::json& j) {
    SanityState s; if (!j.is_object()) return s;
    const auto loss = j.find("chapterLoss");
    if (loss != j.end() && loss->is_number_integer()) {
        const auto v = loss->get<int64_t>(); s.chapterLoss = static_cast<int>((std::clamp)(v,int64_t(0),int64_t(1000000)));
    }
    if (j.contains("shockRounds") && j["shockRounds"].is_number_integer())
        s.shockRounds = static_cast<int>((std::clamp)(j["shockRounds"].get<int64_t>(), int64_t(0), int64_t(10)));
    if (j.contains("mythosPending") && j["mythosPending"].is_number_integer())
        s.mythosPending = static_cast<int>((std::clamp)(j["mythosPending"].get<int64_t>(),int64_t(0),int64_t(1000000)));
    for (auto entry : {std::pair{"shock",&s.shock}, {"aftershock",&s.aftershock}, {"changed",&s.changed}, {"reassured",&s.reassured}, {"realityProtected",&s.realityProtected}})
        if (j.contains(entry.first) && j[entry.first].is_boolean()) *entry.second = j[entry.first].get<bool>();
    return s;
}
inline nlohmann::json jsonState(const SanityState& s) {
    return {{"chapterLoss",s.chapterLoss},{"shock",s.shock},{"aftershock",s.aftershock},{"changed",s.changed},{"reassured",s.reassured},
        {"shockRounds",s.shockRounds},{"realityProtected",s.realityProtected},{"mythosPending",s.mythosPending}};
}
struct SanityOutcome {
    SanityState state;
    int remaining = 0, loss = 0;
    bool intRequired = false, newShock = false, newChanged = false, lost = false;
};
inline SanityOutcome loseSanity(SanityState state, int san, int requestedLoss, int pow,
                                std::optional<bool> intSucceeded = {}) {
    SanityOutcome out; out.state = state; out.remaining = san;
    if (san <= 0) { out.remaining = 0; out.lost = true; return out; }
    if (state.shock) return out; // p.88: no additional SAN loss during Shock.
    out.loss = (std::clamp)(requestedLoss, 0, san);
    out.remaining -= out.loss;
    out.state.chapterLoss = (std::min)(1000000, state.chapterLoss + out.loss);
    if (out.loss > 0) out.state.realityProtected = false;
    // The Changed box is the characteristic's fifth value, rounded down like
    // other characteristic fractions. A zero loss never creates this state.
    if (!state.changed && out.state.chapterLoss > 0 && pow > 0 && out.state.chapterLoss >= pow / 5) {
        out.newChanged = true; out.state.changed = true;
    }
    if (out.remaining == 0) { out.lost = true; return out; }
    if (out.loss >= (state.changed ? 10 : 5)) {
        out.intRequired = true;
        if (intSucceeded.value_or(false)) {
            out.newShock = true; out.state.shock = true; out.state.aftershock = true;
        }
    }
    return out;
}

struct ChaseResult { int mvp, location; bool out; };
inline ChaseResult chase(int mvp, int location, bool success, int loss, bool failedPush = false) {
    const int next = failedPush ? 0 : success ? mvp : (std::max)(0, mvp - loss);
    return {next, location + (next > 0 ? 1 : 0), next <= 0};
}
} // namespace dice::coc7r
