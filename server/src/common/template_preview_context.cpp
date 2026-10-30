#include "template_preview_context.h"
#include "../core/dice/dice_expression.h"
#include "../core/dice/dice_result.h"
#include "check_reply.h"
#include "check_rules.h"
#include "markdown.h"
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

namespace dice::outbound {
namespace {
bool starts(const std::string &value, const std::string &prefix) { return value.rfind(prefix, 0) == 0; }

struct Example {
    PreviewContext context;
    std::mt19937 random;
    const I18n *i18n;
    Locale locale;
    bool chinese;

    Example(std::uint32_t seed, const I18n *translations, Locale loc)
        : random(seed), i18n(translations), locale(loc), chinese(loc == Locale::kZhHans || loc == Locale::kZhHant) {
        context.seed = seed;
    }
    int die(int sides) { return 1 + static_cast<int>(random() % static_cast<unsigned>(sides)); }
    int between(int low, int high) { return low + die(high - low + 1) - 1; }
    std::string word(const char *zh, const char *en) const { return chinese ? zh : en; }
    std::string tr(const std::string &key, const I18n::Args &args = {}) const {
        const auto fragment = I18n::captureFragment([&] {
            return i18n ? i18n->tr(locale, key, args) : I18n::interpolate(key, args);
        });
        // Only this trusted, computed fragment is Markdown. Values such as a
        // player's name remain literal arguments, never template source. This
        // also covers styled grade labels, receipts and generated totals.
        return fragment.format == ContentFormat::kMarkdown ? markdown::toPlainText(fragment.text)
                                                           : fragment.text;
    }
    void number(const std::string &key, int value) { context.args[key] = std::to_string(value); }
    DiceResult roll(const std::string &expression) {
        DiceExpression engine([this](int sides) { return die(sides); });
        auto result = engine.evaluate(expression);
        if (!result.ok())
            throw std::logic_error("invalid internal preview expression");
        return result;
    }
    void result(const DiceResult &value) {
        auto &a = context.args;
        a["expr"] = value.expression;
        a["res"] = a["detail"] = value.formattedOutput;
        number("result", value.modifiedTotal);
        number("total", value.modifiedTotal);
        a["roll"] = value.formattedOutput;
    }
    std::string label(const std::string &grade, bool brp = false) const {
        if (grade == "tie")
            return tr("dice.rav.tie");
        if (brp)
            return tr("dice.brp." + (grade == "regular" ? std::string("success") : grade));
        if (grade == "special")
            return tr("dice.brp.special");
        return tr("dice.level." + grade);
    }
    void outcome(const std::string &family, const std::string &grade) {
        context.family = family;
        context.grade = grade;
        context.args["level"] = context.args["outcome"] = label(grade, family == "brp");
    }
    int percentile(const std::string &wanted, bool brp = false, int rate = 60) {
        std::vector<int> candidates;
        for (int value = 1; value <= 100; ++value)
            if (wanted.empty() ||
                wanted == check_rules::grade(brp ? check_rules::brp(value, rate) : check_rules::coc(value, rate), brp))
                candidates.push_back(value);
        if (candidates.empty())
            throw std::invalid_argument("unsupported preview outcome");
        return candidates[static_cast<size_t>(die(static_cast<int>(candidates.size())) - 1)];
    }
    void check(const std::string &family, const std::string &wanted) {
        const bool brp = family == "brp" || wanted == "special";
        const int value = percentile(wanted, brp);
        const auto grade = check_rules::grade(brp ? check_rules::brp(value, 60) : check_rules::coc(value, 60), brp);
        auto &a = context.args;
        outcome(family, grade);
        number("rate", 60);
        number("result", value);
        number("total", value);
        a["expr"] = "1d100";
        a["roll"] = a["res"] = a["detail"] = "1D100=" + std::to_string(value);
        if (family == "bonus" || family == "penalty") {
            // Build possible tens dice conditioned on the selected result. This
            // mirrors a single bonus/penalty die, including 00 => 100.
            const int units = value % 10, tens = value == 100 ? 0 : value / 10;
            std::vector<int> alternatives;
            for (int other = 0; other < 10; ++other) {
                const int alternative = other * 10 + units == 0 ? 100 : other * 10 + units;
                if ((family == "bonus" && alternative >= value) || (family == "penalty" && alternative <= value))
                    alternatives.push_back(other);
            }
            const int other = alternatives[static_cast<size_t>(die(static_cast<int>(alternatives.size())) - 1)];
            context.facts["tens"] = json::array({tens, other});
            context.facts["units"] = units;
            a["expr"] = family == "bonus" ? "b" : "p";
            a["roll"] = a["res"] = a["detail"] = a["expr"] + "={" + std::to_string(tens * 10) + "," +
                                                 std::to_string(other * 10) + "}+" + std::to_string(units) + "=" +
                                                 std::to_string(value);
        }
        context.command = std::string(family == "bonus"     ? ".rab "
                                      : family == "penalty" ? ".rap "
                                                            : ".ra ") +
                          a["attr"] + " 60";
        if (family == "brp")
            context.command = ".ba " + a["attr"] + " 60";
        context.facts["rule"] = 0;
    }
    void fixtures() {
        // Synthetic, deliberately non-sensitive state for non-roll operations.
        // These are examples, not reads of real players, credentials or files.
        auto &a = context.args;
        a = {{"nick", word("测试玩家", "TestPlayer")},
             {"name", word("调查员", "Investigator")},
             {"qqnick", word("测试玩家", "TestPlayer")},
             {"card", word("调查员", "Investigator")},
             {"pcname", word("调查员", "Investigator")},
             {"self", "Dice!Next"},
             {"user", "10001"},
             {"uid", "10001"},
             {"group", "100000"},
             {"gid", "100000"},
             {"selfId", "90001"},
             {"attr", word("侦查", "Spot Hidden")},
             {"reason", word("寻找线索", "Looking for clues")},
             {"target", word("另一位调查员", "Another investigator")},
             {"agent", word("代掷玩家", "ProxyPlayer")},
             {"old", word("旧称呼", "OldName")},
             {"new", word("新称呼", "NewName")},
             {"owner", "10001"},
             {"source", word("全局", "Global")},
             {"destination", word("团务副本", "Session copy")},
             {"key", "HP"},
             {"val", "12"},
             {"para", "COC7"},
             {"id", "1001"},
             {"rev", "2"},
             {"version", "1.0.0"},
             {"author", word("示例作者", "ExampleAuthor")},
             {"brief", word("这是示例扩展。", "An example extension.")},
             {"error", word("示例：连接超时，请检查网络。", "Example: connection timed out. Check the network.")},
             {"url", "https://example.com/preview"},
             {"home", "https://example.com/"},
             {"code", "DEMO-1234"},
             {"official", "OPENID_EXAMPLE"},
             {"qq", "10001"},
             {"seconds", "600"},
             {"time", "04:00:00"},
             {"count", "2"},
             {"total", "2"},
             {"page", "1"},
             {"current", "1"},
             {"running", "1"},
             {"more", ""},
             {"max", "10"},
             {"min", "1"},
             {"n", "11"},
             {"cur", "2"},
             {"ring", "1"},
             {"turn", "1"},
             {"rule", "0"},
             {"lang", "zh-Hans"},
             {"sides", "100"},
             {"default", "100"},
             {"active", "COC7"},
             {"hp", "12"},
             {"san", "60"},
             {"loss", "0"},
             {"final", "60"},
             {"change", "0"},
             {"s", "0"},
             {"f", "0"},
             {"db", "1D4"},
             {"base", "3"},
             {"mod", "+3"},
             {"threshold", "15"},
             // coc7r（Keeper Companion）理智、移动、自动射击与追逐
             {"value", "60"},
             {"gain", "2"},
             {"rounds", "4"},
             {"minutes", "3"},
             {"values", "60 / 45 / 70 / 55 / 50 / 65 / 40 / 75"},
             {"mov", "8"},
             {"action", "16"},
             {"damage", "45"},
             {"armor", "10"},
             {"net", "35"},
             {"width", "5"},
             {"mvp", "5"},
             {"location", "2"},
             {"out", word("否", "No")},
             {"bases", "off"},
             {"specialties", "off"},
             {"take5", "off"},
             {"shock", word("否", "No")},
             {"aftershock", word("否", "No")},
             {"changed", word("否", "No")},
             {"reassured", word("否", "No")},
             {"reality", word("否", "No")},
             {"attrs", "HP/SAN/MP"},
             {"力量", "60"},
             {"敏捷", "55"},
             {"意志", "60"},
             {"理智", "60"},
             {"生命值", "12"},
             {"role", word("管理员", "Administrator")},
             {"trust", "3"},
             {"user.trust", "3"},
             {"favor", "10"},
             {"user.favor", "10"},
             {"add", "+1"},
             {"title", word("友善", "Friendly")},
             {"state", word("已启用", "Enabled")},
             {"status", word("进行中", "Running")},
             {"feature", word("掷骰", "Dice rolls")},
             {"blacklist", "0"},
             {"mute", "0"},
             {"kick", "0"},
             {"log", "1"},
             {"kw", word("示例关键词", "Example keyword")},
             {"quitLevel", "3"},
             {"term", "30"},
             {"pass", "6"},
             {"gm", word("守秘人", "Keeper")},
             {"players", "2"},
             {"observers", "1"},
             {"game.pls", "2"},
             {"game.gms", "1"},
             {"type", word("跑团日志", "Session log")},
             {"from", "2026-10-01"},
             {"to", "2026-10-21"},
             {"tops", ""},
             {"tid", "1001"},
             {"game_id", "demo"},
             {"face", "100"},
             {"set_item", "rounds"},
             {"set_val", "3"},
             {"sub", "1"},
             {"mode", "COC7"},
             {"at", "[CQ:at,qq=10001]"},
             {"topic", word("自定义帮助", "Custom help")},
             {"entry", word("示例帮助条目", "Example help entry")},
             {"scope", word("全局", "Global")},
             {"src", word("原人格", "Source persona")},
             {"dst", word("新人格", "New persona")},
             {"book", "COC7"},
             {"item", word("急救", "First Aid")},
             {"desc", word("恢复 1 点生命值。", "Recover 1 HP.")},
             {"cmd", ".r"},
             {"command", ".ra 60"},
             {"task", ".r 1d100"},
             {"alias", word("小号", "Alt")},
             {"main", "10001"},
             {"shortcut", word("侦查", "spot")},
             {"cmds", "128"},
             {"cores", "4"},
             {"cpu", "8%"},
             {"memload", "25%"},
             {"memtotal", "8 GiB"},
             {"memused", "2 GiB"},
             {"os", "Linux"},
             {"proc", "128 MiB"},
             {"friends", "2"},
             {"groups", "1"},
             {"logs", "1"},
             {"uptime", word("2 天 3 小时", "2 days 3 hours")},
             {"replies", "2"},
             {"rules", "1"},
             {"scripts", "0"},
             {"events", "1"},
             {"cards", "2"},
             {"commands", "128"},
             {"created", "2026-10-01 12:00:00"},
             {"last", "2026-10-21 20:00:00"},
             {"mapped", "dice.roll.result"},
             {"day", "30"},
             {"text", word("欢迎来到本群！", "Welcome to the group!")},
             {"sec", "30"},
             {"a", word("主动方", "Active side")},
             {"b", word("被动方", "Opposing side")},
             {"get", word("调查旧宅", "Investigate the old house")},
             {"left", "77"},
             {"crit", "1"},
             {"extreme", "2"},
             {"hard", "3"},
             {"regular", "4"},
             {"fail", "5"},
             {"fumble", "1"},
             {"success", "10"},
             {"skill", word("侦查", "Spot Hidden")}};
        a["command"] = ".ra " + a["attr"] + " 60";
        for (const auto *key : {"nick", "qqnick", "card", "pcname"})
            a[std::string(key) + "w"] = "<" + a[key] + ">";
        std::time_t now = std::time(nullptr);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        std::ostringstream date, time;
        date << std::put_time(&local, "%Y-%m-%d");
        time << std::put_time(&local, "%H:%M:%S");
        a["date"] = date.str();
        a["time"] = time.str();
        a["lang"] = localeToString(locale);
        a["list"] = a["items"] = "1. " + word("调查员", "Investigator") + "\n2. " + word("守秘人", "Keeper");
        a["body"] = a["detail"] = a["output"] = "HP:12/12 SAN:60 MP:12/12";
        a["hist"] = "1. " + a["nick"];
        a["diff"] = "HP: 12 | 11 | 12 (local)";
        a["causes"] = word("称号规则已匹配", "Title rule matched");
        a["list_all_deck"] = a["list_extern_deck"] = a["list_reply_deck"] = word("塔罗牌", "Tarot");
        a["list_extern_order"] = word("示例牌堆", "Example deck");
        a["list_dice_sister"] = "Dice!Next";
        result(roll("1d100"));
        number("rate", 60);
        const auto grade = check_rules::grade(check_rules::coc(std::stoi(a["result"]), 60));
        a["level"] = a["outcome"] = label(grade);
        a["grade"] = a["level"];
    }
};

std::string familyFor(const std::string &key) {
    if (const auto *family = check_reply::familyForKey(key))
        return family->name;
    if (starts(key, "card.sc.") || starts(key, "dice.sc.") || starts(key, "dice.compat.sanity."))
        return "sanity";
    if (starts(key, "card.en.") || starts(key, "dice.compat.growth."))
        return "growth";
    if (starts(key, "dice.brp.resist"))
        return "resist";
    if (starts(key, "dice.brp."))
        return "brp";
    if (starts(key, "dice.rav."))
        return "opposed";
    if (starts(key, "dice.rx."))
        return "psychology";
    if (starts(key, "dnd.ds."))
        return "death_save";
    if (starts(key, "dnd.rdc.") || starts(key, "dnd.rc."))
        return "dnd_check";
    if (starts(key, "dice.check.") || starts(key, "dice.st.") || starts(key, "dice.compat.check.") ||
        starts(key, "dice.level."))
        return "standard";
    return "";
}
} // namespace

PreviewContext templatePreviewContext(const json &request, const I18n *i18n) {
    if (!request.is_object() || request.value("version", 0) != 1)
        throw std::invalid_argument("unsupported preview simulation");
    const auto key = request.value("key", std::string());
    const auto sourceKey = request.value("sourceKey", key);
    if (key.empty() || key.size() > 256 || sourceKey.size() > 256)
        throw std::invalid_argument("invalid preview key");
    std::uint32_t seed;
    if (request.contains("seed")) {
        if (!request["seed"].is_number_integer() || request["seed"].get<int64_t>() < 0 ||
            request["seed"].get<uint64_t>() > UINT32_MAX)
            throw std::invalid_argument("invalid preview seed");
        seed = request["seed"].get<std::uint32_t>();
    } else
        seed = std::random_device{}();
    sample_template::RandomScope randomScope(seed ^ 0x9e3779b9u);
    Example ex(seed, i18n, localeFromString(request.value("locale", std::string("zh-Hans"))));
    ex.fixtures();
    auto &c = ex.context;
    auto &a = c.args;
    for (const auto &[prefix, command] : std::vector<std::pair<std::string, std::string>>{
             {"pc.", ".pc list"},
             {"card.", ".st show"},
             {"log.", ".log list"},
             {"group.", ".group show"},
             {"admin.", ".admin list"},
             {"master.", ".master"},
             {"plugin.", ".plugin list"},
             {"mod.", ".mod list"},
             {"cloud_card.", ".cloud list"},
             {"identity_email.", ".bind qq 10001"},
             {"bind.", ".bind"},
             {"info.", ".info"},
             {"bot.", ".bot"},
             {"ob.", ".ob list"},
             {"link.", ".link list"},
             {"ak.", ".ak"},
             {"help", ".help"},
             {"favor.", ".favor"},
             {"notice.", ".notice"},
             {"persona.", ".rpmode list"},
             {"lang.", ".lang"},
             {"fun.set.", ".set 100"},
             {"fun.nn.", ".nn " + a["new"]},
             {"fun.sleep", ".sleep"},
             {"welcome.", ".welcome"},
             {"rule.", ".rules COC7"},
             {"buff.", ".buff"},
             {"system.", ".system"},
             {"user.", ".user"},
             {"event.", ex.word("事件通知（虚拟示例）", "Event notice (fictional example)")}})
        if (starts(key, prefix)) {
            c.command = command;
            break;
        }
    auto family = familyFor(key);
    std::string wanted;
    if (check_reply::familyForKey(key))
        wanted = check_reply::gradeForKey(key);
    if (request.contains("outcome")) {
        const auto &outcome = request["outcome"];
        if (!outcome.is_object())
            throw std::invalid_argument("invalid preview outcome");
        const auto name = outcome.value("family", std::string()), grade = outcome.value("grade", std::string());
        if (!check_reply::familyForKey(check_reply::key(name, grade)))
            throw std::invalid_argument("invalid preview outcome");
        family = name;
        wanted = grade;
    }
    // Editing an original result slot must exercise that slot's branch, rather
    // than randomly showing a success for a failure-only or no-loss template.
    if (wanted.empty()) {
        if (sourceKey == "card.en.success" || sourceKey == "dice.compat.growth.success")
            wanted = "regular";
        if (sourceKey == "card.en.fail" || sourceKey == "card.en.fail_grow" ||
            sourceKey == "dice.compat.growth.unchanged" || sourceKey == "dice.compat.growth.failure")
            wanted = "failure";
        if (sourceKey == "card.sc.result_noloss")
            wanted = "regular";
        if (sourceKey == "dnd.ds.miracle")
            wanted = "critical";
        if (sourceKey == "dnd.ds.critfail")
            wanted = "fumble";
        if (sourceKey == "dnd.ds.success" || sourceKey == "dnd.ds.stabilized")
            wanted = "regular";
        if (sourceKey == "dnd.ds.fail" || sourceKey == "dnd.ds.dead")
            wanted = "failure";
        for (const auto *prefix : {"dice.level.", "dice.brp.", "dice.rx.msg.", "dice.compat.check.single."})
            if (starts(sourceKey, prefix)) {
                const auto grade = sourceKey.substr(std::string(prefix).size());
                if (grade == "success")
                    wanted = "regular";
                else if (grade == "critical" || grade == "extreme" || grade == "hard" || grade == "regular" ||
                         grade == "failure" || grade == "fumble" || grade == "special")
                    wanted = grade;
            }
    }

    if (family == "standard" && wanted == "tie")
        family = "opposed";
    if (family == "sanity" || family == "psychology" || family == "standard" || family == "bonus" ||
        family == "penalty" || family == "brp") {
        ex.check(family, wanted);
        if (family == "sanity") {
            a["attr"] = ex.word("理智", "SAN");
            const bool failed = c.grade == "failure" || c.grade == "fumble";
            const bool noLoss = sourceKey == "card.sc.result_noloss";
            const auto lossRoll = ex.roll(noLoss ? "0" : failed ? "1d6" : "1");
            const int loss = c.grade == "fumble" ? 6 : lossRoll.modifiedTotal;
            ex.number("san", 60);
            ex.number("loss", loss);
            ex.number("final", (std::max)(0, 60 - loss));
            a["grade"] = ex.label(c.grade == "fumble" ? "fumble" : failed ? "failure" : "regular");
            a["res"] = a["result"];
            a["change"] = c.grade == "fumble" ? "Max{1d6}=6" : lossRoll.formattedOutput;
            ex.number("rank", c.grade == "fumble" ? 0
                              : failed            ? 1
                                                  : static_cast<int>(check_rules::coc(std::stoi(a["result"]), 60)));
            if (starts(sourceKey, "dice.compat.sanity."))
                a["res"] += "/60";
            c.command = std::string(".sc ") + (noLoss ? "0" : "1") + "/1d6 60";
            c.facts["lossExpression"] = noLoss ? "0" : failed ? "1d6" : "1";
        } else if (family == "psychology") {
            a["attr"] = ex.word("心理学", "Psychology");
            a["roll"] = a["result"];
            a["receipt"] = ex.tr("dice.rx.msg." + c.grade, a);
            c.command = ".rx " + a["target"] + " 60";
        }
    } else if (family == "growth") {
        const bool success = wanted.empty() ? ex.die(2) == 1 : wanted == "regular";
        const int value = success ? ex.between(61, 100) : ex.between(1, 60);
        ex.outcome(family, success ? "regular" : "failure");
        ex.number("result", value);
        ex.number("total", value);
        ex.number("rate", 60);
        a["roll"] = "1D100=" + std::to_string(value);
        a["res"] = a["roll"] + "/60";
        const bool failedGrowth = sourceKey == "card.en.fail_grow" || sourceKey == "dice.compat.growth.failure";
        const auto growth = ex.roll(success ? "1d10" : failedGrowth ? "0-1d4" : "0");
        a["change"] = success || failedGrowth ? growth.formattedOutput : "";
        ex.number("final", 60 + growth.modifiedTotal);
        c.facts["changeValue"] = growth.modifiedTotal;
        c.command = ".en " + a["attr"] + (failedGrowth ? " 60 -1d4/+1d10" : " 60");
        if (starts(sourceKey, "dice.compat.growth."))
            a["res"] += " " + a["level"];
    } else if (family == "opposed") {
        const auto grade =
            wanted.empty() ? std::vector<std::string>{"regular", "failure", "tie"}[ex.die(3) - 1] : wanted;
        const int left = ex.percentile(grade == "regular" ? "regular" : "failure");
        const int right = ex.percentile(grade == "failure" ? "regular" : "failure");
        ex.outcome(family, grade);
        a["la"] = a["attr"];
        a["lb"] = ex.word("潜行", "Stealth");
        a["a"] = a["la"];
        a["b"] = a["lb"];
        ex.number("ra", left);
        ex.number("rb", right);
        ex.number("va", 60);
        ex.number("vb", 60);
        ex.number("result", left);
        ex.number("rate", 60);
        a["lva"] = ex.label(check_rules::grade(check_rules::coc(left, 60)));
        a["lvb"] = ex.label(check_rules::grade(check_rules::coc(right, 60)));
        a["roll"] = a["res"] = "1D100=" + a["ra"] + " vs 1D100=" + a["rb"];
        a["level"] = a["outcome"] = ex.tr(grade == "tie"       ? "dice.rav.tie"
                                          : grade == "regular" ? "dice.rav.a_wins"
                                                               : "dice.rav.b_wins",
                                          a);
        c.command = ".rav 60 60";
    } else if (family == "resist") {
        const int active = 12, passive = 10, target = 50 + (active - passive) * 5;
        const bool win = wanted.empty() ? ex.die(2) == 1 : wanted == "regular";
        const int value = win ? ex.between(1, target) : ex.between(target + 1, 100);
        ex.outcome(family, win ? "regular" : "failure");
        a["la"] = a["a"] = ex.word("力量", "STR");
        a["lb"] = a["b"] = ex.word("体型", "SIZ");
        ex.number("va", active);
        ex.number("vb", passive);
        ex.number("target", target);
        ex.number("rate", target);
        ex.number("result", value);
        a["roll"] = a["res"] = "1D100=" + a["result"];
        a["level"] = a["outcome"] = ex.tr(win ? "dice.brp.resist_win" : "dice.brp.resist_lose", a);
        c.command = ".bav 12 10";
    } else if (family == "dnd_check" || family == "death_save") {
        const bool death = family == "death_save";
        auto grade = wanted.empty() ? (ex.die(2) == 1 ? std::string("regular") : std::string("failure")) : wanted;
        const int modifier = death ? 0 : 3, target = death ? 10 : 15;
        const int value = death && wanted.empty() ? ex.die(20)
                          : grade == "critical"   ? 20
                          : grade == "fumble"     ? 1
                          : grade == "regular"    ? ex.between(target - modifier, death ? 19 : 20)
                                                  : ex.between(death ? 2 : 1, target - modifier - 1);
        if (death)
            grade = value == 20 ? "critical" : value == 1 ? "fumble" : value + modifier >= 10 ? "regular" : "failure";
        ex.outcome(family, grade);
        ex.number("result", value);
        ex.number("total", value + modifier);
        ex.number("rate", target);
        ex.number("threshold", target);
        a["mod"] = death ? "0" : "+3";
        a["expr"] = "1d20";
        a["roll"] = death ? std::to_string(value) : "1D20=" + std::to_string(value);
        a["res"] = "1D20=" + std::to_string(value) + a["mod"] + "=" + a["total"];
        a["detail"] = a["res"] + " " + a["level"];
        a["attr"] = ex.word("力量", "STR");
        if (death) {
            const auto message = grade == "critical"  ? "miracle"
                                 : grade == "fumble"  ? "critfail"
                                 : grade == "regular" ? "success"
                                                      : "fail";
            a["level"] = a["outcome"] = ex.tr(std::string("dnd.ds.") + message, a);
            ex.number("s", grade == "regular" ? (sourceKey == "dnd.ds.stabilized" ? 3 : 1) : 0);
            ex.number("f", grade == "fumble" ? 2 : grade == "failure" ? (sourceKey == "dnd.ds.dead" ? 3 : 1) : 0);
            ex.number("hp", grade == "critical" ? 1 : 0);
        }
        c.command = death ? ".ds" : ".rdc +3 " + a["attr"] + " 15";
    } else {
        if (c.command.empty())
            c.command = ".r 1d100";
        if (!starts(key, "dice.roll."))
            a["detail"] = "HP:12/12 SAN:60 MP:12/12";
        if (starts(key, "dice.roll.multi")) {
            ex.number("turn", 3);
            a["res"] = "\n";
            for (int i = 0; i < 3; ++i)
                a["res"] += (i ? ", \n" : "") + ex.roll("1d100").formattedOutput;
            c.command = ".r 3#1d100";
        } else if (starts(key, "dice.pool.")) {
            int pool = 5, successes = 0;
            std::string detail = "5=", counts;
            static const char *circled[]{"", "①", "②", "③", "④", "⑤", "⑥", "⑦", "⑧", "⑨", "⑩"};
            for (int round = 0; pool > 0 && round < 50; ++round) {
                int next = 0, passed = 0;
                detail += round ? "+{" : "{";
                for (int i = 0; i < pool; ++i) {
                    const int value = ex.die(10);
                    if (i)
                        detail += ",";
                    detail += value >= 8 ? circled[value] : std::to_string(value);
                    if (value >= 8)
                        ++passed;
                    if (value == 10)
                        ++next;
                }
                detail += "}";
                counts += (round ? "+" : "") + std::to_string(passed);
                successes += passed;
                pool = next;
            }
            a["expr"] = a["res"] = a["detail"] = detail + "=" + counts + "=" + std::to_string(successes);
            ex.number("result", successes);
            ex.number("total", successes);
            c.command = ".ww 5";
        } else if (starts(key, "dice.coc.")) {
            static const char *names[]{"str", "con", "siz", "dex", "app", "int", "pow", "edu", "luck"};
            std::string detail = "\n";
            int total = 0, luck = 0;
            for (int i = 0; i < 9; ++i) {
                const int value = ex.roll(i == 2 || i == 5 || i == 7 ? "2d6+6" : "3d6").modifiedTotal * 5;
                detail += ex.tr(std::string("dice.coc.attr.") + names[i]) + ":" + std::to_string(value) + " ";
                total += value;
                if (i == 8)
                    luck = value;
            }
            a["res"] =
                detail + ex.tr("dice.coc.total") + ":" + std::to_string(total - luck) + "/" + std::to_string(total);
            ex.number("total", total);
            c.facts["luck"] = luck;
            c.command = ".coc";
        } else if (starts(key, "dice.dnd.")) {
            std::vector<int> scores;
            int total = 0;
            for (int i = 0; i < 6; ++i) {
                auto dice = ex.roll("4d6").individualResults;
                std::sort(dice.begin(), dice.end(), std::greater<int>());
                const int score = dice[0] + dice[1] + dice[2];
                scores.push_back(score);
                total += score;
            }
            std::sort(scores.begin(), scores.end(), std::greater<int>());
            a["res"] = "\n";
            for (size_t i = 0; i < scores.size(); ++i)
                a["res"] += (i ? " /" : "") + std::to_string(scores[i]);
            a["res"] += " " + ex.tr("dice.dnd.total") + std::to_string(total);
            ex.number("total", total);
            c.facts["scores"] = scores;
            c.command = ".dnd";
        } else if (starts(key, "fun.jrrp") || starts(key, "fun.mrrp") || starts(key, "fun.zrrp")) {
            ex.number("res", ex.between(1, 100));
            a["result"] = a["res"];
            c.command = "." + key.substr(4);
        } else if (starts(key, "fun.name.")) {
            a["res"] = ex.word("林晓风", "Morgan Reed");
            c.command = ".name";
        } else if (starts(key, "shortcut.")) {
            a["source"] = ex.tr("shortcut.group");
            a["shortcut"] = ex.locale == Locale::kZhHant ? "偵查"
                            : ex.locale == Locale::kJa   ? "目星"
                                                         : ex.word("侦查", "spot");
            a["command"] = ".ra " +
                           (ex.locale == Locale::kZhHant ? std::string("偵查")
                            : ex.locale == Locale::kJa   ? std::string("目星")
                                                         : ex.word("侦查", "Spot")) +
                           " 60";
            a["list"] = "[" + a["source"] + "] .&" + a["shortcut"] + " → " + a["command"];
            c.command = ".alias " + a["shortcut"] + " " + a["command"];
        } else if (starts(key, "deck.")) {
            a["name"] = ex.word("塔罗牌", "Tarot");
            a["list"] = ex.word("塔罗牌 (78)、硬币 (2)", "Tarot (78), Coin (2)");
            a["res"] = ex.word("愚者（正位）", "The Fool (upright)");
            ex.number("total", 78);
            ex.number("left", 77);
            ex.number("count", 1);
            c.command = ".draw " + a["name"];
        } else if (starts(key, "log.")) {
            a["name"] = ex.word("示例跑团", "ExampleSession");
            a["list"] = a["name"] + " (12)\n" + ex.word("另一场跑团 (8)", "AnotherSession (8)");
            ex.number("total", 2);
            ex.number("from", 1);
            ex.number("to", 2);
            c.command = ".log list";
        } else if (starts(key, "card.insane.")) {
            a["res"] = ex.word("恐惧症：暂时害怕黑暗，持续 ", "Phobia: temporarily afraid of the dark, duration: ") +
                       ex.roll("1d10").formattedOutput;
            c.command = key == "card.insane.long" ? ".li" : ".ti";
        } else if (starts(key, "card.") || starts(key, "pc.")) {
            a["body"] = a["detail"] = a["output"] = "HP:12/12 SAN:60 MP:12/12\n" + a["attr"] + ":60";
            a["list"] = "★ " + a["card"] + " (4)\n" + ex.word("守秘人 (0)", "Keeper (0)");
            c.command = ".st show";
            if (starts(key, "pc."))
                c.command = ".pc list";
        } else if (starts(key, "hiy.")) {
            for (const auto *name : {"crit", "extreme", "hard", "regular", "fail", "fumble"})
                ex.number(name, 0);
            for (int i = 0; i < 100; ++i) {
                const std::string grade = check_rules::grade(check_rules::coc(ex.die(100), 60));
                const auto field = grade == "critical" ? "crit" : grade == "failure" ? "fail" : grade;
                ex.number(field, std::stoi(a[field]) + 1);
            }
            ex.number("total", 100);
            ex.number("success",
                      std::stoi(a["crit"]) + std::stoi(a["extreme"]) + std::stoi(a["hard"]) + std::stoi(a["regular"]));
            a["rate"] = a["success"];
            c.command = ".hiy";
        } else if (starts(key, "init.")) {
            a["base"] = ex.word("怪物", "Monster");
            a["name"] = a["base"];
            a["detail"] = a["list"] = "";
            for (int i = 0; i < 2; ++i) {
                const auto roll = ex.roll("1d20+3");
                const auto line =
                    std::to_string(i + 1) + ". " + a["base"] + std::to_string(i + 1) + " " + roll.formattedOutput;
                a["detail"] += "\n" + line;
                a["list"] += "\n" + line;
                a["roll"] = roll.formattedOutput;
                ex.number("total", roll.modifiedTotal);
            }
            c.command = ".ri 2#1d20+3 " + a["base"];
        } else if (starts(key, "ak.")) {
            a["title"] = ex.word("下一步行动", "Next action");
            a["list"] = ex.word("1. 调查旧宅\n2. 拜访图书馆", "1. Investigate the old house\n2. Visit the library");
            a["get"] = ex.die(2) == 1 ? ex.word("调查旧宅", "Investigate the old house")
                                      : ex.word("拜访图书馆", "Visit the library");
        } else if (starts(key, "buff.")) {
            a["detail"] = ex.word("力量 +2，敏捷 +1", "STR +2, DEX +1");
        } else if (starts(key, "group.")) {
            a["result"] = ex.word("群设置已更新。", "Group settings updated.");
        } else if (starts(key, "dnd.ss.") || starts(key, "dnd.cast.")) {
            a["detail"] = ex.word("1环 2/4；2环 1/2", "Level 1: 2/4; level 2: 1/2");
            ex.number("max", 4);
            ex.number("cur", 2);
            c.command = ".ss";
        } else if (starts(key, "help")) {
            a["list"] = ".r — " + ex.word("掷骰", "Roll dice") + "\n.ra — " + ex.word("技能检定", "Skill check");
            c.command = ".help";
        }
    }
    if (key == "dice.sc.result")
        a["output"] = ex.tr("card.sc.result", a);
    if (key == "dice.st.result")
        a["output"] = ex.tr("dice.check.result", a);
    // All families expose the actual data used by this example, not arbitrary
    // replacement numbers. Useful to inspect a custom template's calculations.
    c.facts["readOnly"] = true;
    return c;
}
} // namespace dice::outbound
