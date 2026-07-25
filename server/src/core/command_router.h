#pragma once
// ─── Dice!Next v3.0.0 — Command Router ───────────────────────
// Routes incoming messages to dice engine, reply system, etc.
// Faithfully implements original Dice! command set (.r .coc .dnd etc.)
//
// Every user-facing string goes through I18n: the router resolves the
// reply locale once per message (LocaleResolver, "platform default +
// overridable") and passes it to each handler, which looks up text via
// tr(locale, key, args). No hardcoded natural-language strings here.

#include "../adapter/adapter_interface.h"
#include "../adapter/adapter_manager.h"
#include "../core/dice/dice_engine.h"
#include "../core/dice/madness_data.h"
#include "../core/reply/reply_manager.h"
#include "character/card_store.h"
#include "deck/card_deck.h"
#include "../storage/database.h"
#include "rules_lock.h"
#include "../service/notice_manager.h"   // B：通知系统（权限变更等推送给骰主）

#include <sqlite_orm/sqlite_orm.h>
#include <ctime>
#include <chrono>
#include <functional>
#include "../config/config_manager.h"
#include "../common/logger.h"
#include "../common/version.h"
#include "../common/utils.h"
#include "../platform/system_info.h"
#include "../i18n/i18n.h"
#include "../i18n/locale_resolver.h"
#include "../service/log_service.h"
#include "../service/image_host.h"
#include "../service/ai_translate.h"   // C#68 阶段三：.lang AI 翻译语言
#include "persona/persona_manager.h"
#include <onedice/onedice.h>

#include <functional>
#include <regex>
#include <memory>
#include <atomic>
#include <mutex>
#include <random>
#include <drogon/HttpAppFramework.h>   // C#62：.dismiss 随机延时退群用事件循环定时器
#include <variant>
#include <sstream>
#include <optional>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <algorithm>
#include <array>
#include <vector>
#include <unordered_set>
#include <set>
#include <fstream>
#include <filesystem>
#include <yaml-cpp/yaml.h>   // 解析 seal.gameSystem.newTemplateByYaml 的 YAML 模板

namespace dice {

// 路径 → UTF-8 窄串（Windows 上 .string() 走 ANSI 代码页，遇到 GBK 无映射字符
// （emoji 等文件名）抛 system_error「No mapping for the Unicode character…」——
// Server 2012/2016 启动即崩的根因（crash_*.txt 定位）。u8string 永不抛。
inline std::string u8str(const std::filesystem::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}


class CommandRouter {
public:
    CommandRouter(Database& db, ConfigManager& cfg, DiceEngine& engine,
                  I18n& i18n, LocaleResolver& resolver, CharacterCardStore& cards,
                  CardDeck& deck, AdapterManager& adapters)
        : db_(db), cfg_(cfg), engine_(engine), i18n_(i18n), resolver_(resolver),
          cards_(cards), deck_(deck), adapters_(adapters) {}

    /// Set the PersonaManager (called after construction from main.cpp).
    void setPersonaManager(PersonaManager* pm) { personaMgr_ = pm; }

    /// 智能化阶段D（工具调用）：按发送者的人物卡读取一个属性值（含关联属性/衍生值回退）。
    /// 供 AI function-calling 的 get_attr 工具使用。无卡/无此属性返回 nullopt。
    std::optional<int> aiGetAttr(const Message& msg, const std::string& name) {
        std::string attr = CharacterCardStore::canonical(name);
        auto v = cards_.getAttr(msg.senderId, cardScope(msg), attr);
        if (!v) v = evalStrAttr(msg, attr);       // C#37 关联属性表达式
        if (!v) v = derivedAttr(msg, attr);       // model.xml 衍生值
        return v;
    }

    /// AI 深化（工具写卡）：设置发送者人物卡的一个属性为绝对值（走 canonical 同义词归一，
    /// 与 aiGetAttr 一致）。供 AI function-calling 的 set_attr 工具使用。仅改本人卡。
    bool aiSetAttr(const Message& msg, const std::string& name, int value) {
        std::string attr = CharacterCardStore::canonical(name);
        if (attr.empty()) return false;
        cards_.setAttr(msg.senderId, cardScope(msg), attr, value);
        return true;
    }

    // C#68/C#78：本条消息回复的「类别」（供 AI 润色/翻译按覆盖范围过滤）。取值：
    //   roll(掷骰/检定) / deck(牌堆抽取) / fun(娱乐:jrrp/ti/li/name/favor/sleep) / ""(其它)。
    // 自定义回复(custom)/插件(plugin) 由 main.cpp 按来源另行分类。thread_local：handleMessage
    // 与调用方（main.cpp 出站前）在同一线程，无竞态。
    inline static thread_local std::string s_replyCat;
    const std::string& lastReplyCategory() const { return s_replyCat; }
    bool lastReplyWasRoll() const { return s_replyCat == "roll"; }

    /// Process an incoming message and return the response (or empty string if no match).
    /// Called by adapter message callbacks.
    std::string handleMessage(const Message& msg,
                              std::optional<Locale> forcedLocale = std::nullopt) {
        quoteOverride_.clear();   // reset per-message reply-quote override (#10)
        forwardNodes_.clear();    // reset per-message 合并转发节点 (#6)
        s_replyCat.clear();       // C#68/C#78：每条消息重置回复类别
        detectDiceBot(msg);       // C#45/C#73：被动识别其他骰子的 .bot 横幅回执（须紧跟探测）
        recordBotProbe(msg);      // C#73：记录本群 .bot 探测时间，作为识别的时间窗
        std::string text = trim(msg.content);
        if (text.empty()) return "";

        // 好感度查询走原版的「无前缀关键词」习惯：整条消息恰为「好感 / 好感排行 /
        // 群内好感排行」时直接响应（不劫持其它含「好感」的消息）。操控类仍需前缀。
        if (text == "\xe5\xa5\xbd\xe6\x84\x9f" ||                                  // 好感
            text == "\xe5\xa5\xbd\xe6\x84\x9f\xe6\x8e\x92\xe8\xa1\x8c" ||          // 好感排行
            text == "\xe7\xbe\xa4\xe5\x86\x85\xe5\xa5\xbd\xe6\x84\x9f\xe6\x8e\x92\xe8\xa1\x8c") { // 群内好感排行
            if (isGroupLocked(msg) || (isGroupDisabled(msg))) return "";
            return handleFavor(forcedLocale.value_or(resolver_.resolve(msg)), msg, text);
        }

        // 外置开 / 外置关（青果/原版：无前缀关键词，切换本群「外置模式」=停用内置
        // 指令、保留自定义回复）。简繁两形都接受。
        if (text == "\xe5\xa4\x96\xe7\xbd\xae\xe5\xbc\x80" ||                  // 外置开
            text == "\xe5\xa4\x96\xe7\xbd\xae\xe9\x96\x8b") {                 // 外置開
            if (isGroupLocked(msg)) return "";
            return handleExternalToggle(forcedLocale.value_or(resolver_.resolve(msg)), msg, true);
        }
        if (text == "\xe5\xa4\x96\xe7\xbd\xae\xe5\x85\xb3" ||                  // 外置关
            text == "\xe5\xa4\x96\xe7\xbd\xae\xe9\x97\x9c") {                 // 外置關
            if (isGroupLocked(msg)) return "";
            return handleExternalToggle(forcedLocale.value_or(resolver_.resolve(msg)), msg, false);
        }

        // 召唤词（无前缀唤醒，原版 strSummonWord）：消息以召唤词开头时等效于带前缀；
        // 仅有召唤词时回应 strSummonEmpty。config dice/summon_word，默认空（关闭）。
        std::string summon = cfg_.get<std::string>("dice/summon_word", std::string());
        bool summoned = false;
        if (!summon.empty() && text.rfind(summon, 0) == 0) {
            std::string after = trim(text.substr(summon.size()));
            if (after.empty()) {
                if (isGroupLocked(msg) || isGroupDisabled(msg)) return "";
                return i18n_.tr(forcedLocale.value_or(resolver_.resolve(msg)),
                                "fun.summon_empty", {{"nick", displayName(msg)}});
            }
            summoned = true;
        }

        // Match against the configured command prefixes (hot-reloadable, multi).
        std::string matchedPrefix;
        if (summoned) {
            matchedPrefix = summon;
        } else {
            for (const auto& p : commandPrefixes()) {
                if (!p.empty() && text.rfind(p, 0) == 0) { matchedPrefix = p; break; }
            }
        }
        if (matchedPrefix.empty()) {
            // Not a command — the caller checks custom replies for this message.
            return "";
        }

        // Resolve which language to reply in for this message.
        // The Playground/test endpoint can force a locale to preview each language.
        Locale loc = forcedLocale.value_or(resolver_.resolve(msg));

        std::string cmd = trim(text.substr(matchedPrefix.size()));
        if (cmd.empty()) return "";

        // Hard lock (web admin "彻底禁用"): EVERYTHING is silent, including .bot —
        // only the web panel can lift it. Stronger than .bot off.
        if (isGroupLocked(msg)) return "";
        // Group-disabled gate (.bot off): normally only .bot works, but an explicit
        // @ to this bot is a deliberate wake-up and may run commands again. A hard
        // web-admin lock was already handled above and is never bypassed.
        if (isGroupDisabled(msg) && !isAtSelf(msg) && toLower(cmd).rfind("bot", 0) != 0) {
            return "";
        }

        std::string cmdL0 = toLower(cmd);
        // ── 规则包指令层（C#12）：本群激活规则的 别名重写 + 自定义指令 + 屏蔽 ──
        if (auto rp = activeRulePack(msg);
            rp && (!rp->cmdAlias.empty() || !rp->disableCmds.empty() || !rp->customCmds.empty())) {
            if (!rp->cmdAlias.empty()) {
                auto [w, rest] = splitCommand(cmd);
                auto it = rp->cmdAlias.find(w);
                if (it != rp->cmdAlias.end()) cmd = it->second + (rest.empty() ? "" : " " + rest);
                else for (auto& [k, tgt] : rp->cmdAlias)
                    if (!k.empty() && cmd.rfind(k, 0) == 0) { cmd = tgt + cmd.substr(k.size()); break; }
                cmdL0 = toLower(cmd);
            }
            // C#12-A②：自定义指令（commands.add）。先于屏蔽判定，确保规则新增的指令
            // 不会被某条 disable 前缀误伤；按指令首词匹配（精确，其次忽略大小写）。
            if (!rp->customCmds.empty()) {
                auto [w, rest] = splitCommand(cmd);
                const std::string* tmpl = nullptr;
                if (auto it = rp->customCmds.find(w); it != rp->customCmds.end()) tmpl = &it->second;
                else { std::string wl = toLower(w); for (auto& [k, v] : rp->customCmds) if (toLower(k) == wl) { tmpl = &v; break; } }
                if (tmpl) {
                    if (auto outp = renderCustomCmd(*tmpl, msg, rest)) return *outp;
                    return i18n_.tr(loc, "fun.rulecmd.fail", {{"cmd", w}});
                }
            }
            if (!rp->disableCmds.empty()) {
                std::string w = toLower(splitCommand(cmd).first);
                for (auto& d : rp->disableCmds)
                    if (w == d || cmdL0.rfind(d, 0) == 0) return "";   // 本规则屏蔽该指令 → 静默
            }
        }
        const bool botCmd = cmdL0.rfind("bot", 0) == 0;
        const bool privileged = isMaster(msg) || senderTrust(msg) >= 4;
        // 全局静默 (console DisabledGlobal)：非信任用户完全沉默；.bot / Master 例外。
        if (silentGlobal() && !privileged && !botCmd) return "";
        // 外置模式 (停用指令)：停用内置指令，但返回 "" 让上层继续匹配自定义回复。
        // .bot / Master 例外（否则无法在群内恢复）。
        if (groupExternalMode(msg) && !isMaster(msg) && !botCmd) return "";
        // 全局 / 单群 单条命令停用 (.me/.jrrp/.draw/.help…)。
        {
            Locale gloc = forcedLocale.value_or(resolver_.resolve(msg));
            if (auto blocked = gateCommand(gloc, msg, cmdL0)) return *blocked;
        }

        // 代骰: a roll/check command that @s a real person (not the bot / not @all)
        // is rolled from THAT person's perspective — their nick + character card.
        // Build a "perspective" message (sender = the @'d player; atList cleared so
        // it isn't re-interpreted as a target downstream) and prepend a note
        // crediting the代骰者. Non-roll commands keep the real sender (`msg`).
        bool proxied = false;
        Message pmsg = msg;
        std::string proxyNote;
        if (std::string tgt = atTarget(msg); !tgt.empty()) {
            // C#45：@ 的对象是已识别的骰娘 → 用户是在叫那只骰子执行指令，本骰静默
            //（不当代骰目标执行）。
            if (isDiceBot(tgt)) return "";
            pmsg.senderId = tgt;
            pmsg.senderName = lookupNick(msg.platform, tgt);
            pmsg.atList.clear();
            proxied = true;
            proxyNote = i18n_.tr(loc, "dice.proxy_note", {{"agent", displayName(msg)}});
        }
        auto PX = [&](std::optional<std::string> r) -> std::optional<std::string> {
            if (r && proxied && !r->empty()) *r = proxyNote + *r;
            return r;
        };

        // These commands let their argument attach directly (".r3d6", ".ra侦查60",
        // ".coc5"), so they're parsed before the space-splitting path.
        // C#68/C#78：CAT()/RM() 给回复打「类别」标签（AI 润色/翻译按覆盖范围过滤，
        // 不动 help/错误/配置回复）。RM=掷骰/检定类；CAT 用于牌堆/娱乐。
        auto CAT = [](const char* c, std::optional<std::string> r) { if (r) s_replyCat = c; return r; };
        auto RM = [&CAT](std::optional<std::string> r) { return CAT("roll", std::move(r)); };
        if (auto r = RM(tryHandleRdc(loc, msg, cmd)))         return *r;  // DND .rdc must precede generic .r
        if (auto r = RM(PX(tryHandleRdx(loc, pmsg, cmd))))    return *r;  // .rdx is the legacy alias of .dx; must precede generic .r
        if (auto r = RM(PX(tryHandleRoll(loc, pmsg, cmd))))  return *r;  // .r / .rh / .rs (含 NdF 命运骰)
        if (auto r = RM(PX(tryHandleRAV(loc, pmsg, cmd))))   return *r;  // .rav / .rcv 对抗 (须在 .ra 前)
        // .rx 用原始 msg（@ 的是被检定的调查员，KP 自己是掷骰者，不走代骰透视）。
        if (auto r = RM(tryHandleRx(loc, msg, cmd)))         return *r;  // .rx 心理学暗骰 (须在 .ra 前)
        if (auto r = RM(PX(tryHandleBrp(loc, pmsg, cmd))))   return *r;  // .ba / .bav  BRP 检定/对抗
        if (auto r = RM(PX(tryHandleCheck(loc, pmsg, cmd)))) return *r;  // .ra / .rc 检定
        if (auto r = RM(PX(tryHandleBP(loc, pmsg, cmd))))    return *r;  // .rb / .rp 奖励/惩罚骰
        if (auto r = tryHandlePersona(loc, msg, cmd))    return *r;  // .rpmode 人格切换 (C#28-B)
        if (auto r = RM(tryHandleDnd(loc, pmsg, cmd)))  return *r;  // .ss/.cast/.longrest/.ds (DND，@可代操作)
        if (auto r = tryHandleGame(loc, msg, cmd))  return *r;  // C#107 .game 团务（须在 .ga 类之前独立匹配）
        if (auto r = tryHandleGen(loc, msg, cmd))   return *r;  // .coc / .dnd 生成
        if (auto r = tryHandleMaster(loc, msg, cmd))return *r;  // boton/botoff/blackqq/whitegroup… (须在 .bot 前)
        if (auto r = tryHandleBot(loc, msg, cmd))   return *r;  // .bot / .bot on/off (+账号定向)
        if (auto r = tryHandleSelfText(loc, msg, cmd)) return *r;  // .strSelfName/.strSelfCall (须在 .st 前)
        if (auto r = tryHandleST(loc, msg, cmd))    return *r;  // .st 人物卡 (改自己的卡，不代骰)
        if (auto r = RM(PX(tryHandleSC(loc, pmsg, cmd))))    return *r;  // .sc 理智检定
        if (auto r = RM(PX(tryHandleWW(loc, pmsg, cmd))))    return *r;  // .ww 骰池
        if (auto r = RM(PX(tryHandleDX(loc, pmsg, cmd))))    return *r;  // .dx 双十字
        if (auto r = CAT("fun", tryHandleFavor(loc, msg, cmd))) return *r;  // .favor / 好感（好感度系统）
        if (auto r = CAT("fun", tryHandleJrrp(loc, msg, cmd)))  return *r;  // .jrrp 今日人品
        if (auto r = CAT("fun", tryHandleInsane(loc, msg, cmd)))return *r;  // .ti / .li 疯狂症状
        if (auto r = CAT("fun", tryHandleName(loc, msg, cmd)))  return *r;  // .name / .gn 随机名
        if (auto r = RM(PX(tryHandleEn(loc, pmsg, cmd))))    return *r;  // .en 技能成长
        if (auto r = CAT("fun", tryHandleMe(loc, msg, cmd))) return *r;  // .me 第三人称动作
        if (auto r = CAT("fun", tryHandleAk(loc, msg, cmd))) return *r;  // .ak 抉择分歧
        if (auto r = CAT("fun", tryHandleNNN(loc, msg, cmd))) return *r; // .nnn 随机改名 (须在 .nn 前)
        if (auto r = tryHandleNN(loc, msg, cmd))    return *r;  // .nn 改名
        if (auto r = tryHandleSetcoc(loc, msg, cmd))return *r;  // .setcoc 房规 (须在 .set 前)
        if (auto r = tryHandlePlugin(loc, msg, cmd)) return *r;  // .plugin 分群插件启停 (C#33)
        if (auto r = tryHandleSystem(loc, msg, cmd)) return *r;  // .system info/stats (C#53, 骰主)
        if (auto r = tryHandleSetsn(loc, msg, cmd)) return *r;  // .setsn 群名片模板 (须在 .set 前)
        if (auto r = tryHandleSetdnd(loc, msg, cmd))return *r;  // .setdnd DND模式开关 (须在 .set 前)
        if (auto r = tryHandleSet(loc, msg, cmd))   return *r;  // .set 默认骰
        if (auto r = CAT("fun", tryHandleSleep(loc, msg, cmd))) return *r;  // .sleep 休息
        if (auto r = CAT("deck", tryHandleDraw(loc, msg, cmd)))  return *r;  // .draw 抽牌堆
        if (auto r = CAT("deck", tryHandleDeck(loc, msg, cmd)))  return *r;  // .deck 牌堆列表
        if (auto r = CAT("deck", tryHandleGacha(loc, msg, cmd))) return *r;  // .gacha

        // Parse command and arguments
        auto [command, args] = splitCommand(cmd);
        std::string cmdLower = toLower(command);

        // 原版兼容（Shia 2026-07-11）：指令与参数之间的空格可省略——原版 DiceEvent
        // 是纯前缀匹配（".help指令"".ri+2"".trust3" 均合法）。首词精确匹配不到本段
        // 任何指令时，按「最长指令优先」回退拆分；交叉指令永远长者优先（.helpdoc >
        // .help、.alias > .ai），想让短指令吃字母参数须用空格隔开，与原版语义一致。
        {
            static const char* kWordCmds[] = {   // 本段全部词指令，按长度降序排列
                "helpdoc", "welcome", "dismiss", "ruleset",
                "notice",
                "alias", "trust", "admin", "rules", "group", "reply",
                // C#107：“game”不做免空格前缀回退——.game xx 由 tryHandleGame 直达（带空格），
                // 否则 .gameXXX 类 Lua 插件触发词会被拆成 game+XXX 吞掉。
                "buff", "send", "help", "text", "link", "init", "lang", "rule",
                "npc", "log", "hiy", "mod",
                "ai", "ob", "ri", "sn", "pc",
            };
            bool exact = false;   // 精确命中本段指令（如 ".rules"）→ 不做前缀回退，
            for (const char* w : kWordCmds)   // 否则会被更短的表项误拆（rules→rule+"s"）。
                if (cmdLower == w) { exact = true; break; }
            if (!exact) {
                for (const char* w : kWordCmds) {
                    size_t wl = std::char_traits<char>::length(w);
                    if (cmdLower.size() > wl && cmdLower.compare(0, wl, w) == 0) {
                        args = trim(cmd.substr(wl));
                        cmdLower = w;
                        break;
                    }
                }
            }
        }

        // ─── Core Commands ───────────────────────────────────
        if (cmdLower == "ai")      return handleAi(loc, args, msg);   // C#84：本群 AI 开关
        if (cmdLower == "trust")   return handleTrust(loc, args, msg); // C：用户信任等级
        if (cmdLower == "admin")   return handleAdmin(loc, args, msg); // C：管理员授撤
        if (cmdLower == "notice")  return handleNotice(loc, args, msg); // B：通知窗口注册
        if (cmdLower == "alias")   return handleAlias(loc, args, msg);  // C：账号别名（TinyList）
        if (cmdLower == "help")    return handleHelp(loc, args, msg);
        if (cmdLower == "helpdoc") return handleHelpDoc(loc, args, msg);
        if (cmdLower == "text")    return handleText(loc, args, msg);
        if (cmdLower == "hiy")     return handleHiy(loc, args, msg);
        if (cmdLower == "buff")    return handleBuff(loc, args, msg);
        if (cmdLower == "send")    return handleSend(loc, args, msg);
        if (cmdLower == "dismiss") return handleDismiss(loc, args, msg);
        if (cmdLower == "game")    return handleGame(loc, args, msg);
        if (cmdLower == "log")     return handleLog(loc, args, msg);
        if (cmdLower == "group")   return handleGroup(loc, args, msg);
        if (cmdLower == "rules" || cmdLower == "rule") return handleRules(loc, args, msg);
        if (cmdLower == "ruleset") return handleRuleSet(loc, args, msg);
        if (cmdLower == "ob")      return handleObserve(loc, args, msg);
        if (cmdLower == "link")    return handleLink(loc, args, msg);   // 原版移植：跨窗口链接
        if (cmdLower == "ri")      return handleRi(loc, args, msg);
        if (cmdLower == "init")    return handleInit(loc, args, msg);
        if (cmdLower == "sn")      return handleSn(loc, args, msg);
        if (cmdLower == "welcome") return handleWelcome(loc, args, msg);
        if (cmdLower == "reply")   return handleReply(loc, args, msg);
        if (cmdLower == "pc")      return handlePC(loc, args, msg);
        if (cmdLower == "npc")     return handleNpc(loc, args, msg);   // D#06 NPC 卡+代骰
        if (cmdLower == "mod")     return handleMod(loc, args, msg);
        if (cmdLower == "lang")    return handleLang(loc, args, msg);

        return "";
    }

private:
    // ─── Command Parsing ─────────────────────────────────────

    /// Strip a single leading prefix character. ASCII prefixes (. !) are one
    /// byte; full-width 。and ！are three UTF-8 bytes each.
    static std::string stripPrefix(const std::string& text) {
        if (text.empty()) return text;
        if (text[0] == '.' || text[0] == '!') return text.substr(1);
        if (text.rfind("\xe3\x80\x82", 0) == 0 ||   // 。
            text.rfind("\xef\xbc\x81", 0) == 0)      // ！
            return text.substr(3);
        return text;
    }

    std::pair<std::string, std::string> splitCommand(const std::string& raw) {
        auto pos = raw.find(' ');
        if (pos == std::string::npos) return {raw, ""};
        return {raw.substr(0, pos), trim(raw.substr(pos + 1))};
    }

    static std::string toLower(const std::string& s) {
        std::string r = s;
        for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return r;
    }

    /// 模糊查找：在 @p cands 里找匹配 @p q 的项。精确命中→只返回它；否则优先
    /// 前缀匹配，没有再退子串匹配（ASCII 大小写不敏感）。用于牌堆名 / help 主题。
    static std::vector<std::string> fuzzyFind(const std::string& q,
                                              const std::vector<std::string>& cands) {
        if (q.empty()) return {};
        std::string ql = toLower(q);
        std::vector<std::string> prefix, sub;
        for (const auto& c : cands) {
            std::string cl = toLower(c);
            if (cl == ql) return {c};                       // 精确
            if (cl.rfind(ql, 0) == 0) prefix.push_back(c);  // 前缀
            else if (cl.find(ql) != std::string::npos) sub.push_back(c);  // 子串
        }
        return !prefix.empty() ? prefix : sub;
    }

    static std::string trim(const std::string& s) {
        auto start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        auto end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    static int parseIntOr(const std::string& s, int fallback) {
        try { return std::stoi(s); } catch (...) { return fallback; }
    }

    // ─── Command Handlers ────────────────────────────────────

    /// Detect & handle a roll command (".r", ".rh", ".rs", ".r3d6", ".r3d6 攻击").
    /// Returns nullopt if `cmd` is not a roll command (so other handlers run).
    std::optional<std::string> tryHandleRoll(Locale loc, const Message& msg,
                                             const std::string& cmd) {
        if (cmd.empty() || (cmd[0] != 'r' && cmd[0] != 'R')) return std::nullopt;

        // Parse flags after 'r': 's' (short) and 'h' (hidden), in any order.
        size_t j = 1;
        bool hidden = false;
        while (j < cmd.size()) {
            char cj = static_cast<char>(std::tolower(static_cast<unsigned char>(cmd[j])));
            if (cj == 's') { ++j; }            // short form — accepted (full detail for now)
            else if (cj == 'h') { hidden = true; ++j; }
            else break;
        }
        // What follows the flags must look like a roll; otherwise this is some
        // other command that merely starts with 'r' (reply, ra, rc, ...).
        if (j < cmd.size()) {
            char cj = cmd[j];
            bool ok = (cj == ' ' || std::isdigit(static_cast<unsigned char>(cj)) ||
                       cj == 'd' || cj == 'D' || cj == '(' ||
                       cj == '+' || cj == '-' || cj == '#');
            if (!ok) return std::nullopt;
        }

        std::string out = handleRoll(loc, msg, trim(cmd.substr(j)));
        if (hidden) { sendPrivate(msg, out); return i18n_.tr(loc, "dice.roll.hidden", {{"nick", displayName(msg)}}); }
        return out;
    }

    std::string handleRoll(Locale loc, const Message& msg, const std::string& restRaw) {
        std::string rest = substituteFormulaAttrs(restRaw, msg);   // 展开 +db 等公式属性
        // Separate the leading dice expression from an optional reason.
        std::string diceToken = readDiceToken(rest);
        std::string expr, reason;
        if (hasNonDigit(diceToken)) {
            expr = diceToken;
            reason = trim(rest.substr(diceToken.size()));
        } else {
            // Pure number or empty → not a dice expression; whole rest is the reason.
            expr.clear();
            reason = rest;
        }
        // C#63: 裸 d/D（无面数，如 .rd / .r d）视同「掷默认骰」，与 .r 一致——落入下方
        // 默认骰逻辑（.set 默认骰 / 本群默认骰生效），否则引擎会把裸 d 当死板 d100。
        if (expr == "d" || expr == "D") expr.clear();

        // Multi-roll: "N#expr"
        int turns = 1;
        auto hashPos = expr.find('#');
        if (hashPos != std::string::npos) {
            turns = parseIntOr(expr.substr(0, hashPos), 1);
            expr = expr.substr(hashPos + 1);
        }
        if (turns < 1) turns = 1;
        // Multi-roll is capped at 10. Tell the user when they exceed it rather
        // than silently clamping (".r 12N#" → error stating the max).
        if (turns > 10)
            return i18n_.tr(loc, "dice.roll.too_many",
                {{"max", "10"}, {"n", std::to_string(turns)}});
        // COC weapon binding: ".r 手枪" rolls the damage expression saved via
        // ".st &手枪=1d6" (kept as a string, rolled fresh each time).
        if (expr.empty() && !reason.empty()) {
            std::string wexpr = getUserSetting(msg, "wpn:" + trim(reason));
            if (!wexpr.empty()) expr = wexpr;     // reason stays as the weapon name label
        }
        if (expr.empty()) {                       // default die (.set override)
            std::string df = getUserSetting(msg, "defaultDice");
            if (df.empty()) df = getGroupSetting(msg, "groupDefaultDice");  // 本群规则默认骰
            expr = "d" + (df.empty() ? std::string("100") : df);
        }

        const std::string nick = displayName(msg);

        // C#107：轮盘骰——GM 用 .game rou N 启用后，本群纯 1dN/dN 掷骰改为袋中
        // 不放回抽取（防「重骰刷点」，原版 DiceRoulette）。仅拦单骰、单轮。
        if (turns == 1 && msg.type == MessageType::kGroup) {
            std::string le = toLower(expr);
            if (le.rfind("1d", 0) == 0) le = le.substr(1);
            if (le.size() > 1 && le[0] == 'd' && le.find_first_not_of("0123456789", 1) == std::string::npos) {
                int face = parseIntOr(le.substr(1), 0);
                if (face > 0) if (auto rv = rouletteDraw(msg, face)) {
                    std::string res = "1D" + std::to_string(face) + "=" + std::to_string(*rv);
                    return i18n_.tr(loc, reason.empty() ? "dice.roll.result" : "dice.roll.result_reason",
                        {{"nick", nick}, {"reason", reason}, {"res", res}});
                }
            }
        }

        // 命运骰 NdF (Fate / Fudge dice)
        if (auto fate = tryFate(expr)) {
            return i18n_.tr(loc, reason.empty() ? "dice.roll.result" : "dice.roll.result_reason",
                {{"nick", nick}, {"reason", reason}, {"res", *fate}});
        }

        if (turns == 1) {
            auto result = engine_.roll(expr);
            std::string res;
            if (result.ok()) {
                res = result.formattedOutput;  // 原版 Dice! 格式优先
            } else {
                // Fall back to the OneDice standard engine for richer expressions
                // the original parser doesn't handle (kh/kl、a/c 骰池、max/min …).
                auto od = onedice::eval(expr, 100);
                if (!od.ok) return i18n_.tr(loc, "dice.error.roll", {{"error", result.error}});
                res = od.detail;
            }
            return i18n_.tr(loc, reason.empty() ? "dice.roll.result" : "dice.roll.result_reason",
                {{"nick", nick}, {"reason", reason}, {"res", res}});
        }

        // Multi-roll: one result per line (each line but the last ends with ", ").
        std::ostringstream res;
        for (int i = 0; i < turns; ++i) {
            auto result = engine_.roll(expr);
            if (!result.ok()) {
                return i18n_.tr(loc, "dice.error.roll", {{"error", result.error}});
            }
            if (i > 0) res << ", \n";
            res << result.formattedOutput;
        }
        return i18n_.tr(loc, reason.empty() ? "dice.roll.multi" : "dice.roll.multi_reason",
            {{"nick", nick}, {"reason", reason},
             {"turn", std::to_string(turns)}, {"res", "\n" + res.str()}});
    }

    /// Read the leading dice-expression token. Includes all ASCII alphanumerics
    /// (so OneDice operators like kh/kl/a/c/max work when glued, e.g. "4d6kh3")
    /// plus arithmetic/tuple symbols. Stops at a space or any non-ASCII byte
    /// (e.g. a Chinese reason), so ".r3d6攻击" still splits expr/reason correctly.
    static std::string readDiceToken(const std::string& s) {
        size_t i = 0;
        while (i < s.size()) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            bool ok = std::isalnum(c) ||
                c=='+'||c=='-'||c=='*'||c=='/'||c=='^'||
                c=='('||c==')'||c=='['||c==']'||c==','||c=='#';
            if (!ok) break;
            ++i;
        }
        return s.substr(0, i);
    }

    static bool hasNonDigit(const std::string& s) {
        if (s.empty()) return false;
        for (char c : s)
            if (!std::isdigit(static_cast<unsigned char>(c))) return true;
        return false;
    }

    // ─── Skill-check success levels (faithful to original RollSuccessLevel) ──

    enum class SuccessLevel { kFumble, kFailure, kRegular, kHard, kExtreme, kCritical };

    /// COC7 success level under house rule @p rule (0-7), ported verbatim from
    /// the original RD::RollSuccessLevel. Rule is set per-group/user via .setcoc.
    static SuccessLevel rollSuccessLevel(int res, int rate, int rule = 0) {
        using S = SuccessLevel;
        switch (rule) {
        case 1:
            if (res == 100) return S::kFumble;
            if (res == 1 || (res <= 5 && rate >= 50)) return S::kCritical;
            if (res <= rate / 5) return S::kExtreme;
            if (res <= rate / 2) return S::kHard;
            if (res <= rate)     return S::kRegular;
            if (rate >= 50 || res < 96) return S::kFailure;
            return S::kFumble;
        case 2:
            if (res == 100) return S::kFumble;
            if (res <= 5 && res <= rate) return S::kCritical;
            if (res <= rate / 5) return S::kExtreme;
            if (res <= rate / 2) return S::kHard;
            if (res <= rate)     return S::kRegular;
            if (res < 96) return S::kFailure;
            return S::kFumble;
        case 3:
            if (res >= 96) return S::kFumble;
            if (res <= 5)  return S::kCritical;
            if (res <= rate / 5) return S::kExtreme;
            if (res <= rate / 2) return S::kHard;
            if (res <= rate)     return S::kRegular;
            return S::kFailure;
        case 4:
            if (res == 100) return S::kFumble;
            if (res <= 5 && res <= rate / 10) return S::kCritical;
            if (res <= rate / 5) return S::kExtreme;
            if (res <= rate / 2) return S::kHard;
            if (res <= rate)     return S::kRegular;
            if (rate >= 50 || res < 96 + rate / 10) return S::kFailure;
            return S::kFumble;
        case 5:
            if (res >= 99) return S::kFumble;
            if (res <= 2 && res < rate / 10) return S::kCritical;
            if (res <= rate / 5) return S::kExtreme;
            if (res <= rate / 2) return S::kHard;
            if (res <= rate)     return S::kRegular;
            if (rate >= 50 || res < 96) return S::kFailure;
            return S::kFumble;
        case 6:
            if (res > rate) return (res == 100 || res % 11 == 0) ? S::kFumble : S::kFailure;
            return (res == 1 || res % 11 == 0) ? S::kCritical : S::kRegular;
        case 7:
            if (res >= 100) return S::kFumble;
            if (res >= 96)  return ((90 - rate) / 20 + res >= 100) ? S::kFumble : S::kFailure;
            if (res == 1 || res <= rate / 5) return S::kExtreme;
            if (res <= rate) return S::kRegular;
            return S::kFailure;
        case 0:
        default:
            if (res == 100) return S::kFumble;
            if (res == 1)   return S::kCritical;
            if (res <= rate / 5) return S::kExtreme;
            if (res <= rate / 2) return S::kHard;
            if (res <= rate)     return S::kRegular;
            if (rate >= 50 || res < 96) return S::kFailure;
            return S::kFumble;
        }
    }

    const char* levelKey(SuccessLevel lv) const {
        switch (lv) {
            case SuccessLevel::kCritical: return "dice.level.critical";
            case SuccessLevel::kExtreme:  return "dice.level.extreme";
            case SuccessLevel::kHard:     return "dice.level.hard";
            case SuccessLevel::kRegular:  return "dice.level.regular";
            case SuccessLevel::kFailure:  return "dice.level.failure";
            case SuccessLevel::kFumble:   return "dice.level.fumble";
        }
        return "dice.level.failure";
    }

    // .rx 心理学暗骰：每个成功等级对应一句自定义回执（参考原版 rx.lua）。
    const char* rxReceiptKey(SuccessLevel lv) const {
        switch (lv) {
            case SuccessLevel::kCritical: return "dice.rx.msg.critical";
            case SuccessLevel::kExtreme:  return "dice.rx.msg.extreme";
            case SuccessLevel::kHard:     return "dice.rx.msg.hard";
            case SuccessLevel::kRegular:  return "dice.rx.msg.regular";
            case SuccessLevel::kFailure:  return "dice.rx.msg.failure";
            case SuccessLevel::kFumble:   return "dice.rx.msg.fumble";
        }
        return "dice.rx.msg.failure";
    }

    /// 命运骰 NdF: N fudge dice each {-1,0,+1}. Returns formatted "NdF=[+ - 0]=sum".
    std::optional<std::string> tryFate(const std::string& expr) {
        static const std::regex re(R"(^(\d*)[dD][fF]$)");
        std::smatch m;
        if (!std::regex_match(expr, m, re)) return std::nullopt;
        int n = m[1].str().empty() ? 4 : parseIntOr(m[1].str(), 4);
        if (n < 1) n = 1; if (n > 100) n = 100;
        int sum = 0;
        std::ostringstream ss;
        ss << n << "dF=[";
        for (int i = 0; i < n; ++i) {
            int v = engine_.roll("1d3").modifiedTotal - 2;   // 1..3 → -1..+1
            sum += v;
            if (i) ss << " ";
            ss << (v > 0 ? "+" : v < 0 ? "-" : "0");
        }
        ss << "]=" << sum;
        return ss.str();
    }

    // ─── Dice pool: .ww (success pool, d10, configurable explosion) ──

    std::optional<std::string> tryHandleWW(Locale loc, const Message& msg, const std::string& cmd) {
        if (toLower(cmd).rfind("ww", 0) != 0) return std::nullopt;
        std::string rest = trim(cmd.substr(2));
        // Legacy: .ww N [success-line] (10 explodes).  Extended Shiki-style:
        // .ww NaAcS+X — a=add/explosion line (default 10), c=success line
        // (default 8), +X=extra successes.  Whitespace is optional in the
        // extended form, so `.ww 10 a8 c7 +2` equals `.ww 10a8c7+2`.
        int n = 0, addLine = 10, successLine = 8, bonus = 0;
        bool explicitAdd = false, explicitSuccess = false, explicitBonus = false;
        const bool extended = rest.find_first_of("aAcC+") != std::string::npos;
        if (!extended) {
            std::istringstream iss(rest);
            iss >> n;
            if (iss) { int t; if (iss >> t) successLine = t; }
            std::string extra;
            if (iss >> extra) return i18n_.tr(loc, "dice.pool.usage");
        } else {
            std::string spec;
            for (unsigned char ch : rest) if (!std::isspace(ch)) spec.push_back(static_cast<char>(ch));
            size_t p = 0;
            auto readNumber = [&](int& out) {
                if (p >= spec.size() || !std::isdigit(static_cast<unsigned char>(spec[p]))) return false;
                long long value = 0;
                while (p < spec.size() && std::isdigit(static_cast<unsigned char>(spec[p]))) {
                    value = value * 10 + (spec[p++] - '0');
                    if (value > 1000000) return false;
                }
                out = static_cast<int>(value);
                return true;
            };
            if (!readNumber(n)) return i18n_.tr(loc, "dice.pool.usage");
            while (p < spec.size()) {
                char part = static_cast<char>(std::tolower(static_cast<unsigned char>(spec[p++])));
                int value = 0;
                if (part == 'a' && !explicitAdd && readNumber(value)) {
                    addLine = value; explicitAdd = true;
                } else if (part == 'c' && !explicitSuccess && readNumber(value)) {
                    successLine = value; explicitSuccess = true;
                } else if (part == '+' && !explicitBonus && readNumber(value)) {
                    bonus = value; explicitBonus = true;
                } else {
                    return i18n_.tr(loc, "dice.pool.usage");
                }
            }
        }
        if (n < 1 || addLine < 2 || addLine > 10 || successLine < 1 || successLine > 10 || bonus < 0)
            return i18n_.tr(loc, "dice.pool.usage");
        if (n > 100) n = 100;

        // Keep the concise form users typed whenever it has the old defaults;
        // otherwise surface every active rule in the reply so the result can be
        // independently checked from the message alone.
        std::ostringstream notation;
        notation << n;
        if (explicitAdd || addLine != 10) notation << "a" << addLine;
        if (explicitSuccess || successLine != 8) notation << "c" << successLine;
        if (bonus) notation << "+" << bonus;

        static constexpr const char* kCircled[] = {
            "", "①", "②", "③", "④", "⑤", "⑥", "⑦", "⑧", "⑨", "⑩"
        };
        std::vector<std::vector<int>> rounds;
        std::vector<int> roundSuccesses;
        int successes = bonus, pool = n, rolled = 0;
        for (int round = 0; pool > 0 && round < 50 && rolled < 100000; ++round) {
            std::vector<int> dice;
            int explode = 0, thisRound = 0;
            for (int i = 0; i < pool && rolled < 100000; ++i, ++rolled) {
                int v = engine_.roll("1d10").modifiedTotal;  // 1..10
                dice.push_back(v);
                if (v >= successLine) { ++thisRound; ++successes; }
                if (v >= addLine) ++explode;
            }
            rounds.push_back(std::move(dice));
            roundSuccesses.push_back(thisRound);
            pool = explode;
        }

        std::ostringstream expr;
        expr << notation.str() << "=";
        for (size_t r = 0; r < rounds.size(); ++r) {
            if (r) expr << "+";
            expr << "{";
            for (size_t i = 0; i < rounds[r].size(); ++i) {
                if (i) expr << ",";
                const int v = rounds[r][i];
                expr << (v >= successLine ? kCircled[v] : std::to_string(v));
            }
            expr << "}";
        }
        if (bonus) expr << "+" << bonus;
        expr << "=";
        for (size_t r = 0; r < roundSuccesses.size(); ++r) {
            if (r) expr << "+";
            expr << roundSuccesses[r];
        }
        if (bonus) expr << "+" << bonus;
        expr << "=" << successes;

        return i18n_.tr(loc, "dice.pool.result",
            {{"nick", displayName(msg)}, {"expr", expr.str()}});
    }

    // ─── Double cross: .dx (routes to OneDice 'c' operator) ──

    /// 旧版 Dice 使用 `.rdx`，Dice!Next 早期实现使用 `.dx`；两者完全同义。
    /// `.rdx` 以 r 开头，若不在通用 `.r` 前拦截会被误当作骰子表达式。
    std::optional<std::string> tryHandleRdx(Locale loc, const Message& msg, const std::string& cmd) {
        if (toLower(cmd).rfind("rdx", 0) != 0) return std::nullopt;
        return tryHandleDX(loc, msg, cmd.substr(1));
    }

    std::optional<std::string> tryHandleDX(Locale loc, const Message& msg, const std::string& cmd) {
        if (toLower(cmd).rfind("dx", 0) != 0) return std::nullopt;
        std::string rest = trim(cmd.substr(2));

        std::string lc = toLower(rest);
        // a = OneDice 无限加骰骰池（WoD/数成功，≥加骰线重骰累加、数≥成功线个数），
        // 不同于双十字(c 求和)。交给 OneDice 引擎求值并展示全过程（与 .r 一致）。
        if (lc.find('a') != std::string::npos) {
            auto od = onedice::eval(rest, 10);   // 池默认 d10
            if (!od.ok) return i18n_.tr(loc, "dice.dx.usage");
            return i18n_.tr(loc, "dice.roll.result",
                {{"nick", displayName(msg)}, {"reason", ""}, {"res", od.detail}});
        }

        // Parse "AcB" (A dice, B critical line) or "A [B]" (default B=10).
        int pool = 0, crit = 10, faces = 10;
        if (auto cp = lc.find('c'); cp != std::string::npos) {
            pool = parseIntOr(trim(rest.substr(0, cp)), 0);
            crit = parseIntOr(trim(rest.substr(cp + 1)), 10);
        } else {
            std::istringstream iss(rest);
            iss >> pool; if (iss) { int kk; if (iss >> kk) crit = kk; }
        }
        if (pool < 1) return i18n_.tr(loc, "dice.dx.usage");
        if (pool > 100) pool = 100;
        if (crit < 2) crit = 2;           // a critical line of 1 would loop forever
        if (crit > faces) crit = faces;

        // Replicate the OneDice double-cross algorithm (onedice.cpp applyCross),
        // but keep every round's dice so we can show the full process.
        std::vector<std::vector<int>> rounds;
        int live = pool, guard = 0;
        for (int r = 0; r < 100 && live > 0 && guard < 100000; ++r) {
            std::vector<int> dice; int next = 0;
            for (int i = 0; i < live; ++i) {
                int v = engine_.roll("1d" + std::to_string(faces)).modifiedTotal;
                dice.push_back(v); ++guard;
                if (v >= crit) ++next;
            }
            rounds.push_back(dice);
            live = next;
        }
        int total = 0;
        if (!rounds.empty()) {
            int mx = 0; for (int v : rounds.back()) if (v > mx) mx = v;
            total = (int)(rounds.size() - 1) * faces + mx;
        }

        // Build a readable process: a critical die is shown as <value,critical-line>
        // rather than a trailing '*', so both the trigger and its threshold are clear.
        std::ostringstream proc;
        proc << pool << "c" << crit << "=";
        for (size_t r = 0; r < rounds.size(); ++r) {
            if (r) proc << "+";
            proc << "{";
            for (size_t i = 0; i < rounds[r].size(); ++i) {
                if (i) proc << ",";
                const int value = rounds[r][i];
                if (value >= crit) proc << "<" << value << "," << crit << ">";
                else proc << value;
            }
            proc << "}";
        }
        proc << "=" << total;

        const std::string nick = displayName(msg);
        return i18n_.tr(loc, "dice.roll.result", {{"nick", nick}, {"reason", ""}, {"res", proc.str()}});
    }

    // ─── Opposed check: .rav / .rcv ──────────────────────────

    static int levelRank(SuccessLevel lv) {
        switch (lv) {
            case SuccessLevel::kCritical: return 5;
            case SuccessLevel::kExtreme:  return 4;
            case SuccessLevel::kHard:     return 3;
            case SuccessLevel::kRegular:  return 2;
            case SuccessLevel::kFailure:  return 1;
            case SuccessLevel::kFumble:   return 0;
        }
        return 0;
    }

    bool resolveSide(Locale loc, const Message& msg, const std::string& tok,
                     int& rate, std::string& label, std::string& err) {
        if (tok.empty()) { err = i18n_.tr(loc, "dice.rav.usage"); return false; }
        if (isAllDigits(tok)) { rate = parseIntOr(tok, 0); label = tok; return true; }
        auto v = cards_.getAttr(msg.senderId, cardScope(msg), tok);
        if (!v) { err = i18n_.tr(loc, "dice.check.no_card", {{"attr", tok}}); return false; }
        rate = *v; label = tok; return true;
    }

    std::optional<std::string> tryHandleRAV(Locale loc, const Message& msg, const std::string& cmd) {
        std::string lc = toLower(cmd);
        if (lc.rfind("rav", 0) != 0 && lc.rfind("rcv", 0) != 0) return std::nullopt;
        std::string rest = trim(cmd.substr(3));

        std::vector<std::string> toks;
        std::istringstream iss(rest);
        std::string t;
        while (iss >> t) { std::string tl = toLower(t); if (tl == "vs") continue; toks.push_back(t); }
        if (toks.size() < 2) return i18n_.tr(loc, "dice.rav.usage");

        int rateA = 0, rateB = 0; std::string la, lb, err;
        if (!resolveSide(loc, msg, toks[0], rateA, la, err)) return err;
        if (!resolveSide(loc, msg, toks[1], rateB, lb, err)) return err;

        auto rA = engine_.roll("1d100");
        auto rB = engine_.roll("1d100");
        int crule = getCocRule(msg);
        SuccessLevel lvA = rollSuccessLevel(rA.modifiedTotal, rateA, crule);
        SuccessLevel lvB = rollSuccessLevel(rB.modifiedTotal, rateB, crule);
        int rankA = levelRank(lvA), rankB = levelRank(lvB);

        std::string outcomeKey;
        if (rankA > rankB) outcomeKey = "dice.rav.a_wins";
        else if (rankB > rankA) outcomeKey = "dice.rav.b_wins";
        else if (rankA >= 2 && rA.modifiedTotal != rB.modifiedTotal)
            outcomeKey = (rA.modifiedTotal < rB.modifiedTotal) ? "dice.rav.a_wins" : "dice.rav.b_wins";
        else outcomeKey = "dice.rav.tie";

        const std::string nick = displayName(msg);
        return i18n_.tr(loc, "dice.rav.result", {
            {"nick", nick},
            {"la", la}, {"ra", std::to_string(rA.modifiedTotal)},
            {"va", std::to_string(rateA)}, {"lva", i18n_.tr(loc, levelKey(lvA))},
            {"lb", lb}, {"rb", std::to_string(rB.modifiedTotal)},
            {"vb", std::to_string(rateB)}, {"lvb", i18n_.tr(loc, levelKey(lvB))},
            {"outcome", i18n_.tr(loc, outcomeKey, {{"a", la}, {"b", lb}})}
        });
    }

    // ─── BRP（基础角色扮演）规则 .ba / .bav ──────────────────────
    // d100 低骰：roll ≤ 技能 成功。等级（参考 BRP 检定表）：
    //   大成功 ≤ 技能/20(至少1) · 特殊成功 ≤ 技能/5 · 成功 ≤ 技能 · 失败 > 技能
    //   大失败：掷出 100 必为大失败；失败且掷出 99 也为大失败。
    static SuccessLevel brpSuccessLevel(int roll, int skill) {
        if (skill < 1) skill = 1;
        if (roll >= 100) return SuccessLevel::kFumble;                 // 100 → 大失败
        if (roll > skill) return (roll == 99) ? SuccessLevel::kFumble  // 失败且 99 → 大失败
                                              : SuccessLevel::kFailure; // 失败
        int crit = skill / 20; if (crit < 1) crit = 1;                 // 大成功 ≤ 技能/20
        int spec = skill / 5;                                          // 特殊成功 ≤ 技能/5
        if (roll <= crit) return SuccessLevel::kCritical;
        if (roll <= spec) return SuccessLevel::kExtreme;               // 复用 kExtreme = 特殊成功
        return SuccessLevel::kRegular;                                 // 成功
    }
    const char* brpLevelKey(SuccessLevel lv) const {
        switch (lv) {
            case SuccessLevel::kCritical: return "dice.brp.critical";   // 大成功
            case SuccessLevel::kExtreme:  return "dice.brp.special";    // 特殊成功
            case SuccessLevel::kRegular:  return "dice.brp.success";    // 成功
            case SuccessLevel::kFumble:   return "dice.brp.fumble";     // 大失败
            default:                      return "dice.brp.failure";    // 失败
        }
    }

    // 派发：.ba（检定）/ .bav（对抗：BRP 抵抗表）。须在通用回退之前。
    std::optional<std::string> tryHandleBrp(Locale loc, const Message& msg, const std::string& cmd) {
        std::string lc = toLower(cmd);
        if (lc.rfind("bav", 0) == 0) {                 // .bav 对抗（抵抗表）
            if (lc.size() > 3 && std::isalpha(static_cast<unsigned char>(lc[3]))) return std::nullopt;
            return handleBrpResist(loc, msg, trim(cmd.substr(3)));
        }
        if (lc.rfind("ba", 0) == 0) {                  // .ba 检定
            if (lc.size() > 2 && std::isalpha(static_cast<unsigned char>(lc[2]))) return std::nullopt;
            return handleBrpCheck(loc, msg, trim(cmd.substr(2)));
        }
        return std::nullopt;
    }

    // .ba <技能|属性> [成功率] [原因]，支持 @某人 代骰、内联成功率、连掷 N#。
    std::optional<std::string> handleBrpCheck(Locale loc, const Message& msg, const std::string& restRaw) {
        std::string s = trim(restRaw);
        int multi = 1;
        if (auto hp = s.find('#'); hp != std::string::npos) {
            std::string n = s.substr(0, hp);
            if (isAllDigits(n)) { multi = parseIntOr(n, 1); s = trim(s.substr(hp + 1)); }
        }
        if (multi < 1) multi = 1; if (multi > 10) multi = 10;

        std::string target = atTarget(msg);
        std::string attr, reason, err; int rate = 0;
        if (!resolveRate(loc, msg, s, attr, rate, reason, err, target)) return err;
        std::string forWhom = target.empty() ? "" : "@" + target + " ";
        std::string label = forWhom + attr;
        const std::string nick = displayName(msg);

        std::string out;
        for (int i = 0; i < multi; ++i) {
            auto r = engine_.roll("1d100");
            if (!r.ok()) return i18n_.tr(loc, "dice.error.roll", {{"error", r.error}});
            SuccessLevel lv = brpSuccessLevel(r.modifiedTotal, rate);
            recordRollStat(msg, attr, lv);
            if (i) out += "\n";
            out += i18n_.tr(loc, reason.empty() ? "dice.brp.result" : "dice.brp.result_reason",
                {{"nick", nick}, {"attr", label}, {"reason", reason},
                 {"roll", r.formattedOutput}, {"rate", std::to_string(rate)},
                 {"level", i18n_.tr(loc, brpLevelKey(lv))}});
        }
        return out;
    }

    // .bav <主动> <被动>：BRP 抵抗表。目标% = 50 + (主动 − 被动)×5，1d100 ≤ 目标 → 主动方成功。
    std::optional<std::string> handleBrpResist(Locale loc, const Message& msg, const std::string& rest) {
        std::vector<std::string> toks;
        std::istringstream iss(rest); std::string t;
        while (iss >> t) { std::string tl = toLower(t); if (tl == "vs" || tl == "对" || tl == "\xe5\xaf\xb9\xe6\x8a\x97") continue; toks.push_back(t); }
        if (toks.size() < 2) return i18n_.tr(loc, "dice.brp.resist_usage");

        int actV = 0, pasV = 0; std::string la, lb, err;
        if (!resolveSide(loc, msg, toks[0], actV, la, err)) return err;
        if (!resolveSide(loc, msg, toks[1], pasV, lb, err)) return err;
        int targetPct = 50 + (actV - pasV) * 5;        // 抵抗表目标值
        int shown = targetPct < 1 ? 1 : (targetPct > 99 ? 99 : targetPct);  // 显示用钳到 1..99（仍各留一线）
        auto r = engine_.roll("1d100");
        if (!r.ok()) return i18n_.tr(loc, "dice.error.roll", {{"error", r.error}});
        bool win = r.modifiedTotal <= targetPct;       // 实际判定用未钳目标
        const std::string nick = displayName(msg);
        return i18n_.tr(loc, "dice.brp.resist", {
            {"nick", nick}, {"la", la}, {"lb", lb},
            {"va", std::to_string(actV)}, {"vb", std::to_string(pasV)},
            {"target", std::to_string(shown)}, {"roll", r.formattedOutput},
            {"outcome", i18n_.tr(loc, win ? "dice.brp.resist_win" : "dice.brp.resist_lose", {{"a", la}, {"b", lb}})}
        });
    }

    std::optional<std::string> tryHandleCheck(Locale loc, const Message& msg,
                                              const std::string& cmd) {
        if (cmd.size() < 2) return std::nullopt;
        char c0 = static_cast<char>(std::tolower(static_cast<unsigned char>(cmd[0])));
        char c1 = static_cast<char>(std::tolower(static_cast<unsigned char>(cmd[1])));
        if (c0 != 'r' || (c1 != 'a' && c1 != 'c')) return std::nullopt;

        size_t j = 2;
        bool hidden = false;
        // .rah / .rch 暗检定: 'h' right after ra/rc, BUT only when the next char
        // isn't an ASCII letter — so ".ra hp"/".rahp" still read attribute "hp".
        if (j < cmd.size() && (cmd[j] == 'h' || cmd[j] == 'H')) {
            char nx = (j + 1 < cmd.size()) ? cmd[j + 1] : ' ';
            if (!std::isalpha(static_cast<unsigned char>(nx))) { hidden = true; ++j; }
        }
        // In DND mode, .rc is a d20 ability/skill check (.ra stays COC d100).
        if (c1 == 'c' && dndModeOn(msg)) return handleDndCheck(loc, msg, trim(cmd.substr(j)));
        // 黏着形（".rapass23"，b/p 与 ra/rc 之间无空格）→ b/p 可吃后随 ASCII 字母技能，
        // 即 ".rapass23" 按「最长指令优先」= .rap ass 23；空格形 ".ra pass 23" 仍检定 pass。
        bool glued = j < cmd.size() && cmd[j] != ' ';
        return handleCheck(loc, msg, trim(cmd.substr(j)), hidden, glued);
    }

    /// Parse ".ra" arguments: `<属性>[成功率] [原因]`. Whitespace is the separator
    /// (faithful to original), so a trailing reason is NEVER folded into the
    /// attribute/value. Forms handled: "力量"、"力量 哈哈"、"力量 60"、"力量 60 攻击"、
    /// "力量60"、"力量60 攻击"、"60"（仅数值）.
    /// Returns false (and fills @p err) if no rate can be determined.
    bool resolveRate(Locale loc, const Message& msg, const std::string& rest,
                     std::string& attr, int& rate, std::string& reason, std::string& err,
                     const std::string& cardUser = "") {
        const std::string group = cardScope(msg);
        const std::string owner = cardUser.empty() ? msg.senderId : cardUser;  // .ra @某人 用对方卡
        std::string s = trim(rest);
        // First whitespace-delimited token = the attribute (may carry an inline rate).
        size_t sp = s.find(' ');
        std::string first = (sp == std::string::npos) ? s : s.substr(0, sp);
        std::string remainder = (sp == std::string::npos) ? "" : trim(s.substr(sp + 1));

        // Quick adjust (青果): "技能+10" / "技能-20" → rate = 卡值(技能) ± N.
        // Name part must be non-numeric, adjust part must be sign + digits.
        {
            size_t pm = first.find_first_of("+-");
            if (pm != std::string::npos && pm > 0) {
                std::string nm = first.substr(0, pm), adj = first.substr(pm);
                bool adjOk = adj.size() >= 2;
                for (size_t i = 1; i < adj.size() && adjOk; ++i)
                    if (!std::isdigit(static_cast<unsigned char>(adj[i]))) adjOk = false;
                if (adjOk && nm.find_first_of("0123456789") == std::string::npos) {
                    auto v = cards_.getAttr(owner, group, nm);
                    if (!v && owner == msg.senderId && group == cardScope(msg)) v = derivedAttr(msg, nm);  // model.xml 衍生值
                    if (!v) v = defaultAttr(nm);   // C#102：未录入 → 规则默认值（如 急救30）
                    if (!v) { err = i18n_.tr(loc, "dice.check.no_card", {{"attr", nm}}); return false; }
                    rate = *v + parseIntOr(adj, 0);
                    if (rate < 1) rate = 1;
                    attr = nm + adj;          // display "侦查+10"
                    reason = remainder;
                    return true;
                }
            }
        }

        // Inline rate inside the first token, e.g. "力量60".
        size_t d = first.find_first_of("0123456789");
        if (d != std::string::npos) {
            attr = trim(first.substr(0, d));
            size_t e = d;
            while (e < first.size() && std::isdigit(static_cast<unsigned char>(first[e]))) ++e;
            rate = parseIntOr(first.substr(d, e - d), 0);
            std::string tail = trim(first.substr(e));   // anything after the digits in the token
            reason = trim(tail.empty() ? remainder : tail + " " + remainder);
            return true;
        }

        // First token is a pure name.
        attr = first;
        if (attr.empty()) { err = i18n_.tr(loc, "dice.check.need_value"); return false; }

        // Is the NEXT token a number? → that's the rate; the rest is the reason.
        if (!remainder.empty()) {
            size_t sp2 = remainder.find(' ');
            std::string t1 = (sp2 == std::string::npos) ? remainder : remainder.substr(0, sp2);
            if (isAllDigits(t1)) {
                rate = parseIntOr(t1, 0);
                reason = (sp2 == std::string::npos) ? "" : trim(remainder.substr(sp2 + 1));
                return true;
            }
        }

        // No inline/next rate → read the value from the card; remainder is the reason.
        auto v = cards_.getAttr(owner, group, attr);
        if (!v && owner == msg.senderId && group == cardScope(msg)) v = evalStrAttr(msg, CharacterCardStore::canonical(attr));  // C#37 关联属性
        if (!v && owner == msg.senderId && group == cardScope(msg)) v = derivedAttr(msg, attr);  // model.xml 衍生值
        if (!v) v = defaultAttr(attr);   // C#102：未录入 → 规则默认值（如 急救30、聆听20）
        if (!v) { err = i18n_.tr(loc, "dice.check.no_card", {{"attr", attr}}); return false; }
        rate = *v;
        reason = remainder;
        return true;
    }

    /// Format a check reply given a precomputed roll (so .ra and .rb/.rp share it).
    std::string formatCheck(Locale loc, const Message& msg, const std::string& attr,
                            int rate, const std::string& reason,
                            const std::string& rollDetail, int rollValue) {
        SuccessLevel lv = rollSuccessLevel(rollValue, rate, getCocRule(msg));
        recordRollStat(msg, attr, lv);   // accumulate per-skill for .hiy 统计
        const std::string nick = displayName(msg);
        return i18n_.tr(loc, reason.empty() ? "dice.check.result" : "dice.check.result_reason",
            {{"nick", nick}, {"attr", attr}, {"reason", reason},
             {"roll", rollDetail}, {"rate", std::to_string(rate)},
             {"level", i18n_.tr(loc, levelKey(lv))}});
    }

    // ─── 好感度系统 (DiceFavor) ──────────────────────────────
    // Favor is stored per (platform, user) on the player profile.

    int getFavor(const std::string& platform, const std::string& uid) const {
        auto* st = db_.getStorage(); if (!st) return 0;
        try {
            namespace orm = sqlite_orm;
            auto rows = st->get_all<PlayerProfileRow>(
                orm::where(orm::c(&PlayerProfileRow::platform) == platform
                    and orm::c(&PlayerProfileRow::userId) == uid), orm::limit(1));
            if (!rows.empty()) return rows.front().favor;
        } catch (...) {}
        return 0;
    }
    void setFavorValue(const std::string& platform, const std::string& uid, int favor) {
        auto* st = db_.getStorage(); if (!st || uid.empty()) return;
        try {
            namespace orm = sqlite_orm;
            auto rows = st->get_all<PlayerProfileRow>(
                orm::where(orm::c(&PlayerProfileRow::platform) == platform
                    and orm::c(&PlayerProfileRow::userId) == uid), orm::limit(1));
            if (rows.empty()) {
                PlayerProfileRow r; r.platform = platform; r.userId = uid; r.favor = favor;
                r.createdAt = nowIso(); st->insert(r);
            } else { auto r = rows.front(); r.favor = favor; st->update(r); }
        } catch (...) {}
    }
    // ─── C：用户权限阶梯（对齐原版 nTrust）──────────────────────
    static constexpr int kTrustBanned  = -1;   // 拉黑（预留；另有 BanlistRow 黑名单）
    static constexpr int kTrustNormal  = 0;    // 普通用户
    static constexpr int kTrustTrusted = 1;    // 信任层（1-3）
    static constexpr int kTrustAdmin   = 4;    // 管理员
    static constexpr int kTrustSelf    = 255;  // 骰娘自身
    static constexpr int kTrustMaster  = 256;  // Master（骰主）

    /// C：账号别名（原版 TinyList）——把别名账号映射到主账号，信任跟人不跟号。
    /// config dice/aliases: [{platform, alias, main}]（platform 空=任意平台）。未命中返回原 id。
    std::string resolveAlias(const std::string& platform, const std::string& uid) const {
        try {
            auto arr = cfg_.get<nlohmann::json>("dice/aliases", nlohmann::json::array());
            if (arr.is_array())
                for (auto& a : arr) {
                    if (!a.is_object() || a.value("alias", std::string()) != uid) continue;
                    std::string p = a.value("platform", std::string());
                    if (p.empty() || p == platform) return a.value("main", uid);
                }
        } catch (...) {}
        return uid;
    }
    int getTrust(const std::string& platform, const std::string& uidRaw) const {
        auto* st = db_.getStorage(); if (!st || uidRaw.empty()) return 0;
        std::string uid = resolveAlias(platform, uidRaw);   // 别名 → 主号（原版 TinyList）
        try {
            namespace orm = sqlite_orm;
            auto rows = st->get_all<PlayerProfileRow>(
                orm::where(orm::c(&PlayerProfileRow::platform) == platform
                    and orm::c(&PlayerProfileRow::userId) == uid), orm::limit(1));
            if (!rows.empty()) return rows.front().trustLevel;
        } catch (...) {}
        return 0;
    }
    void setTrust(const std::string& platform, const std::string& uidRaw, int level) {
        auto* st = db_.getStorage(); if (!st || uidRaw.empty()) return;
        std::string uid = resolveAlias(platform, uidRaw);   // 别名写主号，信任跟人
        try {
            namespace orm = sqlite_orm;
            auto rows = st->get_all<PlayerProfileRow>(
                orm::where(orm::c(&PlayerProfileRow::platform) == platform
                    and orm::c(&PlayerProfileRow::userId) == uid), orm::limit(1));
            if (rows.empty()) { PlayerProfileRow r; r.platform = platform; r.userId = uid;
                                r.trustLevel = level; r.createdAt = nowIso(); st->insert(r); }
            else { auto r = rows.front(); r.trustLevel = level; st->update(r); }
        } catch (...) {}
    }
    // 有效权限等级：Master=256、骰娘自身=255，否则读人物档 trustLevel。所有权限门控用它。
    // 别名账号先归并到主号（原版 trustedQQ 里 TinyList 在 master 判断之前生效）。
    int trustOf(const std::string& platform, const std::string& uidRaw, const std::string& selfId = "") const {
        std::string uid = resolveAlias(platform, uidRaw);
        if (isMaster(platform, uid)) return kTrustMaster;
        if (!selfId.empty() && (uid == selfId || uidRaw == selfId)) return kTrustSelf;
        return getTrust(platform, uid);
    }
    int trustOf(const Message& msg) const { return trustOf(msg.platform, msg.senderId, msg.selfId); }
    /// C：发送者对本群的有效权限（对齐原版 getGroupTrust，DiceEvent.cpp:4695）：
    /// 个人信任 >0 直接用；否则 群管=0、普通成员=-1、非群上下文=-2。
    /// .group 选项等按门槛（原版 mChatConf：0=群管可设，2/3=信任层，4=管理员）门控。
    int groupTrustOf(const Message& msg) const {
        int t = trustOf(msg);
        if (t > 0) return t;
        if (msg.type == MessageType::kGroup && !msg.targetId.empty())
            return senderIsGroupAdmin(msg) ? 0 : -1;
        return -2;
    }

    int senderTrust(const Message& msg) const {
        auto* st = db_.getStorage(); if (!st) return 0;
        try {
            namespace orm = sqlite_orm;
            auto rows = st->get_all<PlayerProfileRow>(
                orm::where(orm::c(&PlayerProfileRow::platform) == msg.platform
                    and orm::c(&PlayerProfileRow::userId) == msg.senderId), orm::limit(1));
            if (!rows.empty()) return rows.front().trustLevel;
        } catch (...) {}
        return 0;
    }
    /// i18n title key for a favor value (tiered ladder).
    std::string favorTitleKey(int favor) const {
        if (favor < 0)    return "favor.title.hostile";
        if (favor < 30)   return "favor.title.stranger";
        if (favor < 60)   return "favor.title.friendly";
        if (favor < 100)  return "favor.title.close";
        if (favor < 200)  return "favor.title.intimate";
        return "favor.title.lover";
    }

    /// Faithful DiceFavor growth: roll(1..face); if roll<=favor and roll!=face → no
    /// growth (harder as favor rises); else gain 1d10. Returns the gain, or -1.
    int favorGrow(const std::string& platform, const std::string& uid) {
        int favor = getFavor(platform, uid);
        int face = 100;
        int roll = engine_.roll("1d" + std::to_string(face)).modifiedTotal;
        if (roll <= favor && roll != face) return -1;     // growth failed
        int g = engine_.roll("1d10").modifiedTotal;
        setFavorValue(platform, uid, favor + g);
        return g;
    }

    /// Resolve the display name for a profile row (nickname → id).
    std::string profileName(const PlayerProfileRow& r) const {
        return r.nickname.empty() ? r.userId : r.nickname;
    }

    // .favor / 好感 family. Detects English ".favor …" and Chinese 好感* prefixes.
    static bool isFavorCmd(const std::string& cmd) {
        std::string lc = toLower(cmd);
        if (lc.rfind("favor", 0) == 0) return true;
        const char* zh[] = {"\xe5\xa5\xbd\xe6\x84\x9f",                 // 好感
                            "\xe7\xbe\xa4\xe5\x86\x85\xe5\xa5\xbd\xe6\x84\x9f", // 群内好感
                            "\xe5\xbc\xba\xe5\x88\xb6\xe5\xa5\xbd\xe6\x84\x9f"}; // 强制好感
        for (auto* w : zh) if (cmd.rfind(w, 0) == 0) return true;
        return false;
    }

    std::optional<std::string> tryHandleFavor(Locale loc, const Message& msg, const std::string& cmd) {
        if (!isFavorCmd(cmd)) return std::nullopt;
        return handleFavor(loc, msg, cmd);
    }

    std::string handleFavor(Locale loc, const Message& msg, const std::string& cmd) {
        std::string lc = toLower(cmd);
        auto has = [&](const char* s) { return cmd.find(s) != std::string::npos; };
        // Determine subcommand.
        std::string sub;
        std::string rest;
        if (lc.rfind("favor", 0) == 0) {
            std::string r = trim(cmd.substr(5));
            auto sp = r.find(' ');
            std::string w = toLower(sp == std::string::npos ? r : r.substr(0, sp));
            if (w == "rank" || w == "grouprank" || w == "grow" || w == "add" || w == "set" ||
                w == "clr" || w == "clear") { sub = (w == "clear") ? "clr" : w; rest = (sp == std::string::npos) ? "" : trim(r.substr(sp + 1)); }
            else { sub = ""; rest = r; }   // bare show (r may hold @/number)
        } else {
            if (has("\xe6\x8e\x92\xe8\xa1\x8c")) sub = has("\xe7\xbe\xa4") ? "grouprank" : "rank"; // 排行 / 群
            else if (has("\xe6\x88\x90\xe9\x95\xbf")) sub = "grow";   // 成长
            else if (has("\xe8\xa6\x86\xe5\x86\x99")) sub = "set";    // 覆写
            else if (has("\xe6\x93\xa6\xe9\x99\xa4") || has("\xe6\xb8\x85\xe9\x99\xa4")) sub = "clr"; // 擦除/清除
            else if (has("\xe5\xa2\x9e\xe5\x8a\xa0") || cmd.rfind("\xe5\xbc\xba\xe5\x88\xb6\xe5\xa5\xbd\xe6\x84\x9f", 0) == 0) sub = "add"; // 增加/强制好感
            else sub = "";
            rest = cmd;
        }

        if (sub == "rank") return favorRank(loc, msg);
        if (sub == "grouprank") return favorGroupRank(loc, msg);

        // Numbers + @target for the remaining ops.
        std::vector<long long> nums;
        { std::string cur; auto flush = [&]() { if (!cur.empty()) { try { nums.push_back(std::stoll(cur)); } catch (...) {} cur.clear(); } };
          for (size_t i = 0; i < rest.size(); ++i) { char c = rest[i];
            if (std::isdigit((unsigned char)c) || (c == '-' && cur.empty())) cur += c; else flush(); } flush(); }
        std::string at = atTarget(msg);
        const std::string self = msg.senderId;

        if (sub.empty()) {   // show
            std::string subj = !at.empty() ? at : (!nums.empty() ? std::to_string(nums[0]) : self);
            int favor = getFavor(msg.platform, subj);
            return i18n_.tr(loc, subj == self ? "favor.show" : "favor.show_other",
                {{"nick", displayName(msg)}, {"target", subj}, {"self", botSelfName(msg)},
                 {"favor", std::to_string(favor)}, {"title", i18n_.tr(loc, favorTitleKey(favor))}});
        }

        // Master/trusted-only operations.
        const bool perm = isMaster(msg) || senderTrust(msg) >= 4;
        if (!perm) return i18n_.tr(loc, "favor.no_perm");

        if (sub == "grow") {
            std::string subj = !at.empty() ? at : (!nums.empty() ? std::to_string(nums[0]) : self);
            int g = favorGrow(msg.platform, subj);
            if (g < 0) return i18n_.tr(loc, "favor.no_grow", {{"target", subj}});
            return i18n_.tr(loc, "favor.grew", {{"target", subj}, {"add", std::to_string(g)},
                {"favor", std::to_string(getFavor(msg.platform, subj))}});
        }
        if (sub == "add") {
            int val = !nums.empty() ? (int)nums[0] : 100;
            std::string subj = !at.empty() ? at : (nums.size() >= 2 ? std::to_string(nums[1]) : self);
            int nf = getFavor(msg.platform, subj) + val;
            setFavorValue(msg.platform, subj, nf);
            return i18n_.tr(loc, "favor.added", {{"target", subj}, {"add", std::to_string(val)}, {"favor", std::to_string(nf)}});
        }
        if (sub == "set") {
            if (nums.empty()) return i18n_.tr(loc, "favor.value_empty");
            int val = (int)nums[0];
            std::string subj = !at.empty() ? at : (nums.size() >= 2 ? std::to_string(nums[1]) : self);
            setFavorValue(msg.platform, subj, val);
            return i18n_.tr(loc, "favor.set", {{"target", subj}, {"favor", std::to_string(val)}});
        }
        if (sub == "clr") {
            std::string subj = !at.empty() ? at : (!nums.empty() ? std::to_string(nums[0]) : self);
            setFavorValue(msg.platform, subj, 0);
            return i18n_.tr(loc, "favor.erased", {{"target", subj}});
        }
        return i18n_.tr(loc, "favor.show", {{"nick", displayName(msg)}, {"self", botSelfName(msg)},
            {"favor", std::to_string(getFavor(msg.platform, self))}, {"title", i18n_.tr(loc, favorTitleKey(getFavor(msg.platform, self)))}});
    }

    std::string botSelfName(const Message& msg) const {
        if (auto a = adapters_.getAdapter(msg.adapterId)) { std::string n = a->getLoginName(); if (!n.empty()) return n; }
        return i18n_.tr(localeForGroup(msg), "log.bot_name");
    }

    std::string favorRank(Locale loc, const Message& msg) {
        auto* st = db_.getStorage(); if (!st) return i18n_.tr(loc, "favor.rank_empty");
        try {
            namespace orm = sqlite_orm;
            auto rows = st->get_all<PlayerProfileRow>(
                orm::where(orm::c(&PlayerProfileRow::platform) == msg.platform),
                orm::order_by(&PlayerProfileRow::favor).desc(), orm::limit(10));
            std::string list; int i = 0;
            for (auto& r : rows) { if (r.favor == 0) continue; if (i) list += "\n";
                list += std::to_string(++i) + ". " + profileName(r) + "：" + std::to_string(r.favor); }
            if (list.empty()) return i18n_.tr(loc, "favor.rank_empty");
            return i18n_.tr(loc, "favor.rank", {{"self", botSelfName(msg)}, {"list", list}});
        } catch (...) { return i18n_.tr(loc, "favor.rank_empty"); }
    }

    std::string favorGroupRank(Locale loc, const Message& msg) {
        if (msg.type == MessageType::kPrivate) return i18n_.tr(loc, "favor.group_only");
        auto a = adapters_.getAdapter(msg.adapterId);
        if (!a) return i18n_.tr(loc, "favor.rank_empty");
        json members = a->getMembers(msg.targetId);
        if (!members.is_array() || members.empty()) { a->refreshMembers(msg.targetId); return i18n_.tr(loc, "favor.members_pending"); }
        std::vector<std::pair<int, std::string>> scored;   // (favor, uid)
        for (auto& m : members) {
            std::string uid;
            if (m.contains("user_id")) { if (m["user_id"].is_string()) uid = m["user_id"].get<std::string>();
                else if (m["user_id"].is_number()) uid = std::to_string(m["user_id"].get<int64_t>()); }
            if (uid.empty()) continue;
            int f = getFavor(msg.platform, uid);
            if (f != 0) scored.push_back({f, uid});
        }
        std::sort(scored.begin(), scored.end(), [](auto& x, auto& y) { return x.first > y.first; });
        std::string list; int i = 0;
        for (auto& [f, uid] : scored) { if (i >= 10) break; if (i) list += "\n";
            list += std::to_string(++i) + ". " + uid + "：" + std::to_string(f); }
        if (list.empty()) return i18n_.tr(loc, "favor.rank_empty");
        return i18n_.tr(loc, "favor.rank", {{"self", botSelfName(msg)}, {"list", list}});
    }

    /// Accumulate one check outcome into the user's per-skill .hiy statistics.
    void recordRollStat(const Message& msg, const std::string& skill, SuccessLevel lv) {
        if (msg.senderId.empty()) return;
        auto* st = db_.getStorage();
        if (!st) return;
        std::string sk = CharacterCardStore::canonical(skill);
        try {
            namespace orm = sqlite_orm;
            auto rows = st->get_all<RollStatRow>(
                orm::where(orm::c(&RollStatRow::platform) == msg.platform
                    and orm::c(&RollStatRow::userId) == msg.senderId
                    and orm::c(&RollStatRow::skill) == sk), orm::limit(1));
            RollStatRow r = rows.empty() ? RollStatRow{} : rows.front();
            if (rows.empty()) { r.platform = msg.platform; r.userId = msg.senderId; r.skill = sk; }
            r.total += 1;
            switch (lv) {
                case SuccessLevel::kCritical: r.crit++; break;
                case SuccessLevel::kExtreme:  r.extreme++; break;
                case SuccessLevel::kHard:     r.hard++; break;
                case SuccessLevel::kRegular:  r.regular++; break;
                case SuccessLevel::kFailure:  r.fail++; break;
                case SuccessLevel::kFumble:   r.fumble++; break;
            }
            if (rows.empty()) st->insert(r); else st->update(r);
        } catch (...) {}
    }

    std::string handleHiy(Locale loc, const std::string& args, const Message& msg) {
        const std::string nick = displayName(msg);
        auto* st = db_.getStorage();
        if (!st) return i18n_.tr(loc, "hiy.empty", {{"nick", nick}});
        std::string skill = trim(args);
        try {
            namespace orm = sqlite_orm;
            auto rows = st->get_all<RollStatRow>(
                orm::where(orm::c(&RollStatRow::platform) == msg.platform
                    and orm::c(&RollStatRow::userId) == msg.senderId));
            // Aggregate (no skill arg): sum every skill row for this user.
            // Per-skill (.hiy 侦查): pick the matching row.
            RollStatRow agg{};
            bool found = false;
            std::string want = skill.empty() ? std::string() : CharacterCardStore::canonical(skill);
            for (const auto& r : rows) {
                if (skill.empty()) {
                    agg.total += r.total; agg.crit += r.crit; agg.extreme += r.extreme;
                    agg.hard += r.hard; agg.regular += r.regular; agg.fail += r.fail; agg.fumble += r.fumble;
                    found = true;
                } else if (r.skill == want) { agg = r; found = true; break; }
            }
            if (!found || agg.total == 0) {
                if (skill.empty()) return i18n_.tr(loc, "hiy.empty", {{"nick", nick}});
                return i18n_.tr(loc, "hiy.skill_empty", {{"nick", nick}, {"skill", skill}});
            }
            int success = agg.crit + agg.extreme + agg.hard + agg.regular;
            int rate = agg.total > 0 ? (int)((success * 100.0) / agg.total + 0.5) : 0;
            I18n::Args a = {
                {"nick", nick}, {"skill", skill}, {"total", std::to_string(agg.total)},
                {"crit", std::to_string(agg.crit)}, {"extreme", std::to_string(agg.extreme)},
                {"hard", std::to_string(agg.hard)}, {"regular", std::to_string(agg.regular)},
                {"fail", std::to_string(agg.fail)}, {"fumble", std::to_string(agg.fumble)},
                {"success", std::to_string(success)}, {"rate", std::to_string(rate)}};
            return i18n_.tr(loc, skill.empty() ? "hiy.result" : "hiy.skill_result", a);
        } catch (...) { return i18n_.tr(loc, "hiy.empty", {{"nick", nick}}); }
    }

    std::string handleCheck(Locale loc, const Message& msg, const std::string& rest,
                            bool hidden = false, bool glued = false) {
        std::string s = trim(rest);

        // Multi-check (海豹): leading "N#" → roll the check N times. e.g. ".ra 3#侦查".
        int multi = 1;
        if (auto hp = s.find('#'); hp != std::string::npos) {
            std::string n = s.substr(0, hp);
            if (isAllDigits(n)) { multi = parseIntOr(n, 1); s = trim(s.substr(hp + 1)); }
        }
        if (multi < 1) multi = 1; if (multi > 10) multi = 10;

        // Bonus/penalty (青果 ".rab侦查"/".ra(b/p)" 海豹 ".ra b 侦查"): leading b/p[count],
        // either attached to the skill ("b侦查") or as its own token ("b" / "b2").
        int bp = 0; char bpType = 0;
        {
            auto [tok, after] = splitCommand(s);
            if (!tok.empty() && (tok[0] == 'b' || tok[0] == 'p' || tok[0] == 'B' || tok[0] == 'P')) {
                size_t k = 1; std::string num;
                while (k < tok.size() && std::isdigit(static_cast<unsigned char>(tok[k]))) num += tok[k++];
                std::string restTok = tok.substr(k);
                // 紧跟中文属性（".rab侦查"）恒可；紧跟 ASCII 字母技能（".rapass23"）仅
                // 黏着形可——最长指令优先解析为 .rap ass 23；".ra pass 23" 仍是检定 pass。
                bool attached = !restTok.empty()
                    && (static_cast<unsigned char>(restTok[0]) >= 0x80
                        || (glued && std::isalpha(static_cast<unsigned char>(restTok[0]))));
                if (restTok.empty() || attached) {
                    bpType = static_cast<char>(std::tolower(static_cast<unsigned char>(tok[0])));
                    bp = num.empty() ? 1 : parseIntOr(num, 1);
                    if (bp < 1) bp = 1; if (bp > 9) bp = 9;
                    s = attached ? trim(restTok + (after.empty() ? "" : " " + after)) : trim(after);
                }
            }
        }

        // Difficulty prefix ".ra 困难侦查"/".ra 极难侦查" → reduced threshold.
        int div = 1; std::string diffLabel;
        if (s.rfind("\xe6\x9e\x81\xe9\x9a\xbe", 0) == 0)        // 极难
            { div = 5; diffLabel = "\xe6\x9e\x81\xe9\x9a\xbe"; s = trim(s.substr(6)); }
        else if (s.rfind("\xe5\x9b\xb0\xe9\x9a\xbe", 0) == 0)   // 困难
            { div = 2; diffLabel = "\xe5\x9b\xb0\xe9\x9a\xbe"; s = trim(s.substr(6)); }
        else if (s.rfind("\xe5\xb8\xb8\xe8\xa7\x84", 0) == 0)   // 常规
            { s = trim(s.substr(6)); }
        else if (s.rfind("\xe6\x99\xae\xe9\x80\x9a", 0) == 0)   // 普通
            { s = trim(s.substr(6)); }

        // ".ra <技能> @某人" → check using the @'d player's card (青果/海豹).
        std::string target = atTarget(msg);
        std::string attr, reason, err;
        int rate = 0;
        // .rad（海豹隐藏功能）: 先 d 一个 d100 当作成功率，再对它做检定。
        // 技能名就是字面 "d"，回复形如 `{nick}的d检定: D100=61/62 成功`。
        if (toLower(splitCommand(s).first) == "d") {
            auto rr = engine_.roll("1d100");
            if (!rr.ok()) return i18n_.tr(loc, "dice.error.roll", {{"error", rr.error}});
            std::string after = trim(splitCommand(s).second);
            // C#101：.rad <技能> = .r + .ra 的合并——第一行按 .r 展示掷骰过程，第二行
            // 用**同一个骰值**对技能成功率判定。"d" 后解析不出技能时保留原行为（当 reason）。
            if (!after.empty()) {
                std::string attr2, reason2, err2; int rate2 = 0;
                if (resolveRate(loc, msg, after, attr2, rate2, reason2, err2, target)) {
                    int eff2 = (div > 1) ? rate2 / div : rate2;
                    std::string lbl = (target.empty() ? "" : "@" + target + " ") + diffLabel + attr2;
                    std::string out2 = i18n_.tr(loc, "dice.roll.result",
                        {{"nick", displayName(msg)}, {"res", rr.formattedOutput}});
                    out2 += "\n" + formatCheck(loc, msg, lbl, eff2, reason2, rr.formattedOutput, rr.modifiedTotal);
                    if (hidden) { sendPrivate(msg, out2); return i18n_.tr(loc, "dice.check.hidden", {{"nick", displayName(msg)}}); }
                    return out2;
                }
            }
            rate = rr.modifiedTotal;
            attr = "d";
            reason = after;
        } else if (!resolveRate(loc, msg, s, attr, rate, reason, err, target)) {
            return err;
        }
        int effRate = (div > 1) ? rate / div : rate;
        std::string forWhom = target.empty() ? "" : "@" + target + " ";   // 标注被检定者
        std::string label = forWhom + diffLabel + attr +
            (bpType == 'b' ? i18n_.tr(loc, "dice.bp.bonus_label")
           : bpType == 'p' ? i18n_.tr(loc, "dice.bp.penalty_label") : "");  // (奖)/(惩)，随语言切换

        std::string out;
        for (int i = 0; i < multi; ++i) {
            std::string detail; int val;
            if (bpType) { val = rollBonusPenalty(bp, bpType == 'b', loc, detail); }
            else { auto r = engine_.roll("1d100");
                   if (!r.ok()) return i18n_.tr(loc, "dice.error.roll", {{"error", r.error}});
                   val = r.modifiedTotal; detail = r.formattedOutput; }
            if (i) out += "\n";
            out += formatCheck(loc, msg, label, effRate, reason, detail, val);
        }
        if (hidden) { sendPrivate(msg, out); return i18n_.tr(loc, "dice.check.hidden", {{"nick", displayName(msg)}}); }
        return out;
    }

    // ─── .rx 心理学暗骰 (COC) ─────────────────────────────────
    // 守秘人对 @ 的调查员做一次隐藏的心理学检定：详细结果（含成功等级与
    // 自定义回执）私聊发给 KP，群里只出一句不剧透的回执。参考原版 rx.lua。
    std::optional<std::string> tryHandleRx(Locale loc, const Message& msg,
                                           const std::string& cmd) {
        std::string lc = toLower(cmd);
        if (lc != "rx" && lc.rfind("rx ", 0) != 0 && lc.rfind("rx@", 0) != 0) return std::nullopt;
        std::string rest = trim(cmd.size() > 2 ? cmd.substr(2) : "");

        // 仅限群聊（暗骰需要一个群上下文 + 私聊回 KP）。
        if (msg.type == MessageType::kPrivate) return i18n_.tr(loc, "dice.rx.no_group");
        std::string target = atTarget(msg);
        if (target.empty()) return i18n_.tr(loc, "dice.rx.no_target");

        // 心理学数值：rest 里若给了显式数字则用它，否则读对方卡，默认 10（原版默认）。
        const std::string PSY = "\xe5\xbf\x83\xe7\x90\x86\xe5\xad\xa6";  // 心理学
        int explicitRate = -1;
        { std::istringstream iss(rest); std::string tok;
          while (iss >> tok) { if (isAllDigits(tok)) { explicitRate = parseIntOr(tok, -1); break; } } }
        int rate = 10;
        if (auto v = cards_.getAttr(target, cardScope(msg), PSY)) rate = *v;
        if (explicitRate >= 0) rate = explicitRate;

        auto r = engine_.roll("1d100");
        if (!r.ok()) return i18n_.tr(loc, "dice.error.roll", {{"error", r.error}});
        int result = r.modifiedTotal;
        SuccessLevel lv = rollSuccessLevel(result, rate, getCocRule(msg));

        std::string targetName = personNameOf(msg, target);
        std::string kpName = personName(msg);

        // 私聊给 KP 的详细结果（含成功等级 + 自定义回执）。
        sendPrivate(msg, i18n_.tr(loc, "dice.rx.private", {
            {"target", targetName}, {"gid", msg.targetId},
            {"roll", std::to_string(result)}, {"rate", std::to_string(rate)},
            {"level", i18n_.tr(loc, levelKey(lv))},
            {"receipt", i18n_.tr(loc, rxReceiptKey(lv))}}));

        // 群里只出一句不剧透的回执。
        return i18n_.tr(loc, "dice.rx.receipt", {{"nick", kpName}, {"target", targetName}});
    }

    // ─── Bonus / Penalty dice: .rb / .rp (COC7) ──────────────

    /// Roll a d100 with @p n extra tens dice; bonus keeps the lowest tens,
    /// penalty the highest. Faithful to the original RD bonus/penalty logic.
    /// @p detail receives "base[奖励骰/惩罚骰:e1 e2]" for display.
    int rollBonusPenalty(int n, bool bonus, Locale loc, std::string& detail) {
        int base = engine_.roll("1d100").modifiedTotal;   // 1..100
        int d100 = base;
        std::vector<int> extras;
        for (int i = 0; i < n; ++i) {
            int roll = engine_.roll("1d10").modifiedTotal;          // 1..10
            int tens = (base % 10 == 0) ? roll : roll - 1;
            extras.push_back(tens);
            if (bonus) { if (tens < d100 / 10) d100 = tens * 10 + base % 10; }
            else       { if (tens > d100 / 10) d100 = tens * 10 + base % 10; }
        }
        std::ostringstream ss;
        ss << base << "[" << i18n_.tr(loc, bonus ? "dice.bp.bonus" : "dice.bp.penalty") << ":";
        for (size_t i = 0; i < extras.size(); ++i) { if (i) ss << " "; ss << extras[i]; }
        ss << "]=" << d100;
        detail = ss.str();
        return d100;
    }

    std::optional<std::string> tryHandleBP(Locale loc, const Message& msg, const std::string& cmd) {
        if (cmd.size() < 2) return std::nullopt;
        char c0 = static_cast<char>(std::tolower(static_cast<unsigned char>(cmd[0])));
        char c1 = static_cast<char>(std::tolower(static_cast<unsigned char>(cmd[1])));
        if (c0 != 'r' || (c1 != 'b' && c1 != 'p')) return std::nullopt;
        // C#39: don't let ".rp"/".rb" greedily swallow longer ASCII command words such
        // as ".rpmode" (persona). Valid chars right after r[bp] are: end / space / a
        // digit (dice count) / a non-ASCII attribute (.rp侦查). An ASCII letter here means
        // this is a different command — defer so e.g. .rpmode reaches tryHandlePersona.
        if (cmd.size() > 2) {
            unsigned char nx = static_cast<unsigned char>(cmd[2]);
            if (nx < 0x80 && std::isalpha(nx)) return std::nullopt;
        }
        bool bonus = (c1 == 'b');

        size_t j = 2;
        std::string cntStr;
        while (j < cmd.size() && std::isdigit(static_cast<unsigned char>(cmd[j]))) cntStr += cmd[j++];
        int n = cntStr.empty() ? 1 : parseIntOr(cntStr, 1);
        if (n < 1) n = 1; if (n > 9) n = 9;   // COC caps at 9 bonus/penalty dice

        std::string rest = trim(cmd.substr(j));
        const std::string nick = displayName(msg);

        if (rest.empty()) {
            // Plain bonus/penalty roll, no check.
            std::string detail;
            rollBonusPenalty(n, bonus, loc, detail);
            return i18n_.tr(loc, "dice.roll.result", {{"nick", nick}, {"res", detail}});
        }
        std::string attr, reason, err;
        int rate = 0;
        if (!resolveRate(loc, msg, rest, attr, rate, reason, err)) return err;
        std::string detail;
        int result = rollBonusPenalty(n, bonus, loc, detail);
        return formatCheck(loc, msg, attr, rate, reason, detail, result);
    }

    // ─── Persona switching: .rpmode (C#28-B) ─────────────────────
    // .rpmode is an independent command — no conflict with .rp (COC7 penalty dice).
    // Permissions: show/list/info = everyone; set/off/default = group admin;
    //              create/copy/del = Master only.

    std::optional<std::string> tryHandlePersona(Locale loc, const Message& msg,
                                                 const std::string& cmd) {
        if (cmd.size() < 6) return std::nullopt;
        std::string prefix = toLower(cmd.substr(0, 6));
        if (prefix != "rpmode") return std::nullopt;
        if (cmd.size() > 6 && cmd[6] != ' ' && cmd[6] != '\t') return std::nullopt;

        std::string rest = trim(cmd.substr(6));
        // Extract subcommand (first word)
        std::string subcmd;
        std::string args;
        size_t spacePos = rest.find(' ');
        if (spacePos == std::string::npos) {
            subcmd = toLower(rest);
        } else {
            subcmd = toLower(rest.substr(0, spacePos));
            args = trim(rest.substr(spacePos + 1));
        }

        if (!personaMgr_) return std::nullopt;

        const bool isGroupAdmin = senderIsGroupAdmin(msg);   // C#48：统一群管权限（含群主/管理/邀请人/骰主）
        const bool isMasterUser = isMaster(msg);

        // .rpmode (no args) → show current persona
        if (subcmd.empty()) return handlePersonaShow(loc, msg);
        if (subcmd == "list")    return handlePersonaList(loc, msg);
        if (subcmd == "info")    return handlePersonaInfo(loc, msg, args);
        if (subcmd == "set") {
            if (!isGroupAdmin) return i18n_.tr(loc, "persona.no_perm_admin");
            return handlePersonaSet(loc, msg, args);
        }
        if (subcmd == "off" || subcmd == "default") {
            if (!isGroupAdmin) return i18n_.tr(loc, "persona.no_perm_admin");
            return handlePersonaOff(loc, msg);
        }
        if (subcmd == "create") {
            if (!isMasterUser) return i18n_.tr(loc, "persona.no_perm_master");
            return handlePersonaCreate(loc, msg, args);
        }
        if (subcmd == "copy") {
            if (!isMasterUser) return i18n_.tr(loc, "persona.no_perm_master");
            return handlePersonaCopy(loc, msg, args);
        }
        if (subcmd == "del") {
            if (!isMasterUser) return i18n_.tr(loc, "persona.no_perm_master");
            return handlePersonaDel(loc, msg, args);
        }
        return std::nullopt;
    }

    std::optional<std::string> handlePersonaShow(Locale loc, const Message& msg) {
        std::string groupId = (msg.type == MessageType::kGroup) ? msg.targetId : std::string();
        int activeId = personaMgr_->getActivePersona(groupId);
        if (activeId <= 0) {
            return i18n_.tr(loc, "persona.current", {{"name", i18n_.tr(loc, "persona.default_name")}});
        }
        auto tmpl = personaMgr_->getTemplateById(activeId);
        if (tmpl.id <= 0) {
            return i18n_.tr(loc, "persona.current", {{"name", i18n_.tr(loc, "persona.default_name")}});
        }
        int entryCount = personaMgr_->getEntryCount(activeId);
        std::string result = i18n_.tr(loc, "persona.current", {{"name", tmpl.name}});
        result += "\n" + i18n_.tr(loc, "persona.entry_count", {{"count", std::to_string(entryCount)}});
        return result;
    }

    std::optional<std::string> handlePersonaList(Locale loc, const Message& msg) {
        auto templates = personaMgr_->listTemplates();
        if (templates.empty()) {
            return i18n_.tr(loc, "persona.no_personas");
        }
        std::string list;
        for (const auto& t : templates) {
            list += "• " + t.name;
            if (t.isBuiltin) list += "（内置）";
            if (!t.description.empty()) list += " — " + t.description;
            list += "\n";
        }
        return i18n_.tr(loc, "persona.list", {{"list", list}});
    }

    std::optional<std::string> handlePersonaInfo(Locale loc, const Message& msg,
                                                  const std::string& name) {
        if (name.empty()) return i18n_.tr(loc, "persona.name_empty");
        auto tmpl = personaMgr_->getTemplateByName(name);
        if (tmpl.id <= 0) return i18n_.tr(loc, "persona.not_found", {{"name", name}});
        int entryCount = personaMgr_->getEntryCount(tmpl.id);
        std::string result = i18n_.tr(loc, "persona.info_name", {{"name", tmpl.name}});
        result += "\n" + (tmpl.description.empty() ? "（无描述）" : tmpl.description);
        result += "\n" + i18n_.tr(loc, "persona.entry_count", {{"count", std::to_string(entryCount)}});
        if (tmpl.isBuiltin) result += "\n（内置人格）";
        return result;
    }

    std::optional<std::string> handlePersonaSet(Locale loc, const Message& msg,
                                                 const std::string& name) {
        if (name.empty()) return i18n_.tr(loc, "persona.name_empty");
        auto tmpl = personaMgr_->getTemplateByName(name);
        if (tmpl.id <= 0) return i18n_.tr(loc, "persona.not_found", {{"name", name}});
        std::string groupId = (msg.type == MessageType::kGroup) ? msg.targetId : std::string();
        personaMgr_->setActivePersona(tmpl.id, groupId);
        return i18n_.tr(loc, "persona.set", {{"name", tmpl.name}});
    }

    std::optional<std::string> handlePersonaOff(Locale loc, const Message& msg) {
        std::string groupId = (msg.type == MessageType::kGroup) ? msg.targetId : std::string();
        personaMgr_->setActivePersona(0, groupId);
        return i18n_.tr(loc, "persona.off");
    }

    std::optional<std::string> handlePersonaCreate(Locale loc, const Message& msg,
                                                    const std::string& name) {
        if (name.empty()) return i18n_.tr(loc, "persona.name_empty");
        // Check for duplicate
        auto existing = personaMgr_->getTemplateByName(name);
        if (existing.id > 0) return i18n_.tr(loc, "persona.name_exists", {{"name", name}});
        int newId = personaMgr_->createTemplate(name, "");
        if (newId < 0) return i18n_.tr(loc, "persona.create_fail");
        return i18n_.tr(loc, "persona.created", {{"name", name}});
    }

    std::optional<std::string> handlePersonaCopy(Locale loc, const Message& msg,
                                                  const std::string& args) {
        // Parse "src dst"
        size_t spacePos = args.find(' ');
        if (spacePos == std::string::npos) return i18n_.tr(loc, "persona.copy_usage");
        std::string srcName = trim(args.substr(0, spacePos));
        std::string dstName = trim(args.substr(spacePos + 1));
        if (srcName.empty() || dstName.empty()) return i18n_.tr(loc, "persona.copy_usage");
        auto src = personaMgr_->getTemplateByName(srcName);
        if (src.id <= 0) return i18n_.tr(loc, "persona.not_found", {{"name", srcName}});
        auto existing = personaMgr_->getTemplateByName(dstName);
        if (existing.id > 0) return i18n_.tr(loc, "persona.name_exists", {{"name", dstName}});
        int newId = personaMgr_->copyTemplate(src.id, dstName);
        if (newId < 0) return i18n_.tr(loc, "persona.copy_fail");
        return i18n_.tr(loc, "persona.copied", {{"src", srcName}, {"dst", dstName}});
    }

    std::optional<std::string> handlePersonaDel(Locale loc, const Message& msg,
                                                 const std::string& name) {
        if (name.empty()) return i18n_.tr(loc, "persona.name_empty");
        auto tmpl = personaMgr_->getTemplateByName(name);
        if (tmpl.id <= 0) return i18n_.tr(loc, "persona.not_found", {{"name", name}});
        if (tmpl.isBuiltin) return i18n_.tr(loc, "persona.builtin_no_del");
        if (!personaMgr_->deleteTemplate(tmpl.id))
            return i18n_.tr(loc, "persona.del_fail");
        return i18n_.tr(loc, "persona.deleted", {{"name", name}});
    }

    // ─── Generators: .coc (investigator) / .dnd (ability scores) ────

    std::optional<std::string> tryHandleGen(Locale loc, const Message& msg,
                                            const std::string& cmd) {
        std::string lc = toLower(cmd);
        if (lc.rfind("coc", 0) == 0) {
            std::string rest = trim(cmd.substr(3));
            // Skip edition/silent markers (7/6/s); COC6 distinction is on the roadmap.
            while (!rest.empty() && (rest[0]=='7'||rest[0]=='6'||rest[0]=='s'||rest[0]=='S'))
                rest = trim(rest.substr(1));
            return handleCOC(loc, msg, clampCount(parseIntOr(rest, 1)));
        }
        if (lc.rfind("dnd", 0) == 0) {
            return handleDND(loc, msg, clampCount(parseIntOr(trim(cmd.substr(3)), 1)));
        }
        return std::nullopt;
    }

    static int clampCount(int n) { return n < 1 ? 1 : (n > 10 ? 10 : n); }

    std::string handleCOC(Locale loc, const Message& msg, int count) {
        // 9 attributes, each rolled then ×5 (original COC7 table).
        static const char* attrKeys[9] = {
            "dice.coc.attr.str", "dice.coc.attr.con", "dice.coc.attr.siz",
            "dice.coc.attr.dex", "dice.coc.attr.app", "dice.coc.attr.int",
            "dice.coc.attr.pow", "dice.coc.attr.edu", "dice.coc.attr.luck"
        };
        static const char* attrRolls[9] = {
            "3d6", "3d6", "2d6+6", "3d6", "3d6", "2d6+6", "3d6", "2d6+6", "3d6"
        };
        std::vector<std::string> blocks;
        for (int c = 0; c < count; ++c) {
            std::ostringstream b;
            int allTotal = 0, luck = 0;
            for (int i = 0; i < 9; ++i) {
                int v = engine_.roll(attrRolls[i]).modifiedTotal * 5;
                b << i18n_.tr(loc, attrKeys[i]) << ":" << v << " ";
                allTotal += v;
                if (i == 8) luck = v;
            }
            b << i18n_.tr(loc, "dice.coc.total")
              << ":" << (allTotal - luck) << "/" << allTotal;
            blocks.push_back(b.str());
        }
        offerForward(msg, blocks);   // #6 多张车卡 → 合并转发（每张一个气泡）
        std::ostringstream res;
        for (auto& b : blocks) res << "\n" << b;
        const std::string nick = displayName(msg);
        return i18n_.tr(loc, "dice.coc.build", {{"nick", nick}, {"res", res.str()}});
    }

    std::string handleDND(Locale loc, const Message& msg, int count) {
        std::vector<std::string> blocks;
        for (int c = 0; c < count; ++c) {
            std::ostringstream b;
            std::vector<int> scores;
            int allTotal = 0;
            for (int i = 0; i < 6; ++i) {
                // 4d6 keep highest 3.
                auto r = engine_.roll("4d6");
                std::vector<int> dice = r.individualResults;
                std::sort(dice.begin(), dice.end(), std::greater<int>());
                int s = 0;
                for (size_t k = 0; k < dice.size() && k < 3; ++k) s += dice[k];
                scores.push_back(s);
                allTotal += s;
            }
            std::sort(scores.begin(), scores.end(), std::greater<int>());
            for (size_t i = 0; i < scores.size(); ++i) {
                if (i > 0) b << " /";
                b << scores[i];
            }
            b << " " << i18n_.tr(loc, "dice.dnd.total") << allTotal;
            blocks.push_back(b.str());
        }
        offerForward(msg, blocks);   // #6 多组属性 → 合并转发（每组一个气泡）
        std::ostringstream res;
        for (auto& b : blocks) res << "\n" << b;
        const std::string nick = displayName(msg);
        return i18n_.tr(loc, "dice.dnd.build", {{"nick", nick}, {"res", res.str()}});
    }

    // #6：若开了合并转发开关、且这是群消息且结果不止一条，把每条结果登记为一个
    // 转发气泡（#1/#2…前缀便于区分）。main.cpp 取走后以 send_group_forward_msg 发出。
    void offerForward(const Message& msg, const std::vector<std::string>& blocks) {
        if (blocks.size() < 2 || msg.type == MessageType::kPrivate || !forwardEnabled()) return;
        std::vector<std::string> nodes;
        nodes.reserve(blocks.size());
        for (size_t k = 0; k < blocks.size(); ++k)
            nodes.push_back("#" + std::to_string(k + 1) + " " + blocks[k]);
        setForwardNodes(std::move(nodes));
    }

    /// Card scope: per group for group/channel messages, personal ("") in private.
    static std::string cardScope(const Message& msg) {
        return msg.type == MessageType::kPrivate ? std::string("") : msg.targetId;
    }

    // ─── DND5e: spell slots / cast / long rest / death saves ─────
    // Backed by reserved card attributes (no new table): spell-slot ring i (1-9)
    // current="ss{i}" max="ssmax{i}"; death-save counters="dssuccess"/"dsfail".
    // hp uses the canonical "生命值"; max uses "hpmax". All support 代骰 (the
    // dispatcher passes the perspective message, so they act on the @'d card).

    /// Parse a spell ring spec: "3环" / "lv3" / "3" → 1..9, else -1.
    static int parseRing(const std::string& tok) {
        std::string t = trim(tok);
        const std::string huan = "\xe7\x8e\xaf";   // 环
        if (t.size() > huan.size() && t.compare(t.size() - huan.size(), huan.size(), huan) == 0)
            t = t.substr(0, t.size() - huan.size());
        std::string lt = toLower(t);
        if (lt.rfind("lv", 0) == 0) t = t.substr(2);
        int r = parseIntOr(trim(t), -1);
        return (r >= 1 && r <= 9) ? r : -1;
    }

    std::string slotSummary(const std::string& user, const std::string& group) const {
        std::ostringstream s; bool any = false;
        for (int i = 1; i <= 9; ++i) {
            auto mx = const_cast<CharacterCardStore&>(cards_).getAttr(user, group, "ssmax" + std::to_string(i));
            if (!mx || *mx <= 0) continue;
            auto cur = const_cast<CharacterCardStore&>(cards_).getAttr(user, group, "ss" + std::to_string(i));
            if (any) s << ", ";
            s << i << "\xe7\x8e\xaf:" << (cur ? *cur : 0) << "/" << *mx;   // N环:cur/max
            any = true;
        }
        return any ? s.str() : std::string();
    }

    std::optional<std::string> tryHandleDnd(Locale loc, const Message& msg, const std::string& cmd) {
        auto [word, args] = splitCommand(cmd);
        std::string w = toLower(word);
        if (w == "ss")   return handleSpellSlots(loc, msg, args);
        if (w == "cast") return handleCast(loc, msg, args);
        if (w == "longrest" || word == "\xe9\x95\xbf\xe4\xbc\x91")   // 长休
            return handleLongRest(loc, msg);
        if (w == "ds" || word == "\xe6\xad\xbb\xe4\xba\xa1\xe8\xb1\x81\xe5\x85\x8d")  // 死亡豁免
            return handleDeathSave(loc, msg, args);
        return std::nullopt;
    }

    std::string handleSpellSlots(Locale loc, const Message& msg, const std::string& args) {
        const std::string user = msg.senderId, group = cardScope(msg), nick = displayName(msg);
        std::string a = trim(args), la = toLower(a);

        if (a.empty()) {
            std::string sum = slotSummary(user, group);
            return sum.empty() ? i18n_.tr(loc, "dnd.ss.empty", {{"nick", nick}})
                               : i18n_.tr(loc, "dnd.ss.show", {{"nick", nick}, {"detail", sum}});
        }
        if (la == "clr" || la == "clear") {
            for (int i = 1; i <= 9; ++i) { cards_.eraseAttr(user, group, "ss" + std::to_string(i));
                cards_.eraseAttr(user, group, "ssmax" + std::to_string(i)); }
            return i18n_.tr(loc, "dnd.ss.cleared", {{"nick", nick}});
        }
        if (la == "rest") {
            for (int i = 1; i <= 9; ++i) {
                auto mx = cards_.getAttr(user, group, "ssmax" + std::to_string(i));
                if (mx) cards_.setAttr(user, group, "ss" + std::to_string(i), *mx);
            }
            return i18n_.tr(loc, "dnd.ss.rested", {{"nick", nick}});
        }
        if (la.rfind("init", 0) == 0) {
            std::istringstream iss(trim(a.substr(4)));
            int v, i = 1; bool any = false;
            while ((iss >> v) && i <= 9) { if (v < 0) v = 0;
                cards_.setAttr(user, group, "ssmax" + std::to_string(i), v);
                cards_.setAttr(user, group, "ss" + std::to_string(i), v); ++i; any = true; }
            if (!any) return i18n_.tr(loc, "dnd.ss.usage");
            return i18n_.tr(loc, "dnd.ss.set", {{"nick", nick}, {"detail", slotSummary(user, group)}});
        }
        if (la.rfind("set", 0) == 0) {
            // ".ss set 2环 4" (one pair; multiple pairs comma-separated also work).
            std::string body = trim(a.substr(3)); bool any = false;
            std::string seg;
            std::istringstream parts(body);
            // split on commas first, then whitespace within each piece
            std::string piece;
            auto handlePair = [&](const std::string& p) {
                std::istringstream ps(trim(p)); std::string rs; int val;
                if (!(ps >> rs)) return; if (!(ps >> val)) return;
                int ring = parseRing(rs); if (ring < 0 || val < 0) return;
                cards_.setAttr(user, group, "ssmax" + std::to_string(ring), val);
                cards_.setAttr(user, group, "ss" + std::to_string(ring), val); any = true;
            };
            size_t pos = 0;
            while (pos <= body.size()) {
                size_t comma = body.find_first_of(",\xef\xbc\x8c", pos);   // ,/，
                handlePair(body.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
                if (comma == std::string::npos) break;
                pos = comma + (body[comma] == ',' ? 1 : 3);
            }
            if (!any) return i18n_.tr(loc, "dnd.ss.usage");
            return i18n_.tr(loc, "dnd.ss.set", {{"nick", nick}, {"detail", slotSummary(user, group)}});
        }
        // ".ss 3环 +1" / ".ss lv3 -1": ring then signed delta.
        {
            auto [rtok, dtok] = splitCommand(a);
            int ring = parseRing(rtok);
            std::string d = trim(dtok);
            if (ring >= 1 && !d.empty() && (d[0] == '+' || d[0] == '-')) {
                auto mx = cards_.getAttr(user, group, "ssmax" + std::to_string(ring));
                if (!mx || *mx <= 0) return i18n_.tr(loc, "dnd.ss.no_ring", {{"ring", std::to_string(ring)}});
                int delta = parseIntOr(d, 0);
                int cur = cards_.getAttr(user, group, "ss" + std::to_string(ring)).value_or(0) + delta;
                if (cur < 0) cur = 0; if (cur > *mx) cur = *mx;
                cards_.setAttr(user, group, "ss" + std::to_string(ring), cur);
                return i18n_.tr(loc, "dnd.ss.changed",
                    {{"nick", nick}, {"ring", std::to_string(ring)}, {"cur", std::to_string(cur)}, {"max", std::to_string(*mx)}});
            }
        }
        return i18n_.tr(loc, "dnd.ss.usage");
    }

    std::string handleCast(Locale loc, const Message& msg, const std::string& args) {
        const std::string user = msg.senderId, group = cardScope(msg), nick = displayName(msg);
        std::istringstream iss(trim(args));
        std::string rtok; int n = 1;
        if (!(iss >> rtok)) return i18n_.tr(loc, "dnd.cast.usage");
        int ring = parseRing(rtok);
        if (ring < 0) return i18n_.tr(loc, "dnd.cast.usage");
        if (!(iss >> n)) n = 1; if (n < 1) n = 1;
        auto mx = cards_.getAttr(user, group, "ssmax" + std::to_string(ring));
        if (!mx || *mx <= 0) return i18n_.tr(loc, "dnd.ss.no_ring", {{"ring", std::to_string(ring)}});
        int cur = cards_.getAttr(user, group, "ss" + std::to_string(ring)).value_or(0);
        if (cur < n) return i18n_.tr(loc, "dnd.cast.empty", {{"nick", nick}, {"ring", std::to_string(ring)}});
        cur -= n;
        cards_.setAttr(user, group, "ss" + std::to_string(ring), cur);
        return i18n_.tr(loc, "dnd.ss.changed",
            {{"nick", nick}, {"ring", std::to_string(ring)}, {"cur", std::to_string(cur)}, {"max", std::to_string(*mx)}});
    }

    std::string handleLongRest(Locale loc, const Message& msg) {
        const std::string user = msg.senderId, group = cardScope(msg), nick = displayName(msg);
        auto hpmax = cards_.getAttr(user, group, "hpmax");
        if (!hpmax) return i18n_.tr(loc, "dnd.longrest.no_hpmax", {{"nick", nick}});
        cards_.setAttr(user, group, "hp", *hpmax);
        for (int i = 1; i <= 9; ++i) {
            auto mx = cards_.getAttr(user, group, "ssmax" + std::to_string(i));
            if (mx) cards_.setAttr(user, group, "ss" + std::to_string(i), *mx);
        }
        cards_.eraseAttr(user, group, "dssuccess");
        cards_.eraseAttr(user, group, "dsfail");
        return i18n_.tr(loc, "dnd.longrest.done", {{"nick", nick}, {"hp", std::to_string(*hpmax)}});
    }

    std::string handleDeathSave(Locale loc, const Message& msg, const std::string& args) {
        const std::string user = msg.senderId, group = cardScope(msg), nick = displayName(msg);
        std::string a = trim(args), la = toLower(a);

        if (la == "stat") {
            int s = cards_.getAttr(user, group, "dssuccess").value_or(0);
            int f = cards_.getAttr(user, group, "dsfail").value_or(0);
            return i18n_.tr(loc, "dnd.ds.stat", {{"nick", nick}, {"s", std::to_string(s)}, {"f", std::to_string(f)}});
        }
        // ".ds 成功±N" / ".ds s±N" / ".ds f±N" (manual adjust).
        {
            const std::string suc = "\xe6\x88\x90\xe5\x8a\x9f", fai = "\xe5\xa4\xb1\xe8\xb4\xa5";  // 成功/失败(简)
            const std::string fai2 = "\xe5\xa4\xb1\xe6\x95\x97";  // 失敗(繁)
            auto adjustKey = [&](const std::string& key) -> std::optional<std::string> {
                size_t pm = a.find_first_of("+-");
                if (pm == std::string::npos) return std::nullopt;
                int delta = parseIntOr(a.substr(pm), 0);
                int v = cards_.getAttr(user, group, key).value_or(0) + delta;
                if (v < 0) v = 0;
                cards_.setAttr(user, group, key, v);
                return i18n_.tr(loc, "dnd.ds.stat",
                    {{"nick", nick},
                     {"s", std::to_string(cards_.getAttr(user, group, "dssuccess").value_or(0))},
                     {"f", std::to_string(cards_.getAttr(user, group, "dsfail").value_or(0))}});
            };
            if (la.rfind("s", 0) == 0 || a.rfind(suc, 0) == 0) { if (auto r = adjustKey("dssuccess")) return *r; }
            if (la.rfind("f", 0) == 0 || a.rfind(fai, 0) == 0 || a.rfind(fai2, 0) == 0) { if (auto r = adjustKey("dsfail")) return *r; }
        }

        // Actual death-saving throw — HP must be 0.
        int hp = cards_.getAttr(user, group, "hp").value_or(-1);
        if (hp != 0) return i18n_.tr(loc, "dnd.ds.need_zero", {{"nick", nick}});

        int bonus = 0;
        if (!a.empty() && (a[0] == '+' || a[0] == '-')) {
            auto r = engine_.roll("0" + a);   // "+1d4" → "0+1d4"
            if (r.ok()) bonus = r.modifiedTotal;
        }
        int d20 = engine_.roll("1d20").modifiedTotal;
        int total = d20 + bonus;
        int s = cards_.getAttr(user, group, "dssuccess").value_or(0);
        int f = cards_.getAttr(user, group, "dsfail").value_or(0);

        std::string outcome, status;
        if (d20 == 20) {
            // Medical miracle: regain 1 HP, reset counters.
            cards_.setAttr(user, group, "hp", 1);
            cards_.eraseAttr(user, group, "dssuccess"); cards_.eraseAttr(user, group, "dsfail");
            outcome = i18n_.tr(loc, "dnd.ds.miracle");
            return i18n_.tr(loc, "dnd.ds.result",
                {{"nick", nick}, {"roll", std::to_string(d20)}, {"total", std::to_string(total)}, {"outcome", outcome}});
        }
        if (d20 == 1) { f += 2; outcome = i18n_.tr(loc, "dnd.ds.critfail"); }
        else if (total >= 10) { s += 1; outcome = i18n_.tr(loc, "dnd.ds.success"); }
        else { f += 1; outcome = i18n_.tr(loc, "dnd.ds.fail"); }
        if (s > 3) s = 3; if (f > 3) f = 3;
        cards_.setAttr(user, group, "dssuccess", s);
        cards_.setAttr(user, group, "dsfail", f);

        if (f >= 3) { status = "\n" + i18n_.tr(loc, "dnd.ds.dead", {{"nick", nick}});
            cards_.eraseAttr(user, group, "dssuccess"); cards_.eraseAttr(user, group, "dsfail"); }
        else if (s >= 3) { status = "\n" + i18n_.tr(loc, "dnd.ds.stabilized", {{"nick", nick}});
            cards_.eraseAttr(user, group, "dssuccess"); cards_.eraseAttr(user, group, "dsfail"); }

        return i18n_.tr(loc, "dnd.ds.result",
            {{"nick", nick}, {"roll", std::to_string(d20)}, {"total", std::to_string(total)}, {"outcome", outcome}}) + status;
    }

    // ─── DND5e mode toggle + d20 .rc + .buff (临时属性) ───────

    bool dndModeOn(const Message& msg) const { return getGroupSetting(msg, "dndMode") == "1"; }

    static std::optional<std::string> dndAbilityName(const std::string& name) {
        // DND six abilities have stable storage names. Do not depend solely on the
        // global rule alias table: a loaded COC rule may map the same Chinese word
        // to a COC-specific card field.
        std::string key = toLower(name);
        static const std::array<std::pair<const char*, const char*>, 21> names{{
            {"力量", "力量"}, {"str", "力量"}, {"strength", "力量"},
            {"敏捷", "敏捷"}, {"dex", "敏捷"}, {"dexterity", "敏捷"},
            {"体质", "体质"}, {"con", "体质"}, {"constitution", "体质"},
            {"智力", "智力"}, {"int", "智力"}, {"intelligence", "智力"},
            {"感知", "感知"}, {"wis", "感知"}, {"wisdom", "感知"},
            {"魅力", "魅力"}, {"cha", "魅力"}, {"charisma", "魅力"},
            {"力量骰面初始值", "力量"}, {"敏捷骰面初始值", "敏捷"}, {"体质骰面初始值", "体质"}
        }};
        for (const auto& [alias, stable] : names) if (key == alias) return stable;
        return std::nullopt;
    }
    static bool isDndAbility(const std::string& name) {
        if (dndAbilityName(name)) return true;
        return dndAbilityName(CharacterCardStore::canonical(name)).has_value();
    }
    static bool isDndCardSpecial(const std::string& canonical) {
        if (isDndAbility(canonical)) return true;
        static const std::array<std::string, 13> special{{
            "生命值", "临时生命值", "铂金币", "金币", "银金币", "银币", "铜币",
            "hpmax", "先攻", "熟练加值", "护甲等级", "速度", "等级"}};
        return std::find(special.begin(), special.end(), canonical) != special.end();
    }
    static bool isDndCardSpecialInput(const std::string& input) {
        return isDndAbility(input) || isDndCardSpecial(CharacterCardStore::canonical(input));
    }
    static int dndAbilityModifier(int score) {
        int delta = score - 10;
        return delta >= 0 ? delta / 2 : -(((-delta) + 1) / 2);  // floor((score - 10) / 2)
    }
    std::optional<int> dndSkillValue(const Message& msg, const std::string& attr) const {
        std::string canonical = CharacterCardStore::canonical(attr);
        std::string saved = getUserSetting(msg, "dndskill:" + canonical);
        if (!saved.empty()) return parseIntOr(saved, 0);
        // Compatibility for cards recorded before DND skills were split out.
        return cards_.getAttr(msg.senderId, cardScope(msg), canonical);
    }
    std::map<std::string, int> listDndSkills(const Message& msg) const {
        std::map<std::string, int> out;
        auto* st = db_.getStorage(); if (!st) return out;
        try { namespace orm = sqlite_orm;
            auto rows = st->get_all<UserSettingRow>(orm::where(
                orm::c(&UserSettingRow::userId) == msg.senderId and
                orm::c(&UserSettingRow::groupId) == cardScope(msg)));
            for (const auto& row : rows)
                if (row.key.rfind("dndskill:", 0) == 0 && !row.value.empty())
                    out[row.key.substr(9)] = parseIntOr(row.value, 0);
        } catch (...) {}
        return out;
    }

    std::optional<std::string> tryHandleSetdnd(Locale loc, const Message& msg, const std::string& cmd) {
        if (toLower(cmd).rfind("setdnd", 0) != 0) return std::nullopt;
        std::string a = toLower(trim(cmd.substr(6)));
        // C#48：群模式切换需群管权限（与 .setcoc 同级，对齐原版 canRoomHost 门控）。
        if (a != "show" && !senderIsGroupAdmin(msg)) return i18n_.tr(loc, "gate.no_perm");
        if (a == "off" || a == "0" || a == "clr") { setGroupSetting(msg, "dndMode", "0"); return i18n_.tr(loc, "setdnd.off"); }
        if (a == "on" || a == "1")               { setGroupSetting(msg, "dndMode", "1"); return i18n_.tr(loc, "setdnd.on"); }
        if (a == "show") return i18n_.tr(loc, dndModeOn(msg) ? "setdnd.on" : "setdnd.off");
        if (a.empty()) { bool on = dndModeOn(msg); setGroupSetting(msg, "dndMode", on ? "0" : "1");  // .setdnd 切换
            return i18n_.tr(loc, on ? "setdnd.off" : "setdnd.on"); }
        return i18n_.tr(loc, "setdnd.usage");
    }

    // ── .buff: temporary attribute modifiers (user_settings key "buff:<attr>") ──
    int getBuff(const Message& msg, const std::string& attr) const {
        std::string v = getUserSetting(msg, "buff:" + CharacterCardStore::canonical(attr));
        return v.empty() ? 0 : parseIntOr(v, 0);
    }
    std::map<std::string, int> listBuffs(const Message& msg) const {
        std::map<std::string, int> out;
        auto* st = db_.getStorage(); if (!st) return out;
        try {
            namespace orm = sqlite_orm;
            auto rows = st->get_all<UserSettingRow>(orm::where(
                orm::c(&UserSettingRow::userId) == msg.senderId and
                orm::c(&UserSettingRow::groupId) == cardScope(msg)));
            for (auto& r : rows)
                if (r.key.rfind("buff:", 0) == 0) { int v = parseIntOr(r.value, 0); if (v) out[r.key.substr(5)] = v; }
        } catch (...) {}
        return out;
    }
    /// Card attribute + any active buff (the value DND checks/show should use).
    /// model.xml 衍生属性值（未显式设置时按公式算，如 闪避=敏捷/2、生命=&max_hp=(体质+体型)/10）。
    /// 公式里引用的属性：先取卡值，没有则递归取其衍生值（如 生命→max_hp→公式）。无则 nullopt。
    std::optional<int> derivedAttr(const Message& msg, const std::string& attr, int depth = 0) const {
        if (depth > 8) return std::nullopt;   // 防环
        std::string canon = CharacterCardStore::canonical(attr), formula;
        {
            std::shared_lock<std::shared_mutex> lk(rulesLock());
            // C#102：规则包声明的派生关系优先（可覆盖内置/model.xml 同名公式）。
            auto& rp = rulePackDerivedRegistry();
            if (auto itp = rp.find(canon); itp != rp.end()) formula = itp->second;
            else {
                auto& reg = derivedRegistry();
                auto it = reg.find(canon);
                if (it == reg.end()) return std::nullopt;
                formula = it->second;
            }
        }
        auto look = [&](const std::string& n) -> std::optional<int> {
            if (auto v = cards_.getAttr(msg.senderId, cardScope(msg), n)) return v;
            return derivedAttr(msg, n, depth + 1);   // 卡上没有 → 递归衍生
        };
        return evalComputedFormula(formula, msg, look);
    }
    std::optional<int> effectiveAttr(const Message& msg, const std::string& attr) const {
        auto base = cards_.getAttr(msg.senderId, cardScope(msg), attr);
        if (!base) base = evalStrAttr(msg, CharacterCardStore::canonical(attr));  // C#37 关联属性求值
        if (!base) base = derivedAttr(msg, attr);   // 未设置 → model.xml 衍生值
        if (!base) base = defaultAttr(attr);        // C#102：规则默认值（如 急救30）
        int buff = getBuff(msg, attr);
        if (!base && buff == 0) return std::nullopt;
        return base.value_or(0) + buff;
    }

    // 「公式属性」：值是投掷公式（如伤害加值 DB=1d4/-2），录入时存原文不摇、掷骰时展开。
    static bool isFormulaAttr(const std::string& canon) {
        return canon == "\xe4\xbc\xa4\xe5\xae\xb3\xe5\x8a\xa0\xe5\x80\xbc";   // 伤害加值
    }
    std::string getFormulaAttr(const Message& msg, const std::string& canon) const {
        return getUserSetting(msg, "attrf:" + canon);
    }
    void setFormulaAttr(const Message& msg, const std::string& canon, const std::string& f) {
        setUserSetting(msg, "attrf:" + canon, f);
    }

    // D#03：COC7 伤害加值(DB) —— 按 力量+体型 之和查表，返回投掷公式原文（-2/-1/0/1d4/1d6/2d6…）。
    // 表：<65 -2，<85 -1，<125 0，<165 1d4，<205 1d6，之后每满 80 点多 1d6。
    static std::string coc7DamageBonus(int strPlusSiz) {
        if (strPlusSiz < 65)  return "-2";
        if (strPlusSiz < 85)  return "-1";
        if (strPlusSiz < 125) return "0";
        if (strPlusSiz < 165) return "1d4";
        if (strPlusSiz < 205) return "1d6";
        int n = 2 + (strPlusSiz - 205) / 80;
        return std::to_string(n) + "d6";
    }
    // 本卡当前生效的 DB 公式：手动/已存的 attrf 优先；未存则按 力量+体型 现算（供 +db 展开）。空=无法确定。
    std::string effectiveDamageBonus(const Message& msg) const {
        std::string f = getFormulaAttr(msg, "\xe4\xbc\xa4\xe5\xae\xb3\xe5\x8a\xa0\xe5\x80\xbc");  // 伤害加值
        if (!f.empty()) return f;
        auto sv = cards_.getAttr(msg.senderId, cardScope(msg), "\xe5\x8a\x9b\xe9\x87\x8f");  // 力量
        auto zv = cards_.getAttr(msg.senderId, cardScope(msg), "\xe4\xbd\x93\xe5\x9e\x8b");  // 体型
        if (sv && zv) return coc7DamageBonus(*sv + *zv);
        return "";
    }

    // 「关联/表达式属性」(C#37)：值是引用其它属性的算式（如 物防=敏捷+1 / pd='dex+1'），
    // 存原文字符串，读取时按当前属性值求值（联动）。存在 user_settings "sattr:<canon>"。
    std::string getStrAttr(const Message& msg, const std::string& canon) const {
        return getUserSetting(msg, "sattr:" + canon);
    }
    void setStrAttr(const Message& msg, const std::string& canon, const std::string& expr) {
        setUserSetting(msg, "sattr:" + canon, expr);
    }
    // 列出本人本群所有表达式属性（canon → 表达式），供 .st show / .st 列举。
    std::map<std::string, std::string> listStrAttrs(const Message& msg) const {
        std::map<std::string, std::string> out;
        auto* st = db_.getStorage(); if (!st) return out;
        try { namespace orm = sqlite_orm;
            auto rows = st->get_all<UserSettingRow>(
                orm::where(orm::c(&UserSettingRow::userId) == msg.senderId
                    and orm::c(&UserSettingRow::groupId) == cardScope(msg)));
            for (auto& r : rows)
                if (r.key.rfind("sattr:", 0) == 0 && !r.value.empty())
                    out[r.key.substr(6)] = r.value;
        } catch (...) {}
        return out;
    }
    // 求一个表达式属性的当前值（按引用的其它属性求值）。失败→nullopt。
    std::optional<int> evalStrAttr(const Message& msg, const std::string& canon) const {
        std::string e = getStrAttr(msg, canon);
        if (e.empty()) return std::nullopt;
        if (!e.empty() && e[0] == '-') { /* 负号开头由 eval 处理 */ }
        return evalComputedFormula(e, msg);
    }
    /// 把掷骰表达式里的「公式属性」引用（db/DB/伤害加值…）替换成其存储的公式（带括号）。
    /// 如 .r 1d4+db（db 存了 1d4）→ 1d4+(1d4)。未录入则原样。
    std::string substituteFormulaAttrs(const std::string& expr, const Message& msg) const {
        // D#03：未单独录入 DB 时也按 力量+体型 现算，避免 +db 被当骰子(d100 奖励骰)。
        std::string f = effectiveDamageBonus(msg);  // 伤害加值（手动优先，未存则现算）
        if (f.empty() || expr.empty()) return expr;
        std::string repl = "(" + f + ")", out = expr;
        static const char* toks[] = { "\xe4\xbc\xa4\xe5\xae\xb3\xe5\x8a\xa0\xe5\x80\xbc",  // 伤害加值
            "DamageBonus", "db", "DB", "Db", "dB" };
        for (auto* t : toks) {
            std::string tok = t; size_t p = 0;
            while ((p = out.find(tok, p)) != std::string::npos) {
                bool okB = (p == 0) || !std::isalnum((unsigned char)out[p - 1]);
                size_t e = p + tok.size();
                bool okA = (e >= out.size()) || !std::isalnum((unsigned char)out[e]);
                if (okB && okA) { out.replace(p, tok.size(), repl); p += repl.size(); }
                else p = e;
            }
        }
        return out;
    }

    bool autoCardEnabled() const { return cfg_.get<bool>("dice/auto_card", true); }
    /// 某次 .st 改动是否影响生命/理智/魔法（决定是否追加卡面摘要，避免改技能也刷摘要）。
    /// 两边都 canonical 化比对，规避「生命↔体力」这类同义词不一致。
    static bool touchesVital(const std::map<std::string, int>& changes) {
        static const char* seeds[] = {
            "\xe4\xbd\x93\xe8\xb4\xa8", "\xe4\xbd\x93\xe5\x9e\x8b", "\xe6\x84\x8f\xe5\xbf\x97",   // 体质 体型 意志
            "\xe5\x85\x8b\xe8\x8b\x8f\xe9\xb2\x81\xe7\xa5\x9e\xe8\xaf\x9d",                       // 克苏鲁神话
            "\xe7\x94\x9f\xe5\x91\xbd", "\xe7\x90\x86\xe6\x99\xba", "\xe9\xad\x94\xe6\xb3\x95" };  // 生命 理智 魔法
        std::set<std::string> vc;
        for (auto* s : seeds) vc.insert(CharacterCardStore::canonical(s));
        // 字面别名兜底（规避个别 canonical 异常）。
        static const std::set<std::string> raw = {
            "\xe4\xbd\x93\xe8\xb4\xa8","\xe4\xbd\x93\xe5\x9e\x8b","\xe6\x84\x8f\xe5\xbf\x97",      // 体质 体型 意志
            "\xe7\x94\x9f\xe5\x91\xbd","\xe4\xbd\x93\xe5\x8a\x9b","\xe7\x90\x86\xe6\x99\xba","\xe9\xad\x94\xe6\xb3\x95",  // 生命 体力 理智 魔法
            "\xe5\x85\x8b\xe8\x8b\x8f\xe9\xb2\x81\xe7\xa5\x9e\xe8\xaf\x9d",                          // 克苏鲁神话
            "CON","SIZ","POW","HP","SAN","MP","STR","DEX",
            "\xe7\x94\x9f\xe5\x91\xbd\xe5\x80\xbc","\xe7\x90\x86\xe6\x99\xba\xe5\x80\xbc","\xe9\xad\x94\xe6\xb3\x95\xe5\x80\xbc" };  // 生命值 理智值 魔法值
        for (auto& [k, _] : changes)
            if (vc.count(CharacterCardStore::canonical(k)) || raw.count(k) || raw.count(CharacterCardStore::canonical(k)))
                return true;
        return false;
    }

    std::string handleBuff(Locale loc, const std::string& args, const Message& msg) {
        const std::string nick = displayName(msg);
        std::string a = trim(args), la = toLower(a);
        if (a.empty() || la == "show") {
            auto bs = listBuffs(msg);
            return bs.empty() ? i18n_.tr(loc, "buff.empty", {{"nick", nick}})
                              : i18n_.tr(loc, "buff.show", {{"nick", nick}, {"detail", joinAttrs(bs)}});
        }
        if (la == "clr" || la == "clear") {
            auto* st = db_.getStorage();
            if (st) try { namespace orm = sqlite_orm;
                auto rows = st->get_all<UserSettingRow>(orm::where(
                    orm::c(&UserSettingRow::userId) == msg.senderId and
                    orm::c(&UserSettingRow::groupId) == cardScope(msg)));
                for (auto& r : rows) if (r.key.rfind("buff:", 0) == 0)
                    st->remove_all<UserSettingRow>(orm::where(orm::c(&UserSettingRow::id) == r.id));
            } catch (...) {}
            return i18n_.tr(loc, "buff.cleared", {{"nick", nick}});
        }
        if (la.rfind("del", 0) == 0) {
            std::istringstream iss(trim(a.substr(3))); std::string t;
            while (iss >> t) clearUserSetting(msg, "buff:" + CharacterCardStore::canonical(t));
            return i18n_.tr(loc, "buff.deleted", {{"nick", nick}});
        }
        // Entries: "属性:值" / "属性=值" / "属性±表达式"（值/表达式可含骰点，立即求值为整数）。
        std::map<std::string, int> changes;
        std::istringstream iss(a); std::string tok;
        while (iss >> tok) {
            size_t sp = tok.find_first_of(":=+-");
            if (sp == std::string::npos || sp == 0) continue;
            std::string name = tok.substr(0, sp);
            bool adjust = (tok[sp] == '+' || tok[sp] == '-');
            std::string valStr = adjust ? tok.substr(sp) : tok.substr(sp + 1);
            if (valStr.empty()) continue;
            int val;
            if (adjust) { auto r = engine_.roll("0" + valStr); if (!r.ok()) continue; val = getBuff(msg, name) + r.modifiedTotal; }
            else { auto r = engine_.roll(valStr); val = r.ok() ? r.modifiedTotal : parseIntOr(valStr, 0); }
            std::string key = "buff:" + CharacterCardStore::canonical(name);
            if (val == 0) clearUserSetting(msg, key); else setUserSetting(msg, key, std::to_string(val));
            changes[CharacterCardStore::canonical(name)] = val;
        }
        if (changes.empty()) return i18n_.tr(loc, "buff.usage");
        return i18n_.tr(loc, "buff.set", {{"nick", nick}, {"detail", joinAttrs(changes)}});
    }

    /// DND5e d20 check (.rc when DND mode is on): d20 + 属性调整(含 buff) [优势/劣势].
    std::string handleDndCheck(Locale loc, const Message& msg, const std::string& rest) {
        std::string s = trim(rest);
        int adv = 0;
        if (s.rfind("\xe4\xbc\x98\xe5\x8a\xbf", 0) == 0)      { adv = 1;  s = trim(s.substr(6)); }  // 优势
        else if (s.rfind("\xe5\x8a\xa3\xe5\x8a\xbf", 0) == 0) { adv = -1; s = trim(s.substr(6)); }  // 劣势
        if (s.empty()) return i18n_.tr(loc, "dnd.rc.usage");

        auto [first, remainder] = splitCommand(s);
        std::string namePart = first; int inlineAdj = 0;
        if (size_t pm = first.find_first_of("+-"); pm != std::string::npos && pm > 0) {
            namePart = first.substr(0, pm);
            inlineAdj = parseIntOr(first.substr(pm), 0);
        }
        int mod = 0;
        if (isAllDigits(namePart)) { mod = parseIntOr(namePart, 0); }
        else {
            auto v = effectiveAttr(msg, namePart);
            if (!v) return i18n_.tr(loc, "dice.check.no_card", {{"attr", namePart}});
            mod = isDndAbility(namePart) ? dndAbilityModifier(*v) : dndSkillValue(msg, namePart).value_or(*v);
        }
        mod += inlineAdj;

        int d1 = engine_.roll("1d20").modifiedTotal;
        int rolled = d1; std::string rollStr = "1d20=" + std::to_string(d1);
        if (adv != 0) {
            int d2 = engine_.roll("1d20").modifiedTotal;
            // Ternary (not std::max/min) — avoids the Windows min/max macro clash.
            rolled = adv > 0 ? (d1 > d2 ? d1 : d2) : (d1 < d2 ? d1 : d2);
            rollStr = std::string(adv > 0 ? "\xe4\xbc\x98\xe5\x8a\xbf" : "\xe5\x8a\xa3\xe5\x8a\xbf")  // 优势/劣势
                + "[" + std::to_string(d1) + "," + std::to_string(d2) + "]\xe2\x86\x92" + std::to_string(rolled);
        }
        int total = rolled + mod;
        std::string modStr = (mod >= 0 ? "+" : "") + std::to_string(mod);
        std::string label = remainder.empty() ? first : remainder;
        return i18n_.tr(loc, "dnd.rc.result",
            {{"nick", displayName(msg)}, {"attr", label}, {"roll", rollStr},
             {"mod", modStr}, {"total", std::to_string(total)}});
    }

    // D#07: DND ability-modifier check. Ability scores are converted with
    // floor((score - 10) / 2); non-ability names use the separate dndskill: store.
    std::optional<std::string> tryHandleRdc(Locale loc, const Message& msg, const std::string& cmd) {
        if (toLower(cmd).rfind("rdc", 0) != 0) return std::nullopt;
        if (!dndModeOn(msg)) return i18n_.tr(loc, "dnd.rdc.mode_off");
        std::string s = trim(cmd.substr(3));
        if (s.empty()) return i18n_.tr(loc, "dnd.rdc.usage");

        std::vector<std::string> tokens;
        { std::istringstream iss(s); std::string token; while (iss >> token) tokens.push_back(token); }
        int turns = 1, advantage = 0, extra = 0, threshold = -1;
        if (!tokens.empty()) {
            size_t hash = tokens.front().find('#');
            if (hash != std::string::npos) {
                std::string count = tokens.front().substr(0, hash);
                if (!count.empty() && isAllDigits(count)) turns = parseIntOr(count, 1);
                std::string tail = tokens.front().substr(hash + 1);
                if (tail.empty()) tokens.erase(tokens.begin()); else tokens.front() = tail;
            }
        }
        if (turns < 1 || turns > 9) return i18n_.tr(loc, "dnd.rdc.turns");
        if (!tokens.empty()) {
            std::string flag = toLower(tokens.front());
            if (flag == "b") { advantage = 1; tokens.erase(tokens.begin()); }
            else if (flag == "p") { advantage = -1; tokens.erase(tokens.begin()); }
        }
        if (!tokens.empty() && isAllDigits(tokens.back())) {
            threshold = parseIntOr(tokens.back(), -1);
            tokens.pop_back();
        }
        std::string attr;
        if (!tokens.empty() && !tokens.front().empty() && tokens.front()[0] != '+' && tokens.front()[0] != '-') {
            attr = tokens.front();
            tokens.erase(tokens.begin());
            size_t plusMinus = attr.find_first_of("+-");
            if (plusMinus != std::string::npos && plusMinus > 0) {
                extra += parseIntOr(attr.substr(plusMinus), 0);
                attr = attr.substr(0, plusMinus);
            }
        }
        if (!tokens.empty() && (tokens.front()[0] == '+' || tokens.front()[0] == '-')) {
            extra += parseIntOr(tokens.front(), 0);
            tokens.erase(tokens.begin());
        }
        std::string reason;
        for (const auto& token : tokens) { if (!reason.empty()) reason += " "; reason += token; }

        int modifier = extra;
        std::string label = attr.empty() ? "D20" : attr;
        if (!attr.empty()) {
            std::string canonical = CharacterCardStore::canonical(attr);
            // A separately stored DND skill always wins. Otherwise an existing
            // numeric card attribute is an ability score (including legacy cards
            // created before the split), so turn it into the DND adjustment value.
            // This deliberately follows the live card lookup rather than a fixed
            // alias list: rule packs are allowed to customize attribute canonicals.
            const std::string skillKey = "dndskill:" + canonical;
            const bool isAbility = isDndAbility(attr);
            // Keep the original token here, matching `.rc` exactly; effectiveAttr
            // performs its own canonicalization during the card lookup.
            auto score = effectiveAttr(msg, attr);
            if (isAbility && score) {
                // Go through the normal card lookup chain so ability aliases and any
                // active derived value stay consistent with `.rc` / `.st show`.
                // effectiveAttr already includes a temporary buff in the score, so do
                // not add getBuff again here.
                modifier += dndAbilityModifier(*score);
            } else {
                auto skill = dndSkillValue(msg, canonical);
                if (!skill) return i18n_.tr(loc, "dnd.rdc.no_value", {{"attr", attr}});
                modifier += *skill + getBuff(msg, canonical);
            }
        }

        std::string detail;
        for (int round = 1; round <= turns; ++round) {
            int first = engine_.roll("1d20").modifiedTotal, chosen = first;
            std::string roll = "1D20=" + std::to_string(first);
            if (advantage != 0) {
                int second = engine_.roll("1d20").modifiedTotal;
                chosen = advantage > 0 ? (first > second ? first : second) : (first < second ? first : second);
                roll = std::string(advantage > 0 ? "B" : "P") + "[" + std::to_string(first) + "," + std::to_string(second) + "]=" + std::to_string(chosen);
            }
            int total = chosen + modifier;
            if (!detail.empty()) detail += "\n";
            detail += std::to_string(round) + ". " + roll + (modifier >= 0 ? "+" : "") + std::to_string(modifier) + "=" + std::to_string(total);
            if (threshold >= 0) detail += total >= threshold ? " 成功" : " 失败";
        }
        return i18n_.tr(loc, "dnd.rdc.result", {{"nick", displayName(msg)}, {"attr", label},
            {"reason", reason.empty() ? "" : "（" + reason + "）"}, {"detail", detail},
            {"threshold", threshold >= 0 ? std::to_string(threshold) : ""}});
    }

    // ─── .st — character card set/show/clear/del ─────────────

    // D#02：清空该用户在本卡作用域下所有「卡相关」user_settings（关联属性 sattr: /
    // 公式属性 attrf: / 武器 wpn: / 上限 max:）。.st clr 原先只清 CharacterCard 整数属性，
    // 这些存在 user_settings 里的会残留（.st show 仍显示、.st clr 清不掉、回执报 card empty）。
    bool clearCardUserSettings(const Message& msg) {
        auto* st = db_.getStorage(); if (!st) return false;
        bool removed = false;
        try {
            namespace orm = sqlite_orm;
            auto rows = st->get_all<UserSettingRow>(orm::where(
                orm::c(&UserSettingRow::userId) == msg.senderId and
                orm::c(&UserSettingRow::groupId) == cardScope(msg)));
            for (auto& r : rows) {
                if (r.key.rfind("sattr:", 0) == 0 || r.key.rfind("attrf:", 0) == 0
                    || r.key.rfind("attrfauto:", 0) == 0   // D#03：DB 自动/手动标记
                    || r.key.rfind("wpn:", 0) == 0 || r.key.rfind("max:", 0) == 0
                    || r.key.rfind("dndskill:", 0) == 0) { // D#07：DND 独立技能检定加值
                    st->remove<UserSettingRow>(r.id); removed = true;
                }
            }
        } catch (...) {}
        return removed;
    }

    std::optional<std::string> tryHandleST(Locale loc, const Message& msg,
                                           const std::string& cmd) {
        if (toLower(cmd).rfind("st", 0) != 0) return std::nullopt;
        std::string rest = trim(cmd.substr(2));
        const std::string nick = displayName(msg);
        const std::string user = msg.senderId;
        const std::string group = cardScope(msg);
        std::string lrest = toLower(rest);

        if (rest.empty()) return i18n_.tr(loc, "card.st.usage");

        // 原版卡片锁定（CharaCard::locked）：w=锁写（set/del/clr 拒），r=锁读（show 拒）。
        const bool lockedW = cards_.cardLocked(user, group, "w");

        if (lrest == "clr") {
            if (lockedW) return i18n_.tr(loc, "card.locked_w");
            bool had = cards_.clear(user, group);
            had = clearCardUserSettings(msg) || had;   // D#02：连带清关联属性/公式/武器/上限
            return i18n_.tr(loc, had ? "card.cleared" : "card.empty", {{"nick", nick}});
        }
        if (lrest == "show" || lrest.rfind("show ", 0) == 0) {
            if (cards_.cardLocked(user, group, "r")) return i18n_.tr(loc, "card.locked_r");
            std::string attr = trim(rest.substr(4));
            if (attr.empty()) {
                auto attrs = cards_.getAttrs(user, group);
                std::string detail = joinAttrsVital(attrs, attrMax("\xe7\x94\x9f\xe5\x91\xbd\xe5\x80\xbc", msg));
                // 关联/表达式属性 (C#37)：附在后面，显示「属性=表达式（=当前值）」。
                for (auto& [k, e] : listStrAttrs(msg)) {
                    if (!detail.empty()) detail += " ";
                    detail += k + "=" + e;
                    if (auto cv = evalComputedFormula(e, msg)) detail += "(=" + std::to_string(*cv) + ")";
                }
                if (dndModeOn(msg)) for (auto& [k, v] : listDndSkills(msg)) {
                    if (!detail.empty()) detail += " ";
                    detail += "技能." + k + ":" + std::to_string(v);
                }
                if (detail.empty()) return i18n_.tr(loc, "card.empty", {{"nick", nick}});
                return i18n_.tr(loc, "card.show", {{"nick", nick}, {"detail", detail}});
            }
            // SealDice compat: ".st show <数字>" → attributes ≥ N.
            if (isAllDigits(attr)) {
                int th = parseIntOr(attr, 0);
                std::map<std::string, int> hi;
                for (auto& [k, v] : cards_.getAttrs(user, group)) if (v >= th) hi[k] = v;
                if (hi.empty()) return i18n_.tr(loc, "card.empty", {{"nick", nick}});
                return i18n_.tr(loc, "card.show", {{"nick", nick}, {"detail", joinAttrs(hi)}});
            }
            // ".st show 力量 敏捷 …" → show several specific attributes.
            if (attr.find(' ') != std::string::npos) {
                std::map<std::string, int> picked;
                std::istringstream iss(attr); std::string tk;
                while (iss >> tk) {
                    std::string canonical = CharacterCardStore::canonical(tk);
                    auto v = cards_.getAttr(user, group, tk);
                    if (!v && dndModeOn(msg) && !isDndCardSpecialInput(tk)) v = dndSkillValue(msg, canonical);
                    if (v) picked[canonical] = *v;
                }
                if (picked.empty()) return i18n_.tr(loc, "card.empty", {{"nick", nick}});
                return i18n_.tr(loc, "card.show", {{"nick", nick}, {"detail", joinAttrs(picked)}});
            }
            std::string canonical = CharacterCardStore::canonical(attr);
            auto v = cards_.getAttr(user, group, attr);
            if (!v && dndModeOn(msg) && !isDndCardSpecialInput(attr)) v = dndSkillValue(msg, canonical);
            if (v) return i18n_.tr(loc, "card.show_one",
                {{"nick", nick}, {"attr", attr}, {"val", std::to_string(*v)}});
            return i18n_.tr(loc, "card.attr_missing", {{"attr", attr}});
        }
        if (lrest.rfind("del ", 0) == 0 || lrest == "del") {
            if (lockedW) return i18n_.tr(loc, "card.locked_w");
            std::string attr = trim(rest.substr(3));
            if (attr.empty()) return i18n_.tr(loc, "card.st.usage");
            std::string canonical = CharacterCardStore::canonical(attr);
            bool ok = false;
            if (dndModeOn(msg) && !isDndCardSpecialInput(attr)) {
                const std::string skillKey = "dndskill:" + canonical;
                ok = !getUserSetting(msg, skillKey).empty();
                clearUserSetting(msg, skillKey);
            }
            ok = cards_.eraseAttr(user, group, attr) || ok; // also clear pre-split legacy values
            return i18n_.tr(loc, ok ? "card.attr_deleted" : "card.attr_missing",
                {{"nick", nick}, {"attr", attr}});
        }
        // ".st export" → dump the card as a re-importable ".st 力量50 敏捷60 …" string.
        if (lrest == "export") {
            auto attrs = cards_.getAttrs(user, group);
            auto skills = dndModeOn(msg) ? listDndSkills(msg) : std::map<std::string, int>{};
            if (attrs.empty() && skills.empty()) return i18n_.tr(loc, "card.empty", {{"nick", nick}});
            std::string body;
            for (auto& [k, val] : attrs) { if (!body.empty()) body += " "; body += k + std::to_string(val); }
            for (auto& [k, val] : skills) { if (!body.empty()) body += " "; body += k + std::to_string(val); }
            return i18n_.tr(loc, "card.export", {{"nick", nick}, {"body", body}});
        }
        // ".st fmt" → re-normalize attribute names via the synonym table (转卡).
        if (lrest == "fmt") {
            auto attrs = cards_.getAttrs(user, group);
            for (auto& [k, val] : attrs) {
                std::string canon = CharacterCardStore::canonical(k);
                if (canon != k) { cards_.setAttr(user, group, canon, val); cards_.eraseAttr(user, group, k); }
            }
            return i18n_.tr(loc, "card.fmt_done", {{"nick", nick}});
        }

        // 到这里都是「写卡」路径：卡片被锁写（w）则统一拒绝（原版 strPcLockedWrite）。
        if (lockedW) return i18n_.tr(loc, "card.locked_w");

        // COC 武器映射: peel off "&<名>=<伤害式>" tokens — stored as a rollable
        // string (NOT rolled now) so ".r 手枪" rolls the damage each time. The
        // remaining tokens fall through to normal numeric attribute parsing.
        {
            std::map<std::string, std::string> weapons;
            std::string cleaned; std::istringstream iss(rest); std::string tok;
            while (iss >> tok) {
                if (tok.size() > 1 && tok[0] == '&') {
                    std::string body = tok.substr(1);
                    size_t eq = body.find_first_of("=:");
                    if (eq != std::string::npos && eq > 0 && eq + 1 < body.size()) {
                        weapons[body.substr(0, eq)] = body.substr(eq + 1);
                        continue;
                    }
                }
                if (!cleaned.empty()) cleaned += " ";
                cleaned += tok;
            }
            if (!weapons.empty()) {
                std::string detail;
                for (auto& [wn, we] : weapons) {
                    setUserSetting(msg, "wpn:" + wn, we);
                    if (!detail.empty()) detail += " ";
                    detail += wn + "=" + we;
                }
                rest = cleaned;
                if (trim(rest).empty())
                    return i18n_.tr(loc, "card.weapon.set", {{"nick", nick}, {"detail", detail}});
            }
        }

        // Attribute entry: "力量50 敏捷60 hp-2 理智+1d3" ...
        std::map<std::string, int> changes;
        std::map<std::string, std::string> formulaChanges;   // 公式属性（伤害加值）：存原文
        std::map<std::string, std::string> strChanges;       // 关联/表达式属性 (C#37)：存原文

        // D#07：DND 钱币按最小单位（cp）统一结算，写回时按 pp/gp/ep/sp/cp
        // 标准面额分解。这样 `.st gp-1` 在金币不足时会自动从更高面额换开，
        // `.st cp+1` 也会把已有零钱规整为可读的面额组合，且总价值绝不为负。
        struct CoinUnit { const char* name; long long cp; };
        static constexpr std::array<CoinUnit, 5> DND_COINS{{
            {"铂金币", 1000}, {"金币", 100}, {"银金币", 50}, {"银币", 10}, {"铜币", 1}
        }};
        auto isDndCoin = [](const std::string& key) {
            for (const auto& coin : DND_COINS) if (key == coin.name) return true;
            return false;
        };
        auto applyDndCoinChange = [&](const std::string& key, int value, bool modifier) {
            long long totalCp = 0, oldTargetCp = 0, unitCp = 0;
            for (const auto& coin : DND_COINS) {
                long long old = cards_.getAttr(user, group, coin.name).value_or(0);
                if (old > 0) totalCp += old * coin.cp;
                if (key == coin.name) { oldTargetCp = (old > 0 ? old : 0) * coin.cp; unitCp = coin.cp; }
            }
            if (modifier) totalCp += static_cast<long long>(value) * unitCp;
            else totalCp = totalCp - oldTargetCp + static_cast<long long>(value > 0 ? value : 0) * unitCp;
            if (totalCp < 0) totalCp = 0;

            for (const auto& coin : DND_COINS) {
                const int old = cards_.getAttr(user, group, coin.name).value_or(0);
                const int next = static_cast<int>(totalCp / coin.cp);
                totalCp %= coin.cp;
                if (old != next) {
                    cards_.setAttr(user, group, coin.name, next);
                    changes[coin.name] = next;
                }
            }
        };

        // 自动卡面（仅 COC 类、非 DND 模式）：先记录三围(生命/理智/魔法)改动前的当前值+上限，
        // 应用改动后据「上限变化量」同步当前值（4/8 体质↑→5/9，而非重置满）。开关 dice.auto_card。
        // 上限取法同原版 maxOf：先显式上限/「max:」，再退 model.xml 的「最大X」衍生公式。
        struct VitalSnap { std::string canon; std::string zh; std::string maxName; std::optional<int> oldCur; std::optional<int> oldMax; };
        auto vitalMax = [&](const std::string& baseCanon, const std::string& maxName) -> std::optional<int> {
            if (auto m = attrMax(baseCanon, msg)) return m;
            return derivedAttr(msg, maxName);
        };
        std::vector<VitalSnap> vitalSnap;
        const bool autoCardOn = autoCardEnabled() && !dndModeOn(msg);
        if (autoCardOn) {
            // {当前值查询名, 显示名, 最大值衍生名}。查询名用「生命/理智/魔法」而非「生命值/理智/魔法值」：
            // 前者一步 canonical 到用户卡面真正使用的入口（与 .sn/.sc/.st hp 一致）；后者可能被某些
            // mod 的 model.xml 反向别名到「体力」等另一个键，导致读写到不同条目（实测踩过）。见 card_store kSyn。
            struct VK { const char* look; const char* zh; const char* mx; };
            const VK vk[] = {
                {"\xe7\x94\x9f\xe5\x91\xbd", "\xe7\x94\x9f\xe5\x91\xbd\xe5\x80\xbc", "\xe6\x9c\x80\xe5\xa4\xa7\xe7\x94\x9f\xe5\x91\xbd\xe5\x80\xbc"},  // 生命 / 生命值 / 最大生命值
                {"\xe7\x90\x86\xe6\x99\xba", "\xe7\x90\x86\xe6\x99\xba",             "\xe6\x9c\x80\xe5\xa4\xa7\xe7\x90\x86\xe6\x99\xba\xe5\x80\xbc"},  // 理智 / 理智 / 最大理智值
                {"\xe9\xad\x94\xe6\xb3\x95", "\xe9\xad\x94\xe6\xb3\x95\xe5\x80\xbc", "\xe6\x9c\x80\xe5\xa4\xa7\xe9\xad\x94\xe6\xb3\x95\xe5\x80\xbc"} };  // 魔法 / 魔法值 / 最大魔法值
            for (auto& v : vk) {
                std::string canon = CharacterCardStore::canonical(v.look);
                vitalSnap.push_back({canon, v.zh, v.mx, cards_.getAttr(user, group, canon), vitalMax(canon, v.mx)});
            }
        }

        const std::string& p = rest;
        size_t i = 0;
        while (i < p.size()) {
            while (i < p.size() && (p[i] == ' ' || p[i] == '|' || p[i] == '&')) ++i;  // & = 青果/海豹前缀，忽略
            if (i >= p.size()) break;
            // attribute name = run until a digit / sign / separator
            size_t nameStart = i;
            while (i < p.size()) {
                char c = p[i];
                if (std::isdigit(static_cast<unsigned char>(c)) ||
                    c == '+' || c == '-' || c == '=' || c == ':' || c == ' ' || c == '|') break;
                ++i;
            }
            std::string attr = p.substr(nameStart, i - nameStart);
            if (attr.empty()) { ++i; continue; }
            while (i < p.size() && (p[i] == '=' || p[i] == ':')) ++i;
            bool modifier = false; int sign = 1;
            if (i < p.size() && (p[i] == '+' || p[i] == '-')) {
                modifier = true; if (p[i] == '-') sign = -1; ++i;
            }
            // C#37 关联/表达式属性：值以引号开头（'dex+1'），或以字母/中文开头的属性引用
            // 算式（敏捷+1）——把整段当表达式存为字符串属性，读取时按引用的属性求值。
            // 'd'+数字 仍当骰子（交给下方数字路径），如 .st x=d4。
            {
                char c0 = (i < p.size()) ? p[i] : '\0';
                bool quoted = (c0 == '\'' || c0 == '"');
                bool attrRef = false;
                if (!quoted && c0 != '\0' && !std::isdigit(static_cast<unsigned char>(c0))) {
                    unsigned char uc = static_cast<unsigned char>(c0);
                    bool diceLike = (uc == 'd' || uc == 'D') && (i + 1 < p.size())
                                    && std::isdigit(static_cast<unsigned char>(p[i + 1]));
                    if (!diceLike && (uc >= 0x80 || std::isalpha(uc))) attrRef = true;
                }
                if (quoted || attrRef) {
                    std::string expr;
                    if (quoted) { char q = c0; ++i; size_t s = i;
                        while (i < p.size() && p[i] != q) ++i;
                        expr = p.substr(s, i - s); if (i < p.size()) ++i; }
                    else { size_t s = i;
                        while (i < p.size() && p[i] != ' ' && p[i] != '|') ++i;
                        expr = p.substr(s, i - s); }
                    expr = trim(expr);
                    if (!expr.empty()) {
                        std::string scanon = CharacterCardStore::canonical(attr);
                        std::string full = (modifier && sign < 0 ? "-" : "") + expr;
                        setStrAttr(msg, scanon, full);
                        cards_.eraseAttr(msg.senderId, cardScope(msg), scanon);  // 清掉同名数值，避免歧义
                        strChanges[scanon] = full;
                    }
                    continue;
                }
            }
            size_t valStart = i;
            // Integer part.
            while (i < p.size() && std::isdigit(static_cast<unsigned char>(p[i]))) ++i;
            // Optional dice/arithmetic continuation (e.g. "1d3", "2d6+1"). Only
            // consume a 'd'/operator when it is genuinely part of the expression
            // — i.e. followed by a digit (or another 'd'). Otherwise it belongs
            // to the NEXT attribute's name, so "敏捷55dex55" does NOT eat the 'd'
            // of "dex" into "55d" (which would drop 敏捷 and invent an "ex").
            while (i < p.size()) {
                char c = p[i];
                // NOTE: '/' is intentionally NOT an operator here — it separates
                // current/max ("hp4/10"), handled right after this loop.
                if (c == 'd' || c == 'D' || c == '+' || c == '-' || c == '*') {
                    char nxt = (i + 1 < p.size()) ? p[i + 1] : '\0';
                    if (std::isdigit(static_cast<unsigned char>(nxt)) || nxt == 'd' || nxt == 'D') {
                        ++i; continue;
                    }
                    break;
                }
                if (std::isdigit(static_cast<unsigned char>(c))) { ++i; continue; }
                break;
            }
            std::string valTok = p.substr(valStart, i - valStart);
            if (valTok.empty()) continue;
            // 公式属性（伤害加值 DB）：存投掷公式原文，不摇成数。.st db=+1d4 带 + 不报错。
            if (std::string fcanon = CharacterCardStore::canonical(attr); isFormulaAttr(fcanon)) {
                std::string formula = (modifier && sign < 0 ? "-" : "") + valTok;   // 去掉前导 +，保留 -
                setFormulaAttr(msg, fcanon, formula);
                setUserSetting(msg, "attrfauto:" + fcanon, "0");   // D#03：手动录入 → 关自动，优先保留
                formulaChanges[fcanon] = formula;
                continue;
            }
            int value;
            if (valTok.find('d') != std::string::npos || valTok.find('D') != std::string::npos) {
                auto r = engine_.roll(valTok);
                if (!r.ok()) continue;
                value = r.modifiedTotal;
            } else {
                value = parseIntOr(valTok, 0);
            }
            value *= sign;
            std::string key = CharacterCardStore::canonical(attr);
            // Keep DND ability labels stable in replies even when another active
            // rule pack has registered a different global alias for the same word.
            if (dndModeOn(msg)) if (auto ability = dndAbilityName(attr)) key = *ability;
            // Optional "/<max>" companion: ".st hp4/10" / "生命值13/13" stores the
            // cap so .sn can render "<cur>/<max>". (Common in 车卡 tool output.)
            if (i < p.size() && p[i] == '/') {
                ++i; size_t ms = i;
                while (i < p.size() && (std::isdigit((unsigned char)p[i]) || p[i] == 'd' || p[i] == 'D')) ++i;
                std::string maxTok = p.substr(ms, i - ms);
                if (!maxTok.empty()) {
                    int mx;
                    if (maxTok.find('d') != std::string::npos || maxTok.find('D') != std::string::npos) {
                        auto rr = engine_.roll(maxTok); mx = rr.ok() ? rr.modifiedTotal : parseIntOr(maxTok, 0);
                    } else mx = parseIntOr(maxTok, 0);
                    setUserSetting(msg, "max:" + key, std::to_string(mx));
                }
            }
            // D#06：生命值上限夹取 + 临时生命值（护盾）——受击优先扣临时，溢出再扣真实，
            // 且真实血量不小于 0、不超过上限；治疗/设值同样夹在 [0, 上限]。
            static const std::string HP_CANON = "\xe7\x94\x9f\xe5\x91\xbd\xe5\x80\xbc";       // 生命值
            static const std::string HPTEMP_CANON = "\xe4\xb8\xb4\xe6\x97\xb6\xe7\x94\x9f\xe5\x91\xbd\xe5\x80\xbc";  // 临时生命值
            if (key == HP_CANON) {
                int curHp = cards_.getAttr(user, group, HP_CANON).value_or(0);
                auto hpmaxOpt = attrMax(HP_CANON, msg);
                if (modifier && value < 0) {                     // 受击：护盾先扛
                    int dmg = -value;
                    int temp = cards_.getAttr(user, group, HPTEMP_CANON).value_or(0);
                    int fromTemp = temp >= dmg ? dmg : temp;
                    if (fromTemp > 0) {
                        int nt = temp - fromTemp;
                        cards_.setAttr(user, group, HPTEMP_CANON, nt);
                        changes[HPTEMP_CANON] = nt;
                    }
                    int nv = curHp - (dmg - fromTemp);
                    if (nv < 0) nv = 0;
                    cards_.setAttr(user, group, HP_CANON, nv);
                    changes[key] = nv;
                } else {                                         // 设值/治疗：夹 [0, 上限]
                    int nv = modifier ? curHp + value : value;
                    if (nv < 0) nv = 0;
                    if (hpmaxOpt && nv > *hpmaxOpt) nv = *hpmaxOpt;
                    cards_.setAttr(user, group, HP_CANON, nv);
                    changes[key] = nv;
                }
                continue;
            }
            if (isDndCoin(key)) {
                applyDndCoinChange(key, value, modifier);
                continue;
            }
            // D#07：DND 的技能检定加值与六项属性值分区保存。技能曾被录在普通卡
            // 属性中的旧数据仍可由 dndSkillValue 读取；一旦修改即迁移到独立 dndskill: 键。
            if (dndModeOn(msg) && !isDndCardSpecialInput(attr)) {
                int newVal = modifier ? dndSkillValue(msg, key).value_or(0) + value : value;
                setUserSetting(msg, "dndskill:" + key, std::to_string(newVal));
                cards_.eraseAttr(user, group, key); // remove legacy same-name skill to avoid split ambiguity
                changes["技能." + key] = newVal;
                continue;
            }
            int newVal = modifier ? cards_.getAttr(user, group, attr).value_or(0) + value : value;
            if (key == HPTEMP_CANON && newVal < 0) newVal = 0;   // 临时血量不为负
            cards_.setAttr(user, group, attr, newVal);
            changes[key] = newVal;
        }
        if (changes.empty() && formulaChanges.empty() && strChanges.empty()) return i18n_.tr(loc, "card.st.usage");
        // 公式属性（伤害加值）单独拼进 detail（值是公式原文，不是数）。
        std::string fdetail = joinAttrs(changes);
        for (auto& [k, f] : formulaChanges) { if (!fdetail.empty()) fdetail += " "; fdetail += k + ":" + f; }
        // 关联/表达式属性 (C#37)：显示「属性=表达式（=当前值）」。
        for (auto& [k, e] : strChanges) {
            if (!fdetail.empty()) fdetail += " ";
            fdetail += k + "=" + e;
            if (auto v = evalComputedFormula(e, msg)) fdetail += "(=" + std::to_string(*v) + ")";
        }
        // 自动卡面：基础属性(体质/体型/意志/克苏鲁神话)变化 → 重算三围上限，并按「上限变化量」
        // 同步当前值（用户本次没显式改的才动）。只把「真正变化」的项汇报给用户，没变就不输出。
        std::string recalcDetail;
        if (autoCardOn && touchesVital(changes)) {
            for (auto& vs : vitalSnap) {
                if (changes.count(vs.canon)) continue;        // 本次显式改了 → 尊重用户输入
                auto newMax = vitalMax(vs.canon, vs.maxName);
                if (!newMax) continue;                         // 该规则无此衍生（非 COC）→ 跳过
                std::optional<int> newCur;
                if (!vs.oldCur) {
                    newCur = *newMax;                          // 首次可推导 → 落地为满值
                } else if (vs.oldMax && *vs.oldMax != *newMax) {
                    int v = *vs.oldCur + (*newMax - *vs.oldMax);   // 上限变了 → 当前按差值同步
                    if (v < 0) v = 0; if (v > *newMax) v = *newMax; // 夹 [0,新上限]（避开 windows.h max 宏）
                    newCur = v;
                } else {
                    continue;                                  // 上限没变且已有当前值 → 无需调整
                }
                if (newCur == vs.oldCur) continue;             // 实际没变 → 不汇报
                cards_.setAttr(user, group, vs.canon, *newCur);
                if (!recalcDetail.empty()) recalcDetail += "，";
                if (!vs.oldCur)
                    recalcDetail += vs.zh + " " + std::to_string(*newCur) + "/" + std::to_string(*newMax);
                else
                    recalcDetail += vs.zh + " " + std::to_string(*vs.oldCur) + "/"
                                  + std::to_string(vs.oldMax ? *vs.oldMax : *newCur)
                                  + " → " + std::to_string(*newCur) + "/" + std::to_string(*newMax);
            }
        }
        // D#03：伤害加值(DB) 自动计算。力量/体型有变、且 DB 非手动、本次未显式录 DB 时，
        // 按 STR+SIZ 查表算 DB 存为公式属性（手动录入的自定义 DB 优先，不覆盖）。
        std::string dbDetail;
        {
            static const std::string DB = "\xe4\xbc\xa4\xe5\xae\xb3\xe5\x8a\xa0\xe5\x80\xbc";   // 伤害加值
            std::string cs = CharacterCardStore::canonical("\xe5\x8a\x9b\xe9\x87\x8f");   // 力量
            std::string cz = CharacterCardStore::canonical("\xe4\xbd\x93\xe5\x9e\x8b");   // 体型
            bool touched = changes.count(cs) || changes.count(cz);
            bool dbManual = !getFormulaAttr(msg, DB).empty() && getUserSetting(msg, "attrfauto:" + DB) != "1";
            if (touched && !formulaChanges.count(DB) && !dbManual) {
                auto sv = cards_.getAttr(user, group, "\xe5\x8a\x9b\xe9\x87\x8f");
                auto zv = cards_.getAttr(user, group, "\xe4\xbd\x93\xe5\x9e\x8b");
                if (sv && zv) {
                    std::string oldDb = getFormulaAttr(msg, DB);
                    std::string dbf = coc7DamageBonus(*sv + *zv);
                    setFormulaAttr(msg, DB, dbf);
                    setUserSetting(msg, "attrfauto:" + DB, "1");
                    if (dbf != oldDb) dbDetail = dbf;   // 只在变化时汇报
                }
            }
        }
        std::string reply = i18n_.tr(loc, "card.st.done", {{"nick", nick}, {"detail", fdetail}});
        if (!recalcDetail.empty()) {
            // 列出本次触发重算的基础属性。
            std::string causes;
            for (const char* base : { "\xe4\xbd\x93\xe8\xb4\xa8","\xe4\xbd\x93\xe5\x9e\x8b","\xe6\x84\x8f\xe5\xbf\x97","\xe5\x85\x8b\xe8\x8b\x8f\xe9\xb2\x81\xe7\xa5\x9e\xe8\xaf\x9d" }) {  // 体质 体型 意志 克苏鲁神话
                if (changes.count(CharacterCardStore::canonical(base))) { if (!causes.empty()) causes += "\xe3\x80\x81"; causes += base; }
            }
            reply += "\n" + i18n_.tr(loc, "card.autocard.recalc", {{"causes", causes}, {"detail", recalcDetail}});
        }
        if (!dbDetail.empty())   // D#03：告知自动算出的 DB
            reply += "\n" + i18n_.tr(loc, "card.db.auto", {{"db", dbDetail}});
        maybeAutoSn(msg);   // .sn auto：属性变化后实时刷新群名片 (C#34)
        return reply;
    }

    static std::string joinAttrs(const std::map<std::string, int>& attrs) {
        std::ostringstream s;
        bool first = true;
        for (const auto& [k, v] : attrs) {
            if (!first) s << " ";
            s << k << ":" << v;
            first = false;
        }
        return s.str();
    }

    // D#06：像 joinAttrs，但把「临时生命值」折进「生命值」——显示为 有效值[/上限]（如 13/10），
    // 临时>0 时体现「护盾」。@p hpMax 为空则不显示 /上限（改注「(+临时N)」）。
    static std::string joinAttrsVital(const std::map<std::string, int>& attrs, std::optional<int> hpMax) {
        static const std::string HP = "\xe7\x94\x9f\xe5\x91\xbd\xe5\x80\xbc";              // 生命值
        static const std::string TEMP = "\xe4\xb8\xb4\xe6\x97\xb6\xe7\x94\x9f\xe5\x91\xbd\xe5\x80\xbc";  // 临时生命值
        auto ti = attrs.find(TEMP);
        int temp = ti != attrs.end() ? ti->second : 0;
        std::ostringstream s; bool first = true;
        for (const auto& [k, v] : attrs) {
            if (k == TEMP) continue;                        // 折进生命值，不单列
            if (!first) s << " "; first = false;
            if (k == HP) {
                int eff = v + temp;
                s << k << ":" << eff;
                if (hpMax) s << "/" << *hpMax;
                else if (temp > 0) s << "(+\xe4\xb8\xb4\xe6\x97\xb6" << temp << ")";   // (+临时N)
            } else s << k << ":" << v;
        }
        if (temp > 0 && !attrs.count(HP)) { if (!first) s << " "; s << TEMP << ":" << temp; }
        return s.str();
    }

    // ─── .sc — sanity check ──────────────────────────────────

    std::optional<std::string> tryHandleSC(Locale loc, const Message& msg,
                                           const std::string& cmd) {
        if (toLower(cmd).rfind("sc", 0) != 0) return std::nullopt;
        std::string rest = trim(cmd.substr(2));

        // Bonus/penalty sanity (青果/海豹 ".sc b ..." / ".scb ..."): leading b/p[count].
        int scBp = 0; char scBpType = 0;
        if (!rest.empty() && (rest[0] == 'b' || rest[0] == 'p' || rest[0] == 'B' || rest[0] == 'P')) {
            size_t k = 1; std::string num;
            while (k < rest.size() && std::isdigit(static_cast<unsigned char>(rest[k]))) num += rest[k++];
            char nx = (k < rest.size()) ? rest[k] : ' ';
            if (nx == ' ' || nx == '/' || std::isdigit(static_cast<unsigned char>(nx))) {
                scBpType = static_cast<char>(std::tolower(static_cast<unsigned char>(rest[0])));
                scBp = num.empty() ? 1 : parseIntOr(num, 1);
                if (scBp < 1) scBp = 1; if (scBp > 9) scBp = 9;
                rest = trim(rest.substr(k));
            }
        }

        if (rest.empty() || rest.find('/') == std::string::npos)
            return i18n_.tr(loc, "card.sc.usage");

        // "succ/fail [san]"
        std::string costPart = rest, sanArg;
        if (auto sp = rest.find(' '); sp != std::string::npos) {
            costPart = trim(rest.substr(0, sp));
            sanArg = trim(rest.substr(sp + 1));
        }
        std::string sucExpr = trim(costPart.substr(0, costPart.find('/')));
        std::string failExpr = trim(costPart.substr(costPart.find('/') + 1));
        if (sucExpr.empty() || failExpr.empty()) return i18n_.tr(loc, "card.sc.usage");

        const std::string user = msg.senderId;
        const std::string group = cardScope(msg);
        const std::string nick = displayName(msg);

        int san; bool fromCard = false;
        if (!sanArg.empty() && isAllDigits(sanArg)) {
            san = parseIntOr(sanArg, 0);
        } else {
            auto v = cards_.getAttr(user, group, "理智");
            if (!v) return i18n_.tr(loc, "card.sc.no_san");
            san = *v; fromCard = true;
        }
        if (san <= 0 || san > 99) return i18n_.tr(loc, "card.sc.invalid_san");

        int res; std::string bpDetail;
        if (scBpType) {
            res = rollBonusPenalty(scBp, scBpType == 'b', loc, bpDetail);
        } else {
            auto roll = engine_.roll("1d100");
            if (!roll.ok()) return i18n_.tr(loc, "dice.error.roll", {{"error", roll.error}});
            res = roll.modifiedTotal;
        }
        SuccessLevel lv = rollSuccessLevel(res, san, getCocRule(msg));

        int loss = 0;
        if (lv == SuccessLevel::kFumble) {
            // Fumble = maximum possible loss of the failure expression.
            auto m = maxOfSimpleExpr(failExpr);
            loss = m ? *m : engine_.roll(failExpr).modifiedTotal;
        } else if (lv == SuccessLevel::kFailure) {
            loss = engine_.roll(failExpr).modifiedTotal;
        } else {
            loss = engine_.roll(sucExpr).modifiedTotal;
        }

        int finalSan = san - loss;
        if (finalSan < 0) finalSan = 0;
        if (fromCard && loss != 0) cards_.setAttr(user, group, "理智", finalSan);

        const char* gradeKey =
            (lv == SuccessLevel::kFumble) ? "dice.level.fumble" :
            (lv == SuccessLevel::kFailure) ? "dice.level.failure" : "dice.level.regular";

        I18n::Args args = {
            {"nick", nick}, {"res", std::to_string(res)}, {"san", std::to_string(san)},
            {"grade", i18n_.tr(loc, gradeKey)},
            {"loss", std::to_string(loss)}, {"final", std::to_string(finalSan)}};
        return i18n_.tr(loc, loss == 0 ? "card.sc.result_noloss" : "card.sc.result", args);
    }

    /// Maximum value of a simple expression ("N", "NdM", "NdM±K"); nullopt otherwise.
    static std::optional<int> maxOfSimpleExpr(const std::string& e) {
        std::smatch m;
        static const std::regex reDice(R"(^(\d*)[dD](\d+)([+-]\d+)?$)");
        if (std::regex_match(e, m, reDice)) {
            int n = m[1].str().empty() ? 1 : std::stoi(m[1].str());
            int s = std::stoi(m[2].str());
            int mod = m[3].matched ? std::stoi(m[3].str()) : 0;
            return n * s + mod;
        }
        static const std::regex reNum(R"(^\d+$)");
        if (std::regex_match(e, reNum)) return std::stoi(e);
        return std::nullopt;
    }

    // ─── .help / .helpdoc 文档系统 ───────────────────────────
public:   // helpTopics/setHelpProvider/allHelp 供 main.cpp 注入与 api_service 聚合
    /// Known built-in help topics (used by .help <topic> and .helpdoc listing).
    static const std::vector<std::string>& helpTopics() {
        static const std::vector<std::string> t = {
            "r", "rh", "ra", "rx", "rb", "rav", "ba", "bav", "sc", "st", "pc", "npc", "coc", "dnd", "ww", "dx", "rdx",
            "en", "ti", "setcoc", "deck", "jrrp", "mrrp", "zrrp", "name",
            "bot", "group", "ob", "send", "log", "nn", "set", "sleep", "lang", "favor", "hiy",
            "setsn", "sn", "ri", "welcome", "dismiss", "rules", "plugin"
        };
        return t;
    }

    // C#10：插件帮助提供器（由 main.cpp 从 JsPluginManager 注入，返回 {指令名, 帮助文本}）。
    // CommandRouter 不直接依赖 JS 引擎，故用回调解耦。
    using HelpProviderFn = std::function<std::vector<std::pair<std::string, std::string>>()>;
    void setHelpProvider(HelpProviderFn f) { helpProvider_ = std::move(f); }

    // C#33：插件清单提供器（main.cpp 注入 lua/js 插件列表），供 .plugin 指令枚举/启停。
    struct PluginEntry { std::string id; std::string name; std::string kind; bool enabledGlobal = true; };
    using PluginListFn = std::function<std::vector<PluginEntry>()>;
    void setPluginProvider(PluginListFn f) { pluginProvider_ = std::move(f); }

    // C#10：一条帮助条目（合并自内置 i18n / 规则包 / 插件 / 文件四源）。
    struct HelpEntry { std::string key, content, source; };  // builtin | rule:<包> | plugin:<名> | file:<名>

    // C#10：data/help/*.md 用户自管帮助文档（第四源）。专用读写锁；启动期与 CRUD 后 loadHelpFiles。
    static std::shared_mutex& helpLock() { static std::shared_mutex m; return m; }
    static std::map<std::string, std::string>& helpFiles() { static std::map<std::string, std::string> m; return m; }
    static void loadHelpFiles() {
        std::unique_lock<std::shared_mutex> lk(helpLock());
        helpFiles().clear();
        std::error_code ec;
        if (!std::filesystem::is_directory("data/help", ec)) return;
        for (auto& e : std::filesystem::directory_iterator("data/help", ec)) {
            if (ec || !e.is_regular_file()) continue;
            std::string fn = u8str(e.path().filename());
            if (fn.size() <= 3 || fn.substr(fn.size() - 3) != ".md") continue;
            std::ifstream f(e.path(), std::ios::binary);
            std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            helpFiles()[u8str(e.path().stem())] = body;
        }
    }

    // C#26：data/helpdoc/**/*.json 结构化帮助文档（海豹 SealDice 兼容 + 随包分发的规则速查）。
    // 格式 {mod, brief, helpdoc:{词条:内容}}（值以 & 开头=别名，一级解析）。xlsx 在打包时转 json。
    struct HelpDocItem { std::string topic, content, pack; };
    static std::shared_mutex& helpDocLock() { static std::shared_mutex m; return m; }
    static std::vector<HelpDocItem>& helpDocs() { static std::vector<HelpDocItem> v; return v; }
    // 扫描某目录下所有 *.json helpdoc，追加进 helpDocs()。packPrefix 非空时给 pack 名加前缀
    //（如规则包内的 helpdoc 标成 "<包>/<mod>"）。需在 helpDocLock 写锁下调用。
    static int scanHelpDocDir(const std::string& dir, const std::string& packPrefix = "") {
        namespace fs = std::filesystem; std::error_code ec; int added = 0;
        if (!fs::is_directory(dir, ec)) return 0;
        for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            std::error_code fe;
            if (!it->is_regular_file(fe) || it->path().extension() != ".json") continue;
            try {
                std::ifstream f(it->path(), std::ios::binary);
                nlohmann::json j = nlohmann::json::parse(f, nullptr, false, true);
                if (!j.is_object() || !j.contains("helpdoc") || !j["helpdoc"].is_object()) continue;
                std::string pack = j.value("mod", u8str(it->path().stem()));
                if (!packPrefix.empty()) pack = packPrefix + "/" + pack;
                std::map<std::string, std::string> raw;
                for (auto e = j["helpdoc"].begin(); e != j["helpdoc"].end(); ++e)
                    if (e.value().is_string()) raw[e.key()] = e.value().get<std::string>();
                for (auto& [k, c] : raw) {
                    std::string txt = c;
                    if (!txt.empty() && txt[0] == '&') {   // 别名 &其他词条 → 取其内容（一级）
                        auto a = raw.find(txt.substr(1));
                        if (a != raw.end()) txt = a->second;
                    }
                    if (!k.empty() && !txt.empty()) { helpDocs().push_back({k, txt, pack}); ++added; }
                }
            } catch (...) { /* 跳过坏文件 */ }
        }
        return added;
    }
    static void loadHelpDocs() {
        std::unique_lock<std::shared_mutex> lk(helpDocLock());
        helpDocs().clear();
        scanHelpDocDir("data/helpdoc");
        // C#27：各规则包自带的 helpdoc（data/rulepacks/<包>/helpdoc/）。
        namespace fs = std::filesystem; std::error_code ec;
        if (fs::is_directory("data/rulepacks", ec))
            for (auto& e : fs::directory_iterator("data/rulepacks", ec)) {
                if (ec || !e.is_directory()) continue;
                auto u = e.path().filename().u8string();
                std::string pk(u.begin(), u.end());   // UTF-8（避免非 ASCII 包名产代理字符）
                if (pk.size() > 9 && pk.substr(pk.size() - 9) == ".disabled") continue;   // 停用的包不加载 helpdoc
                scanHelpDocDir(u8str(e.path() / "helpdoc"), pk);
            }
    }

    /// 汇总当前所有可用帮助条目（去重保序：内置 → 规则包 → 插件 → 文件）。
    std::vector<HelpEntry> allHelp(Locale loc, const std::string& scope = "") const {
        std::vector<HelpEntry> v;
        // O(1) 去重：小写 key 哈希集（旧实现是 O(n²) 线性扫描，海量 mod helpdoc
        // 会让 .help 卡死/超时——真实查询类 mod 动辄上千词条）。
        // scope 非空 = 搜索域锁定：内置指令帮助保留，其余只收来源匹配的（在「去重前」过滤，
        // 否则别的来源的同名词条会先占位、再被过滤掉，导致域内词条反而查不到）。
        std::unordered_set<std::string> seenKeys;
        auto add = [&](const std::string& k, const std::string& c, const std::string& src) {
            if (!scope.empty() && src != "builtin" && !helpSourceMatches(src, scope)) return;
            if (!seenKeys.insert(toLower(k)).second) return;   // 已存在
            v.push_back({k, c, src});
        };
        for (const auto& t : helpTopics())
            add(t, i18n_.tr(loc, "help.topic." + t), "builtin");
        {
            std::shared_lock<std::shared_mutex> lk(rulesLock());
            for (const auto& rp : rulePacks()) if (rp.enabled)
                for (const auto& [k, c] : rp.helpEntries) add(k, c, "rule:" + rp.name);
        }
        if (helpProvider_)
            for (const auto& [k, c] : helpProvider_())
                if (!c.empty()) add(k, c, "plugin");
        {
            std::shared_lock<std::shared_mutex> lk(helpLock());
            for (const auto& [k, c] : helpFiles())
                if (!c.empty()) add(k, c, "file:" + k);
        }
        {   // C#26：结构化帮助文档（data/helpdoc，海豹兼容 + 随包速查）
            std::shared_lock<std::shared_mutex> lk(helpDocLock());
            for (const auto& h : helpDocs())
                if (!h.content.empty()) add(h.topic, h.content, "helpdoc:" + h.pack);
        }
        return v;
    }

    // 溯洄/OneDice 引用：查一个帮助词条的原始内容（供 {克苏鲁} 这类交叉引用 + 牌堆回调）。
    std::optional<std::string> helpEntryContent(const std::string& name) const {
        std::string n = toLower(trim(name));
        if (n.empty()) return std::nullopt;
        std::shared_lock<std::shared_mutex> lk(helpDocLock());
        for (const auto& h : helpDocs())
            if (!h.content.empty() && toLower(h.topic) == n) return h.content;
        return std::nullopt;
    }
    // 「像骰子/算式」：含数字且仅由 0-9 dD +-*/() 空格 kKhHlLxX 组成。
    static bool looksDiceExpr(const std::string& s) {
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
    // 只展开 {引用}：帮助词条(递归) / {%_1D3} 内联骰子。不碰 [..]。
    std::string expandBracesOnly(std::string s, const Message& msg, int depth) const {
        if (depth > 8) return s;
        size_t pos = 0;
        while (true) {
            size_t lq = s.find('{', pos); if (lq == std::string::npos) break;
            size_t rq = s.find('}', lq); if (rq == std::string::npos) break;
            std::string name = s.substr(lq + 1, rq - lq - 1);
            std::string res; bool handled = false;
            if (auto h = helpEntryContent(name)) { res = expandHelpRefs(*h, msg, depth + 1); handled = true; }
            if (!handled) {
                std::string dx = name;
                while (!dx.empty() && (dx.front() == '%' || dx.front() == '_')) dx.erase(dx.begin());
                if (looksDiceExpr(dx)) { auto r = engine_.roll(dx); if (r.ok()) { res = std::to_string(r.modifiedTotal); handled = true; } }
            }
            if (handled) { s.replace(lq, rq - lq + 1, res); pos = lq + res.size(); }
            else pos = rq + 1;
        }
        return s;
    }
    // 展开帮助/牌堆词条里的溯洄引用与计算（#帮助文档和牌堆支持溯洄格式函数）：
    //   {克苏鲁} → 展开该词条内容；[{%_1D3}*{%_1D20}] → 含 {的方括号块当算式求值；
    //   纯 [例子]（不含 {）保持原样，避免把帮助里的示例表达式误算。
    std::string expandHelpRefs(std::string s, const Message& msg, int depth = 0) const {
        if (depth > 8) return s;
        if (s.find('{') == std::string::npos && s.find('[') == std::string::npos) return s;
        size_t pos = 0;
        while (true) {
            size_t lb = s.find('[', pos); if (lb == std::string::npos) break;
            size_t rb = s.find(']', lb); if (rb == std::string::npos) break;
            std::string inner = s.substr(lb + 1, rb - lb - 1);
            if (inner.find('{') != std::string::npos) {        // 计算块：先展开内部引用/骰子再整体求值
                std::string e = expandBracesOnly(inner, msg, depth + 1);
                auto r = engine_.roll(e);
                std::string val = r.ok() ? std::to_string(r.modifiedTotal) : ("[" + inner + "]");
                s.replace(lb, rb - lb + 1, val); pos = lb + val.size();
            } else pos = rb + 1;
        }
        return expandBracesOnly(s, msg, depth);
    }

    // C#12 可视化生成器「实时测试」结果：matched=是否命中规则指令层；reply=渲染输出；
    // status: ""=正常 / "render_fail"=表达式引用缺失属性或未知函数 / "disabled"=被该规则屏蔽。
    struct RuleTestResult { bool matched = false; std::string reply; std::string status; };

    /// 对一段「未保存」的规则包 JSON 求值一条指令，不落库、不依赖群激活。仅覆盖规则
    /// 指令层（别名重写 → 自定义指令 DSL → 屏蔽），供可视化生成器边编辑边试。
    /// mockAttrs 提供测试属性值（{力量} 等公式），nick 用作 {nick}。
    RuleTestResult testRulePackCommand(const std::string& packJson, const std::string& command,
                                       const std::map<std::string, int>& mockAttrsRaw,
                                       const std::string& nick) {
        RuleTestResult res;
        nlohmann::json j;
        try { j = nlohmann::json::parse(packJson); }
        catch (const std::exception& e) { res.status = std::string("json:") + e.what(); return res; }

        std::map<std::string, std::string> add, alias;
        std::vector<std::string> disable;
        if (j.contains("commands") && j["commands"].is_object()) {
            const auto& c = j["commands"];
            if (c.contains("add") && c["add"].is_object())
                for (auto& [k, v] : c["add"].items()) {
                    if (v.is_string()) add[k] = v.get<std::string>();
                    else if (v.is_object() && v.contains("output") && v["output"].is_string())
                        add[k] = v["output"].get<std::string>();
                }
            if (c.contains("alias") && c["alias"].is_object())
                for (auto& [k, v] : c["alias"].items()) if (v.is_string()) alias[k] = v.get<std::string>();
            if (c.contains("disable") && c["disable"].is_array())
                for (auto& d : c["disable"]) if (d.is_string()) disable.push_back(d.get<std::string>());
        }

        std::map<std::string, int> mock;   // 规范化属性名（primary() 按 canonical 查找）
        for (auto& [k, v] : mockAttrsRaw) mock[CharacterCardStore::canonical(k)] = v;

        Message msg;
        msg.platform = "onebot_v11";
        msg.type = MessageType::kGroup;
        msg.senderId = "__ruletest__";
        msg.senderName = nick.empty() ? std::string("\xe6\xb5\x8b\xe8\xaf\x95") : nick;  // 测试
        msg.targetId = "__ruletestgrp__";

        std::string cmd = stripPrefix(trim(command));
        if (cmd.empty()) return res;

        // 1) 别名重写（同 handleMessage 顶部逻辑）。
        if (!alias.empty()) {
            auto [w, rest] = splitCommand(cmd);
            auto it = alias.find(w);
            if (it != alias.end()) cmd = it->second + (rest.empty() ? "" : " " + rest);
            else for (auto& [k, tgt] : alias)
                if (!k.empty() && cmd.rfind(k, 0) == 0) { cmd = tgt + cmd.substr(k.size()); break; }
        }
        // 2) 自定义指令（首词精确，其次忽略大小写）。
        if (!add.empty()) {
            auto [w, rest] = splitCommand(cmd);
            const std::string* tmpl = nullptr;
            if (auto it = add.find(w); it != add.end()) tmpl = &it->second;
            else { std::string wl = toLower(w); for (auto& [k, v] : add) if (toLower(k) == wl) { tmpl = &v; break; } }
            if (tmpl) {
                res.matched = true;
                if (auto out = renderCustomCmd(*tmpl, msg, rest, &mock)) res.reply = *out;
                else res.status = "render_fail";
                return res;
            }
        }
        // 3) 屏蔽（命中即静默）。
        if (!disable.empty()) {
            std::string w = toLower(splitCommand(cmd).first), cl = toLower(cmd);
            for (auto& d : disable)
                if (w == d || cl.rfind(d, 0) == 0) { res.matched = true; res.status = "disabled"; return res; }
        }
        return res;   // matched=false：规则层未命中（会落到内置/其他处理）
    }

private:
    std::string handleHelp(Locale loc, const std::string& args, const Message& msg) {
        std::string topic = toLower(trim(args));
        if (topic.empty()) return i18n_.tr(loc, "help.main");
        topic = toLower(trim(stripPrefix(topic)));   // 容忍 ".help .r"
        // C#27 深化：本群激活的规则包，其同名帮助词条优先于内置/其它来源。
        if (auto rp = activeRulePack(msg))
            for (auto& [k, c] : rp->helpEntries) if (toLower(k) == topic) return expandHelpRefs(c, msg);
        // C#36：设了默认帮助来源 → 同名词条优先该来源（如 dnd5r 压过 dnd3r）。
        if (std::string defSrc = helpdocDefaultSource(msg); !defSrc.empty()) {
            std::shared_lock<std::shared_mutex> lk(helpDocLock());
            for (const auto& h : helpDocs())
                if (!h.content.empty() && toLower(h.topic) == topic && helpSourceMatches(h.pack, defSrc))
                    return expandHelpRefs(h.content, msg);
        }
        // C#（搜索域）：本群锁定了搜索范围 → 在「构建时」就只收该来源(+内置)，过滤掉其它内容。
        std::string scope = getGroupSetting(msg, "helpScope");
        auto entries = allHelp(loc, scope);
        if (!scope.empty()) {
            bool hasDoc = false; for (auto& e : entries) if (e.source != "builtin") { hasDoc = true; break; }
            if (!hasDoc) return i18n_.tr(loc, "helpdoc.scope.empty", {{"scope", scope}, {"topic", topic}});
        }
        // 精确命中（忽略大小写）任一来源。
        for (auto& e : entries) if (toLower(e.key) == topic) return expandHelpRefs(e.content, msg);
        // 模糊查找：唯一命中→显示；多个→列候选；无→未知。
        std::vector<std::string> keys; keys.reserve(entries.size());
        for (auto& e : entries) keys.push_back(e.key);
        auto hits = fuzzyFind(topic, keys);
        if (hits.size() == 1)
            for (auto& e : entries) if (e.key == hits[0]) return expandHelpRefs(e.content, msg);
        if (hits.size() > 1) {
            std::string list;
            for (auto& c : hits) { if (!list.empty()) list += " "; list += "." + c; }
            return i18n_.tr(loc, "help.ambiguous", {{"topic", topic}, {"list", list}});
        }
        return i18n_.tr(loc, "help.unknown", {{"topic", topic}});
    }

    // C#36：当前生效的默认帮助来源（本群优先，回退全局配置）。空=未设。
    std::string helpdocDefaultSource(const Message& msg) const {
        std::string g = getGroupSetting(msg, "helpDefault");
        if (!g.empty()) return g;
        return cfg_.get<std::string>("dice/helpdoc_default", std::string());
    }
    // helpdoc 条目的来源(pack)是否匹配用户给的来源名：整体相等 / 末段相等 / 子串包含（不分大小写）。
    static bool helpSourceMatches(const std::string& pack, const std::string& src) {
        std::string p = toLower(pack), s = toLower(src);
        if (s.empty()) return false;
        if (p == s) return true;
        if (auto sl = p.rfind('/'); sl != std::string::npos && p.substr(sl + 1) == s) return true;
        return p.find(s) != std::string::npos;
    }
    // .helpdoc default [来源名 | clear]：设/查/清本群默认帮助来源。
    std::string handleHelpDefault(Locale loc, const std::string& arg, const Message& msg) {
        std::string a = trim(arg);
        if (a.empty()) {   // 查看当前 + 列出可选来源
            std::string cur = helpdocDefaultSource(msg);
            std::set<std::string> srcs;
            { std::shared_lock<std::shared_mutex> lk(helpDocLock());
              for (const auto& h : helpDocs()) if (!h.pack.empty()) srcs.insert(h.pack); }
            std::string list; for (const auto& s : srcs) { if (!list.empty()) list += " "; list += s; }
            return i18n_.tr(loc, "helpdoc.default.show",
                {{"cur", cur.empty() ? i18n_.tr(loc, "helpdoc.default.none") : cur}, {"list", list}});
        }
        if (!senderIsGroupAdmin(msg)) return i18n_.tr(loc, "gate.no_perm");
        std::string la = toLower(a);
        if (la == "clear" || la == "off" || la == "none" || la == "clr") {
            setGroupSetting(msg, "helpDefault", "");
            return i18n_.tr(loc, "helpdoc.default.cleared");
        }
        setGroupSetting(msg, "helpDefault", a);
        return i18n_.tr(loc, "helpdoc.default.set", {{"src", a}});
    }

    // 搜索域（每群独立）：.helpdoc scope [来源名 | clear]
    //   设定后，本群 .help 查询只在该来源内进行（内置指令帮助仍保留），直到清除或改设。
    std::string handleHelpScope(Locale loc, const std::string& arg, const Message& msg) {
        std::string a = trim(arg);
        if (a.empty()) {   // 查看当前 + 列出可选来源
            std::string cur = getGroupSetting(msg, "helpScope");
            std::set<std::string> srcs;
            { std::shared_lock<std::shared_mutex> lk(helpDocLock());
              for (const auto& h : helpDocs()) if (!h.pack.empty()) srcs.insert(h.pack); }
            std::string list; for (const auto& s : srcs) { if (!list.empty()) list += " "; list += s; }
            return i18n_.tr(loc, "helpdoc.scope.show",
                {{"cur", cur.empty() ? i18n_.tr(loc, "helpdoc.scope.none") : cur}, {"list", list}});
        }
        if (msg.type == MessageType::kPrivate) return i18n_.tr(loc, "helpdoc.scope.group_only");
        if (!senderIsGroupAdmin(msg)) return i18n_.tr(loc, "gate.no_perm");
        std::string la = toLower(a);
        if (la == "clear" || la == "off" || la == "none" || la == "clr" ||
            la == "\xe8\xa7\xa3\xe9\x99\xa4" || la == "\xe5\x85\xa8\xe9\x83\xa8") {   // 解除 / 全部
            setGroupSetting(msg, "helpScope", "");
            return i18n_.tr(loc, "helpdoc.scope.cleared");
        }
        setGroupSetting(msg, "helpScope", a);
        return i18n_.tr(loc, "helpdoc.scope.set", {{"src", a}});
    }

    std::string handleHelpDoc(Locale loc, const std::string& args, const Message& msg) {
        // C#36：.helpdoc default [来源] —— 切换默认帮助来源优先级（dnd3r/dnd5r 同名词条择优）。
        // 搜索域：.helpdoc scope [来源|clear] —— 把本群帮助搜索「锁定」到某来源，只查它（更强）。
        {
            auto [sub, rest] = splitCommand(trim(args));
            std::string sl = toLower(trim(sub));
            if (sl == "default") return handleHelpDefault(loc, trim(rest), msg);
            if (sl == "scope" || sl == "\xe6\x90\x9c\xe7\xb4\xa2\xe5\x9f\x9f" || sl == "\xe5\x9f\x9f")  // 搜索域 / 域
                return handleHelpScope(loc, trim(rest), msg);
        }
        std::string topic = toLower(trim(args));
        if (!topic.empty()) return handleHelp(loc, args, msg);   // .helpdoc <topic> == .help <topic>
        // C#25：默认文案只给「怎么用」提示，不再罗列全部条目（海量 mod helpdoc 会刷屏）。
        // 仅当骰主把模板改回含 {list} 时，才构建完整条目列表（保留 {list} 变量）。
        if (i18n_.tr(loc, "helpdoc.list", {}).find("{list}") == std::string::npos)
            return i18n_.tr(loc, "helpdoc.list", {});
        std::string list;
        for (const auto& e : allHelp(loc)) { if (!list.empty()) list += " "; list += "." + e.key; }
        return i18n_.tr(loc, "helpdoc.list", {{"list", list}});
    }

    /// .bot / .bot on / .bot off, with account targeting for multi-bot groups:
    ///   .bot5080  /  .bot on 5080  → only the bot whose id ends with 5080 responds.
    std::optional<std::string> tryHandleBot(Locale loc, const Message& msg,
                                            const std::string& cmd) {
        if (toLower(cmd).rfind("bot", 0) != 0) return std::nullopt;
        std::string rest = trim(cmd.substr(3));

        // Exact match only: the bot responds to ".bot", ".bot on/off" and an
        // optional account suffix (".bot5080" / ".bot on 5080"). Any other token
        // (e.g. ".bot sdsadas") is NOT a bot command → stay out of the way.
        std::string target, action;
        std::istringstream iss(rest);
        std::string tok;
        while (iss >> tok) {
            std::string tl = toLower(tok);
            if (tl == "on" || tl == "off") {
                if (!action.empty()) return std::nullopt;   // duplicate action → reject
                action = tl;
            } else if (isAllDigits(tok)) {
                if (!target.empty()) return std::nullopt;    // duplicate target → reject
                target = tok;
            } else {
                return std::nullopt;                          // garbage token → not a .bot command
            }
        }

        // Account targeting: a target id was given but it isn't this bot → stay silent.
        if (!target.empty() && !idMatchesSelf(target, msg.selfId)) {
            return std::string();
        }

        // C#48：.bot on/off 需群管权限（原版 DiceEvent.cpp:3216 canRoomHost 门控 on/off）。
        if (!action.empty() && !senderIsGroupAdmin(msg))
            return i18n_.tr(loc, "gate.no_perm");
        // Repeated on/off: tell the user it's already in that state (customizable).
        const bool currentlyOff = (getGroupSetting(msg, "enabled") == "0");
        if (action == "on") {
            if (!currentlyOff) return i18n_.tr(loc, "bot.already_on");
            setGroupEnabled(msg, true);  return i18n_.tr(loc, "bot.on");
        }
        if (action == "off") {
            if (currentlyOff) return i18n_.tr(loc, "bot.already_off");
            setGroupEnabled(msg, false); return i18n_.tr(loc, "bot.off");
        }
        // 版本横幅，外加可配置的页眉/页脚（原版 strBotHeader / strBotMsg）。
        // {self} 等占位符由 main.cpp 的 applySelf 统一在最终回复里替换。
        std::string out;
        std::string header = cfg_.get<std::string>("dice/bot_header", std::string());
        std::string footer = cfg_.get<std::string>("dice/bot_msg", std::string());
        if (!header.empty()) out += header + "\n";
        out += botBanner(msg.platform);
        if (!footer.empty()) out += "\n" + footer;
        return out;
    }

    static bool isAllDigits(const std::string& s) {
        if (s.empty()) return false;
        for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        return true;
    }

    /// A target id matches this bot if it equals the full self-id or its suffix
    /// (so the last 4 digits of the account work as a shorthand).
    static bool idMatchesSelf(const std::string& target, const std::string& selfId) {
        if (selfId.empty() || target.empty()) return false;
        if (target == selfId) return true;
        return target.size() < selfId.size() &&
               selfId.compare(selfId.size() - target.size(), target.size(), target) == 0;
    }

public:
    /// Whether this message explicitly @s this bot (not @all).
    static bool isAtSelf(const Message& msg) {
        return !msg.selfId.empty()
            && std::find(msg.atList.begin(), msg.atList.end(), msg.selfId) != msg.atList.end();
    }

    /// Anti-chaos gate for multi-bot groups: returns true if the message @s
    /// another bot but NOT this one (then this bot should stay silent).
    /// Mirrors the original DiceFilter's `isOtherCalled && !isCalled`.
    static bool isForAnotherBot(const Message& msg) {
        if (msg.atList.empty()) return false;
        bool atSelf = false, atAll = false, atOther = false;
        for (const auto& id : msg.atList) {
            if (id == "all") atAll = true;
            else if (!msg.selfId.empty() && id == msg.selfId) atSelf = true;
            else atOther = true;
        }
        if (!(atOther && !atSelf && !atAll)) return false;
        // Commands that take an @target as an argument (e.g. ".pc @某人" to view
        // someone's card) legitimately @ a player, not another bot — let them run.
        if (consumesAtTarget(msg.content)) return false;
        return true;
    }

    // ─── C#45：骰娘探测 ───────────────────────────────────────
    /// 是否已识别为骰娘（banlist listType=2）。
    bool isDiceBot(const std::string& userId) const { return banlistHas(0, 2, userId); }

    // C#73：每群最近一次 .bot 探测的时间戳（epoch 秒）。只有骰子横幅在探测后很短时间
    // 内出现，才判定发送者是骰娘——防止有人复制粘贴 .bot 回执被误识别为骰娘。
    mutable std::mutex botProbeMu_;
    std::map<std::string, long> lastBotProbe_;
    /// 骰娘识别时间窗（秒）：横幅须在最近一次 .bot 探测后这么多秒内出现。
    long diceBotProbeWindow() const { return cfg_.get<int>("dice/dicebot_probe_window_sec", 2); }

    /// C#73：记录一次群内 .bot 探测（供 detectDiceBot 的时间窗判定）。
    void recordBotProbe(const Message& msg) {
        if (msg.type != MessageType::kGroup || msg.targetId.empty()) return;
        std::string t = trim(msg.content);
        std::string cmd = stripPrefix(t);
        if (cmd == t) return;                              // 无前缀 → 不是命令
        std::string low = toLower(cmd);
        if (low.rfind("bot", 0) != 0) return;              // 不是 .bot 家族
        if (low.size() > 3 && low[3] != ' ') return;       // 排除 .botxxx（须是 .bot 或 .bot 空格…）
        std::lock_guard<std::mutex> lk(botProbeMu_);
        lastBotProbe_[msg.platform + ":" + msg.targetId] = nowEpoch();
    }

    /// 按各骰系 .bot 横幅的「不可修改部分」判断一段文本是否是骰子回执。
    /// 可自定义的文字（骰名/帮助行等）不参与判断：
    ///   原版 Dice!   → 固定含 "Dice! by 溯洄"（署名不可改）
    ///   Dice!Next    → 固定含 "Dice!Next By"（本系其他骰）
    ///   OlivaDice    → 固定含 "OlivaDice By lunzhiPenxil"
    ///   SealDice     → 仅 "SealDice" 一词固定 → 按格式判断：行内 "SealDice" 后紧跟
    ///                  语义化版本号（如 1.5.1 / v1.4.2+20251010），避免聊天里提到
    ///                  “SealDice”两个字就误判。
    /// 返回骰系名；非骰子回执返回空串。
    static std::string classifyDiceBanner(const std::string& s) {
        if (s.size() < 8 || s.size() > 2048) return "";
        if (s.find("Dice!Next By") != std::string::npos) return "Dice!Next";
        if (s.find("Dice! by \xE6\xBA\xAF\xE6\xB4\x84") != std::string::npos) return "Dice!";   // 溯洄
        if (s.find("OlivaDice By lunzhiPenxil") != std::string::npos) return "OlivaDice";
        static const std::regex sealRe(R"(SealDice\s+v?\d+\.\d+(\.\d+)?)");
        if (std::regex_search(s, sealRe)) return "SealDice";
        return "";
    }

    /// 被动探测：群里其他账号发出骰子横幅（多半是回应某人的 .bot）→ 登记进骰娘名单。
    /// 之后用户 @该账号+指令 时本骰静默，不再把它当代骰目标。
    void detectDiceBot(const Message& msg) {
        if (msg.type != MessageType::kGroup || msg.senderId.empty()) return;
        if (!msg.selfId.empty() && msg.senderId == msg.selfId) return;   // 自己的回执不记
        // 横幅通常在 rawContent/displayContent（content 可能被剥前缀），几路都看。
        std::string kind = classifyDiceBanner(msg.content);
        if (kind.empty()) kind = classifyDiceBanner(msg.rawContent);
        if (kind.empty()) kind = classifyDiceBanner(msg.displayContent);
        if (kind.empty() || isDiceBot(msg.senderId)) return;
        // C#73：横幅须紧跟在一次真实 .bot 探测之后（默认 2 秒内）才登记，否则有人把
        // 骰娘的 .bot 回执复制粘贴出来也会被误识别为骰娘。
        {
            std::lock_guard<std::mutex> lk(botProbeMu_);
            auto it = lastBotProbe_.find(msg.platform + ":" + msg.targetId);
            if (it == lastBotProbe_.end() || nowEpoch() - it->second > diceBotProbeWindow()) {
                DICE_LOG_INFO("C#73 骰娘探测：账号 {} 的横幅未紧跟 .bot 探测（{}），忽略",
                              msg.senderId, kind);
                return;
            }
        }
        banlistAdd(0, 2, msg.senderId, kind);
        DICE_LOG_INFO("C#45 骰娘探测：账号 {} 识别为 {}（已登记骰娘名单）", msg.senderId, kind);
    }

    /// Read a group setting by explicit platform+group (for event handlers that
    /// don't have a Message, e.g. the入群 welcome trigger).
    std::string groupSettingValue(const std::string& platform, const std::string& groupId,
                                  const std::string& key) const {
        Message m; m.platform = platform; m.targetId = groupId; m.type = MessageType::kGroup;
        return getGroupSetting(m, key);
    }

    /// Whether a user is on the blacklist (used by the join退群 check).
    bool isUserBlacklisted(const std::string& userId) const { return banlistHas(0, 0, userId); }

    /// .bot-off / locked check by explicit platform+group (for event handlers).
    bool isGroupDisabledFor(const std::string& platform, const std::string& groupId) const {
        Message m; m.platform = platform; m.targetId = groupId; m.type = MessageType::kGroup;
        return isGroupDisabled(m);
    }

    /// Effective blacklist-quit level for a group: per-group group_settings
    /// "blacklistQuitLevel" overrides the global config dice.blacklist_quit_level
    /// (default "member"). One of {"member","admin"}.
    std::string blacklistQuitLevel(const std::string& platform, const std::string& groupId) const {
        std::string lvl = groupSettingValue(platform, groupId, "blacklistQuitLevel");
        if (lvl.empty()) {
            try { lvl = cfg_.get<std::string>("dice/blacklist_quit_level", std::string("member")); }
            catch (...) { lvl = "member"; }
        }
        return (lvl == "admin") ? "admin" : "member";
    }

    /// True if @p content is a command whose @-mention is a player argument
    /// rather than a bot selector (so the anti-chaos gate must not silence it).
    static bool consumesAtTarget(const std::string& content) {
        std::string t = trim(content);
        if (stripPrefix(t) == t) return false;          // not prefixed → not a command
        std::string cmd = toLower(trim(stripPrefix(t)));
        // Commands whose @ targets a player (not a bot selector): card view (.pc),
        // checks (.ra/.rc/.rav/.rcv/.sc) and代骰-capable rolls (.rb/.rp/.ww/.dx/.en
        // and plain .r/.rh/.rs). These consume the @ as a 代骰/检定 target.
        if (cmd.rfind("pc", 0) == 0 || cmd.rfind("ra", 0) == 0 || cmd.rfind("rc", 0) == 0 ||
            cmd.rfind("rb", 0) == 0 || cmd.rfind("rp", 0) == 0 || cmd.rfind("sc", 0) == 0 ||
            cmd.rfind("rx", 0) == 0 ||   // .rx 心理学暗骰：@ 的是被检定的调查员
            cmd.rfind("ba", 0) == 0 ||   // .ba/.bav BRP 检定/对抗：@ 的是被检定者
            cmd.rfind("ww", 0) == 0 || cmd.rfind("dx", 0) == 0 || cmd.rfind("en", 0) == 0)
            return true;
        // Plain roll: 'r' followed by a roll-ish char (so ".r3d6 @p"/".rh @p" count,
        // but ".reply"/".ri" do not — their @ is treated as a bot selector).
        if (!cmd.empty() && cmd[0] == 'r') {
            if (cmd.size() == 1) return true;
            char c = cmd[1];
            if (c == ' ' || c == '#' || c == '(' || c == '+' || c == '-' ||
                c == 'd' || c == 'D' || c == 'h' || c == 's' ||
                std::isdigit(static_cast<unsigned char>(c))) return true;
        }
        return false;
    }

    /// Whether the message's group is turned off (.bot off / web admin).
    /// Private chats are never disabled. Public so the message loop can also
    /// suppress custom replies for disabled groups.
    bool isGroupDisabled(const Message& msg) const {
        if (msg.type == MessageType::kPrivate || msg.targetId.empty()) return false;
        return isGroupLocked(msg) || getGroupSetting(msg, "enabled") == "0";
    }

    /// Whether the group is HARD-locked from the web admin (彻底禁用): even .bot
    /// is silent; only the web panel can lift it.
    bool isGroupLocked(const Message& msg) const {
        if (msg.type == MessageType::kPrivate || msg.targetId.empty()) return false;
        return getGroupSetting(msg, "locked") == "1";
    }

    /// Black/white-list gate: blacklisted user (anywhere) or group → ignore
    /// completely; in whitelist-only mode, non-whitelisted groups are ignored.
    /// Checked in the message loop BEFORE any handling (incl. custom replies).
    bool isBlocked(const Message& msg) const {
        if (banlistHas(0, 0, msg.senderId)) return true;                 // blacklisted user
        if (msg.type != MessageType::kPrivate && !msg.targetId.empty()) {
            if (banlistHas(1, 0, msg.targetId)) return true;            // blacklisted group
            if (whitelistOnly() && !banlistHas(1, 1, msg.targetId)) return true;
            // C#62：退群宣言已发出（随机延时退群中）→ 本群禁止响应一切指令。
            if (getGroupSetting(msg, "leaving") == "1") return true;
        }
        return false;
    }

    /// Whether a group is on the blacklist (云黑群组) — used to auto-leave on join.
    bool isGroupBanned(const std::string& groupId) const { return banlistHas(1, 0, groupId); }
    /// Whether a user is on the whitelist (whiteqq) — used for invite审批 whitelist policy.
    bool isUserWhitelisted(const std::string& userId) const { return banlistHas(0, 1, userId); }
    /// All black/white-list rows (for WebUI 管理 + JS seal.ban.getList/getUser）。
    std::vector<BanlistRow> banlistAll() const {
        auto* st = db_.getStorage(); std::vector<BanlistRow> v;
        if (st) try { v = st->get_all<BanlistRow>(); } catch (...) {}
        return v;
    }
    /// One blacklist entry's reason (user 黑名单)；不在名单返回 nullopt。供 JS getUser。
    std::optional<std::string> banReason(int type, int list, const std::string& id) const {
        auto* st = db_.getStorage(); if (!st) return std::nullopt;
        try { namespace orm = sqlite_orm;
            auto rows = st->get_all<BanlistRow>(orm::where(
                orm::c(&BanlistRow::targetType) == type and orm::c(&BanlistRow::listType) == list and
                orm::c(&BanlistRow::targetId) == id));
            if (!rows.empty()) return rows.front().reason;
        } catch (...) {}
        return std::nullopt;
    }
    /// Public wrapper for Master check by (platform,id) — used by the invite审批 whitelist policy.
    bool isMasterUser(const std::string& platform, const std::string& id) const { return isMaster(platform, id); }

    /// 解析规则 mod 的属性模板 model/*.xml（原版 Dice! 的 <property>/<any name alias>），
    /// 把属性别名并入全局同义词表 → .st/.ra 认得该规则系统的属性名（如 .st STR50 → 力量）。
    /// 扫 data/mod 与 data/rulepacks（含包内 lua/.../model/）。返回注册的别名数。
    /// 注：text='javascript'/'dicexp' 的衍生/默认/上限值暂不集成（需 JS 上下文；内置
    ///     COC7/DND 的 max 由 builtinComputed 覆盖，自定义规则可在 rules/*.json 的 computed 声明）。
    static int loadModelTemplates() {
        namespace fs = std::filesystem; std::error_code ec; int n = 0;
        derivedRegistry().clear();   // 重载时不累积（alias 由 resetAliases 另清）
        // C#102：先播内置派生（闪避=敏捷/2 等），规则 mod/包可覆盖同名条目。
        for (auto& [k, v] : builtinDerived()) derivedRegistry()[CharacterCardStore::canonical(k)] = v;
        auto attrOf = [](const std::string& tag, const std::string& key) -> std::string {
            for (char q : { '\'', '"' }) {
                std::string pat = key + "=" + q;
                auto p = tag.find(pat);
                if (p == std::string::npos) continue;
                auto s = p + pat.size(); auto e = tag.find(q, s);
                if (e != std::string::npos) return tag.substr(s, e - s);
            }
            return "";
        };
        auto parseXml = [&](const fs::path& xml) {
            std::ifstream f(xml, std::ios::binary);
            std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            for (size_t pos = 0; (pos = s.find("<any", pos)) != std::string::npos; ) {
                auto end = s.find('>', pos);
                if (end == std::string::npos) break;
                std::string tag = s.substr(pos, end - pos);
                std::string name = attrOf(tag, "name"), alias = attrOf(tag, "alias"), text = attrOf(tag, "text");
                if (!name.empty() && !alias.empty()) {
                    std::string cur;
                    for (char c : alias + "/")
                        if (c == '/') { if (!cur.empty()) { CharacterCardStore::registerAlias(cur, name); ++n; } cur.clear(); }
                        else cur += c;
                }
                // 衍生值/默认值：<any name='X' text='javascript'|'dicexp'>EXPR</any> → derivedRegistry。
                if (!name.empty() && (text == "javascript" || text == "dicexp") && end > pos && s[end - 1] != '/') {
                    auto close = s.find("</any>", end);
                    if (close != std::string::npos) {
                        std::string raw = s.substr(end + 1, close - end - 1), dsl;
                        if (text == "javascript") dsl = translateModelExpr(raw);
                        else {                       // dicexp：默认值（&X 引用 / 简单算术）。去 & ；含掷骰/复杂→放弃。
                            for (size_t p; (p = raw.find('&')) != std::string::npos; ) raw.erase(p, 1);
                            auto t0 = raw.find_first_not_of(" \t\r\n");
                            dsl = (t0 == std::string::npos) ? "" : raw.substr(t0, raw.find_last_not_of(" \t\r\n") - t0 + 1);
                            // 含掷骰(数字d)/JS/字符串/比较 → 放弃
                            bool bad = dsl.empty();
                            for (size_t k = 0; k + 1 < dsl.size() && !bad; ++k)
                                if ((dsl[k] == 'd' || dsl[k] == 'D') && std::isdigit((unsigned char)dsl[k + 1])
                                    && (k == 0 || std::isdigit((unsigned char)dsl[k - 1]))) bad = true;
                            if (dsl.find_first_of("\"'<>{}[]?|") != std::string::npos) bad = true;
                            if (bad) dsl.clear();
                        }
                        if (!dsl.empty()) {
                            derivedRegistry()[name] = dsl;
                            // 同时按所有别名注册（canonical 可能落在别名/内建同义词上，确保查得到）。
                            std::string cur2;
                            for (char c : alias + "/")
                                if (c == '/') { if (!cur2.empty()) derivedRegistry()[cur2] = dsl; cur2.clear(); }
                                else cur2 += c;
                        }
                        pos = close + 6; continue;
                    }
                }
                pos = end + 1;
            }
        };
        auto scanDir = [&](const std::string& base) {
            std::error_code e2;
            if (!fs::is_directory(base, e2)) return;
            for (fs::recursive_directory_iterator it(base, e2), endit; it != endit; it.increment(e2)) {
                if (e2) { e2.clear(); continue; }
                std::error_code fe;
                if (it->is_regular_file(fe) && it->path().extension() == ".xml"
                    && it->path().parent_path().filename() == "model")
                    try { parseXml(it->path()); } catch (...) {}
            }
        };
        scanDir("data/mod");
        scanDir("data/rulepacks");
        n += loadJsGameSystems();   // seal.gameSystem.newTemplate 的属性别名/衍生
        return n;
    }

    /// seal.gameSystem 模板原文（main.cpp 从 jsMod 注入），含 alias/defaultsComputed。
    static std::vector<std::string>& jsGameSystemTemplates() { static std::vector<std::string> v; return v; }
    /// jsMod 重载后重新同步 gameSystem 属性模板（重设原文 + 重扫 model.xml/JS 别名衍生）。
    static void reloadJsGameSystems(std::vector<std::string> tpls) {
        jsGameSystemTemplates() = std::move(tpls);
        loadModelTemplates();
    }
    /// 解析 JS 规则插件的 gameSystem 模板（JSON 或 YAML）：alias→同义词表、defaultsComputed→衍生值。
    static int loadJsGameSystems() {
        int n = 0;
        // SealDice rollVM 表达式→本程序 computed DSL：去 $ 临时变量/字符串；含函数调用(name()) 放弃。
        auto translateGame = [](std::string s) -> std::string {
            auto t0 = s.find_first_not_of(" \t\r\n");
            s = (t0 == std::string::npos) ? "" : s.substr(t0, s.find_last_not_of(" \t\r\n") - t0 + 1);
            if (s.empty() || s.find('$') != std::string::npos || s.find('"') != std::string::npos
                || s.find('\'') != std::string::npos || s.find('?') != std::string::npos
                || s.find('{') != std::string::npos) return "";
            for (size_t i = 0; i + 1 < s.size(); ++i)   // 字母后紧跟 '(' = 函数调用 → 放弃
                if (s[i + 1] == '(' && (std::isalpha((unsigned char)s[i]) || (unsigned char)s[i] >= 0x80)) return "";
            return s;
        };
        auto regAlias = [&](const std::string& canon, const std::vector<std::string>& aliases) {
            for (auto& a : aliases) if (!a.empty() && a != canon) { CharacterCardStore::registerAlias(a, canon); ++n; }
        };
        for (auto& tpl : jsGameSystemTemplates()) {
            bool done = false;
            try {   // 先按 JSON（newTemplate）
                json j = json::parse(tpl);
                if (j.is_object()) {
                    if (j.contains("alias") && j["alias"].is_object())
                        for (auto& [canon, arr] : j["alias"].items()) {
                            std::vector<std::string> al;
                            if (arr.is_array()) for (auto& a : arr) { if (a.is_string()) al.push_back(a.get<std::string>()); }
                            else if (arr.is_string()) al.push_back(arr.get<std::string>());
                            regAlias(canon, al);
                        }
                    if (j.contains("defaultsComputed") && j["defaultsComputed"].is_object())
                        for (auto& [name, expr] : j["defaultsComputed"].items())
                            if (expr.is_string()) { std::string d = translateGame(expr.get<std::string>()); if (!d.empty()) derivedRegistry()[name] = d; }
                    done = true;
                }
            } catch (...) {}
            if (done) continue;
            try {   // 再按 YAML（newTemplateByYaml）
                YAML::Node y = YAML::Load(tpl);
                if (y["alias"] && y["alias"].IsMap())
                    for (auto it = y["alias"].begin(); it != y["alias"].end(); ++it) {
                        std::string canon = it->first.as<std::string>("");
                        std::vector<std::string> al;
                        if (it->second.IsSequence()) for (auto&& a : it->second) al.push_back(a.as<std::string>(""));
                        else if (it->second.IsScalar()) al.push_back(it->second.as<std::string>(""));
                        regAlias(canon, al);
                    }
                if (y["defaultsComputed"] && y["defaultsComputed"].IsMap())
                    for (auto it = y["defaultsComputed"].begin(); it != y["defaultsComputed"].end(); ++it) {
                        std::string name = it->first.as<std::string>(""), d = translateGame(it->second.as<std::string>(""));
                        if (!name.empty() && !d.empty()) derivedRegistry()[name] = d;
                    }
            } catch (...) {}
        }
        return n;
    }

    /// 把规则包(rules/*.json)的 computed 区块合并进全局衍生公式表。返回合并条数。
    /// 启动期调用（与 alias 合并并列）。computedRegistry/attrMax 在私有区定义，
    /// 同类内可互访、前向引用无碍。
    static int loadRulePackComputed(const std::string& dir) {
        int n = 0; std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) return 0;
        for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (ec || !e.is_regular_file() || e.path().extension() != ".json") continue;
            try {
                std::ifstream f(e.path(), std::ios::binary);
                std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                json j = json::parse(body);
                if (!j.contains("computed") || !j["computed"].is_object()) continue;
                for (auto& [canon, formula] : j["computed"].items())
                    if (formula.is_string() && !formula.get<std::string>().empty()) {
                        computedRegistry()[canon] = formula.get<std::string>(); ++n;
                    }
            } catch (...) {}
        }
        return n;
    }

    // ─── 规则包注册表（P2：.set 切换 + WebUI 管理）─────────────
    struct RulePack {
        std::string name, fullName, version, file, dir, author;
        std::vector<std::string> setKeys;
        int diceSides = 0, aliasGroups = 0, computedCount = 0, manualCount = 0;
        bool enabled = true;     // 文件 .json=启用 / .json.disabled=停用（C#4）
        // C#12 指令层（commands 块）：本规则启用时生效。
        std::vector<std::string> disableCmds;            // 屏蔽的指令（小写）
        std::map<std::string, std::string> cmdAlias;     // 指令别名：输入词→目标指令（.检定→ra）
        // C#12-A② 自定义指令：指令名→输出模板（含 {表达式} DSL 占位）。
        std::map<std::string, std::string> customCmds;
        // C#10 帮助：主题→帮助文本（随规则包分发）。来源 = 包级 help 块 + 各自定义指令的 help 字段。
        std::map<std::string, std::string> helpEntries;
    };
    static std::vector<RulePack>& rulePacks() { static std::vector<RulePack> v; return v; }
    /// 去掉 .disabled 后缀，得到逻辑文件名（coc7.json.disabled → coc7.json）。
    static std::string ruleBaseFile(const std::string& file) {
        const std::string sfx = ".disabled";
        if (file.size() > sfx.size() && file.substr(file.size() - sfx.size()) == sfx)
            return file.substr(0, file.size() - sfx.size());
        return file;
    }
    /// 内置规则包（随包发行、不可删，但可停用）。数据迁移后内置也住 data/rules，故按文件名判定。
    static bool isBuiltinRulePack(const std::string& file) {
        std::string f = ruleBaseFile(file);
        return f == "coc7.json" || f == "dnd.json";
    }

    /// 加载 @p dir 下所有规则包(rules/*.json)：① 填充元数据表(供 .set / WebUI)
    /// ② 合并 alias/computed 到全局表。返回加载的规则包数。统一入口，替代分散的
    /// loadRulePackAliases / loadRulePackComputed（仍保留以兼容，但 main 只调本函数）。
    static int loadRulePacks(const std::string& dir) {
        int n = 0; std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) return 0;
        for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (ec || !e.is_regular_file()) continue;
            std::string fname = u8str(e.path().filename());
            // 接受 *.json（启用）和 *.json.disabled（停用，14 字符后缀）。
            bool enabled;
            if (fname.size() > 5 && fname.substr(fname.size() - 5) == ".json") enabled = true;
            else if (fname.size() > 14 && fname.substr(fname.size() - 14) == ".json.disabled") enabled = false;
            else continue;
            try {
                std::ifstream f(e.path(), std::ios::binary);
                std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                json j = json::parse(body);
                int ag = 0, cc = 0;
                // 停用的规则包：只读元数据进列表，不合并 alias/computed（不生效）。
                if (enabled && j.contains("alias") && j["alias"].is_object()) {
                    ag = (int)j["alias"].size();
                    for (auto& [canon, arr] : j["alias"].items())
                        if (arr.is_array()) for (auto& a : arr)
                            if (a.is_string()) CharacterCardStore::registerAlias(a.get<std::string>(), canon);
                } else if (j.contains("alias") && j["alias"].is_object()) {
                    ag = (int)j["alias"].size();
                }
                if (enabled && j.contains("computed") && j["computed"].is_object())
                    for (auto& [canon, formula] : j["computed"].items())
                        if (formula.is_string() && !formula.get<std::string>().empty()) {
                            computedRegistry()[canon] = formula.get<std::string>(); ++cc;
                        }
                else if (j.contains("computed") && j["computed"].is_object())
                    cc = (int)j["computed"].size();
                // C#102：技能默认值（"defaults":{"技能":数值}）→ 全局默认值表（可覆盖内置）。
                if (enabled && j.contains("defaults") && j["defaults"].is_object())
                    for (auto& [nm, val] : j["defaults"].items())
                        if (val.is_number_integer())
                            defaultsRegistry()[CharacterCardStore::canonical(nm)] = val.get<int>();
                // C#102：派生关系（"derived":{"闪避":"敏捷/2"}）→ 规则包派生表（独立于
                // derivedRegistry——后者会被 loadModelTemplates 重建，查询时规则包优先）。
                if (enabled && j.contains("derived") && j["derived"].is_object())
                    for (auto& [nm, expr] : j["derived"].items())
                        if (expr.is_string() && !expr.get<std::string>().empty())
                            rulePackDerivedRegistry()[CharacterCardStore::canonical(nm)] = expr.get<std::string>();
                // 是「规则包」才进注册表（仅 entries 的纯速查手册不算，但其别名/公式已合并）。
                bool isPack = j.contains("set") || j.contains("alias")
                           || j.contains("computed") || j.contains("successLevels");
                if (!isPack) continue;
                RulePack p;
                p.name = j.value("name", u8str(e.path().stem()));
                p.fullName = j.value("fullName", p.name);
                p.version = j.value("version", "");
                p.author = j.value("author", "");
                p.file = fname;
                p.dir = dir;
                p.enabled = enabled;
                p.aliasGroups = ag; p.computedCount = cc;
                if (j.contains("set") && j["set"].is_object()) {
                    p.diceSides = j["set"].value("diceSides", 0);
                    if (j["set"].contains("keys") && j["set"]["keys"].is_array())
                        for (auto& k : j["set"]["keys"]) if (k.is_string()) p.setKeys.push_back(k.get<std::string>());
                }
                if (j.contains("entries") && j["entries"].is_object()) p.manualCount = (int)j["entries"].size();
                // C#12 指令层：commands.disable[] / commands.alias{输入→目标}。
                if (j.contains("commands") && j["commands"].is_object()) {
                    auto& c = j["commands"];
                    if (c.contains("disable") && c["disable"].is_array())
                        for (auto& d : c["disable"]) if (d.is_string()) p.disableCmds.push_back(toLower(d.get<std::string>()));
                    if (c.contains("alias") && c["alias"].is_object())
                        for (auto& [k, v] : c["alias"].items()) if (v.is_string()) p.cmdAlias[k] = v.get<std::string>();
                    // C#12-A②：自定义指令。值可为字符串(=输出模板) 或 {output:"...",help:"..."} 对象。
                    if (c.contains("add") && c["add"].is_object())
                        for (auto& [k, v] : c["add"].items()) {
                            if (v.is_string()) p.customCmds[k] = v.get<std::string>();
                            else if (v.is_object()) {
                                if (v.contains("output") && v["output"].is_string()) p.customCmds[k] = v["output"].get<std::string>();
                                if (v.contains("help") && v["help"].is_string()) p.helpEntries[k] = v["help"].get<std::string>();  // C#10
                            }
                        }
                }
                // C#10：包级 help 块（任意主题→帮助文本，随包分发）。
                if (j.contains("help") && j["help"].is_object())
                    for (auto& [k, v] : j["help"].items()) if (v.is_string()) p.helpEntries[k] = v.get<std::string>();
                rulePacks().push_back(std::move(p));
                ++n;
            } catch (...) {}
        }
        return n;
    }
    /// 重新加载规则包：清空注册表 + 把 alias/computed 重置为内置默认 + 重新扫描。
    /// 供 WebUI 导入/删除后热生效。⚠️ 非服务期并发安全，仅在请求线程串行调用。
    static int reloadRulePacks(const std::vector<std::string>& dirs) {
        std::unique_lock<std::shared_mutex> lk(rulesLock());   // 独占：阻塞所有读者
        rulePacks().clear();
        CharacterCardStore::resetAliases();
        resetComputed();
        resetDefaults();                    // C#102：默认值/派生随规则包重载重建
        rulePackDerivedRegistry().clear();
        int n = 0; for (const auto& d : dirs) n += loadRulePacks(d);
        loadRulePackBundles();   // C#27：重新扫描 data/rulepacks/<包>/（并入各包 rules）
        loadModelTemplates();    // 规则 mod 的 model/*.xml 属性别名（alias 已被 resetAliases 清空，须重注册）
        return n;
    }
    /// 按 setKey（如 coc7/dnd/5e）查规则包，**返回副本**（避免重载后指针失效）；无则空。
    static std::optional<RulePack> rulePackByKey(const std::string& key) {
        std::string kl = toLower(key);
        std::shared_lock<std::shared_mutex> lk(rulesLock());
        for (const auto& rp : rulePacks()) {
            if (!rp.enabled) continue;             // 停用的规则包 .set 不可切换
            for (const auto& k : rp.setKeys)
                if (toLower(k) == kl) return rp;   // 拷贝
        }
        return std::nullopt;
    }
    /// 本群当前激活规则系统（group_settings "ruleSystem"）对应的规则包副本，无则空。
    std::optional<RulePack> activeRulePack(const Message& msg) const {
        std::string name = getGroupSetting(msg, "ruleSystem");
        if (name.empty()) return std::nullopt;
        std::shared_lock<std::shared_mutex> lk(rulesLock());
        for (const auto& rp : rulePacks())
            if (rp.enabled && rp.name == name) return rp;   // 拷贝
        return std::nullopt;
    }

    // ── C#27：规则包 bundle = data/rulepacks/<名>/ ─────────────────────
    //   pack.json: {name, version, author, description, setKeys:[..]}
    //   rules/*.json   → 规则文件（现有格式，并入全局 rulePacks()）
    //   helpdoc/*.json → 帮助速查（loadHelpDocs 扫描，标 "<包>/<mod>"）
    //   lua/  js/      → 插件（按群激活 gating，建在「插件分群启停」之上）
    // 整体装卸：删/停用 = 删/改名包目录；目录名带 .disabled 后缀=停用。
    struct RulePackBundle {
        std::string name, version, author, description, dir, folder;
        std::vector<std::string> setKeys;     // 激活键（.set <key>）
        std::vector<std::string> pluginIds;   // 本包拥有的插件 id（"js:.."/"lua:.."），按群激活用
        int ruleFiles = 0, cmdCount = 0, helpdocEntries = 0, luaMods = 0, jsPlugins = 0;
        bool enabled = true;
    };
    static std::vector<RulePackBundle>& rulePackBundles() { static std::vector<RulePackBundle> v; return v; }

    /// 各启用规则包的 lua/ 与 js/ 子目录（UTF-8 路径串），供 luaMod/jsMod.setExtraDirs 附加加载。
    static void packPluginDirs(std::vector<std::string>& luaDirs, std::vector<std::string>& jsDirs) {
        namespace fs = std::filesystem; std::error_code ec;
        std::shared_lock<std::shared_mutex> lk(rulesLock());
        for (auto& b : rulePackBundles()) {
            if (!b.enabled) continue;
            std::string lua = b.dir + "/lua", js = b.dir + "/js";
            auto u8p = [](const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); };
            if (fs::is_directory(u8p(lua), ec)) luaDirs.push_back(lua);
            if (fs::is_directory(u8p(js), ec)) jsDirs.push_back(js);
        }
    }

    /// 加载 data/rulepacks/*/：建 bundle 注册表 + 把各启用包的 rules/ 并入 rulePacks()。
    /// helpdoc/ 由 loadHelpDocs() 另扫；lua/js 由各自管理器另扫（main 注入 packOwns）。返回包数。
    /// ⚠️ 修改 rulePacks()，须在无服务期或 rulesLock 写锁下调用。
    static int loadRulePackBundles() {
        namespace fs = std::filesystem; std::error_code ec;
        rulePackBundles().clear();
        if (!fs::is_directory("data/rulepacks", ec)) return 0;
        // Windows 上 path::string() 对非 ASCII 会产出系统码页（非 UTF-8）→ JSON 出代理字符、
        // 前端拿回的 folder 对不上真实目录。统一用 u8string 取 UTF-8。
        auto u8s = [](const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); };
        int n = 0;
        for (auto& e : fs::directory_iterator("data/rulepacks", ec)) {
            if (ec || !e.is_directory()) continue;
            std::string folder = u8s(e.path().filename());
            bool enabled = true; std::string base = folder;
            const std::string sfx = ".disabled";
            if (folder.size() > sfx.size() && folder.substr(folder.size() - sfx.size()) == sfx) {
                enabled = false; base = folder.substr(0, folder.size() - sfx.size());
            }
            RulePackBundle b; b.name = base; b.folder = folder; b.dir = u8str(e.path()); b.enabled = enabled;
            std::error_code fe;
            if (fs::is_regular_file(e.path() / "pack.json", fe)) {
                try {
                    std::ifstream f(e.path() / "pack.json", std::ios::binary);
                    json j = json::parse(f, nullptr, false, true);
                    if (j.is_object()) {
                        b.name = j.value("name", base);
                        b.version = j.value("version", std::string(""));
                        b.author = j.value("author", std::string(""));
                        b.description = j.value("description", j.value("brief", std::string("")));
                        if (j.contains("setKeys") && j["setKeys"].is_array())
                            for (auto& k : j["setKeys"]) if (k.is_string()) b.setKeys.push_back(k.get<std::string>());
                    }
                } catch (...) {}
            }
            if (enabled) {   // rules/ 并入全局 rulePacks()
                size_t before = rulePacks().size();
                b.ruleFiles = loadRulePacks(u8str(e.path() / "rules"));
                for (size_t i = before; i < rulePacks().size(); ++i) {
                    b.cmdCount += (int)rulePacks()[i].customCmds.size() + (int)rulePacks()[i].cmdAlias.size();
                    if (b.setKeys.empty()) for (auto& k : rulePacks()[i].setKeys) b.setKeys.push_back(k);
                }
            }
            if (fs::is_directory(e.path() / "helpdoc", fe))   // 仅计数（实际加载在 loadHelpDocs）
                for (fs::recursive_directory_iterator it(e.path() / "helpdoc", fe), end; it != end; it.increment(fe)) {
                    if (fe) { fe.clear(); continue; }
                    std::error_code ie;
                    if (it->is_regular_file(ie) && it->path().extension() == ".json")
                        try { std::ifstream hf(it->path(), std::ios::binary); json hj = json::parse(hf, nullptr, false, true);
                            if (hj.is_object() && hj.contains("helpdoc") && hj["helpdoc"].is_object()) b.helpdocEntries += (int)hj["helpdoc"].size();
                        } catch (...) {}
                }
            std::error_code de;
            // pluginIds 用于 gating 比对，须与「实际加载的 mod 名/文件名」(path::string()) 同编码 → 不用 u8s。
            if (fs::is_directory(e.path() / "lua", de))
                for (auto& le : fs::directory_iterator(e.path() / "lua", de)) {
                    if (le.is_directory()) { ++b.luaMods; b.pluginIds.push_back("lua:" + u8str(le.path().filename())); }
                    else if (le.path().extension() == ".lua") { ++b.luaMods; b.pluginIds.push_back("lua:" + u8str(le.path().stem())); }
                }
            if (fs::is_directory(e.path() / "js", de))
                for (auto& je : fs::directory_iterator(e.path() / "js", de))
                    if (je.path().extension() == ".js") { ++b.jsPlugins; b.pluginIds.push_back("js:" + u8str(je.path().filename())); }
            rulePackBundles().push_back(std::move(b));
            ++n;
        }
        return n;
    }

    /// SealDice-style privilege level for the JS subsystem ctx.privilegeLevel:
    /// 100 master / 70 信任 / 60 群主 / 50 管理 / 0 普通。
    int jsPrivilegeLevel(const Message& msg) const {
        if (isMaster(msg)) return 100;
        int t = senderTrust(msg);
        if (t >= 4) return 70;
        std::string role; try { role = msg.extra.value("role", std::string()); } catch (...) {}
        if (role == "owner") return 60;
        if (role == "admin") return 50;
        return 0;
    }

    /// If `text` starts with a configured command prefix, return the text after it
    /// (trimmed); otherwise nullopt. Used to feed the JS plugin subsystem.
    std::optional<std::string> commandBody(const std::string& text) const {
        std::string t = trim(text);
        for (const auto& p : commandPrefixes())
            if (!p.empty() && t.rfind(p, 0) == 0) return trim(t.substr(p.size()));
        return std::nullopt;
    }

private:
    // ─── Group settings (.bot on/off, future group management) ──

    std::string getGroupSetting(const Message& msg, const std::string& key) const {
        auto* st = db_.getStorage();
        if (!st) return "";
        try {
            namespace orm = sqlite_orm;
            auto rows = st->get_all<GroupSettingRow>(
                orm::where(orm::c(&GroupSettingRow::platform) == msg.platform
                    and orm::c(&GroupSettingRow::groupId) == msg.targetId
                    and orm::c(&GroupSettingRow::key) == key));
            if (!rows.empty()) return rows.front().value;
        } catch (...) {}
        return "";
    }
    void setGroupSetting(const Message& msg, const std::string& key, const std::string& value) {
        auto* st = db_.getStorage();
        if (!st) return;
        try {
            namespace orm = sqlite_orm;
            auto rows = st->get_all<GroupSettingRow>(
                orm::where(orm::c(&GroupSettingRow::platform) == msg.platform
                    and orm::c(&GroupSettingRow::groupId) == msg.targetId
                    and orm::c(&GroupSettingRow::key) == key));
            if (rows.empty()) {
                GroupSettingRow r;
                r.platform = msg.platform; r.groupId = msg.targetId; r.key = key; r.value = value;
                st->insert(r);
            } else { auto r = rows.front(); r.value = value; st->update(r); }
        } catch (...) {}
    }
    void setGroupEnabled(const Message& msg, bool enabled) {
        if (msg.type == MessageType::kPrivate || msg.targetId.empty()) return;
        setGroupSetting(msg, "enabled", enabled ? "1" : "0");
    }

public:
    // ── 插件分群启停（C#27 地基；JS/Lua 通用）──────────────────────────
    // 默认：全局启用的插件在所有群生效；某群可单独禁用之。插件 id 形如 "js:<名>"/"lua:<名>"。
    // 存 group_settings 的 "pluginsOff"（\n 分隔的 id 列表）。私聊不 gating。
    std::vector<std::string> disabledPluginsInGroup(const std::string& platform, const std::string& group) const {
        Message m; m.platform = platform; m.targetId = group;
        std::string raw = getGroupSetting(m, "pluginsOff");
        std::vector<std::string> out; std::string cur;
        for (char c : raw) { if (c == '\n') { if (!cur.empty()) out.push_back(cur); cur.clear(); } else cur += c; }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }
    bool isPluginEnabledInGroup(const std::string& platform, const std::string& group, const std::string& pluginId) const {
        if (group.empty()) return true;   // 私聊不 gating
        // C#27：pack-bound 插件（属于某规则包）→ 仅在本群激活了该包对应规则系统时生效。
        // 群的 ruleSystem 存的是「激活规则的名字」；该规则的 setKeys 与本包 setKeys 有交集 = 包已激活。
        {
            std::shared_lock<std::shared_mutex> lk(rulesLock());
            for (auto& b : rulePackBundles()) {
                if (std::find(b.pluginIds.begin(), b.pluginIds.end(), pluginId) == b.pluginIds.end()) continue;
                if (!b.enabled) return false;
                std::string active = groupSettingValue(platform, group, "ruleSystem");
                if (active.empty()) return false;   // 本群未激活任何规则 → 包插件不生效
                for (auto& k : b.setKeys) if (k == active) return true;   // 兼容：ruleSystem 直接是 setKey
                for (auto& rp : rulePacks()) {       // 常规：ruleSystem=规则名 → 查其 setKeys
                    if (rp.name != active) continue;
                    for (auto& sk : rp.setKeys) for (auto& bk : b.setKeys) if (sk == bk) return true;
                    break;
                }
                return false;   // 包未在本群激活 → 其插件不生效
            }
        }
        // free 插件：默认全局启用，群黑名单可单独禁用。
        for (auto& d : disabledPluginsInGroup(platform, group)) if (d == pluginId) return false;
        return true;
    }
    // seal.vars ↔ 人物卡桥接（JS 规则插件用无$前缀属性名读写玩家卡 = .st/.ra 同一份卡）。
    // 私聊 scope=""，群聊 scope=群号；getAttr/setAttr 内部按规范名(canonical)解析。
    bool jsCardGet(const std::string& /*platform*/, const std::string& userId,
                   const std::string& groupId, const std::string& attr, long long& out) {
        const std::string scope = groupId.empty() ? std::string() : groupId;
        if (auto v = cards_.getAttr(userId, scope, attr)) { out = *v; return true; }
        return false;
    }
    void jsCardSet(const std::string& /*platform*/, const std::string& userId,
                   const std::string& groupId, const std::string& attr, long long val) {
        const std::string scope = groupId.empty() ? std::string() : groupId;
        cards_.setAttr(userId, scope, attr, static_cast<int>(val));
    }
    // C#37：读关联/表达式属性原文（.st 物防='dex+1' 存的），供 seal.vars.strGet。
    bool jsCardGetStr(const std::string& /*platform*/, const std::string& userId,
                      const std::string& groupId, const std::string& attr, std::string& out) {
        const std::string scope = groupId.empty() ? std::string() : groupId;
        std::string v = getUserSettingOf(userId, scope, "sattr:" + CharacterCardStore::canonical(attr));
        if (v.empty()) return false;
        out = v; return true;
    }

    void setPluginEnabledInGroup(const std::string& platform, const std::string& group,
                                 const std::string& pluginId, bool enabled) {
        if (group.empty() || pluginId.empty()) return;
        auto cur = disabledPluginsInGroup(platform, group);
        bool present = std::find(cur.begin(), cur.end(), pluginId) != cur.end();
        if (enabled && present) cur.erase(std::remove(cur.begin(), cur.end(), pluginId), cur.end());
        else if (!enabled && !present) cur.push_back(pluginId);
        else return;   // 无变化
        std::string joined; for (auto& s : cur) { if (!joined.empty()) joined += '\n'; joined += s; }
        Message m; m.platform = platform; m.targetId = group;
        setGroupSetting(m, "pluginsOff", joined);
    }

    // C#33：.plugin —— 群主/管理员分群启停插件。
    // ─── .system —— 系统信息 / 运行统计 (C#53, 仅骰主) ──────────
    //   .system info    系统硬件信息 + 占用率
    //   .system stats   运行时长 / 指令数 / 好友 / 群 / 玩家 / 群记录数
    std::optional<std::string> tryHandleSystem(Locale loc, const Message& msg, const std::string& cmd) {
        if (toLower(cmd).rfind("system", 0) != 0) return std::nullopt;
        if (cmd.size() > 6 && cmd[6] != ' ' && cmd[6] != '\t') return std::nullopt;  // 整词
        if (!isMaster(msg)) return std::string();   // 骰主专用，非骰主静默
        std::string sub = toLower(trim(cmd.substr(6)));
        if (sub == "info") {
            auto si = sysinfo::gather();
            std::string cpu = (si.cpuLoadPct < 0) ? "?" : fmtOneDecimal(si.cpuLoadPct);
            return i18n_.tr(loc, "system.info", {
                {"os", si.os},
                {"cores", std::to_string(si.cpuCores)},
                {"cpu", cpu},
                {"memused", std::to_string(si.memUsedMB)},
                {"memtotal", std::to_string(si.memTotalMB)},
                {"memload", std::to_string(si.memLoadPct)},
                {"proc", std::to_string(si.procMemMB)},
            });
        }
        if (sub == "stats") {
            long uptime = static_cast<long>(std::time(nullptr)) - utils::getStartupEpoch();
            if (uptime < 0) uptime = 0;
            // 好友：累加各连接适配器（-1 = 未同步）；全为 -1 → 未知
            int friends = -1;
            int groups = 0;
            for (auto& a : adapters_.allAdapters()) {
                if (!a->isConnected()) continue;
                int fc = a->getFriendCount();
                if (fc >= 0) friends = (friends < 0 ? 0 : friends) + fc;
                groups += static_cast<int>(a->getGroupList().size());
            }
            long players = 0, logs = 0;
            try { if (auto* st = db_.getStorage()) players = static_cast<long>(st->count<PlayerProfileRow>()); } catch (...) {}
            try { if (auto* lst = db_.getLogStorage()) logs = static_cast<long>(lst->count<GameLogRow>()); } catch (...) {}
            return i18n_.tr(loc, "system.stats", {
                {"uptime", fmtDuration(uptime)},
                {"cmds", std::to_string(s_cmdCount.load(std::memory_order_relaxed))},
                {"friends", friends < 0 ? i18n_.tr(loc, "system.unknown") : std::to_string(friends)},
                {"groups", std::to_string(groups)},
                {"players", std::to_string(players)},
                {"logs", std::to_string(logs)},
            });
        }
        return i18n_.tr(loc, "system.usage");
    }
    /// 把秒数格式化为「Xd Yh Zm Ws」（去掉为 0 的高位）。
    static std::string fmtDuration(long sec) {
        long d = sec / 86400; sec %= 86400;
        long h = sec / 3600;  sec %= 3600;
        long m = sec / 60;    long s = sec % 60;
        std::string out;
        if (d > 0) out += std::to_string(d) + "d ";
        if (d > 0 || h > 0) out += std::to_string(h) + "h ";
        if (d > 0 || h > 0 || m > 0) out += std::to_string(m) + "m ";
        out += std::to_string(s) + "s";
        return out;
    }
    static std::string fmtOneDecimal(double v) {
        char b[32]; std::snprintf(b, sizeof(b), "%.1f", v); return b;
    }

    // ─── .plugin —— 分群插件启停 (C#33) ────────────────────────
    //   .plugin list           列出所有插件 + 本群状态
    //   .plugin on/off <名>     启用/停用某插件（按名称或 id 匹配）
    //   .plugin all on/off      批量启停
    std::optional<std::string> tryHandlePlugin(Locale loc, const Message& msg, const std::string& cmd) {
        if (toLower(cmd).rfind("plugin", 0) != 0) return std::nullopt;
        // 须是 "plugin" 整词或其后接空白（避免误吃别的 plugin* 指令）。
        if (cmd.size() > 6 && !std::isspace(static_cast<unsigned char>(cmd[6]))) return std::nullopt;
        if (msg.type == MessageType::kPrivate || msg.targetId.empty())
            return i18n_.tr(loc, "plugin.group_only");
        if (!pluginProvider_) return i18n_.tr(loc, "plugin.unavailable");

        auto plugins = pluginProvider_();
        std::string rest = trim(cmd.size() > 6 ? cmd.substr(6) : "");
        auto [subRaw, argRaw] = splitCommand(rest);
        std::string sub = toLower(trim(subRaw));
        std::string arg = trim(argRaw);

        // .plugin / .plugin list —— 任何人可看
        if (sub.empty() || sub == "list") {
            if (plugins.empty()) return i18n_.tr(loc, "plugin.empty");
            std::string list;
            for (auto& p : plugins) {
                std::string state;
                if (!p.enabledGlobal) state = i18n_.tr(loc, "plugin.state.global_off");
                else state = isPluginEnabledInGroup(msg.platform, msg.targetId, p.id)
                           ? i18n_.tr(loc, "plugin.state.on") : i18n_.tr(loc, "plugin.state.off");
                if (!list.empty()) list += "\n";
                list += p.name + " [" + state + "]";
            }
            return i18n_.tr(loc, "plugin.list", {{"count", std::to_string(plugins.size())}, {"list", list}});
        }

        // 以下为启停操作 —— 仅群主/管理员/Master
        if (!senderIsGroupAdmin(msg)) return i18n_.tr(loc, "gate.no_perm");

        if (sub == "all") {
            std::string onoff = toLower(trim(splitCommand(arg).first));
            if (onoff != "on" && onoff != "off") return i18n_.tr(loc, "plugin.usage");
            bool en = (onoff == "on");
            for (auto& p : plugins) setPluginEnabledInGroup(msg.platform, msg.targetId, p.id, en);
            return i18n_.tr(loc, en ? "plugin.all_on" : "plugin.all_off",
                            {{"count", std::to_string(plugins.size())}});
        }
        if (sub == "on" || sub == "off") {
            if (arg.empty()) return i18n_.tr(loc, "plugin.usage");
            const PluginEntry* found = matchPlugin(plugins, arg);
            if (!found) return i18n_.tr(loc, "plugin.not_found", {{"name", arg}});
            bool en = (sub == "on");
            setPluginEnabledInGroup(msg.platform, msg.targetId, found->id, en);
            return i18n_.tr(loc, en ? "plugin.enabled" : "plugin.disabled", {{"name", found->name}});
        }
        return i18n_.tr(loc, "plugin.usage");
    }

    // 按名称或 id 匹配插件：精确 id / 精确名称(不分大小写) / 去前缀(lua:/js:)与扩展名。
    const PluginEntry* matchPlugin(const std::vector<PluginEntry>& plugins, const std::string& q) const {
        std::string ql = toLower(q);
        auto strip = [](std::string s) {
            if (auto c = s.find(':'); c != std::string::npos) s = s.substr(c + 1);
            if (auto d = s.rfind('.'); d != std::string::npos) s = s.substr(0, d);
            return s;
        };
        for (auto& p : plugins) {
            if (p.id == q || toLower(p.name) == ql) return &p;
            if (toLower(strip(p.id)) == ql) return &p;
        }
        return nullptr;
    }

private:
    // ─── 全局 / 外置 / 单群命令停用 (console + 群管词条) ─────────
    /// Global silent mode (原版 console DisabledGlobal): non-trusted users get no
    /// reply at all. From config dice.silent_global (bool).
    bool silentGlobal() const {
        try {
            json all = cfg_.getAll();
            if (all.contains("dice") && all["dice"].contains("silent_global"))
                return all["dice"]["silent_global"].get<bool>();
        } catch (...) {}
        return false;
    }
    /// Globally-disabled command names for non-trusted users (原版 DisabledMe/Jrrp/
    /// Draw/Send/Deck). Read from individual config bools dice/disabled_<cmd> (系统设置
    /// 页的开关），兼容旧的 dice/disabled_global 数组形式。
    std::vector<std::string> globalDisabledCmds() const {
        std::vector<std::string> v;
        static const char* names[] = {"jrrp", "me", "deck", "draw", "send", "help"};
        try {
            json all = cfg_.getAll();
            if (all.contains("dice") && all["dice"].is_object()) {
                const json& d = all["dice"];
                for (auto* n : names) {
                    std::string key = std::string("disabled_") + n;
                    if (d.contains(key) && d[key].is_boolean() && d[key].get<bool>()) v.push_back(n);
                }
                if (d.contains("disabled_global") && d["disabled_global"].is_array())
                    for (auto& e : d["disabled_global"]) if (e.is_string()) v.push_back(toLower(e.get<std::string>()));
            }
        } catch (...) {}
        return v;
    }
    /// External mode (原版「停用指令」): built-in commands suppressed, custom replies kept.
    bool groupExternalMode(const Message& msg) const {
        if (msg.type == MessageType::kPrivate || msg.targetId.empty()) return false;
        return getGroupSetting(msg, "externalMode") == "1";
    }
    /// Whether custom replies are disabled in this group (原版「禁用回复」). Public so
    /// the message loop can suppress custom replies.
public:
    bool isReplyDisabledFor(const std::string& platform, const std::string& groupId) const {
        Message m; m.platform = platform; m.targetId = groupId; m.type = MessageType::kGroup;
        return getGroupSetting(m, "replyDisabled") == "1";
    }
private:
    /// Per-group single-command disable list (原版「禁用jrrp」等), stored as a
    /// comma-separated group_setting "disabledCmds".
    bool groupCmdDisabled(const Message& msg, const std::string& name) const {
        std::string v = getGroupSetting(msg, "disabledCmds");
        if (v.empty()) return false;
        size_t pos = 0;
        while (pos < v.size()) {
            size_t c = v.find(',', pos);
            std::string tok = trim(v.substr(pos, c == std::string::npos ? std::string::npos : c - pos));
            if (tok == name) return true;
            if (c == std::string::npos) break;
            pos = c + 1;
        }
        return false;
    }
    void setGroupCmdDisabled(const Message& msg, const std::string& name, bool on) {
        std::vector<std::string> toks;
        std::string v = getGroupSetting(msg, "disabledCmds");
        size_t pos = 0;
        while (pos < v.size()) {
            size_t c = v.find(',', pos);
            std::string tok = trim(v.substr(pos, c == std::string::npos ? std::string::npos : c - pos));
            if (!tok.empty() && tok != name) toks.push_back(tok);
            if (c == std::string::npos) break;
            pos = c + 1;
        }
        if (on) toks.push_back(name);
        std::string out;
        for (auto& t : toks) { if (!out.empty()) out += ","; out += t; }
        setGroupSetting(msg, "disabledCmds", out);
    }
    /// Check whether a command is blocked (global or per-group). Returns the block
    /// message, or nullopt if allowed. Only the toggleable commands are gated.
    std::optional<std::string> gateCommand(Locale loc, const Message& msg, const std::string& cmdL) {
        std::string head = cmdL.substr(0, cmdL.find(' '));
        if (head == "mrrp" || head == "zrrp") head = "jrrp";   // 同属 jrrp 家族
        static const std::set<std::string> kGated = {"jrrp", "draw", "deck", "send", "help", "me"};
        if (!kGated.count(head)) return std::nullopt;
        // .me on/off 是开关子指令，须放行到 handler（群管门控），否则关掉后就再也开不回来。
        if (head == "me") {
            std::string sub = trim(cmdL.substr((std::min)(cmdL.size(), cmdL.find(' ') == std::string::npos ? cmdL.size() : cmdL.find(' ') + 1)));
            if (sub == "on" || sub == "off") return std::nullopt;
        }
        int trust = senderTrust(msg);
        bool privileged = trust >= 4 || isMaster(msg);
        if (!privileged) {
            for (auto& g : globalDisabledCmds()) if (g == head) {
                if (head == "jrrp") return i18n_.tr(loc, "gate.jrrp_global");
                if (head == "me")   return i18n_.tr(loc, "gate.me_global");
                return i18n_.tr(loc, "gate.cmd_global", {{"cmd", head}});
            }
        }
        if (trust < 5 && groupCmdDisabled(msg, head))
            return i18n_.tr(loc, "gate.in_group", {{"cmd", head}});
        return std::nullopt;
    }
    /// 外置开 / 外置关 — toggle external mode for this group (admin/master only).
    std::string handleExternalToggle(Locale loc, const Message& msg, bool on) {
        if (msg.type == MessageType::kPrivate) return i18n_.tr(loc, "group.private");
        if (!senderIsGroupAdmin(msg)) return i18n_.tr(loc, "gate.no_perm");
        setGroupSetting(msg, "externalMode", on ? "1" : "0");
        return i18n_.tr(loc, on ? "gate.external_on" : "gate.external_off");
    }
    /// 群管词条 (原版 .group +/-词条) → 内部设定。
    std::string handleGroupTerm(Locale loc, const Message& msg, const std::string& term, bool on) {
        // 停用指令 / 外置
        if (term == "\xe5\x81\x9c\xe7\x94\xa8\xe6\x8c\x87\xe4\xbb\xa4" ||      // 停用指令
            term == "\xe5\xa4\x96\xe7\xbd\xae") {                              // 外置
            setGroupSetting(msg, "externalMode", on ? "1" : "0");
            return i18n_.tr(loc, on ? "gate.external_on" : "gate.external_off");
        }
        // 禁用回复
        if (term == "\xe7\xa6\x81\xe7\x94\xa8\xe5\x9b\x9e\xe5\xa4\x8d") {      // 禁用回复
            setGroupSetting(msg, "replyDisabled", on ? "1" : "0");
            return i18n_.tr(loc, on ? "group.term_set" : "group.term_cleared", {{"term", term}});
        }
        // 禁用<cmd>: jrrp/draw/me/help/deck/send
        static const std::string kBan = "\xe7\xa6\x81\xe7\x94\xa8";           // 禁用
        if (term.rfind(kBan, 0) == 0) {
            std::string cmd = toLower(trim(term.substr(kBan.size())));
            static const std::set<std::string> kKnown = {"jrrp", "draw", "me", "help", "deck", "send"};
            if (kKnown.count(cmd)) {
                setGroupCmdDisabled(msg, cmd, on);
                return i18n_.tr(loc, on ? "group.term_set" : "group.term_cleared", {{"term", term}});
            }
        }
        return i18n_.tr(loc, "group.term_unknown", {{"term", term}});
    }
    /// .group clr — wipe this group's settings.
    void clearGroupSettings(const Message& msg) {
        auto* st = db_.getStorage(); if (!st) return;
        try {
            namespace orm = sqlite_orm;
            st->remove_all<GroupSettingRow>(orm::where(
                orm::c(&GroupSettingRow::platform) == msg.platform and
                orm::c(&GroupSettingRow::groupId) == msg.targetId));
        } catch (...) {}
    }


    /// Configured command prefixes (from config dice.command_prefixes), with a
    /// sensible default. Read per message so edits hot-reload.
    std::vector<std::string> commandPrefixes() const {
        try {
            json all = cfg_.getAll();
            if (all.contains("dice") && all["dice"].contains("command_prefixes") &&
                all["dice"]["command_prefixes"].is_array()) {
                std::vector<std::string> v;
                for (const auto& e : all["dice"]["command_prefixes"])
                    if (e.is_string() && !e.get<std::string>().empty())
                        v.push_back(e.get<std::string>());
                if (!v.empty()) return v;
            }
        } catch (...) {}
        // Default: . 。 ! ！
        return {".", "\xe3\x80\x82", "!", "\xef\xbc\x81"};
    }

    // ─── Master & cross-target messaging (.send) ─────────────

    /// A bot owner ("Master"), scoped per platform (so the same id on different
    /// platforms is distinct). Empty platform = legacy entry matching any platform.
    struct MasterEntry { std::string platform; std::string id; };

    /// Masters from config dice.masters (read per call → hot-reload). Accepts both
    /// new objects {platform,id} and legacy bare-id strings.
    std::vector<MasterEntry> masters() const {
        std::vector<MasterEntry> v;
        try {
            json all = cfg_.getAll();
            if (all.contains("dice") && all["dice"].contains("masters") &&
                all["dice"]["masters"].is_array()) {
                for (const auto& m : all["dice"]["masters"]) {
                    if (m.is_string()) {
                        std::string s = m.get<std::string>();
                        if (!s.empty()) v.push_back({"", s});
                    } else if (m.is_object()) {
                        MasterEntry e{m.value("platform", std::string()), m.value("id", std::string())};
                        if (!e.id.empty()) v.push_back(e);
                    }
                }
            }
        } catch (...) {}
        return v;
    }
    bool isMaster(const std::string& platform, const std::string& id) const {
        for (const auto& m : masters())
            if (m.id == id && (m.platform.empty() || m.platform == platform)) return true;
        return false;
    }
    // 消息重载：先做别名归并（原版 TinyList 在 master 判断之前生效），别名号享主号身份。
    bool isMaster(const Message& msg) const {
        return isMaster(msg.platform, resolveAlias(msg.platform, msg.senderId));
    }

    // ─── Master commands: boton/botoff + black/white lists ───
    // targetType: 0=user, 1=group.  listType: 0=blacklist, 1=whitelist.

    bool banlistHas(int type, int list, const std::string& id) const {
        auto* st = db_.getStorage(); if (!st) return false;
        try {
            namespace orm = sqlite_orm;
            return st->count<BanlistRow>(orm::where(
                orm::c(&BanlistRow::targetType) == type and
                orm::c(&BanlistRow::listType) == list and
                orm::c(&BanlistRow::targetId) == id)) > 0;
        } catch (...) { return false; }
    }
    void banlistAdd(int type, int list, const std::string& id, const std::string& reason) {
        auto* st = db_.getStorage(); if (!st || banlistHas(type, list, id)) return;
        try { BanlistRow r; r.targetType = type; r.listType = list; r.targetId = id;
              r.reason = reason; r.createdAt = nowIso(); st->insert(r); } catch (...) {}
    }
    bool banlistRemove(int type, int list, const std::string& id) {
        auto* st = db_.getStorage(); if (!st) return false;
        bool had = banlistHas(type, list, id);
        try {
            namespace orm = sqlite_orm;
            st->remove_all<BanlistRow>(orm::where(
                orm::c(&BanlistRow::targetType) == type and
                orm::c(&BanlistRow::listType) == list and
                orm::c(&BanlistRow::targetId) == id));
        } catch (...) {}
        return had;
    }
    bool whitelistOnly() const {
        try {
            json all = cfg_.getAll();
            if (all.contains("dice") && all["dice"].contains("whitelist_only"))
                return all["dice"]["whitelist_only"].get<bool>();
        } catch (...) {}
        return false;
    }

public:
    /// Write a group_settings value for an ARBITRARY group (boton/botoff <群号>)。
    /// public：main.cpp 事件层也用它记录群邀请人（C#47）等。
    void setGroupSettingFor(const std::string& platform, const std::string& groupId,
                            const std::string& key, const std::string& value) {
        auto* st = db_.getStorage(); if (!st) return;
        try {
            namespace orm = sqlite_orm;
            auto rows = st->get_all<GroupSettingRow>(orm::where(
                orm::c(&GroupSettingRow::platform) == platform and
                orm::c(&GroupSettingRow::groupId) == groupId and
                orm::c(&GroupSettingRow::key) == key));
            if (rows.empty()) { GroupSettingRow r; r.platform = platform; r.groupId = groupId;
                                r.key = key; r.value = value; st->insert(r); }
            else { auto r = rows.front(); r.value = value; st->update(r); }
        } catch (...) {}
    }
    /// Read a group_settings value for an arbitrary group (public：main.cpp AI 分群开关用)。
    std::string getGroupSettingFor(const std::string& platform, const std::string& groupId,
                                   const std::string& key) const {
        auto* st = db_.getStorage(); if (!st) return "";
        try {
            namespace orm = sqlite_orm;
            auto rows = st->get_all<GroupSettingRow>(orm::where(
                orm::c(&GroupSettingRow::platform) == platform and
                orm::c(&GroupSettingRow::groupId) == groupId and
                orm::c(&GroupSettingRow::key) == key));
            if (!rows.empty()) return rows.front().value;
        } catch (...) {}
        return "";
    }
    /// C#84：本群 AI 功能开关（group_settings key "aiEnabled"，缺省=开）。
    bool aiEnabledForGroup(const std::string& platform, const std::string& groupId) const {
        return getGroupSettingFor(platform, groupId, "aiEnabled") != "0";
    }

    /// AI 白名单模式（dice/ai.whitelist）：开启后仅白名单内的群/私聊可用 AI 对话/NPC，
    /// 非白名单群连 .ai on 都开不了（防群管随意打开）。白名单未启用时恒 true。
    bool aiWhitelistOk(const std::string& platform, const std::string& id, bool isGroup) const {
        try {
            auto a = cfg_.get<nlohmann::json>("dice/ai", nlohmann::json::object());
            if (!a.is_object() || !a.contains("whitelist") || !a["whitelist"].is_object()) return true;
            auto& w = a["whitelist"];
            if (!w.value("enabled", false)) return true;
            if (w.contains("list") && w["list"].is_array())
                for (auto& e : w["list"]) {
                    if (!e.is_object() || e.value("id", std::string()) != id) continue;
                    if (e.value("is_group", true) != isGroup) continue;
                    std::string p = e.value("platform", std::string());
                    if (p.empty() || p == platform) return true;
                }
            return false;
        } catch (...) { return true; }
    }

    // C#84：.ai on/off/status —— 本群 AI（对话/NPC）开关。开关需群管权限（与 .bot 一致）；仅群聊。
    std::string handleAi(Locale loc, const std::string& args, const Message& msg) {
        if (msg.type != MessageType::kGroup || msg.targetId.empty())
            return i18n_.tr(loc, "ai.group_only");
        std::string a = toLower(trim(args));
        bool on = aiEnabledForGroup(msg.platform, msg.targetId);
        const bool wlOk = aiWhitelistOk(msg.platform, msg.targetId, true);
        if (a.empty() || a == "status")
            return i18n_.tr(loc, !wlOk ? "ai.not_whitelisted" : on ? "ai.status_on" : "ai.status_off");
        if (!senderIsGroupAdmin(msg)) return i18n_.tr(loc, "gate.no_perm");
        if (a == "on" || a == "1") {
            // 白名单模式下非授权群拒绝开启（群管也不行）。
            if (!wlOk) return i18n_.tr(loc, "ai.not_whitelisted");
            if (on) return i18n_.tr(loc, "ai.already_on");
            setGroupSettingFor(msg.platform, msg.targetId, "aiEnabled", "1");
            return i18n_.tr(loc, "ai.on");
        }
        if (a == "off" || a == "0") {
            if (!on) return i18n_.tr(loc, "ai.already_off");
            setGroupSettingFor(msg.platform, msg.targetId, "aiEnabled", "0");
            return i18n_.tr(loc, "ai.off");
        }
        return i18n_.tr(loc, on ? "ai.status_on" : "ai.status_off");
    }

    // ─── C：用户权限指令（.trust / .admin，移植原版 nTrust 管理）──────────
    // .trust <@/QQ> [level] —— 查看或设置某用户信任等级(0-255)。需管理员(≥4)；只能设
    // 低于自己等级、且目标当前等级也低于自己。对齐原版 DiceEvent.cpp 的 trust 门控。
    std::string handleTrust(Locale loc, const std::string& args, const Message& msg) {
        int myLv = trustOf(msg);
        if (myLv < kTrustAdmin) return i18n_.tr(loc, "gate.no_perm");
        std::string at = atTarget(msg), target; std::optional<int> level;
        {
            std::istringstream iss(args); std::string tk; std::vector<std::string> nums;
            while (iss >> tk) if (isAllDigits(tk)) nums.push_back(tk);
            if (!at.empty()) { target = at; if (!nums.empty()) level = parseIntOr(nums.front(), 0); }
            else { if (!nums.empty()) target = nums[0]; if (nums.size() >= 2) level = parseIntOr(nums[1], 0); }
        }
        if (target.empty()) return i18n_.tr(loc, "trust.usage");
        int cur = trustOf(msg.platform, target, msg.selfId);
        std::string name = personNameOf(msg, target);
        if (!level) return i18n_.tr(loc, "trust.query", {{"user", name}, {"level", std::to_string(cur)}});
        int nl = *level;
        if (nl < 0 || nl > 255) return i18n_.tr(loc, "trust.range");
        if (nl >= myLv) return i18n_.tr(loc, "trust.too_high");          // 不能授予 ≥ 自己
        if (cur >= myLv) return i18n_.tr(loc, "trust.target_higher");    // 不能改等级不低于自己的人
        setTrust(msg.platform, target, nl);
        std::string origin = (msg.type == MessageType::kGroup) ? msg.targetId : msg.senderId;
        dice::notice::notify(cfg_, adapters_, dice::notice::kRoutine,
            "\xe5\xb7\xb2\xe5\xb0\x86 " + name + "(" + target + ") \xe7\x9a\x84\xe4\xbf\xa1\xe4\xbb\xbb\xe7\xad\x89\xe7\xba\xa7\xe8\xae\xbe\xe4\xb8\xba " + std::to_string(nl),
            msg.platform, origin, "trust");
        return i18n_.tr(loc, "trust.set", {{"user", name}, {"level", std::to_string(nl)}});
    }

    // .admin add/del/list <@/QQ> —— 授予/撤销管理员(=trust 4) / 列出管理员。需 Master。
    std::string handleAdmin(Locale loc, const std::string& args, const Message& msg) {
        if (!isMaster(msg)) return i18n_.tr(loc, "gate.not_master");
        std::istringstream iss(args); std::string act; iss >> act; act = toLower(act);
        std::string origin = (msg.type == MessageType::kGroup) ? msg.targetId : msg.senderId;
        if (act == "list") {
            std::string out; auto* st = db_.getStorage();
            if (st) try {
                namespace orm = sqlite_orm;
                auto rows = st->get_all<PlayerProfileRow>(orm::where(
                    orm::c(&PlayerProfileRow::platform) == msg.platform
                        and orm::c(&PlayerProfileRow::trustLevel) >= kTrustAdmin));
                for (auto& r : rows) out += (out.empty() ? "" : "\n") + r.userId + (r.nickname.empty() ? "" : "(" + r.nickname + ")");
            } catch (...) {}
            return i18n_.tr(loc, "admin.list", {{"list", out.empty() ? i18n_.tr(loc, "admin.none") : out}});
        }
        std::string target = atTarget(msg);
        if (target.empty()) { std::string tk; while (iss >> tk) if (isAllDigits(tk)) { target = tk; break; } }
        if (target.empty() || (act != "add" && act != "del")) return i18n_.tr(loc, "admin.usage");
        std::string name = personNameOf(msg, target);
        if (act == "add") {
            setTrust(msg.platform, target, kTrustAdmin);
            dice::notice::notify(cfg_, adapters_, dice::notice::kCritical,
                "\xe5\xb7\xb2\xe6\xb7\xbb\xe5\x8a\xa0 " + name + "(" + target + ") \xe7\x9a\x84\xe7\xae\xa1\xe7\x90\x86\xe6\x9d\x83\xe9\x99\x90",
                msg.platform, origin, "admin");
            return i18n_.tr(loc, "admin.added", {{"user", name}});
        }
        if (getTrust(msg.platform, target) < kTrustAdmin) return i18n_.tr(loc, "admin.not_admin");
        setTrust(msg.platform, target, kTrustNormal);
        dice::notice::notify(cfg_, adapters_, dice::notice::kCritical,
            "\xe5\xb7\xb2\xe6\x94\xb6\xe5\x9b\x9e " + name + "(" + target + ") \xe7\x9a\x84\xe7\xae\xa1\xe7\x90\x86\xe6\x9d\x83\xe9\x99\x90",
            msg.platform, origin, "admin");
        return i18n_.tr(loc, "admin.removed", {{"user", name}});
    }

    /// B：便捷推送——把一条通知发给订阅了该级别的骰主通知窗口（供事件层/定时任务调用）。
    void notifyMasters(int level, const std::string& msg, const std::string& op = "") {
        dice::notice::notify(cfg_, adapters_, level, msg, "", "", op);
    }

    /// .link：消息从 (platform,gid) 应转发到的目标群列表（active 链接；with=双向，
    /// to=home→target，from=target→home）。供 main.cpp 转发钩子调用。
    std::vector<std::string> linkAimsFor(const std::string& platform, const std::string& gid) const {
        std::vector<std::string> v;
        try {
            auto arr = cfg_.get<nlohmann::json>("dice/links", nlohmann::json::array());
            if (arr.is_array()) for (auto& l : arr) {
                if (!l.is_object() || !l.value("active", true)) continue;
                if (!l.value("platform", std::string()).empty() && l.value("platform", std::string()) != platform) continue;
                std::string home = l.value("home", std::string()), target = l.value("target", std::string());
                std::string mode = l.value("mode", std::string("with"));
                if (home == gid && (mode == "with" || mode == "to")) v.push_back(target);
                if (target == gid && (mode == "with" || mode == "from")) v.push_back(home);
            }
        } catch (...) {}
        return v;
    }

    /// .link 转发一条消息到目标群（带来源前缀由调用方拼好）。公开给 main.cpp。
    void linkForward(const Message& origin, const std::string& targetGid, const std::string& text) {
        pushMessage(origin, MessageType::kGroup, targetGid, text);
    }

    // ─── B：.notice —— 把当前聊天注册为「通知窗口」（接收骰主级通知）。仅 Master。────
    // .notice [on|off|status|level <n>]。level 位掩码：1 例行 / 2 重要 / 4 关键（可叠加，7=全部）。
    std::string handleNotice(Locale loc, const std::string& args, const Message& msg) {
        if (!isMaster(msg)) return i18n_.tr(loc, "gate.not_master");
        using J = nlohmann::json;
        std::string plat = msg.platform;
        bool isGroup = msg.type == MessageType::kGroup;
        std::string chatId = isGroup ? msg.targetId : msg.senderId;
        J nc = cfg_.get<J>("dice/notice", J::object());
        if (!nc.is_object()) nc = J::object();
        J wins = (nc.contains("windows") && nc["windows"].is_array()) ? nc["windows"] : J::array();
        int idx = -1;
        for (int i = 0; i < (int)wins.size(); ++i)
            if (wins[i].value("platform", std::string()) == plat && wins[i].value("chat_id", std::string()) == chatId) { idx = i; break; }
        std::istringstream iss(args); std::string act; iss >> act; act = toLower(act);
        if (act.empty() || act == "status") {
            if (idx < 0) return i18n_.tr(loc, "notice.off_status");
            return i18n_.tr(loc, "notice.status", {{"level", std::to_string(wins[idx].value("level_mask", (int)dice::notice::kAll))}});
        }
        if (act == "off" || act == "0") {
            if (idx >= 0) { wins.erase(wins.begin() + idx); nc["windows"] = wins; cfg_.set<J>("dice/notice", nc); cfg_.save(); }
            return i18n_.tr(loc, "notice.removed");
        }
        int mask = dice::notice::kAll;
        if (act == "level") { std::string n; iss >> n; if (isAllDigits(n)) mask = parseIntOr(n, dice::notice::kAll); else return i18n_.tr(loc, "notice.usage"); }
        else if (act == "on" || act == "1") { /* 默认全部级别 */ }
        else if (isAllDigits(act)) mask = parseIntOr(act, dice::notice::kAll);
        else return i18n_.tr(loc, "notice.usage");
        if (mask < 1) mask = 1; if (mask > (int)dice::notice::kAll) mask = dice::notice::kAll;
        // 指令按区域订阅 = 勾选该区域全部具体事件（细粒度存储，网页可再逐项调整）。
        J evs = J::array();
        for (auto& op : dice::notice::eventsForMask(mask)) evs.push_back(op);
        J w = {{"platform", plat}, {"chat_id", chatId}, {"is_group", isGroup}, {"level_mask", mask}, {"events", evs}};
        if (idx >= 0) { if (wins[idx].contains("name")) w["name"] = wins[idx]["name"]; wins[idx] = w; }
        else wins.push_back(w);
        nc["windows"] = wins; cfg_.set<J>("dice/notice", nc); cfg_.save();
        return i18n_.tr(loc, "notice.set", {{"level", std::to_string(mask)}});
    }

    // ─── C：.alias —— 账号别名管理（原版 TinyList 的可管理版）。仅 Master。─────
    // .alias add <别名ID> <主ID> / .alias del <别名ID> / .alias list
    // 别名账号的信任等级/Master 身份一律按主号计算（信任跟人不跟号）。
    std::string handleAlias(Locale loc, const std::string& args, const Message& msg) {
        if (!isMaster(msg)) return i18n_.tr(loc, "gate.not_master");
        using J = nlohmann::json;
        std::istringstream iss(args); std::string act; iss >> act; act = toLower(act);
        J arr = cfg_.get<J>("dice/aliases", J::array());
        if (!arr.is_array()) arr = J::array();
        if (act == "list") {
            std::string out;
            for (auto& a : arr) if (a.is_object())
                out += (out.empty() ? "" : "\n") + a.value("alias", std::string()) + " -> " + a.value("main", std::string());
            return i18n_.tr(loc, "alias.list", {{"list", out.empty() ? i18n_.tr(loc, "admin.none") : out}});
        }
        std::string a1, a2; iss >> a1 >> a2;
        if (act == "add") {
            if (a1.empty() || a2.empty() || a1 == a2) return i18n_.tr(loc, "alias.usage");
            J na = J::array();   // 同名别名覆盖
            for (auto& a : arr) if (a.is_object() && a.value("alias", std::string()) != a1) na.push_back(a);
            na.push_back(J{{"platform", msg.platform}, {"alias", a1}, {"main", a2}});
            cfg_.set<J>("dice/aliases", na); cfg_.save();
            dice::notice::notify(cfg_, adapters_, dice::notice::kRoutine,
                "\xe5\xb7\xb2\xe7\xbb\x91\xe5\xae\x9a\xe8\xb4\xa6\xe5\x8f\xb7\xe5\x88\xab\xe5\x90\x8d " + a1 + " -> " + a2,
                msg.platform, msg.type == MessageType::kGroup ? msg.targetId : msg.senderId, "alias");
            return i18n_.tr(loc, "alias.added", {{"alias", a1}, {"main", a2}});
        }
        if (act == "del") {
            if (a1.empty()) return i18n_.tr(loc, "alias.usage");
            J na = J::array(); bool found = false;
            for (auto& a : arr) { if (a.is_object() && a.value("alias", std::string()) == a1) { found = true; continue; } na.push_back(a); }
            if (!found) return i18n_.tr(loc, "alias.not_found");
            cfg_.set<J>("dice/aliases", na); cfg_.save();
            return i18n_.tr(loc, "alias.removed", {{"alias", a1}});
        }
        return i18n_.tr(loc, "alias.usage");
    }
private:

    // ─── COC7 house rule (.setcoc) — per group/user, key "cocRule" ──
    // Read directly (not via getGroupSetting, which skips private chats) so a
    // private-chat rule (keyed by the sender) also works.
    int getCocRule(const Message& msg) const {
        if (msg.targetId.empty()) return 0;
        auto* st = db_.getStorage(); if (!st) return 0;
        try {
            namespace orm = sqlite_orm;
            auto rows = st->get_all<GroupSettingRow>(orm::where(
                orm::c(&GroupSettingRow::platform) == msg.platform and
                orm::c(&GroupSettingRow::groupId) == msg.targetId and
                orm::c(&GroupSettingRow::key) == std::string("cocRule")));
            if (!rows.empty()) return parseIntOr(rows.front().value, 0);
        } catch (...) {}
        return 0;
    }
    void setCocRule(const Message& msg, int rule) {
        setGroupSettingFor(msg.platform, msg.targetId, "cocRule", std::to_string(rule));
    }
    void clearCocRule(const Message& msg) {
        auto* st = db_.getStorage(); if (!st) return;
        try {
            namespace orm = sqlite_orm;
            st->remove_all<GroupSettingRow>(orm::where(
                orm::c(&GroupSettingRow::platform) == msg.platform and
                orm::c(&GroupSettingRow::groupId) == msg.targetId and
                orm::c(&GroupSettingRow::key) == std::string("cocRule")));
        } catch (...) {}
    }

    /// Map a named COC house rule to its numeric id, or -1 if unknown.
    static int namedCocRule(const std::string& name) {
        if (name == "dg" || name == "deltagreen") return 6;  // Delta Green
        return -1;
    }

    std::optional<std::string> tryHandleSetcoc(Locale loc, const Message& msg, const std::string& cmd) {
        if (toLower(cmd).rfind("setcoc", 0) != 0) return std::nullopt;
        std::string rest = trim(cmd.substr(6));
        std::string lr = toLower(rest);
        if (rest.empty() || lr == "show") {
            int r = getCocRule(msg);
            return i18n_.tr(loc, "setcoc.show", {{"rule", std::to_string(r)}});
        }
        // C#48：改动房规需群管权限（原版 DiceEvent.cpp:1699 canRoomHost 门控 .setcoc）。
        if (!senderIsGroupAdmin(msg)) return i18n_.tr(loc, "gate.no_perm");
        if (lr == "clr" || lr == "clear") {
            clearCocRule(msg);
            return i18n_.tr(loc, "setcoc.cleared");
        }
        // Named rules (SealDice ".setcoc dg" → Delta Green: doubles 11/22…=大成功/
        // 大失败, mapped to house rule 6).
        if (int named = namedCocRule(lr); named >= 0) {
            setCocRule(msg, named);
            return i18n_.tr(loc, "setcoc.set", {{"rule", lr}});
        }
        if (isAllDigits(rest)) {
            int r = parseIntOr(rest, -1);
            if (r < 0 || r > 7) return i18n_.tr(loc, "setcoc.invalid");
            setCocRule(msg, r);
            return i18n_.tr(loc, "setcoc.set", {{"rule", std::to_string(r)}});
        }
        return i18n_.tr(loc, "setcoc.invalid");
    }

    /// Master black/white-list & boton/botoff — FAITHFUL to original Dice!:
    ///   blackqq / blackgroup / whitegroup / whiteqq  [-]<id> [id...]   (no id = list)
    ///   blackfriend                                                    (list)
    ///   boton/botoff <群号>
    /// Removal uses a leading '-' (e.g. ".blackqq -12345"); multiple ids allowed.
    std::optional<std::string> tryHandleMaster(Locale loc, const Message& msg, const std::string& cmd) {
        auto [word, args] = splitCommand(cmd);
        std::string w = toLower(word);
        auto isMasterCmd = [](const std::string& x) {
            static const char* cmds[] = {"boton","botoff","blackqq","blackgroup",
                "whitegroup","whiteqq","blackfriend"};
            for (auto* c : cmds) if (x == c) return true;
            return false;
        };
        if (!isMasterCmd(w)) return std::nullopt;
        std::string a = trim(args);

        // boton/botoff：仅「带群号」才是 master 远程开关；裸 .boton/.botoff（无群号）
        // 不属于 master 命令，返回 nullopt 让 tryHandleBot 当作本群 .bot on/off 处理。
        if (w == "boton" || w == "botoff") {
            std::string gid;
            for (char c : a) { if (std::isdigit((unsigned char)c)) gid += c; else if (!gid.empty()) break; }
            if (gid.empty()) return std::nullopt;   // 裸 botoff/boton → 交给 .bot（本群开关）
            if (!isMaster(msg)) return i18n_.tr(loc, "gate.not_master");
            setGroupSettingFor(msg.platform, gid, "enabled", w == "boton" ? "1" : "0");
            return i18n_.tr(loc, w == "boton" ? "master.boton" : "master.botoff", {{"group", gid}});
        }

        if (!isMaster(msg)) return i18n_.tr(loc, "gate.not_master");   // 其余 master 命令需权限（原版 strNotMaster）
        if (w == "blackfriend") return listEntries(loc, 0, 0, "master.list_blackqq");

        // black/white list: [-]<id> [id...] ; no id = list
        int type, list; const char* listKey;
        if (w == "blackqq")         { type = 0; list = 0; listKey = "master.list_blackqq"; }
        else if (w == "blackgroup") { type = 1; list = 0; listKey = "master.list_blackgroup"; }
        else if (w == "whitegroup") { type = 1; list = 1; listKey = "master.list_whitegroup"; }
        else /* whiteqq */          { type = 0; list = 1; listKey = "master.list_whiteqq"; }

        bool erase = false;
        if (!a.empty() && a[0] == '-') { erase = true; a = trim(a.substr(1)); }
        else if (!a.empty() && a[0] == '+') { a = trim(a.substr(1)); }

        std::vector<std::string> ids;
        { std::istringstream iss(a); std::string tk; while (iss >> tk) if (isAllDigits(tk)) ids.push_back(tk); }

        if (ids.empty()) return listEntries(loc, type, list, listKey);   // no id → list

        std::string joined;
        int n = 0;
        for (const auto& id : ids) {
            if (erase) { if (banlistRemove(type, list, id)) ++n; }
            else { banlistAdd(type, list, id, ""); ++n; }
            if (!joined.empty()) joined += "、";
            joined += id;
        }
        const char* okKey = erase ? "master.removed_n"
            : (list == 1 ? "master.white_added_n" : "master.black_added_n");
        return i18n_.tr(loc, okKey, {{"count", std::to_string(n)}, {"list", joined}});
    }

    /// List entries of one (type,list) category, formatted via @p titleKey {list}.
    std::string listEntries(Locale loc, int type, int list, const char* titleKey) {
        auto* st = db_.getStorage();
        std::string out;
        if (st) try {
            namespace orm = sqlite_orm;
            for (auto& r : st->get_all<BanlistRow>(orm::where(
                    orm::c(&BanlistRow::targetType) == type and orm::c(&BanlistRow::listType) == list))) {
                if (!out.empty()) out += "\n";          // 一行一条
                out += r.targetId + (r.reason.empty() ? "" : "(" + r.reason + ")");
            }
        } catch (...) {}
        return i18n_.tr(loc, titleKey, {{"list", out.empty() ? i18n_.tr(loc, "master.list_empty") : out}});
    }

    /// Push a message to an arbitrary target via the originating adapter (or any
    /// connected one as fallback). Used by .send and the web group-management API.
    /// Send text privately to the message sender (for 暗骰/暗检定 .rh/.rah).
    void sendPrivate(const Message& origin, const std::string& text) {
        auto a = adapters_.getAdapter(origin.adapterId);
        if (!a) for (auto& x : adapters_.allAdapters()) { if (x->isConnected()) { a = x; break; } }
        if (!a) return;
        Message m;
        m.platform = origin.platform.empty() ? a->platform() : origin.platform;
        m.type = MessageType::kPrivate;
        m.targetId = origin.senderId;
        m.content = text;
        a->sendMessage(m);
    }

    void pushMessage(const Message& origin, MessageType type,
                     const std::string& target, const std::string& text) {
        auto a = adapters_.getAdapter(origin.adapterId);
        if (!a) for (auto& x : adapters_.allAdapters()) { if (x->isConnected()) { a = x; break; } }
        if (!a) return;
        Message m;
        m.platform = origin.platform.empty() ? a->platform() : origin.platform;
        m.type = type;
        m.targetId = target;
        m.content = text;
        a->sendMessage(m);
    }

    std::string handleSend(Locale loc, const std::string& args, const Message& msg) {
        std::string body = trim(args);
        if (body.empty()) return i18n_.tr(loc, "send.empty");

        // Master push: ".send group <群号> <消息>" / ".send user <QQ> <消息>".
        if (isMaster(msg)) {
            auto [kw, rest] = splitCommand(body);
            std::string kwl = toLower(kw);
            if (kwl == "group" || kwl == "user") {
                auto [tgt, text] = splitCommand(rest);
                if (tgt.empty() || trim(text).empty())
                    return i18n_.tr(loc, "send.master_usage");
                pushMessage(msg, kwl == "group" ? MessageType::kGroup : MessageType::kPrivate,
                            tgt, trim(text));
                return i18n_.tr(loc, "send.master_done", {{"target", tgt}});
            }
        }

        // Otherwise: forward the message to every configured Master (private).
        auto ms = masters();
        if (ms.empty()) return i18n_.tr(loc, "send.no_master");
        std::string header = (msg.type == MessageType::kPrivate)
            ? i18n_.tr(loc, "send.from_private", {{"user", displayName(msg)}, {"uid", msg.senderId}})
            : i18n_.tr(loc, "send.from_group",
                  {{"group", msg.targetId}, {"user", displayName(msg)}, {"uid", msg.senderId}});
        std::string full = header + "\n" + body;
        for (const auto& m : ms) {
            // Deliver to each master via an adapter of their platform (fallback: origin/any).
            AdapterPtr a;
            if (!m.platform.empty())
                for (auto& x : adapters_.allAdapters())
                    if (x->isConnected() && x->platform() == m.platform) { a = x; break; }
            if (!a) a = adapters_.getAdapter(msg.adapterId);
            if (!a) for (auto& x : adapters_.allAdapters()) if (x->isConnected()) { a = x; break; }
            if (!a) continue;
            Message pm;
            pm.platform = a->platform();
            pm.type = MessageType::kPrivate;
            pm.targetId = m.id;
            pm.content = full;
            a->sendMessage(pm);
        }
        return i18n_.tr(loc, "send.done");
    }

    // ─── .group 群设定（聊天里只读，编辑交给网页后台）────────

    std::string handleGroup(Locale loc, const std::string& args, const Message& msg) {
        if (msg.type == MessageType::kPrivate) return i18n_.tr(loc, "group.private");

        // .group clr / .group +/-词条。门控对齐原版 getGroupTrust：群管(0) 或个人信任>0
        // 均可管理本群（原版 DiceEvent.cpp:1980 getGroupTrust<0 → strGroupDenied）。
        std::string at = trim(args);
        if (!at.empty()) {
            if (groupTrustOf(msg) < 0) return i18n_.tr(loc, "gate.no_perm");
            std::string atl = toLower(at);
            // C#58：群自动化 .group auto pass/kick/mute ……
            if (atl == "auto" || atl.rfind("auto ", 0) == 0)
                return handleGroupAuto(loc, msg, trim(at.substr(4)));
            if (atl == "clr" || atl == "clear") {
                clearGroupSettings(msg);
                return i18n_.tr(loc, "group.cleared");
            }
            if (at[0] == '+' || at[0] == '-')
                return handleGroupTerm(loc, msg, trim(at.substr(1)), at[0] == '+');
            // 其它参数：忽略，继续展示群信息。
        }

        auto a = adapters_.getAdapter(msg.adapterId);

        bool enabled = !isGroupDisabled(msg);
        int logId = activeLogId(msg);
        std::string logName = getGroupSetting(msg, "activeLogName");
        if (logName.empty() && logId > 0) logName = "log" + std::to_string(logId);
        auto obs = getObservers(msg);

        // Group name + bot role + member count from the adapter cache.
        std::string name = a ? a->getGroupName(msg.targetId) : msg.targetId;
        std::string roleRaw = a ? a->getSelfRole(msg.targetId) : "";
        std::string role = i18n_.tr(loc,
            roleRaw == "owner" ? "group.role_owner" :
            roleRaw == "admin" ? "group.role_admin" :
            roleRaw == "member" ? "group.role_member" : "group.role_unknown");
        int memberCount = a ? a->getGroupMemberCount(msg.targetId) : 0;

        // GM = the active log's creator, if a log is running.
        std::string gm = i18n_.tr(loc, "group.none");
        if (logId > 0) {
            if (auto* st = db_.getLogStorage()) {   // game_logs live in logs.db
                try { auto r = st->get<GameLogRow>(logId); if (!r.gmId.empty()) gm = r.gmId; } catch (...) {}
            }
        }

        // Blacklisted members present (best-effort: needs the member-list cache).
        int blk = 0;
        if (a) {
            json members = a->getMembers(msg.targetId);
            if (members.is_array())
                for (auto& m : members) {
                    std::string uid;
                    if (m.contains("user_id")) {
                        if (m["user_id"].is_string()) uid = m["user_id"].get<std::string>();
                        else if (m["user_id"].is_number()) uid = std::to_string(m["user_id"].get<int64_t>());
                    }
                    if (!uid.empty() && banlistHas(0, 0, uid)) ++blk;
                }
        }

        std::string lvl = blacklistQuitLevel(msg.platform, msg.targetId);
        std::string lvlLabel = i18n_.tr(loc, lvl == "admin" ? "group.quit_admin" : "group.quit_member");

        return i18n_.tr(loc, "group.info", {
            {"name", name},
            {"group", msg.targetId},
            {"role", role},
            {"status", i18n_.tr(loc, enabled ? "group.enabled" : "group.disabled")},
            {"log", logId > 0 ? logName : i18n_.tr(loc, "group.none")},
            {"players", std::to_string(memberCount)},
            {"gm", gm},
            {"observers", std::to_string(obs.size())},
            {"blacklist", std::to_string(blk)},
            {"quitLevel", lvlLabel}
        });
    }

    // ─── C#58 群自动化：.group auto pass/kick/mute ─────────────
    // pass=加群自动审核（验证消息含关键字/all 全过），kick/mute=按消息关键字踢人/禁言。
    // 需群管配置（handleGroup 已门控）+ 骰子在群内有管理权限才实际生效。
    std::string handleGroupAuto(Locale loc, const Message& msg, const std::string& rest) {
        std::string r = trim(rest), rl = toLower(r);
        // .group auto / .group auto show → 展示当前设置
        if (r.empty() || rl == "show") {
            auto shown = [&](const std::string& v) {
                return v.empty() ? i18n_.tr(loc, "group.auto.off")
                     : (toLower(v) == "all" ? i18n_.tr(loc, "group.auto.all") : v);
            };
            std::string mm = getGroupSetting(msg, "autoMuteMin"); if (mm.empty()) mm = "10";
            return i18n_.tr(loc, "group.auto.show", {
                {"pass", shown(getGroupSetting(msg, "autoPass"))},
                {"kick", shown(getGroupSetting(msg, "autoKick"))},
                {"mute", shown(getGroupSetting(msg, "autoMute"))},
                {"min", mm}});
        }
        // 骰子须在群内有管理权限（否则踢人/禁言/审批都无效）。仅当**明确是普通成员**时
        // 拦下并提示；admin/owner 或角色未缓存("")时放行（未缓存也允许配置，避免冷启动
        // 时真管理员被误挡；若实际无权，踢/禁在 NapCat 端会无害失败）。
        auto a = adapters_.getAdapter(msg.adapterId);
        std::string botRole = a ? a->getSelfRole(msg.targetId) : "";
        if (botRole == "member")
            return i18n_.tr(loc, "group.auto.need_bot_admin");

        size_t sp = r.find(' ');
        std::string sub = toLower(sp == std::string::npos ? r : r.substr(0, sp));
        std::string kw = sp == std::string::npos ? "" : trim(r.substr(sp + 1));

        if (sub == "pass") {
            if (kw.empty()) { setGroupSetting(msg, "autoPass", ""); return i18n_.tr(loc, "group.auto.pass_off"); }
            bool all = (toLower(kw) == "all");
            setGroupSetting(msg, "autoPass", all ? "all" : kw);
            return i18n_.tr(loc, all ? "group.auto.pass_all" : "group.auto.pass_on", {{"kw", kw}});
        }
        if (sub == "kick") {
            if (kw.empty()) { setGroupSetting(msg, "autoKick", ""); return i18n_.tr(loc, "group.auto.kick_off"); }
            setGroupSetting(msg, "autoKick", kw);
            return i18n_.tr(loc, "group.auto.kick_on", {{"kw", kw}});
        }
        if (sub == "mute") {
            if (kw.empty()) { setGroupSetting(msg, "autoMute", ""); return i18n_.tr(loc, "group.auto.mute_off"); }
            // 可选尾部分钟数：.group auto mute 关键字 30
            int minutes = 10;
            size_t sp2 = kw.find_last_of(' ');
            if (sp2 != std::string::npos) {
                std::string tail = trim(kw.substr(sp2 + 1));
                if (!tail.empty() && std::all_of(tail.begin(), tail.end(), [](unsigned char c){ return std::isdigit(c); })) {
                    minutes = atoi(tail.c_str()); if (minutes < 1) minutes = 1;
                    kw = trim(kw.substr(0, sp2));
                }
            }
            if (kw.empty()) return i18n_.tr(loc, "group.auto.usage");
            setGroupSetting(msg, "autoMute", kw);
            setGroupSetting(msg, "autoMuteMin", std::to_string(minutes));
            return i18n_.tr(loc, "group.auto.mute_on", {{"kw", kw}, {"min", std::to_string(minutes)}});
        }
        return i18n_.tr(loc, "group.auto.usage");
    }

public:
    /// C#58：一条群消息命中「自动踢出/禁言」关键字时执行动作，返回 "kick"/"mute" 或空。
    /// 豁免骰主/群管/信任/邀请人/骰娘自身；骰子须有管理权限。由 main.cpp 消息循环调用。
    std::string applyGroupAutoModeration(const Message& msg) {
        if (msg.type != MessageType::kGroup || msg.targetId.empty() || msg.senderId.empty()) return "";
        if (!msg.selfId.empty() && msg.senderId == msg.selfId) return "";
        if (isMaster(msg) || senderIsGroupAdmin(msg)) return "";        // 豁免管理/骰主/信任/邀请人/自控
        auto a = adapters_.getAdapter(msg.adapterId);
        if (!a) return "";
        std::string botRole = a->getSelfRole(msg.targetId);
        if (botRole == "member") return "";                             // 明确是普通成员 → 无管理权，不执行
        std::string text = !msg.rawContent.empty() ? msg.rawContent
                         : (!msg.displayContent.empty() ? msg.displayContent : msg.content);
        std::string kick = groupSettingValue(msg.platform, msg.targetId, "autoKick");
        if (!kick.empty() && text.find(kick) != std::string::npos) {
            a->setGroupKick(msg.targetId, msg.senderId);
            return "kick";
        }
        std::string mute = groupSettingValue(msg.platform, msg.targetId, "autoMute");
        if (!mute.empty() && text.find(mute) != std::string::npos) {
            int minutes = parseIntOr(groupSettingValue(msg.platform, msg.targetId, "autoMuteMin"), 10);
            if (minutes < 1) minutes = 10;
            a->setGroupBan(msg.targetId, msg.senderId, minutes * 60);
            return "mute";
        }
        return "";
    }
private:

    // ─── .ob 旁观（观战名单，存 group_settings）──────────────

    std::vector<std::string> getObservers(const Message& msg) const {
        std::string v = getGroupSetting(msg, "observers");
        std::vector<std::string> ids;
        if (v.empty()) return ids;
        try {
            json j = json::parse(v);
            if (j.is_array()) for (auto& e : j) if (e.is_string()) ids.push_back(e.get<std::string>());
        } catch (...) {}
        return ids;
    }
    void setObservers(const Message& msg, const std::vector<std::string>& ids) {
        json j = json::array();
        for (auto& s : ids) j.push_back(s);
        setGroupSetting(msg, "observers", j.dump());
    }

    std::string handleObserve(Locale loc, const std::string& args, const Message& msg) {
        if (msg.type == MessageType::kPrivate) return i18n_.tr(loc, "ob.group_only");
        std::string sub = toLower(trim(args));
        const std::string nick = displayName(msg);
        auto ids = getObservers(msg);
        auto it = std::find(ids.begin(), ids.end(), msg.senderId);
        bool present = (it != ids.end());
        const bool obOff = getGroupSetting(msg, "obOff") == "1";

        // 原版对齐（DiceEvent.cpp:3286）：on/off 是「旁观功能开关」（需群管），
        // exit 才是退出旁观；clr 需群管/GM。
        if (sub == "off") {
            if (!senderIsGroupAdmin(msg)) return i18n_.tr(loc, "gate.no_perm");
            if (obOff) return i18n_.tr(loc, "ob.off_already");
            setGroupSetting(msg, "obOff", "1");
            return i18n_.tr(loc, "ob.off");
        }
        if (sub == "on") {
            if (!senderIsGroupAdmin(msg)) return i18n_.tr(loc, "gate.no_perm");
            if (!obOff) return i18n_.tr(loc, "ob.on_already");
            setGroupSetting(msg, "obOff", "0");
            return i18n_.tr(loc, "ob.on");
        }
        if (sub == "list") {
            if (ids.empty()) return i18n_.tr(loc, "ob.empty");
            std::string list;
            for (auto& s : ids) { if (!list.empty()) list += "\n"; list += s; }   // 一行一条
            return i18n_.tr(loc, "ob.list", {{"count", std::to_string(ids.size())}, {"list", list}});
        }
        if (sub == "clr" || sub == "clear") {
            if (!senderIsGroupAdmin(msg)) return i18n_.tr(loc, "gate.no_perm");
            setObservers(msg, {});
            return i18n_.tr(loc, "ob.cleared", {{"nick", nick}});
        }
        if (sub == "exit") {
            if (!present) return i18n_.tr(loc, "ob.not_in", {{"nick", nick}});
            ids.erase(it); setObservers(msg, ids);
            return i18n_.tr(loc, "ob.exit", {{"nick", nick}});
        }
        // default / "join": join as observer（旁观功能被关时拒绝）
        if (obOff) return i18n_.tr(loc, "ob.disabled");
        if (present) return i18n_.tr(loc, "ob.already", {{"nick", nick}});
        ids.push_back(msg.senderId); setObservers(msg, ids);
        return i18n_.tr(loc, "ob.joined", {{"nick", nick}});
    }

    // ─── .link 跨窗口消息链接（原版 DiceSession linker，DiceEvent.cpp:2521）────
    // .link with/to/from <群号>（with=双向 to=本群→目标 from=目标→本群）/ start / close /
    // list / state。需信任≥3。链接存 config dice/links；转发由 main.cpp 调 linkAimsFor。
    std::string handleLink(Locale loc, const std::string& args, const Message& msg) {
        if (trustOf(msg) < 3) return i18n_.tr(loc, "gate.no_perm");
        using J = nlohmann::json;
        std::istringstream iss(args); std::string act, target; iss >> act >> target; act = toLower(act);
        J arr = cfg_.get<J>("dice/links", J::array());
        if (!arr.is_array()) arr = J::array();
        auto save = [&]() { cfg_.set<J>("dice/links", arr); cfg_.save(); };
        auto modeText = [&](const std::string& m) {
            return i18n_.tr(loc, m == "with" ? "link.mode_with" : m == "to" ? "link.mode_to" : "link.mode_from");
        };
        if (act == "list") {
            std::string out;
            for (auto& l : arr) if (l.is_object())
                out += (out.empty() ? "" : "\n") + l.value("home", std::string()) + " " + modeText(l.value("mode", std::string("with")))
                     + " " + l.value("target", std::string()) + (l.value("active", true) ? "" : i18n_.tr(loc, "link.suspended"));
            return i18n_.tr(loc, "link.list", {{"list", out.empty() ? i18n_.tr(loc, "link.none") : out}});
        }
        if (msg.type != MessageType::kGroup || msg.targetId.empty()) return i18n_.tr(loc, "link.group_only");
        const std::string home = msg.targetId;
        if (act == "state") {
            std::string out;
            for (auto& l : arr) if (l.is_object() && (l.value("home", std::string()) == home || l.value("target", std::string()) == home))
                out += (out.empty() ? "" : "\n") + l.value("home", std::string()) + " " + modeText(l.value("mode", std::string("with")))
                     + " " + l.value("target", std::string()) + (l.value("active", true) ? "" : i18n_.tr(loc, "link.suspended"));
            return out.empty() ? i18n_.tr(loc, "link.state_none") : i18n_.tr(loc, "link.state", {{"list", out}});
        }
        if (act == "close" || act == "start") {
            bool on = (act == "start"); int n = 0;
            for (auto& l : arr) if (l.is_object() && (l.value("home", std::string()) == home || l.value("target", std::string()) == home)) {
                l["active"] = on; ++n;
            }
            if (n == 0) return i18n_.tr(loc, "link.state_none");
            save();
            return i18n_.tr(loc, on ? "link.started" : "link.closed");
        }
        if (act == "with" || act == "to" || act == "from") {
            if (target.empty() || !isAllDigits(target)) return i18n_.tr(loc, "link.usage");
            if (target == home) return i18n_.tr(loc, "link.self");
            J na = J::array();   // 同 home+target 组合覆盖旧配置
            for (auto& l : arr) {
                if (l.is_object() && ((l.value("home", std::string()) == home && l.value("target", std::string()) == target)
                    || (l.value("home", std::string()) == target && l.value("target", std::string()) == home))) continue;
                na.push_back(l);
            }
            na.push_back(J{{"platform", msg.platform}, {"home", home}, {"target", target}, {"mode", act}, {"active", true}});
            arr = na; save();
            return i18n_.tr(loc, "link.built", {{"home", home}, {"mode", modeText(act)}, {"target", target}});
        }
        return i18n_.tr(loc, "link.usage");
    }

    // ─── .me 第三人称动作（原版 DiceEvent.cpp:3184）──────────────────────
    // 群聊：.me <动作> → 以「人物卡名/昵称+动作」代述（Master 直出动作不带名）；
    // .me on/off 本群开关（群管，同 .group ±禁用me）。私聊：.me <群号> <动作> 远程代述。
    std::optional<std::string> tryHandleMe(Locale loc, const Message& msg, const std::string& cmd) {
        if (toLower(cmd).rfind("me", 0) != 0) return std::nullopt;
        if (cmd.size() > 2) { char c = cmd[2]; if (std::isalnum((unsigned char)c)) {
            // on/off 子指令要先于禁用 gate 处理（原版顺序），其余 me* 字母开头的不是本指令。
            std::string low2 = toLower(trim(cmd.substr(2)));
            if (low2 != "on" && low2 != "off") return std::nullopt;
        } }
        std::string rest = trim(cmd.substr(2));
        const bool master = isMaster(msg);
        // 原版顺序：先处理 on/off 开关（否则关了就再也开不回来），再走禁用 gate。
        if (msg.type == MessageType::kGroup) {
            std::string low = toLower(rest);
            if (low == "on" || low == "off") {
                if (!senderIsGroupAdmin(msg)) return i18n_.tr(loc, "gate.no_perm");
                bool disable = (low == "off");
                if (groupCmdDisabled(msg, "me") == disable)
                    return i18n_.tr(loc, disable ? "me.off_already" : "me.on_already");
                setGroupCmdDisabled(msg, "me", disable);
                return i18n_.tr(loc, disable ? "me.off" : "me.on");
            }
        }
        if (auto g = gateCommand(loc, msg, "me")) return *g;   // 全局/本群禁用 gate
        if (msg.type == MessageType::kPrivate) {
            // 远程：.me <群号> <动作>
            std::istringstream iss(rest); std::string gid; iss >> gid;
            std::string action = trim(rest.substr((std::min)(rest.size(), rest.find(gid) + gid.size())));
            if (gid.empty() || !isAllDigits(gid)) return i18n_.tr(loc, "me.private_usage");
            if (action.empty()) return i18n_.tr(loc, "me.empty");
            Message gm = msg; gm.type = MessageType::kGroup; gm.targetId = gid;
            if (groupCmdDisabled(gm, "me") && !master) return i18n_.tr(loc, "me.disabled");
            pushMessage(msg, MessageType::kGroup, gid, master ? action : (displayName(msg) + action));
            return i18n_.tr(loc, "me.sent");
        }
        if (rest.empty()) return i18n_.tr(loc, "me.empty");
        // Master 直出动作；普通用户带人物卡名/昵称前缀（原版 idx_pc + strAction）。
        return master ? rest : (displayName(msg) + rest);
    }

    // ─── .ak 抉择分歧（原版 DiceEvent.cpp:2996 __Ank 列表）────────────────
    // .ak#标题 / .ak new 标题 新建；.ak+选项1|选项2 添加；.ak-序号 删除；
    // .ak= / .ak get 随机抽取并清空；.ak show 列出；.ak clr 清空。按窗口保存。
    std::optional<std::string> tryHandleAk(Locale loc, const Message& msg, const std::string& cmd) {
        std::string low = toLower(cmd);
        if (low.rfind("ak", 0) != 0) return std::nullopt;
        if (cmd.size() > 2) { char c = cmd[2]; if (std::isalnum((unsigned char)c)) return std::nullopt; }
        using J = nlohmann::json;
        const std::string gid = (msg.type == MessageType::kGroup && !msg.targetId.empty())
            ? msg.targetId : ("pm_" + msg.senderId);
        auto loadAk = [&]() {
            J j = J::object();
            try { std::string s = getGroupSettingFor(msg.platform, gid, "akData"); if (!s.empty()) j = J::parse(s); } catch (...) {}
            if (!j.is_object()) j = J::object();
            if (!j.contains("items") || !j["items"].is_array()) j["items"] = J::array();
            return j;
        };
        auto saveAk = [&](const J& j) { setGroupSettingFor(msg.platform, gid, "akData", j.dump()); };
        auto listText = [&](const J& j) {
            std::string out; int i = 1;
            for (auto& e : j["items"]) if (e.is_string()) { out += (out.empty() ? "" : "\n") + std::to_string(i++) + ". " + e.get<std::string>(); }
            return out;
        };
        auto titleOf = [&](const J& j) { std::string tt = j.value("title", std::string()); return tt.empty() ? i18n_.tr(loc, "ak.default_title") : tt; };
        std::string rest = trim(cmd.substr(2));
        if (rest.empty()) return i18n_.tr(loc, "ak.usage");
        char sign = rest[0];
        std::string act; { std::istringstream iss(rest); iss >> act; act = toLower(act); }

        if (sign == '#' || act == "new") {
            std::string title = trim(sign == '#' ? rest.substr(1) : trim(rest.substr(3)));
            // 标题后可直接跟 +选项（.ak#标题+选项）
            std::string inline_add;
            if (auto p = title.find('+'); p != std::string::npos) { inline_add = title.substr(p + 1); title = trim(title.substr(0, p)); }
            J j = J::object(); j["title"] = title; j["items"] = J::array();
            if (!inline_add.empty()) {
                size_t q = 0;
                while (q <= inline_add.size()) {
                    size_t c = inline_add.find('|', q);
                    std::string it = trim(inline_add.substr(q, c == std::string::npos ? std::string::npos : c - q));
                    if (!it.empty()) j["items"].push_back(it);
                    if (c == std::string::npos) break; q = c + 1;
                }
            }
            saveAk(j);
            if (j["items"].empty()) return i18n_.tr(loc, "ak.created", {{"title", titleOf(j)}});
            return i18n_.tr(loc, "ak.added", {{"title", titleOf(j)}, {"list", listText(j)}});
        }
        if (sign == '+' || act == "add") {
            std::string items = trim(sign == '+' ? rest.substr(1) : trim(rest.substr(3)));
            if (items.empty()) return i18n_.tr(loc, "ak.add_empty");
            J j = loadAk();
            size_t q = 0;
            while (q <= items.size()) {
                size_t c = items.find('|', q);
                std::string it = trim(items.substr(q, c == std::string::npos ? std::string::npos : c - q));
                if (!it.empty()) j["items"].push_back(it);
                if (c == std::string::npos) break; q = c + 1;
            }
            saveAk(j);
            return i18n_.tr(loc, "ak.added", {{"title", titleOf(j)}, {"list", listText(j)}});
        }
        if (sign == '-' || act == "del") {
            std::string ns = trim(sign == '-' ? rest.substr(1) : trim(rest.substr(3)));
            J j = loadAk();
            int no = parseIntOr(ns, 0);
            if (no <= 0 || no > (int)j["items"].size()) return i18n_.tr(loc, "ak.num_err");
            j["items"].erase(j["items"].begin() + (no - 1));
            saveAk(j);
            return i18n_.tr(loc, "ak.deleted", {{"title", titleOf(j)}, {"list", listText(j).empty() ? i18n_.tr(loc, "ak.empty_list") : listText(j)}});
        }
        if (sign == '=' || act == "get") {
            J j = loadAk();
            if (j["items"].empty()) return i18n_.tr(loc, "ak.opt_empty");
            int n = (int)j["items"].size();
            int pick = engine_.roll("1d" + std::to_string(n)).modifiedTotal;
            std::string got = std::to_string(pick) + ". " + j["items"][pick - 1].get<std::string>();
            std::string all = listText(j);
            setGroupSettingFor(msg.platform, gid, "akData", "");   // 抽取后清空（原版行为）
            return i18n_.tr(loc, "ak.got", {{"title", titleOf(j)}, {"list", all}, {"get", got}});
        }
        if (act == "show") {
            J j = loadAk();
            return i18n_.tr(loc, "ak.show", {{"title", titleOf(j)}, {"list", listText(j).empty() ? i18n_.tr(loc, "ak.empty_list") : listText(j)}});
        }
        if (act == "clr" || act == "clear") {
            setGroupSettingFor(msg.platform, gid, "akData", "");
            return i18n_.tr(loc, "ak.cleared");
        }
        return i18n_.tr(loc, "ak.usage");
    }

    // ─── .nnn 随机改名（原版 DiceEvent.cpp:2935：抽随机姓名并设为昵称）──────
    // .nnn [cn|en|jp] —— 随机起名（复用 .name 生成器）并直接设为本群昵称。
    // 未指定类型时随机国家（原版「随机姓名」混合牌堆语义）。
    std::optional<std::string> tryHandleNNN(Locale loc, const Message& msg, const std::string& cmd) {
        std::string low = toLower(cmd);
        if (low.rfind("nnn", 0) != 0) return std::nullopt;
        std::string type = toLower(trim(cmd.substr(3)));
        if (type == "zh") type = "cn";
        if (type != "cn" && type != "en" && type != "jp") {
            static const char* kTypes[] = {"cn", "en", "jp"};
            type = kTypes[engine_.roll("1d3").modifiedTotal - 1];
        }
        std::string oldName = displayNameRaw(msg);
        std::string name = genName(type);
        std::string bound = cards_.boundCard(msg.senderId, cardScope(msg));
        if (!bound.empty() && bound != name) cards_.renameCard(msg.senderId, bound, name);   // 与 .nn 一致：改绑定卡名
        setUserSetting(msg, "nick", name);
        return i18n_.tr(loc, "fun.nn.set", {{"old", oldName}, {"new", name}});
    }

    std::string handleDismiss(Locale loc, const std::string&, const Message& msg) {
        if (msg.type == MessageType::kPrivate) return i18n_.tr(loc, "dismiss.private");
        // C#48：原版 .dismiss 仅 群管理/群主/邀请人/信任>2 可用（DiceEvent.cpp:955-964）。
        if (!senderIsGroupAdmin(msg)) return i18n_.tr(loc, "gate.no_perm");
        // C#62：指令退群 —— 先回退群宣言（本函数返回值），从此刻起本群禁响应一切指令
        //（isBlocked 检查 "leaving"），随机 10~60 秒后真正退群。不删除群记录，退群后
        // 标记 "left"（群组管理显示「已退群」）。
        setGroupSetting(msg, "leaving", "1");
        static thread_local std::mt19937 rng{std::random_device{}()};
        int delay = std::uniform_int_distribution<int>(10, 60)(rng);
        const std::string plat = msg.platform, gid = msg.targetId, aid = msg.adapterId;
        DICE_LOG_INFO("C#62 .dismiss：群 {} 已发退群宣言，{} 秒后退群（期间静默）", gid, delay);
        drogon::app().getLoop()->runAfter(static_cast<double>(delay), [this, plat, gid, aid]() {
            auto a = adapters_.getAdapter(aid);
            if (!a || !a->isConnected())
                for (auto& x : adapters_.allAdapters())
                    if (x->isConnected() && x->platform() == plat) { a = x; break; }
            if (a) a->leaveGroup(gid);
            setGroupSettingFor(plat, gid, "leaving", "0");
            setGroupSettingFor(plat, gid, "left", "1");
            DICE_LOG_INFO("C#62 .dismiss：已退出群 {}（记录保留，状态=已退群）", gid);
            // B：指令退群通知骰主。
            dice::notice::notify(cfg_, adapters_, dice::notice::kImportant,
                "\xe6\x8c\x87\xe4\xbb\xa4\xe9\x80\x80\xe7\xbe\xa4\xef\xbc\x9a\xe5\xb7\xb2\xe9\x80\x80\xe5\x87\xba\xe7\xbe\xa4 " + gid,
                plat, gid, "dismiss");
        });
        return i18n_.tr(loc, "dismiss.leaving");
    }

    // ─── .ri / .init 先攻（DND/COC，三系都有）────────────────
    json loadInit(const Message& msg) const {
        std::string s = getGroupSetting(msg, "init");
        if (!s.empty()) try { auto j = json::parse(s); if (j.is_array()) return j; } catch (...) {}
        return json::array();
    }
    void saveInit(const Message& msg, const json& list) { setGroupSetting(msg, "init", list.dump()); }

    std::string handleRi(Locale loc, const std::string& args, const Message& msg) {
        if (msg.type == MessageType::kPrivate) return i18n_.tr(loc, "init.group_only");
        std::string a = trim(args);
        int adj = 0;
        // optional leading adjust token: +N / -N / N
        auto [t1, rest] = splitCommand(a);
        if (!t1.empty()) {
            std::string core = (t1[0] == '+' || t1[0] == '-') ? t1.substr(1) : t1;
            if (!core.empty() && isAllDigits(core)) {
                adj = (t1[0] == '-') ? -parseIntOr(core, 0) : parseIntOr(core, 0);
                a = trim(rest);
            }
        }
        std::string name = trim(a);

        // D#06：多轮先攻 `.ri [轮数]#怪物名`——为同种怪物自动编号（哥布林1/2/3），
        // 每个各掷一次独立先攻（原版 DiceEvent.cpp:3810 table_add 循环，轮数≤10）。
        if (size_t hp = name.find('#'); hp != std::string::npos) {
            std::string cntTok = trim(name.substr(0, hp));
            std::string base = trim(name.substr(hp + 1));
            if (base.empty()) base = displayNameRaw(msg);
            int cnt = 1;
            if (!cntTok.empty()) {
                // 轮数可为骰点表达式（如 1d4），也可为定数；对齐原版 RD rdTurnCnt。
                if (isAllDigits(cntTok)) cnt = parseIntOr(cntTok, 1);
                else { auto rc = engine_.roll(cntTok); if (rc.ok()) cnt = rc.modifiedTotal; }
            }
            if (cnt <= 0) return i18n_.tr(loc, "init.count_err");
            if (cnt > 10) return i18n_.tr(loc, "init.count_exceeded", {{"max", "10"}});
            json list = loadInit(msg);
            std::string detail;
            for (int no = 1; no <= cnt; ++no) {
                int r = engine_.roll("1d20").modifiedTotal;
                int tot = r + adj;
                std::string ename = base + std::to_string(no);
                bool found = false;
                for (auto& e : list) if (e.value("name", "") == ename) { e["val"] = tot; found = true; break; }
                if (!found) list.push_back({{"name", ename}, {"val", tot}});
                std::string one = "1D20=" + std::to_string(r)
                    + (adj > 0 ? "+" + std::to_string(adj) : adj < 0 ? std::to_string(adj) : "")
                    + (adj != 0 ? "=" + std::to_string(tot) : "");
                detail += "\n" + std::to_string(no) + ". " + ename + " " + one;
            }
            saveInit(msg, list);
            return i18n_.tr(loc, "init.rolled_multi", {{"base", base}, {"count", std::to_string(cnt)}, {"detail", detail}});
        }

        if (name.empty()) name = displayNameRaw(msg);   // 先攻名：存储/比较用原始名
        int roll = engine_.roll("1d20").modifiedTotal;
        int total = roll + adj;
        json list = loadInit(msg);
        bool found = false;
        for (auto& e : list) if (e.value("name", "") == name) { e["val"] = total; found = true; break; }
        if (!found) list.push_back({{"name", name}, {"val", total}});
        saveInit(msg, list);
        std::string detail = "1D20=" + std::to_string(roll) + (adj > 0 ? "+" + std::to_string(adj) : adj < 0 ? std::to_string(adj) : "");
        return i18n_.tr(loc, "init.rolled", {{"name", name}, {"roll", detail}, {"total", std::to_string(total)}});
    }
    std::string handleInit(Locale loc, const std::string& args, const Message& msg) {
        if (msg.type == MessageType::kPrivate) return i18n_.tr(loc, "init.group_only");
        std::string sub = toLower(trim(args));
        if (sub == "clr" || sub == "clear") { setGroupSetting(msg, "init", ""); return i18n_.tr(loc, "init.cleared"); }
        if (sub.rfind("del", 0) == 0) {
            std::string nm = trim(args.substr(3));
            json list = loadInit(msg), nl = json::array();
            for (auto& e : list) if (e.value("name", "") != nm) nl.push_back(e);
            saveInit(msg, nl);
            return i18n_.tr(loc, "init.removed", {{"name", nm}});
        }
        json list = loadInit(msg);
        if (list.empty()) return i18n_.tr(loc, "init.empty");
        std::vector<std::pair<std::string, int>> v;
        for (auto& e : list) v.push_back({e.value("name", ""), e.value("val", 0)});
        std::sort(v.begin(), v.end(), [](auto& x, auto& y) { return x.second > y.second; });
        std::string out; int rank = 1;
        for (auto& [n, val] : v) { if (!out.empty()) out += "\n"; out += std::to_string(rank++) + ". " + n + ": " + std::to_string(val); }
        return i18n_.tr(loc, "init.list", {{"list", out}});
    }

    // ─── .sn 跑团名片（按模板设自己的群名片，青果/海豹）──────
    /// C#48：群设置变更权限，对齐原版 `DiceEvent::canRoomHost()`（DiceEvent.cpp:4686）：
    ///   trusted>3 ‖ 私聊 ‖ 群管理/群主（OneBot sender.role）‖ 群邀请人（C#47 记录）‖ 骰主。
    /// 原版用它门控 .bot on/off、.dismiss、.setcoc、.me on/off、.game new 等开关类指令。
    bool senderIsGroupAdmin(const Message& msg) const {
        if (msg.fromSelf) return true;                               // C#69 自控：用骰娘账号自身发指令视同群管理（操控者可信）
        if (isMaster(msg) || senderTrust(msg) >= 4) return true;
        if (msg.type == MessageType::kPrivate) return true;          // 原版：私聊恒真
        std::string role;
        try { role = msg.extra.value("role", std::string()); } catch (...) {}
        if (role == "owner" || role == "admin") return true;
        // 群邀请人视同管理（原版 pGrp->inviter == fromChat.uid）
        if (msg.type == MessageType::kGroup && !msg.targetId.empty() &&
            groupSettingValue(msg.platform, msg.targetId, "inviter") == msg.senderId) return true;
        return false;
    }

    // ─── C#107：.game 团务（复刻原版 DiceEvent.cpp:1396-1625 + DiceSession）────
    // 团数据与 Lua 的 msg.game/GameTable 同源，保存在 lua_mod.db conf。
    // 新会话采用 game:<群号> → __session → game:session:<id> 的两层映射，
    // 以复刻原版 DiceSessionManager「多个聊天窗口映射到同一 Session」的行为；
    // 未迁移的旧 game:<群号> 数据仍按单群会话读取。
public:
    struct GameConfBridge {
        std::function<std::string(const std::string& scope, const std::string& key)> get;
        std::function<void(const std::string& scope, const std::string& key, const std::string& val)> set;
        std::function<std::vector<std::pair<std::string, std::string>>(const std::string& scope)> all;
    };
    void setGameConf(GameConfBridge b) { gameConf_ = std::move(b); }
private:
    GameConfBridge gameConf_;

    std::string gameGroupScope(const Message& msg) const { return "game:" + msg.targetId; }
    static std::string gameSessionScope(const std::string& id) { return "game:session:" + id; }
    static constexpr const char* gameIndexScope() { return "game:index"; }
    static std::mutex& gameSessionMutex() { static std::mutex m; return m; }
    std::string gameSessionId(const Message& msg) const {
        return gameConf_.get ? gameConf_.get(gameGroupScope(msg), "__session") : std::string();
    }
    std::string gameScope(const Message& msg) const {
        std::string id = gameSessionId(msg);
        return id.empty() ? gameGroupScope(msg) : gameSessionScope(id);
    }
    std::string gGet(const Message& msg, const std::string& k) const {
        return gameConf_.get ? gameConf_.get(gameScope(msg), k) : std::string();
    }
    void gSet(const Message& msg, const std::string& k, const std::string& v) {
        if (gameConf_.set) gameConf_.set(gameScope(msg), k, v);
    }
    // 团是否存在且未暂关（掷骰轮盘、msg.game 语义与此一致）。
    bool gameActive(const Message& msg) const {
        return msg.type == MessageType::kGroup && !gGet(msg, "__name").empty() && gGet(msg, "__closed").empty();
    }
    static std::vector<std::string> gList(const std::string& j) {
        std::vector<std::string> v;
        auto a = nlohmann::json::parse(j.empty() ? "[]" : j, nullptr, false);
        if (a.is_array()) for (auto& e : a) if (e.is_string()) v.push_back(e.get<std::string>());
        return v;
    }
    static std::string gListDump(const std::vector<std::string>& v) {
        nlohmann::json a = nlohmann::json::array();
        for (auto& s : v) a.push_back(s);
        return a.dump();
    }
    std::string gameIdByCode(const std::string& code) const {
        if (!gameConf_.get || code.empty()) return "";
        auto j = nlohmann::json::parse(gameConf_.get(gameIndexScope(), "sessions"), nullptr, false);
        return j.is_object() && j.contains(code) && j[code].is_string() ? j[code].get<std::string>() : std::string();
    }
    void gameIndexSet(const std::string& code, const std::string& id) {
        if (!gameConf_.get || !gameConf_.set || code.empty()) return;
        auto j = nlohmann::json::parse(gameConf_.get(gameIndexScope(), "sessions"), nullptr, false);
        if (!j.is_object()) j = nlohmann::json::object();
        if (id.empty()) j.erase(code); else j[code] = id;
        gameConf_.set(gameIndexScope(), "sessions", j.dump());
    }
    std::string newGameCode() const {
        // C#111：16 位 base36（大写字母 + 数字）= 约 83 bit；团名只是显示名，
        // 无法靠编号或名称推测接入凭证。避免混入小写，方便玩家抄写/核对团号。
        static constexpr char alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        std::random_device rd;
        std::mt19937_64 rng((static_cast<unsigned long long>(rd()) << 32) ^ rd()
                            ^ static_cast<unsigned long long>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
        std::uniform_int_distribution<size_t> pick(0, sizeof(alphabet) - 2);
        std::string code;
        do {
            code.clear(); code.reserve(16);
            for (int i = 0; i < 16; ++i) code.push_back(alphabet[pick(rng)]);
        } while (!gameIdByCode(code).empty());
        return code;
    }
    std::vector<std::string> gameAreas(const std::string& sessionId) const {
        return gameConf_.get ? gList(gameConf_.get(gameSessionScope(sessionId), "__areas")) : std::vector<std::string>{};
    }
    void setGameAreas(const std::string& sessionId, const std::vector<std::string>& areas) {
        if (gameConf_.set) gameConf_.set(gameSessionScope(sessionId), "__areas", gListDump(areas));
    }
    void detachGameArea(const Message& msg) {
        std::string id = gameSessionId(msg);
        if (id.empty()) return;
        auto areas = gameAreas(id);
        areas.erase(std::remove(areas.begin(), areas.end(), msg.targetId), areas.end());
        setGameAreas(id, areas);
        if (gameConf_.set) gameConf_.set(gameGroupScope(msg), "__session", "");
    }
    void attachGameArea(const Message& msg, const std::string& id) {
        if (id.empty() || !gameConf_.set) return;
        detachGameArea(msg);   // 原版 open：本聊天窗口先从旧团脱离
        gameConf_.set(gameGroupScope(msg), "__session", id);
        auto areas = gameAreas(id);
        if (std::find(areas.begin(), areas.end(), msg.targetId) == areas.end()) areas.push_back(msg.targetId);
        setGameAreas(id, areas);
    }
    void clearGameScope(const std::string& scope) {
        if (!gameConf_.all || !gameConf_.set) return;
        for (auto& [k, _] : gameConf_.all(scope)) gameConf_.set(scope, k, "");
    }
    bool gameIsGm(const Message& msg, const std::string& uid) const {
        for (auto& u : gList(gGet(msg, "__gms"))) if (u == uid) return true;
        return false;
    }

    /// 轮盘骰（原版 DiceRoulette）：GM 为某面数启用后，本群 1dN 掷骰改为「袋中不放回
    /// 抽取」（袋=1..N 各 copies 份，抽空自动重填）。返回 nullopt=该面数没有轮盘。
    std::optional<int> rouletteDraw(const Message& msg, int face) {
        if (!gameActive(msg)) return std::nullopt;
        auto j = nlohmann::json::parse(gGet(msg, "__rou").empty() ? "{}" : gGet(msg, "__rou"), nullptr, false);
        if (!j.is_object()) return std::nullopt;
        std::string fk = std::to_string(face);
        if (!j.contains(fk) || !j[fk].is_object()) return std::nullopt;
        auto& r = j[fk];
        int copies = r.value("copies", 1);
        if (!r.contains("left") || !r["left"].is_array() || r["left"].empty()) {
            // 重填（首次或抽空后洗牌）
            nlohmann::json pool = nlohmann::json::array();
            for (int c = 0; c < copies; ++c) for (int i = 1; i <= face; ++i) pool.push_back(i);
            r["left"] = std::move(pool);
        }
        auto& left = r["left"];
        int idx = (int)(engine_.roll("1d" + std::to_string((int)left.size())).modifiedTotal - 1);
        if (idx < 0 || idx >= (int)left.size()) idx = 0;
        int val = left[idx].get<int>();
        left.erase(left.begin() + idx);
        if (!r.contains("hist") || !r["hist"].is_array()) r["hist"] = nlohmann::json::array();
        r["hist"].push_back(val);
        gSet(msg, "__rou", j.dump());
        return val;
    }

    std::optional<std::string> tryHandleGame(Locale loc, const Message& msg, const std::string& cmd) {
        if (toLower(cmd).rfind("game", 0) != 0) return std::nullopt;
        // 前缀边界：.game / .game xx 才算；.gamela 等更长词放行给插件（.rp/.rpmode 同款教训）。
        if (cmd.size() > 4 && !std::isspace((unsigned char)cmd[4])) return std::nullopt;
        // 团号索引、群→会话映射及会话成员必须作为一个事务更新。这样两个群主同时
        // 开团时也会领到不同的随机团号，不会覆盖或串进同一会话。
        std::lock_guard<std::mutex> gameLock(gameSessionMutex());
        std::string rest = trim(cmd.substr(4));
        auto v = [&](std::initializer_list<std::pair<std::string, std::string>> kv) {
            std::map<std::string, std::string> m;
            for (auto& [k, val] : kv) m[k] = val;
            return m;
        };
        const std::string nick = displayName(msg);
        if (msg.type != MessageType::kGroup) return i18n_.tr(loc, "game.group_only");
        std::string action = rest.substr(0, rest.find_first_of(" \t"));
        std::string arg = trim(rest.substr(action.size()));
        std::string lact = toLower(action);
        const std::string uid = msg.senderId;

        if (lact.empty()) return i18n_.tr(loc, "help.topic.game");
        if (lact == "new") {
            if (!senderIsGroupAdmin(msg)) return i18n_.tr(loc, "game.master_denied", {{"nick", nick}});
            // 显示团名可以重复；接入凭证使用不可顺推的 16 位随机团号。当前群若已接入另一团，仅脱离该群，
            // 不会误终结仍在其他群进行的团。
            std::string name = arg.empty() ? (msg.targetId + "-" + std::to_string((long long)std::time(nullptr) % 100000)) : arg;
            std::string code = newGameCode();
            std::string sessionId = msg.targetId + "-" + std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
            if (!gameSessionId(msg).empty()) detachGameArea(msg); else clearGameScope(gameGroupScope(msg));
            attachGameArea(msg, sessionId);
            clearGameScope(gameSessionScope(sessionId));
            gSet(msg, "__name", name);
            gSet(msg, "__code", code);
            gSet(msg, "__gms", gListDump({uid}));   // 开团者即首任 GM（原版 newGame 由操作者主持）
            gSet(msg, "__areas", gListDump({msg.targetId}));
            gameIndexSet(code, sessionId);
            return i18n_.tr(loc, "game.new", {{"game_id", name}})
                + "\n" + i18n_.tr(loc, "game.join_code", {{"code", code}});
        }
        if (lact == "open") {
            std::string sessionId = arg.empty() ? gameSessionId(msg) : gameIdByCode(arg);
            if (sessionId.empty()) return i18n_.tr(loc, "game.not_exist", {{"game_id", arg.empty() ? "?" : arg}});
            std::string scope = gameSessionScope(sessionId);
            std::string name = gameConf_.get ? gameConf_.get(scope, "__name") : "";
            if (name.empty()) return i18n_.tr(loc, "game.not_exist", {{"game_id", arg.empty() ? "?" : arg}});
            bool gm = false;
            if (gameConf_.get) for (auto& g : gList(gameConf_.get(scope, "__gms"))) if (g == uid) { gm = true; break; }
            // 原版：在群内展开其他游戏领域仅允许该团 GM；管理员不绕过此限制。
            if (!gm) return i18n_.tr(loc, "game.not_master", {{"nick", nick}});
            attachGameArea(msg, sessionId);
            return i18n_.tr(loc, "game.area_open", {{"game_id", name}});
        }
        if (lact == "master") {
            if (gGet(msg, "__name").empty()) return i18n_.tr(loc, "game.void_here", {{"nick", nick}});
            auto gms = gList(gGet(msg, "__gms"));
            bool isGm = false; for (auto& g : gms) if (g == uid) { isGm = true; break; }
            if (!isGm) {
                // 原版：首个 GM 需 canRoomHost；已有 GM 时需群管
                bool allow = gms.empty() ? senderIsGroupAdmin(msg)
                                         : (msg.extra.value("role", std::string()) == "owner"
                                            || msg.extra.value("role", std::string()) == "admin" || isMaster(msg));
                if (!allow) return i18n_.tr(loc, "game.master_denied", {{"nick", nick}});
                gms.push_back(uid);
                gSet(msg, "__gms", gListDump(gms));
                return i18n_.tr(loc, "game.mastered", {{"nick", nick}});
            }
            std::string items;
            for (auto& g : gms) { if (!items.empty()) items += "\n"; items += lookupNick(msg.platform, g) + "(" + g + ")"; }
            return i18n_.tr(loc, "game.master_list", {{"items", items}});
        }
        // 以下子指令需团存在（原版 thisGame() 判空 → strGameVoidHere）
        if (gGet(msg, "__name").empty()) return i18n_.tr(loc, "game.void_here", {{"nick", nick}});
        const std::string gname = gGet(msg, "__name");

        if (lact == "state") {
            auto gms = gList(gGet(msg, "__gms")), pls = gList(gGet(msg, "__pls"));
            std::string s = i18n_.tr(loc, "game.state_head", {{"game_id", gname}});
            s += "\nGM(" + std::to_string(gms.size()) + ")";
            for (auto& g : gms) s += " " + lookupNick(msg.platform, g);
            s += "\nPL(" + std::to_string(pls.size()) + ")";
            for (auto& p : pls) s += " " + lookupNick(msg.platform, p);
            if (!gGet(msg, "__closed").empty()) s += "\n" + i18n_.tr(loc, "game.state_closed");
            if (std::string code = gGet(msg, "__code"); !code.empty())
                s += "\n" + i18n_.tr(loc, "game.join_code", {{"code", code}});
            if (gameConf_.all) {
                std::string vars;
                for (auto& [k, val] : gameConf_.all(gameScope(msg)))
                    if (k.rfind("__", 0) != 0 && !val.empty()) vars += "\n  " + k + " = " + val;
                if (!vars.empty()) s += "\n" + i18n_.tr(loc, "game.state_vars") + vars;
            }
            return s;
        }
        if (lact == "set") {
            if (!gameIsGm(msg, uid)) return i18n_.tr(loc, "game.not_master", {{"nick", nick}});
            std::string item = arg, val;
            if (auto sp = arg.find_first_of(" \t="); sp != std::string::npos) {
                item = trim(arg.substr(0, sp)); val = trim(arg.substr(sp + 1));
            }
            if (item.empty()) return i18n_.tr(loc, "game.item_empty", {{"nick", nick}});
            if (val.empty()) {
                std::string cur = gGet(msg, item);
                return i18n_.tr(loc, "game.item_show", {{"set_item", item}, {"set_val", cur.empty() ? "-" : cur}});
            }
            gSet(msg, item, val);
            return i18n_.tr(loc, "game.item_set", {{"set_item", item}, {"set_val", val}});
        }
        if (lact == "join") {
            auto pls = gList(gGet(msg, "__pls"));
            for (auto& p : pls) if (p == uid) return i18n_.tr(loc, "game.player_already", {{"nick", nick}});
            pls.push_back(uid);
            gSet(msg, "__pls", gListDump(pls));
            return i18n_.tr(loc, "game.joined", {{"nick", nick}});
        }
        if (lact == "exit") {
            auto pls = gList(gGet(msg, "__pls")); auto gms = gList(gGet(msg, "__gms"));
            size_t b = pls.size() + gms.size();
            pls.erase(std::remove(pls.begin(), pls.end(), uid), pls.end());
            gms.erase(std::remove(gms.begin(), gms.end(), uid), gms.end());
            if (pls.size() + gms.size() == b) return i18n_.tr(loc, "game.not_joined", {{"nick", nick}});
            gSet(msg, "__pls", gListDump(pls)); gSet(msg, "__gms", gListDump(gms));
            return i18n_.tr(loc, "game.exited", {{"nick", nick}});
        }
        if (lact == "call") {
            if (!gameIsGm(msg, uid)) return i18n_.tr(loc, "game.not_master", {{"nick", nick}});
            auto pls = gList(gGet(msg, "__pls"));
            if (pls.empty()) return i18n_.tr(loc, "game.player_empty");
            std::string items;
            for (auto& p : pls) { if (!items.empty()) items += "\n"; items += "[CQ:at,qq=" + p + "]"; }
            return items;   // 原版 strGamePlayerCall = {items}
        }
        if (lact == "kick") {
            if (!gameIsGm(msg, uid)) return i18n_.tr(loc, "game.not_master", {{"nick", nick}});
            std::string tid;
            for (char c : arg) if (std::isdigit((unsigned char)c)) tid += c;
            auto pls = gList(gGet(msg, "__pls"));
            size_t b = pls.size();
            pls.erase(std::remove(pls.begin(), pls.end(), tid), pls.end());
            if (pls.size() == b) return i18n_.tr(loc, "game.kick_not_player", {{"nick", nick}, {"tid", tid}});
            gSet(msg, "__pls", gListDump(pls));
            return i18n_.tr(loc, "game.kicked", {{"tid", tid}});
        }
        if (lact == "close") {
            if (!gameIsGm(msg, uid) && !senderIsGroupAdmin(msg)) return i18n_.tr(loc, "game.not_master", {{"nick", nick}});
            std::string reopen = gGet(msg, "__code");
            // 共享团只关闭本群的“游戏领域”；其他已 .game open 的群仍继续使用同一团。
            if (!gameSessionId(msg).empty()) detachGameArea(msg);
            else gSet(msg, "__closed", "1");   // 兼容旧版单群存档
            return i18n_.tr(loc, "game.area_closed", {{"nick", nick}, {"game_id", reopen.empty() ? gname : reopen}});
        }
        if (lact == "over") {
            if (!gameIsGm(msg, uid) && !senderIsGroupAdmin(msg)) return i18n_.tr(loc, "game.not_master", {{"nick", nick}});
            std::string extra;
            // 原版 Dice! 的 .game over 会对仍在记录的团务日志执行 log_end。
            // 暂停中的日志不自动结束，和旧版 is_logging() 的语义一致。
            std::string sessionId = gameSessionId(msg);
            std::string sessionCode = gGet(msg, "__code");
            auto endRunningLog = [&](const Message& areaMsg, bool includeReply) {
                int activeLog = activeLogId(areaMsg);
                if (activeLog <= 0) return;
                try {
                    auto* st = db_.getLogStorage();
                    namespace orm = sqlite_orm;
                    if (st) {
                        auto row = st->get<GameLogRow>(activeLog);
                        row.status = 2;
                        st->update(row);
                        int count = (int)st->count<GameLogMessageRow>(
                            orm::where(orm::c(&GameLogMessageRow::logId) == activeLog));
                        std::string timer = timerSuffixStop(loc, areaMsg, activeLog);
                        setGroupSetting(areaMsg, "activeLog", "");
                        setGroupSetting(areaMsg, "activeLogName", "");
                        shareLog(areaMsg, activeLog);
                        if (includeReply)
                            extra = "\n" + i18n_.tr(loc, "log.ended", {{"count", std::to_string(count)}}) + timer;
                    }
                } catch (...) {
                    // 团务结束不能因日志站/历史数据异常而失败；日志仍可由 .log end 再次处理。
                }
            };
            if (sessionId.empty()) {
                endRunningLog(msg, true);
            } else {
                // 新日志库按群存档；共享团结束时逐一收尾各接入群仍在进行的记录。
                for (const auto& groupId : gameAreas(sessionId)) {
                    Message areaMsg = msg; areaMsg.targetId = groupId;
                    endRunningLog(areaMsg, groupId == msg.targetId);
                }
            }
            // 原版 sessions.over 会删除 SessionByName，并解除所有聊天窗口到该 Session 的映射。
            if (!sessionId.empty()) {
                for (const auto& groupId : gameAreas(sessionId)) {
                    Message areaMsg = msg; areaMsg.targetId = groupId;
                    if (gameConf_.set) gameConf_.set(gameGroupScope(areaMsg), "__session", "");
                }
                clearGameScope(gameSessionScope(sessionId));
                gameIndexSet(sessionCode, "");
            } else {
                clearGameScope(gameScope(msg));   // 兼容旧版单群存档
            }
            return i18n_.tr(loc, "game.over", {{"game_id", gname}}) + extra;
        }
        if (lact == "rou") {
            std::string a2 = toLower(arg);
            auto rj = nlohmann::json::parse(gGet(msg, "__rou").empty() ? "{}" : gGet(msg, "__rou"), nullptr, false);
            if (!rj.is_object()) rj = nlohmann::json::object();
            if (!a2.empty() && std::isdigit((unsigned char)a2[0])) {
                if (!gameIsGm(msg, uid)) return i18n_.tr(loc, "game.not_master", {{"nick", nick}});
                int face = 0, copies = 1;
                size_t star = a2.find('*');
                try {
                    face = std::stoi(a2.substr(0, star));
                    if (star != std::string::npos) copies = std::stoi(a2.substr(star + 1));
                } catch (...) {}
                if (face <= 0 || copies <= 0 || face * copies > 100)
                    return i18n_.tr(loc, "game.roulette_too_big", {{"nick", nick}});
                rj[std::to_string(face)] = nlohmann::json{{"copies", copies}};
                gSet(msg, "__rou", rj.dump());
                return i18n_.tr(loc, "game.roulette_set", {{"face", std::to_string(face)}});
            }
            if (a2 == "hist") {
                if (rj.empty()) return i18n_.tr(loc, "game.roulette_empty");
                std::string hist;
                for (auto& [fk, r] : rj.items()) {
                    std::string line = "D" + fk + "=";
                    if (r.contains("hist") && r["hist"].is_array())
                        for (auto& h : r["hist"]) line += std::to_string(h.get<int>()) + " ";
                    if (!hist.empty()) hist += "\n";
                    hist += line;
                }
                return i18n_.tr(loc, "game.roulette_hist", {{"hist", hist}});
            }
            if (a2 == "reset") {
                if (!gameIsGm(msg, uid)) return i18n_.tr(loc, "game.not_master", {{"nick", nick}});
                for (auto& [fk, r] : rj.items()) { r.erase("left"); r.erase("hist"); }
                gSet(msg, "__rou", rj.dump());
                return i18n_.tr(loc, "game.roulette_reset");
            }
            if (a2 == "clr") {
                if (!gameIsGm(msg, uid)) return i18n_.tr(loc, "game.not_master", {{"nick", nick}});
                gSet(msg, "__rou", "");
                return i18n_.tr(loc, "game.roulette_clear");
            }
            return i18n_.tr(loc, "help.topic.game");
        }
        return i18n_.tr(loc, "help.topic.game");
    }

    // ─── 衍生属性上限（规则包 computed，数据驱动）────────────────
    // 全局合并的「规范名 → 公式」表，启动时由规则包 computed 区块覆盖/扩充
    // （loadRulePackComputed，镜像 alias 的全局合并）。内置 COC7 三项作 seed，
    // 即使没有规则包也保持现状行为。⚠️ 注册仅启动期、attrMax 服务期并发读。
    static std::map<std::string, std::string> builtinComputed() {
        return {
            {"生命值", "(体质+体型)/10"},
            {"理智",   "99-克苏鲁神话?0"},   // 克苏鲁神话缺省按 0（?0），故 san 恒可算
            {"魔法值", "意志/5"},
        };
    }
    static std::map<std::string, std::string>& computedRegistry() {
        static std::map<std::string, std::string> reg = builtinComputed();
        return reg;
    }
    static void resetComputed() { computedRegistry() = builtinComputed(); }

    // ─── C#102：技能默认值 + 内置派生关系 ─────────────────────
    /// COC7 技能默认值（原版 RDConstant.h SkillDefaultVal 全表移植）：未录入该技能时
    /// 检定按默认值（如 .ra 急救 → 30）。规则包 rules/*.json 可用 "defaults":{"技能":值}
    /// 增改（不同规则的默认值不同）。
    static std::map<std::string, int> builtinSkillDefaults() {
        return {
            {"会计", 5},   {"人类学", 1},   {"估价", 5},     {"考古学", 1},   {"作画", 5},
            {"摄影", 5},   {"表演", 5},     {"伪造", 5},     {"伪造文书", 5}, {"文学", 5},
            {"书法", 5},   {"乐理", 5},     {"厨艺", 5},     {"裁缝", 5},     {"理发", 5},
            {"建筑", 5},   {"舞蹈", 5},     {"酿酒", 5},     {"捕鱼", 5},     {"歌唱", 5},
            {"制陶", 5},   {"雕塑", 5},     {"杂技", 5},     {"风水", 5},     {"技术制图", 5},
            {"耕作", 5},   {"打字", 5},     {"速记", 5},     {"取悦", 15},    {"魅惑", 15},
            {"攀爬", 20},  {"计算机使用", 5}, {"克苏鲁神话", 0}, {"乔装", 5},   {"汽车驾驶", 20},
            {"电气维修", 10}, {"电子学", 1}, {"话术", 5},     {"鞭子", 5},     {"电锯", 10},
            {"斧", 15},    {"链锯", 10},    {"连枷", 10},    {"绞索", 15},    {"矛", 20},
            {"剑", 20},    {"手枪", 20},    {"步枪/霰弹枪", 25}, {"冲锋枪", 15}, {"弓术", 15},
            {"火焰喷射器", 10}, {"机关枪", 10}, {"重武器", 10}, {"急救", 30},  {"历史", 5},
            {"恐吓", 15},  {"跳跃", 20},    {"法律", 5},     {"图书馆使用", 20}, {"聆听", 20},
            {"锁匠", 1},   {"机械维修", 10}, {"医学", 1},    {"博物学", 10},  {"导航", 10},
            {"神秘学", 5}, {"操作重型机械", 1}, {"说服", 10}, {"飞行器驾驶", 1}, {"船驾驶", 1},
            {"精神分析", 1}, {"心理学", 10}, {"骑乘", 5},    {"地质学", 1},   {"化学", 1},
            {"生物学", 1}, {"数学", 10},    {"天文学", 1},   {"物理学", 1},   {"药学", 1},
            {"植物学", 1}, {"动物学", 1},   {"密码学", 1},   {"工程学", 1},   {"气象学", 1},
            {"司法科学", 1}, {"妙手", 10},  {"侦查", 25},    {"潜行", 20},    {"游泳", 20},
            {"投掷", 20},  {"追踪", 10},    {"驯兽", 5},     {"潜水", 1},     {"爆破", 1},
            {"读唇", 1},   {"催眠", 1},     {"炮术", 1},     {"斗殴", 25},
        };
    }
    /// key 统一 canonical 化（别名/同义词归并后查得到）。
    static std::map<std::string, int> normalizeDefaults(const std::map<std::string, int>& in) {
        std::map<std::string, int> out;
        for (auto& [k, v] : in) out[CharacterCardStore::canonical(k)] = v;
        return out;
    }
    static std::map<std::string, int>& defaultsRegistry() {
        static std::map<std::string, int> reg = normalizeDefaults(builtinSkillDefaults());
        return reg;
    }
    static void resetDefaults() { defaultsRegistry() = normalizeDefaults(builtinSkillDefaults()); }
    /// 规则包声明的派生公式（查询优先于 derivedRegistry；重载规则包时清空重建）。
    static std::map<std::string, std::string>& rulePackDerivedRegistry() {
        static std::map<std::string, std::string> m; return m;
    }
    /// 查技能默认值（canonical 后查表）。无则 nullopt。
    static std::optional<int> defaultAttr(const std::string& attr) {
        std::shared_lock<std::shared_mutex> lk(rulesLock());
        auto& reg = defaultsRegistry();
        auto it = reg.find(CharacterCardStore::canonical(attr));
        if (it != reg.end()) return it->second;
        return std::nullopt;
    }
    /// C#102：内置派生关系（原版 mVariableCOC7）：未录入时按公式算。灵感=智力、
    /// 知识=教育、取悦=魅惑已由同义词归并覆盖；这里补 闪避=敏捷/2、母语=教育、
    /// 理智初始=意志（生命/魔法的公式在 builtinComputed）。
    static std::map<std::string, std::string> builtinDerived() {
        return {
            {"闪避", "敏捷/2"},
            {"母语", "教育"},
            {"理智", "意志"},
        };
    }

    /// model.xml 的衍生属性值（text='javascript' 翻成本程序 DSL）：属性名→公式。
    /// 读属性未显式设置时回退到此（如 闪避=敏捷/2、精神=意志*10、最大生命值=(体质+体型)/10）。
    static std::map<std::string, std::string>& derivedRegistry() { static std::map<std::string, std::string> m; return m; }
    static void resetDerived() { derivedRegistry().clear(); }
    /// 把 model.xml 的 text='javascript' 公式翻成 computed DSL（属性名留待 evalComputedFormula
    /// 自动 canonical 解析）；含无法翻译的 JS（Math.*函数/三元/比较/字符串/对象）→ 返回空。
    static std::string translateModelExpr(std::string s) {
        if (auto p = s.rfind("&&"); p != std::string::npos) s = s.substr(p + 2);   // 取守卫后的实际表达式
        auto rep = [&](const std::string& a, const std::string& b) {
            for (size_t p; (p = s.find(a)) != std::string::npos; ) s.replace(p, a.size(), b);
        };
        rep("this.", ""); rep("pc.", "");
        rep("Math.floor(", "(");      // floor 由整数除法天然实现
        rep(" || 0", ""); rep("||0", "");
        auto t0 = s.find_first_not_of(" \t\r\n");
        s = (t0 == std::string::npos) ? "" : s.substr(t0, s.find_last_not_of(" \t\r\n") - t0 + 1);
        if (s.empty() || s.find("Math.") != std::string::npos || s.find('?') != std::string::npos
            || s.find("function") != std::string::npos || s.find('{') != std::string::npos
            || s.find('[') != std::string::npos || s.find('"') != std::string::npos
            || s.find('\'') != std::string::npos || s.find("==") != std::string::npos
            || s.find('<') != std::string::npos || s.find('>') != std::string::npos
            || s.find('|') != std::string::npos || s.find('&') != std::string::npos)
            return "";
        return s;
    }

    /// 求值一条 computed 公式，如 "(体质+体型)/10" 或 "99-克苏鲁神话?0"。
    /// 引用的属性名→卡值；裸引用缺失 → 返回 nullopt（公式失败）；带 "?N" 的
    /// 引用缺失 → 用 N。整数运算，* / 优先于 + -，支持括号。
    std::optional<int> evalComputedFormula(const std::string& expr, const Message& msg,
            const std::function<std::optional<int>(const std::string&)>& lookOverride = {}) const {
        struct P {
            const std::string& s; size_t i = 0; bool fail = false;
            std::function<std::optional<int>(const std::string&)> look;
            void skip() { while (i < s.size() && s[i] == ' ') ++i; }
            long readNum() { long v = 0; while (i < s.size() && std::isdigit((unsigned char)s[i])) v = v * 10 + (s[i++] - '0'); return v; }
            long factor() {
                skip();
                if (i < s.size() && s[i] == '(') { ++i; long v = expr_(); skip(); if (i < s.size() && s[i] == ')') ++i; return v; }
                if (i < s.size() && std::isdigit((unsigned char)s[i])) return readNum();
                size_t start = i;                                  // 标识符：读到运算符/括号/空格/?
                while (i < s.size()) { char c = s[i];
                    if (c == '+' || c == '-' || c == '*' || c == '/' || c == '(' || c == ')' || c == ' ' || c == '?') break; ++i; }
                std::string name = s.substr(start, i - start);
                std::optional<int> def;
                if (i < s.size() && s[i] == '?') { ++i; bool neg = false; if (i < s.size() && s[i] == '-') { neg = true; ++i; }
                    long d = readNum(); def = (int)(neg ? -d : d); }
                if (name.empty()) { fail = true; return 0; }
                auto v = look(CharacterCardStore::canonical(name));
                if (v) return *v;
                if (def) return *def;
                fail = true; return 0;                              // 裸引用缺失 → 失败
            }
            long term() { long v = factor(); skip();
                while (i < s.size() && (s[i] == '*' || s[i] == '/')) { char op = s[i++]; long r = factor();
                    if (op == '*') v *= r; else v = (r != 0 ? v / r : 0); skip(); } return v; }
            long expr_() { long v = term(); skip();
                while (i < s.size() && (s[i] == '+' || s[i] == '-')) { char op = s[i++]; long r = term();
                    v = (op == '+') ? v + r : v - r; skip(); } return v; }
        } p{expr, 0, false,
            lookOverride ? lookOverride
                         : std::function<std::optional<int>(const std::string&)>(
                               [&](const std::string& name) { return cards_.getAttr(msg.senderId, cardScope(msg), name); })};
        long v = p.expr_();
        if (p.fail) return std::nullopt;
        return (int)v;
    }

    /// C#12-A②: 渲染规则包自定义指令的输出模板。模板中的 `{表达式}` 占位会被
    /// 求值后替换；模板外文字原样输出，字面量 `\n` 转真换行。表达式 DSL 支持：
    ///   数字 / '字符串' 或 "字符串" / NdM 掷骰(dM=1dM) / 属性名(取本人卡值) /
    ///   arg(指令后跟随的参数文本) / 全局变量(nick/user/group/self/date/time) /
    ///   函数 min(a,b,…)/max(a,b,…)/abs(x) / + - * / / 比较(< <= > >= == !=) /
    ///   三元 ?: / 括号。比较与三元用于做「成功/失败」之类判定。同一次渲染内，
    ///   相同掷骰 token 只摇一次（如 `{d100}` 与 `{d100<=力量}` 共用同一结果）。
    ///   引用缺失属性 → 整条渲染失败返回 nullopt（调用方回报提示）。
    std::optional<std::string> renderCustomCmd(const std::string& tmpl, const Message& msg,
                                               const std::string& arg,
                                               const std::map<std::string, int>* mockAttrs = nullptr) const {
        using Val = std::variant<long, std::string>;
        // 引擎掷骰缓存：token(如 "1d100") → 总点数，保证同 token 一次渲染只摇一次。
        auto diceCache = std::make_shared<std::map<std::string, long>>();
        std::function<long(const std::string&)> roll =
            [this, diceCache](const std::string& tok) -> long {
                auto it = diceCache->find(tok);
                if (it != diceCache->end()) return it->second;
                auto r = engine_.roll(tok);
                long v = r.ok() ? r.modifiedTotal : 0;
                (*diceCache)[tok] = v;
                return v;
            };
        // 属性求值：测试模式(mockAttrs!=null)用模拟值，否则取本人卡值。
        std::function<std::optional<long>(const std::string&)> lookAttr =
            [this, &msg, mockAttrs](const std::string& name) -> std::optional<long> {
                if (mockAttrs) { auto it = mockAttrs->find(name);
                    return it != mockAttrs->end() ? std::optional<long>((long)it->second) : std::nullopt; }
                return cards_.getAttr(msg.senderId, cardScope(msg), name);
            };
        // 全局/模板变量（字符串值）：与 applySelf 同源，可在自定义指令输出里引用 {nick} 等。
        std::function<std::optional<std::string>(const std::string&)> globalVar =
            [this, &msg](const std::string& name) -> std::optional<std::string> {
                if (name == "nick" || name == "昵称" || name == "名字") return displayNameRaw(msg);
                if (name == "user" || name == "qq")                    return msg.senderId;
                if (name == "group" || name == "群号")                 return msg.targetId;
                if (name == "self" || name == "骰娘")                  return resolveSelfCall(msg);
                if (name == "date" || name == "time" || name == "日期" || name == "时间") {
                    std::time_t tt = std::time(nullptr); std::tm lt{};
#if defined(_WIN32)
                    localtime_s(&lt, &tt);
#else
                    lt = *std::localtime(&tt);
#endif
                    char b[16]; const char* fmt = (name == "date" || name == "日期") ? "%Y-%m-%d" : "%H:%M:%S";
                    std::strftime(b, sizeof(b), fmt, &lt); return std::string(b);
                }
                return std::nullopt;
            };

        struct P {
            const std::string& s; size_t i = 0; bool fail = false;
            std::function<long(const std::string&)>& roll;
            std::function<std::optional<long>(const std::string&)>& lookAttr;
            std::function<std::optional<std::string>(const std::string&)>& globalVar;
            const std::string& arg;

            static bool isNum(const Val& v) { return std::holds_alternative<long>(v); }
            static long toNum(const Val& v) {
                if (auto p = std::get_if<long>(&v)) return *p;
                try { return std::stol(std::get<std::string>(v)); } catch (...) { return 0; }
            }
            static std::string toStr(const Val& v) {
                if (auto p = std::get_if<long>(&v)) return std::to_string(*p);
                return std::get<std::string>(v);
            }
            static bool truthy(const Val& v) {
                if (auto p = std::get_if<long>(&v)) return *p != 0;
                return !std::get<std::string>(v).empty();
            }
            void skip() { while (i < s.size() && s[i] == ' ') ++i; }
            long readNum() { long v = 0; while (i < s.size() && std::isdigit((unsigned char)s[i])) v = v * 10 + (s[i++] - '0'); return v; }

            Val primary() {
                skip();
                if (i < s.size() && s[i] == '(') { ++i; Val v = expr(); skip(); if (i < s.size() && s[i] == ')') ++i; return v; }
                if (i < s.size() && (s[i] == '\'' || s[i] == '"')) {   // 字符串字面量
                    char q = s[i++]; size_t st = i; while (i < s.size() && s[i] != q) ++i;
                    std::string str = s.substr(st, i - st); if (i < s.size()) ++i; return Val{str};
                }
                if (i < s.size() && std::isdigit((unsigned char)s[i])) {   // 数字 或 NdM
                    size_t st = i; readNum();
                    if (i < s.size() && (s[i] == 'd' || s[i] == 'D') && i + 1 < s.size() && std::isdigit((unsigned char)s[i + 1])) {
                        ++i; readNum(); return Val{roll(s.substr(st, i - st))};
                    }
                    return Val{(long)std::stol(s.substr(st, i - st))};
                }
                if (i + 1 < s.size() && (s[i] == 'd' || s[i] == 'D') && std::isdigit((unsigned char)s[i + 1])) {  // dM = 1dM
                    size_t st = i; ++i; readNum(); return Val{roll("1" + s.substr(st, i - st))};
                }
                size_t st = i;                                          // 标识符（函数 / 属性名 / arg / 全局变量）
                while (i < s.size()) { char c = s[i];
                    if (c == '+' || c == '-' || c == '*' || c == '/' || c == '(' || c == ')' ||
                        c == '<' || c == '>' || c == '=' || c == '!' || c == '?' || c == ':' ||
                        c == ',' || c == ' ' || c == '\'' || c == '"') break; ++i; }
                std::string name = s.substr(st, i - st);
                if (name.empty()) { fail = true; return Val{0L}; }
                skip();
                if (i < s.size() && s[i] == '(') {                      // 函数调用 fn(a, b, …)
                    ++i; std::vector<Val> args; skip();
                    if (!(i < s.size() && s[i] == ')')) {
                        args.push_back(expr()); skip();
                        while (i < s.size() && s[i] == ',') { ++i; args.push_back(expr()); skip(); }
                    }
                    if (i < s.size() && s[i] == ')') ++i; else { fail = true; return Val{0L}; }
                    return callFn(name, args);
                }
                if (name == "arg" || name == "参数") return Val{arg};
                if (auto g = globalVar(name)) return Val{*g};           // 全局/模板变量（字符串）
                auto v = lookAttr(CharacterCardStore::canonical(name));
                if (v) return Val{(long)*v};
                fail = true; return Val{0L};                            // 属性缺失 → 失败
            }
            Val callFn(const std::string& fn, std::vector<Val>& a) {
                if ((fn == "min" || fn == "max") && !a.empty()) {
                    long r = toNum(a[0]);                              // 手写 min/max，避开 windows.h 宏
                    for (size_t k = 1; k < a.size(); ++k) { long x = toNum(a[k]); r = (fn == "min") ? (x < r ? x : r) : (x > r ? x : r); }
                    return Val{r};
                }
                if (fn == "abs" && a.size() == 1) { long x = toNum(a[0]); return Val{x < 0 ? -x : x}; }
                fail = true; return Val{0L};                            // 未知函数 / 参数不符 → 失败
            }
            Val unary() { skip(); if (i < s.size() && s[i] == '-') { ++i; return Val{-toNum(unary())}; } return primary(); }
            Val muldiv() { Val v = unary(); skip();
                while (i < s.size() && (s[i] == '*' || s[i] == '/')) { char op = s[i++]; long a = toNum(v), b = toNum(unary());
                    v = Val{op == '*' ? a * b : (b != 0 ? a / b : 0)}; skip(); } return v; }
            Val addsub() { Val v = muldiv(); skip();
                while (i < s.size() && (s[i] == '+' || s[i] == '-')) { char op = s[i++]; Val r = muldiv();
                    if (op == '+' && (!isNum(v) || !isNum(r))) v = Val{toStr(v) + toStr(r)};   // 任一为串 → 拼接
                    else v = Val{op == '+' ? toNum(v) + toNum(r) : toNum(v) - toNum(r)}; skip(); } return v; }
            Val compare() { Val v = addsub(); skip();
                while (i < s.size()) {
                    std::string op;
                    if (i + 1 < s.size() && (s[i] == '<' || s[i] == '>' || s[i] == '=' || s[i] == '!') && s[i + 1] == '=') { op = s.substr(i, 2); i += 2; }
                    else if (s[i] == '<' || s[i] == '>') { op = s.substr(i, 1); ++i; }
                    else break;
                    Val r = addsub(); bool both = isNum(v) && isNum(r); long res = 0;
                    long a = 0, b = 0; std::string sa, sb;
                    if (both) { a = std::get<long>(v); b = std::get<long>(r); } else { sa = toStr(v); sb = toStr(r); }
                    if (op == "<")  res = both ? (a < b)  : (sa < sb);
                    else if (op == ">")  res = both ? (a > b)  : (sa > sb);
                    else if (op == "<=") res = both ? (a <= b) : (sa <= sb);
                    else if (op == ">=") res = both ? (a >= b) : (sa >= sb);
                    else if (op == "==") res = both ? (a == b) : (sa == sb);
                    else if (op == "!=") res = both ? (a != b) : (sa != sb);
                    v = Val{res}; skip();
                } return v; }
            Val ternary() { Val c = compare(); skip();
                if (i < s.size() && s[i] == '?') { ++i; Val a = ternary(); skip();
                    if (i < s.size() && s[i] == ':') ++i; Val b = ternary(); return truthy(c) ? a : b; }
                return c; }
            Val expr() { return ternary(); }
        };

        std::string out; out.reserve(tmpl.size() + 16);
        for (size_t k = 0; k < tmpl.size(); ++k) {
            if (tmpl[k] == '\\' && k + 1 < tmpl.size() && tmpl[k + 1] == 'n') { out += '\n'; ++k; continue; }
            if (tmpl[k] != '{') { out += tmpl[k]; continue; }
            size_t end = tmpl.find('}', k);
            if (end == std::string::npos) { out += tmpl.substr(k); break; }
            std::string inner = tmpl.substr(k + 1, end - k - 1);
            P p{inner, 0, false, roll, lookAttr, globalVar, arg};
            Val v = p.expr();
            if (p.fail) return std::nullopt;
            out += P::toStr(v);
            k = end;
        }
        return out;
    }

    /// Maximum for a "capped" attribute (hp/san/mp …). Priority:
    ///   1) explicit cap stored via ".st hp4/10" (user_settings "max:<canon>")
    ///   2) rule-pack computed formula (computedRegistry / evalComputedFormula)
    ///   3) fall back to the current value so these stats ALWAYS show "/max".
    /// Non-capped attributes (力量/敏捷/…) return nullopt → shown plain.
    // 某「当前值属性」（生命值/理智/魔法值）对应的「显式上限属性」入口名列表。
    // 用各种常见写法逐个 getAttr —— getAttr 内部按 canonical 解析，用哪个名字存就用同一个
    // 名字读得到（绕开各 mod 全局别名冲突导致 生命值上限↔生命上限↔最大生命值 互相打架的问题）。
    // 返回空=该属性无「上限」概念。
    static std::vector<std::string> explicitMaxNamesFor(const std::string& curCanon) {
        const std::string HP  = "\xe7\x94\x9f\xe5\x91\xbd\xe5\x80\xbc";          // 生命值
        const std::string SAN = "\xe7\x90\x86\xe6\x99\xba";                       // 理智
        const std::string MP  = "\xe9\xad\x94\xe6\xb3\x95\xe5\x80\xbc";          // 魔法值
        if (curCanon == HP)  return {
            "\xe7\x94\x9f\xe5\x91\xbd\xe5\x80\xbc\xe4\xb8\x8a\xe9\x99\x90",       // 生命值上限
            "\xe7\x94\x9f\xe5\x91\xbd\xe4\xb8\x8a\xe9\x99\x90",                   // 生命上限
            "\xe6\x9c\x80\xe5\xa4\xa7\xe7\x94\x9f\xe5\x91\xbd\xe5\x80\xbc",       // 最大生命值
            "hpmax", "maxhp" };
        if (curCanon == SAN) return {
            "\xe7\x90\x86\xe6\x99\xba\xe4\xb8\x8a\xe9\x99\x90",                   // 理智上限
            "\xe7\x90\x86\xe6\x99\xba\xe5\x80\xbc\xe4\xb8\x8a\xe9\x99\x90",       // 理智值上限
            "\xe6\x9c\x80\xe5\xa4\xa7\xe7\x90\x86\xe6\x99\xba\xe5\x80\xbc",       // 最大理智值
            "sanmax", "maxsan" };
        if (curCanon == MP)  return {
            "\xe9\xad\x94\xe6\xb3\x95\xe5\x80\xbc\xe4\xb8\x8a\xe9\x99\x90",       // 魔法值上限
            "\xe9\xad\x94\xe6\xb3\x95\xe4\xb8\x8a\xe9\x99\x90",                   // 魔法上限
            "\xe6\x9c\x80\xe5\xa4\xa7\xe9\xad\x94\xe6\xb3\x95\xe5\x80\xbc",       // 最大魔法值
            "mpmax", "maxmp" };
        return {};
    }

    std::optional<int> attrMax(const std::string& canon, const Message& msg) const {
        // 0. 显式「上限属性」最优先：用户用 .st hpmax=20 / 生命值上限=20 / 最大生命值=20
        //    在卡上录入的上限，以它为准；没录入才往下走推导（如体质换算）。
        for (const auto& mxName : explicitMaxNamesFor(canon))
            if (auto v = cards_.getAttr(msg.senderId, cardScope(msg), mxName)) return *v;
        std::string stored = getUserSetting(msg, "max:" + canon);
        if (!stored.empty()) { try { return std::stoi(stored); } catch (...) {} }
        std::string formula;                               // 在读锁内取出公式副本，再脱锁求值
        {
            std::shared_lock<std::shared_mutex> lk(rulesLock());
            auto& reg = computedRegistry();
            auto it = reg.find(canon);
            if (it == reg.end()) return std::nullopt;      // 非衍生属性
            formula = it->second;
        }
        if (auto v = evalComputedFormula(formula, msg)) return *v;   // 公式求值成功
        if (auto cur = cards_.getAttr(msg.senderId, cardScope(msg), canon)) return *cur;  // 裸引用缺失 → 回退当前值
        return std::nullopt;
    }

    /// The group's configured .sn attribute tokens (e.g. {"hp","san","dex"}),
    /// or the default if none set.
    std::vector<std::string> snAttrs(const Message& msg) const {
        std::string v = getGroupSetting(msg, "snAttrs");
        std::vector<std::string> toks;
        std::istringstream iss(v.empty() ? std::string("hp san dex") : v);
        std::string t; while (iss >> t) toks.push_back(t);
        if (toks.empty()) toks = {"hp", "san", "dex"};
        return toks;
    }

    /// Render the group card from an attribute-token list:
    ///   "<nick> hp10/10 san60/94 dex70"  (cur/max for hp/san/mp, plain otherwise)
    std::string renderSnAttrs(const Message& msg, const std::vector<std::string>& toks) {
        std::string out = displayNameRaw(msg);   // 设群名片：用原始名，不加 <>
        for (const auto& tok : toks) {
            auto v = cards_.getAttr(msg.senderId, cardScope(msg), tok);
            if (!v) v = evalStrAttr(msg, CharacterCardStore::canonical(tok));  // C#37 关联属性求值
            if (!v) continue;                          // skip attributes the card lacks
            out += " " + tok + std::to_string(*v);
            if (auto mx = attrMax(CharacterCardStore::canonical(tok), msg)) out += "/" + std::to_string(*mx);
        }
        return out;
    }

    /// Personal free-form template (back-compat): {nick} + {attr} placeholders.
    std::string renderSn(const Message& msg, const std::string& tmpl) {
        std::string out;
        for (size_t i = 0; i < tmpl.size();) {
            if (tmpl[i] == '{') {
                size_t e = tmpl.find('}', i + 1);
                if (e == std::string::npos) { out += tmpl[i++]; continue; }
                std::string t = tmpl.substr(i + 1, e - i - 1);
                if (t == "nick") out += resolveNickDisplay(msg);   // 名片模板：群名片/QQ昵称
                else { auto v = cards_.getAttr(msg.senderId, cardScope(msg), t); out += v ? std::to_string(*v) : "0"; }
                i = e + 1;
            } else out += tmpl[i++];
        }
        return out;
    }

    // .setsn <属性...>：群主/管理员配置本群 .sn 名片用哪些属性（最多5条，默认 hp san dex）。
    std::optional<std::string> tryHandleSetsn(Locale loc, const Message& msg, const std::string& cmd) {
        if (toLower(cmd).rfind("setsn", 0) != 0) return std::nullopt;
        if (msg.type == MessageType::kPrivate) return i18n_.tr(loc, "sn.group_only");
        std::string rest = trim(cmd.substr(5));
        std::string lr = toLower(rest);
        if (rest.empty() || lr == "show") {
            std::string cur; for (auto& t : snAttrs(msg)) { if (!cur.empty()) cur += " "; cur += t; }
            return i18n_.tr(loc, "setsn.show", {{"attrs", cur}});
        }
        if (!senderIsGroupAdmin(msg)) return i18n_.tr(loc, "setsn.no_perm");
        if (lr == "clr" || lr == "clear" || lr == "off") {
            setGroupSetting(msg, "snAttrs", "");
            return i18n_.tr(loc, "setsn.cleared");
        }
        std::vector<std::string> toks; std::istringstream iss(rest); std::string t;
        while (iss >> t && toks.size() < 5) toks.push_back(t);
        if (toks.empty()) return i18n_.tr(loc, "setsn.usage");
        std::string joined; for (auto& x : toks) { if (!joined.empty()) joined += " "; joined += x; }
        setGroupSetting(msg, "snAttrs", joined);
        return i18n_.tr(loc, "setsn.set", {{"attrs", joined}});
    }

    // 渲染当前 .sn 名片：优先个人模板（含 {属性}），否则用群配置的属性列表。
    std::string renderCurrentSn(const Message& msg) {
        std::string tmpl = getUserSetting(msg, "snTemplate");
        if (!tmpl.empty()) return renderSn(msg, tmpl);
        return renderSnAttrs(msg, snAttrs(msg));
    }
    // 把当前名片静默应用到群（不返回提示）。
    void applySnCard(const Message& msg) {
        if (auto ad = adapters_.getAdapter(msg.adapterId))
            ad->setGroupCard(msg.targetId, msg.senderId, renderCurrentSn(msg));
    }
    bool snAutoOn(const Message& msg) const { return getUserSetting(msg, "snAuto") == "1"; }
    // .st 改卡后调用：若本群开了 .sn auto，则实时刷新群名片。
    void maybeAutoSn(const Message& msg) {
        if (msg.type == MessageType::kPrivate || msg.targetId.empty()) return;
        if (snAutoOn(msg)) applySnCard(msg);
    }

    std::string handleSn(Locale loc, const std::string& args, const Message& msg) {
        if (msg.type == MessageType::kPrivate) return i18n_.tr(loc, "sn.group_only");
        std::string a = trim(args), la = toLower(a);
        if (la == "off" || la == "clr") {
            setUserSetting(msg, "snTemplate", "");
            setUserSetting(msg, "snAuto", "");   // 关名片也关自动同步
            if (auto ad = adapters_.getAdapter(msg.adapterId)) ad->setGroupCard(msg.targetId, msg.senderId, "");
            return i18n_.tr(loc, "sn.cleared");
        }
        // .sn auto [on|off]：监听属性变化、实时更新群名片。
        auto [w0, w1] = splitCommand(a);
        if (toLower(trim(w0)) == "auto") {
            std::string onoff = toLower(trim(w1));
            if (onoff == "off" || onoff == "clr") {
                setUserSetting(msg, "snAuto", "");
                return i18n_.tr(loc, "sn.auto_off");
            }
            setUserSetting(msg, "snAuto", "1");
            applySnCard(msg);   // 立即同步一次
            return i18n_.tr(loc, "sn.auto_on", {{"card", renderCurrentSn(msg)}});
        }
        std::string card;
        if (!a.empty() && a.find('{') != std::string::npos) {
            // Explicit personal template with {placeholders}.
            setUserSetting(msg, "snTemplate", a);
            card = renderSn(msg, a);
        } else {
            // Default: render from the group's configured attribute list.
            card = renderSnAttrs(msg, snAttrs(msg));
        }
        if (auto ad = adapters_.getAdapter(msg.adapterId)) ad->setGroupCard(msg.targetId, msg.senderId, card);
        return i18n_.tr(loc, "sn.done", {{"card", card}});
    }

    // ─── .welcome 入群欢迎词（骰主设置，存 group_settings）────
    std::string handleWelcome(Locale loc, const std::string& args, const Message& msg) {
        if (msg.type == MessageType::kPrivate) return i18n_.tr(loc, "welcome.private");
        std::string a = trim(args), la = toLower(a);
        if (a.empty() || la == "show") {
            std::string w = getGroupSetting(msg, "welcome");
            if (w.empty()) return i18n_.tr(loc, "welcome.none");
            std::string info = i18n_.tr(loc, "welcome.show", {{"text", w}});
            auto wd = getGroupSetting(msg, "welcome_delay");
            auto wc = getGroupSetting(msg, "welcome_cooldown");
            if (!wd.empty()) info += " " + i18n_.tr(loc, "welcome.delay_info", {{"sec", wd}});
            if (!wc.empty()) info += " " + i18n_.tr(loc, "welcome.cd_info", {{"sec", wc}});
            return info;
        }
        // C#48：设置/关闭欢迎词需群管权限（原版 canRoomHost 门控开关类指令）。
        if (!senderIsGroupAdmin(msg)) return i18n_.tr(loc, "gate.no_perm");
        // C#76: .welcome delay <sec>
        if (la.rfind("delay ", 0) == 0) {
            std::string val = trim(args.substr(6));
            if (val.empty() || val == "0") { setGroupSetting(msg, "welcome_delay", ""); return i18n_.tr(loc, "welcome.delay_off"); }
            int sec = 0; try { sec = std::stoi(val); } catch (...) { return i18n_.tr(loc, "welcome.bad_value"); }
            int globalMin = cfg_.get<int>("events/welcome_min_delay", 0);
            if (sec < globalMin) return i18n_.tr(loc, "welcome.below_min", {{"min", std::to_string(globalMin)}});
            if (sec > 300) return i18n_.tr(loc, "welcome.bad_range");
            setGroupSetting(msg, "welcome_delay", std::to_string(sec));
            return i18n_.tr(loc, "welcome.delay_set", {{"sec", std::to_string(sec)}});
        }
        // C#76: .welcome cd <sec>
        if (la.rfind("cd ", 0) == 0) {
            std::string val = trim(args.substr(3));
            if (val.empty() || val == "0") { setGroupSetting(msg, "welcome_cooldown", ""); return i18n_.tr(loc, "welcome.cd_off"); }
            int sec = 0; try { sec = std::stoi(val); } catch (...) { return i18n_.tr(loc, "welcome.bad_value"); }
            int globalMin = cfg_.get<int>("events/welcome_min_cooldown", 0);
            if (sec < globalMin) return i18n_.tr(loc, "welcome.below_min", {{"min", std::to_string(globalMin)}});
            if (sec > 3600) return i18n_.tr(loc, "welcome.bad_range");
            setGroupSetting(msg, "welcome_cooldown", std::to_string(sec));
            return i18n_.tr(loc, "welcome.cd_set", {{"sec", std::to_string(sec)}});
        }

        if (la == "off" || la == "clr") { setGroupSetting(msg, "welcome", ""); return i18n_.tr(loc, "welcome.off"); }
        setGroupSetting(msg, "welcome", a);
        return i18n_.tr(loc, "welcome.set", {{"text", a}});
    }

    // ─── .lang 切换回复语言（按群 / 按私聊用户） ─────────────
    // `.lang`           → 显示当前语言 + 用法
    // `.lang 简体|繁體|en` → 设置本群（私聊则本人）的回复语言
    // `.lang clr`       → 清除覆盖，回到平台/全局默认
    // C#46：显示名走 _meta.name → 本包 lang.name → 语言码（自定义语言没配 lang.name
    // 时不会误落回默认语言的名字）。
    std::string langName(Locale l) const { return i18n_.localeDisplayName(l); }

    // Map free-form user input to a locale; std::nullopt if unrecognized.
    static std::optional<Locale> parseLocaleInput(const std::string& raw) {
        std::string s = toLower(trim(raw));
        if (s == "zh-hans" || s == "zh-cn" || s == "zh_cn" || s == "cn" || s == "hans" ||
            s == "简体" || s == "簡體" || s == "简中" || s == "簡中" ||
            s == "简体中文" || s == "簡體中文" || s == "简" || s == "簡")
            return Locale::kZhHans;
        if (s == "zh-hant" || s == "zh-tw" || s == "zh_tw" || s == "zh-hk" || s == "tw" || s == "hant" ||
            s == "繁體" || s == "繁体" || s == "繁中" ||
            s == "繁體中文" || s == "繁体中文" || s == "繁")
            return Locale::kZhHant;
        if (s == "en" || s.rfind("en-", 0) == 0 || s.rfind("en_", 0) == 0 ||
            s == "english" || s == "英文" || s == "英语" || s == "英語")
            return Locale::kEn;
        // Japanese
        if (s == "ja" || s.rfind("ja-", 0) == 0 || s.rfind("ja_", 0) == 0 ||
            s == "japanese" || s == "日本語" || s == "日文" || s == "日语" || s == "日語")
            return Locale::kJa;
        return std::nullopt;
    }

    std::string handleLang(Locale loc, const std::string& args, const Message& msg) {
        const bool priv = (msg.type == MessageType::kPrivate);
        const std::string scope   = priv ? "user" : "group";
        const std::string scopeKey = msg.platform + ":" + (priv ? msg.senderId : msg.targetId);

        std::string a = trim(args), la = toLower(a);
        if (a.empty() || la == "show") {
            // C#68：设了 AI 翻译语言 → 显示它（基础语言链仍在，翻译发送前才发生）。
            std::string ai = aiLangFor(msg);
            if (!ai.empty()) return i18n_.tr(loc, "lang.current_ai", {{"lang", ai}});
            // Current effective locale (`loc` is what the resolver already chose).
            bool overridden = resolver_.getOverride(scope, scopeKey).has_value();
            return i18n_.tr(loc, overridden ? "lang.current" : "lang.current_default",
                            {{"lang", langName(loc)}});
        }
        // C#49：群回复语言的变更限 群管理/群主/邀请人/骰主（私聊改自己不受限）。
        if (!priv && !senderIsGroupAdmin(msg)) return i18n_.tr(loc, "gate.no_perm");
        if (la == "clr" || la == "off" || la == "default" || la == "reset") {
            resolver_.clearOverride(scope, scopeKey);
            setAiLang(msg, "");                    // C#68：一并清除 AI 翻译语言
            Locale now = resolver_.resolve(msg);   // reverted default
            return i18n_.tr(now, "lang.cleared", {{"lang", langName(now)}});
        }
        auto target = parseLocaleInput(a);
        // C#46：内置别名没匹配到 → 查自定义翻译文件声明的 _meta.keywords（如 ko/韩语/한국어）。
        if (!target) target = i18n_.localeForKeyword(a);
        // C#68：仍没匹配到 → 查骰主定义的 AI 翻译语言（无 i18n 文件，发送前大模型翻译）。
        if (!target) {
            std::string ai = aitrans::matchLang(cfg_, a);
            if (!ai.empty()) {
                setAiLang(msg, ai);
                return i18n_.tr(loc, priv ? "lang.ai_set_user" : "lang.ai_set", {{"lang", ai}});
            }
        }
        if (!target) return i18n_.tr(loc, "lang.usage", {{"lang", langName(loc)}});
        resolver_.setOverride(scope, scopeKey, *target);
        setAiLang(msg, "");                        // C#68：切到真实语言时清除 AI 翻译语言
        // Reply in the NEW language so the confirmation is in what they picked.
        return i18n_.tr(*target, priv ? "lang.set_user" : "lang.set", {{"lang", langName(*target)}});
    }

    // ─── C#68 阶段三：AI 翻译语言（.lang 切自定义语言，发送前大模型翻译）────
public:
    /// 本会话（群/私聊用户）设置的 AI 翻译目标语言显示名；未设置返回空。
    std::string aiLangFor(const Message& msg) {
        return msg.type == MessageType::kPrivate ? getUserSetting(msg, "aiLang")
                                                 : getGroupSetting(msg, "aiLang");
    }
    void setAiLang(const Message& msg, const std::string& name) {
        if (msg.type == MessageType::kPrivate) {
            if (name.empty()) clearUserSetting(msg, "aiLang");
            else setUserSetting(msg, "aiLang", name);
        } else {
            setGroupSetting(msg, "aiLang", name);   // 空值即视为未设置
        }
    }
private:

    // C#107：词指令入口 → 完整团务实现（tryHandleGame）。旧的占位雏形已移除。
    std::string handleGame(Locale loc, const std::string& args, const Message& msg) {
        auto r = tryHandleGame(loc, msg, args.empty() ? std::string("game") : "game " + args);
        return r ? *r : i18n_.tr(loc, "help.topic.game");
    }

    // ─── .log 跑团记录（消息转录） ───────────────────────────

    /// 平台缩写（C#1 日志文件名前缀）：onebot_v11→q、discord→d、kook→k。
    static std::string platformAbbrev(const std::string& platform) {
        if (platform == "onebot_v11" || platform == "qq" || platform == "onebot") return "q";
        if (platform == "discord") return "d";
        if (platform == "kook") return "k";
        return platform.empty() ? "x" : std::string(1, platform[0]);
    }
    /// 清掉文件名非法字符（保留中文）。空则回退 "log"。
    static std::string sanitizeFileName(const std::string& s) {
        std::string r; r.reserve(s.size());
        for (char c : s) {
            unsigned char u = static_cast<unsigned char>(c);
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                c == '"' || c == '<' || c == '>' || c == '|' || u < 0x20) r += '_';
            else r += c;
        }
        return r.empty() ? std::string("log") : r;
    }
    /// 日志文件名：`<平台>_<群号>_<日志名>.txt`，如 q_114514_无尽食欲.txt（C#1）。
    std::string logFileName(const Message& msg, const std::string& logName,
                            const std::string& ext = "txt") const {
        return platformAbbrev(msg.platform) + "_" + msg.targetId + "_" + sanitizeFileName(logName) + "." + ext;
    }

    /// Render the just-ended log, upload it to the log site, and (on success)
    /// post the share URL (+ optionally the transcript file to the group's files).
    /// C#98：群文件格式可由群管 .log type txt|html 设置（日志站收自己的协议格式）。
    /// 上传协议默认 SealDice V1（config dice/logsite_format=legacy 走旧多段 txt POST）；
    /// 目标非官方站时回覆附加提示。@p sendFile=false 仅取链接（.log get / masterget）。
    void shareLog(const Message& msg, int logId, bool sendFile = true) {
        auto* st = db_.getLogStorage();
        if (!st) return;
        std::string txt = logsvc::renderSealdice(db_, logId, &cfg_);   // C#3：图片→稳定图床 URL
        if (txt.empty()) return;
        std::string logName = "log" + std::to_string(logId);
        std::string logGroup = msg.targetId;
        try {
            auto r = st->get<GameLogRow>(logId);
            if (!r.name.empty()) logName = r.name;
            if (!r.groupId.empty()) logGroup = r.groupId;   // masterget：日志属于别的群
        } catch (...) {}
        // C#98：群文件内容按本群设置的格式渲染（html=自包含网页含内嵌图）。
        std::string fileBody, fileName, path;
        if (sendFile) {
            const bool asHtml = getGroupSetting(msg, "logUploadType") == "html";
            fileBody = txt;
            if (asHtml) {
                std::string h = logsvc::renderHtml(db_, logId);
                if (!h.empty()) fileBody = std::move(h);
            }
            fileName = logFileName(msg, logName,
                (asHtml && fileBody != txt) ? "html" : "txt");   // q_<群号>_<日志名>.<ext> (C#1)
            // Write a local copy so the platform client can attach it as a group file.
            try {
                std::filesystem::create_directories("data/logs");
                path = u8str(std::filesystem::absolute(std::filesystem::path("data/logs") / fileName));
                std::ofstream f(path, std::ios::binary); f << fileBody;
            } catch (...) { path.clear(); }
        }

        const std::string url = logsvc::uploadUrl(cfg_);
        const bool official = (url == logsvc::kOfficialLogsite);
        const Locale loc = localeForGroup(msg);
        const std::string adapterId = msg.adapterId, groupId = msg.targetId;
        auto onDone = [this, loc, adapterId, groupId, path, fileBody, fileName, sendFile, official](
                          bool success, std::string res) {
            auto a = adapters_.getAdapter(adapterId);
            if (!a) return;
            if (success) {
                std::string out = i18n_.tr(loc, "log.uploaded", {{"url", res}});
                // 对齐海豹：自定义日志站时提示用户注意（并非官方站点）。
                if (!official) out += "\n" + i18n_.tr(loc, "log.unofficial_site");
                a->sendGroupMessage(groupId, out);
                if (sendFile) {
                    // Pass the raw content (sent as base64://) so a remote OneBot
                    // needn't read our local path; keep the on-disk copy as fallback.
                    a->uploadGroupFile(groupId, fileName, fileBody, path);
                }
            } else {
                a->sendGroupMessage(groupId, i18n_.tr(loc, "log.upload_failed", {{"error", res}}));
            }
        };
        std::string fmt = logsvc::uploadFormat(cfg_);
        if (fmt == "legacy") {
            // 旧自建端点协议：POST 多段 txt，uniform_id 须每次唯一（<gid>:<ts>）。
            logsvc::upload(url, logName, logsvc::makeUniformId(logGroup), txt, std::move(onDone));
        } else if (fmt == "seal_v105") {
            // SealDice V105：PUT Parquet(zstd) 列式文件。
            logsvc::uploadSealV105(url, logName, logGroup,
                logsvc::renderSealParquet(db_, logId, &cfg_, msg.selfId), std::move(onDone));
        } else if (fmt == "dicenext") {
            // DiceNext 专属：PUT zstd 压缩的 items JSON（轻量，配套自建日志站/染色器）。
            logsvc::uploadDiceNext(url, logName, logGroup,
                logsvc::renderDiceNextData(db_, logId, &cfg_, msg.selfId), std::move(onDone));
        } else {
            // SealDice V1 协议（默认）：PUT zlib 压缩 items JSON，标准 UniformID。
            logsvc::uploadSeal(url, logName, logGroup,
                logsvc::renderSealItems(db_, logId, &cfg_, msg.selfId), std::move(onDone));
        }
    }

    int activeLogId(const Message& msg) const {
        std::string v = getGroupSetting(msg, "activeLog");
        if (v.empty()) return 0;
        try { return std::stoi(v); } catch (...) { return 0; }
    }

    /// Date (YYYY-MM-DD) of the last message recorded in a log; "-" if none.
    std::string lastLogDate(int logId) {
        auto* st = db_.getLogStorage();
        if (!st) return "-";
        try { namespace orm = sqlite_orm;
            auto rows = st->get_all<GameLogMessageRow>(
                orm::where(orm::c(&GameLogMessageRow::logId) == logId),
                orm::order_by(&GameLogMessageRow::id).desc(), orm::limit(1));
            if (!rows.empty()) return rows.front().createdAt.substr(0, 10);
        } catch (...) {}
        return "-";
    }

    // ── 跑团计时（C#9）：挂在 .log 生命周期上累计每个记录的跑团时长 ──────────
    // 数据存 group_settings：logTimerTotal:<id>(累计秒) / logTimerStart:<id>(本次开始
    // 的 epoch，空=未在跑) / logTimerLastEnd:<id>(上次停止 epoch)；开关 logTimerOff=1。
    static long parseLongOr(const std::string& s, long d) { try { return s.empty() ? d : std::stol(s); } catch (...) { return d; } }
    static long nowEpoch() { return (long)std::time(nullptr); }
    bool timerEnabled(const Message& msg) const { return getGroupSetting(msg, "logTimerOff") != "1"; }
    std::string fmtDuration(Locale loc, long secs) const {
        if (secs < 0) secs = 0;
        long h = secs / 3600, m = (secs % 3600) / 60, s = secs % 60;
        std::string out; char b[32];
        if (h > 0) { snprintf(b, sizeof b, "%ld", h); out += b; out += i18n_.tr(loc, "log.timer.unit_h"); }
        if (h > 0 || m > 0) { snprintf(b, sizeof b, "%ld", m); out += b; out += i18n_.tr(loc, "log.timer.unit_m"); }
        snprintf(b, sizeof b, "%ld", s); out += b; out += i18n_.tr(loc, "log.timer.unit_s");
        return out;
    }
    void timerStart(const Message& msg, int id) {   // idempotent：已在跑则不动
        if (id <= 0 || !timerEnabled(msg)) return;
        std::string k = "logTimerStart:" + std::to_string(id);
        if (getGroupSetting(msg, k).empty()) setGroupSetting(msg, k, std::to_string(nowEpoch()));
    }
    long timerStop(const Message& msg, int id) {    // 累加本次到 total；返回本次秒数(-1=没在跑)
        if (id <= 0) return -1;
        std::string k = "logTimerStart:" + std::to_string(id), sv = getGroupSetting(msg, k);
        if (sv.empty()) return -1;
        long cur = nowEpoch() - parseLongOr(sv, 0); if (cur < 0) cur = 0;
        long total = parseLongOr(getGroupSetting(msg, "logTimerTotal:" + std::to_string(id)), 0) + cur;
        setGroupSetting(msg, "logTimerTotal:" + std::to_string(id), std::to_string(total));
        setGroupSetting(msg, k, "");
        setGroupSetting(msg, "logTimerLastEnd:" + std::to_string(id), std::to_string(nowEpoch()));
        return cur;
    }
    long timerTotal(const Message& msg, int id) const {   // 含进行中本次
        long t = parseLongOr(getGroupSetting(msg, "logTimerTotal:" + std::to_string(id)), 0);
        std::string sv = getGroupSetting(msg, "logTimerStart:" + std::to_string(id));
        if (!sv.empty()) { long c = nowEpoch() - parseLongOr(sv, 0); if (c > 0) t += c; }
        return t;
    }
    // 计时后缀：开始/继续 → 追加到 .log new/on 回复；停止 → 追加到 .log off/end 回复。
    std::string timerSuffixStart(Locale loc, const Message& msg, int id, bool fresh) {
        if (!timerEnabled(msg)) return "";
        timerStart(msg, id);
        if (fresh) return "\n" + i18n_.tr(loc, "log.timer.started");
        return "\n" + i18n_.tr(loc, "log.timer.resumed", {{"total", fmtDuration(loc, timerTotal(msg, id))}});
    }
    std::string timerSuffixStop(Locale loc, const Message& msg, int id) {
        if (!timerEnabled(msg)) return "";
        long cur = timerStop(msg, id);
        if (cur < 0) return "";   // 没在计时（如计时开关后建的记录）
        return "\n" + i18n_.tr(loc, "log.timer.stopped",
            {{"current", fmtDuration(loc, cur)}, {"total", fmtDuration(loc, timerTotal(msg, id))}});
    }

    std::string handleLog(Locale loc, const std::string& args, const Message& msg) {
        if (msg.type == MessageType::kPrivate)
            return i18n_.tr(loc, "log.group_only");
        auto* st = db_.getLogStorage();   // game_logs / game_log_messages live in logs.db
        if (!st) return i18n_.tr(loc, "log.usage");
        namespace orm = sqlite_orm;

        // first token = subcommand, remainder = optional name
        std::string sub = args, name;
        if (auto sp = args.find(' '); sp != std::string::npos) {
            sub = args.substr(0, sp); name = trim(args.substr(sp + 1));
        }
        sub = toLower(trim(sub));
        int active = activeLogId(msg);

        try {
            if (sub == "new") {
                // Refuse to start a new log while one is already running — tell the
                // user which one is active so they can .log end it first.
                if (active > 0) {
                    std::string cur = getGroupSetting(msg, "activeLogName");
                    if (cur.empty()) cur = "log" + std::to_string(active);
                    return i18n_.tr(loc, "log.exists", {{"name", cur}});
                }
                GameLogRow row;
                row.groupId = msg.targetId;
                row.gmId = msg.senderId;
                row.name = name;
                row.players = name.empty() ? "[]" : ("[\"" + name + "\"]");
                // 跨群团务下，日志仍按群独立存储，但记下所属团以供 WebUI 聚合/跨团导出。
                {
                    std::string code = gGet(msg, "__code"), gameName = gGet(msg, "__name");
                    row.customRules = code.empty() ? "{}" : nlohmann::json{{"gameCode", code}, {"gameName", gameName}}.dump();
                }
                row.status = 0;
                row.createdAt = nowIso();
                int id = st->insert(row);
                std::string logName = name.empty() ? "log" + std::to_string(id) : name;
                if (name.empty()) { try { auto r = st->get<GameLogRow>(id); r.name = logName; st->update(r); } catch (...) {} }
                setGroupSetting(msg, "activeLog", std::to_string(id));
                setGroupSetting(msg, "activeLogName", logName);
                return i18n_.tr(loc, "log.new", {{"name", logName}}) + timerSuffixStart(loc, msg, id, true);
            }
            if (sub == "list") {
                auto all = st->get_all<GameLogRow>(
                    orm::where(orm::c(&GameLogRow::groupId) == msg.targetId),
                    orm::order_by(&GameLogRow::id).desc());   // 最近的在前
                int total = (int)all.size();
                if (total == 0) return i18n_.tr(loc, "log.list_empty");
                const int per = 5;
                int pages = (total + per - 1) / per;
                int page = 1;
                { std::string p = trim(name); if (isAllDigits(p) && !p.empty()) page = parseIntOr(p, 1); }
                if (page < 1) page = 1; if (page > pages) page = pages;
                int from = (page - 1) * per, to = from + per; if (to > total) to = total;
                std::ostringstream body;
                for (int i = from; i < to; ++i) {
                    const auto& g = all[i];
                    std::string nm = g.name.empty() ? ("log" + std::to_string(g.id)) : g.name;
                    const char* stateKey = g.status == 0 ? "log.state_running"
                        : g.status == 1 ? "log.state_paused" : "log.state_ended";
                    body << i18n_.tr(loc, "log.list_item",
                        {{"id", std::to_string(g.id)}, {"name", nm}, {"date", lastLogDate(g.id)},
                         {"state", i18n_.tr(loc, stateKey)}});
                    if (i < to - 1) body << "\n";
                }
                std::string more = (page < pages)
                    ? i18n_.tr(loc, "log.list_more", {{"page", std::to_string(page + 1)}}) : "";
                return i18n_.tr(loc, "log.list", {{"total", std::to_string(total)},
                    {"from", std::to_string(from + 1)}, {"to", std::to_string(to)},
                    {"list", body.str()}, {"more", more}});
            }
            if (sub == "on") {
                if (active > 0) {
                    std::string nm = getGroupSetting(msg, "activeLogName");
                    if (nm.empty()) { try { nm = st->get<GameLogRow>(active).name; } catch (...) {} }
                    if (nm.empty()) nm = "log" + std::to_string(active);
                    int c = (int)st->count<GameLogMessageRow>(orm::where(orm::c(&GameLogMessageRow::logId) == active));
                    return i18n_.tr(loc, "log.already_on",
                        {{"id", std::to_string(active)}, {"name", nm}, {"count", std::to_string(c)}});
                }
                std::vector<GameLogRow> rows;
                if (!name.empty()) {
                    rows = st->get_all<GameLogRow>(
                        orm::where(orm::c(&GameLogRow::groupId) == msg.targetId
                            and orm::c(&GameLogRow::name) == name
                            and orm::c(&GameLogRow::status) == 1),
                        orm::order_by(&GameLogRow::id).desc(), orm::limit(1));
                } else {
                    rows = st->get_all<GameLogRow>(
                        orm::where(orm::c(&GameLogRow::groupId) == msg.targetId
                            and orm::c(&GameLogRow::status) == 1),
                        orm::order_by(&GameLogRow::id).desc(), orm::limit(1));
                }
                if (rows.empty()) {
                    // 没有任何历史日志时，.log on 直接当作新建一个（原版/用户预期：
                    // 不必先 .log new 才能开始记录），自动起名 logN 并入库。
                    auto any = st->get_all<GameLogRow>(
                        orm::where(orm::c(&GameLogRow::groupId) == msg.targetId), orm::limit(1));
                    if (!any.empty()) {
                        return name.empty() ? i18n_.tr(loc, "log.no_paused")
                                            : i18n_.tr(loc, "log.not_found", {{"name", name}});
                    }
                    GameLogRow nr;
                    nr.groupId = msg.targetId; nr.gmId = msg.senderId;
                    nr.players = "[]"; nr.status = 0;
                    {
                        std::string code = gGet(msg, "__code"), gameName = gGet(msg, "__name");
                        nr.customRules = code.empty() ? "{}" : nlohmann::json{{"gameCode", code}, {"gameName", gameName}}.dump();
                    }
                    nr.createdAt = nowIso();
                    int id = st->insert(nr);
                    std::string logName = "log" + std::to_string(id);
                    try { auto r = st->get<GameLogRow>(id); r.name = logName; st->update(r); } catch (...) {}
                    setGroupSetting(msg, "activeLog", std::to_string(id));
                    setGroupSetting(msg, "activeLogName", logName);
                    return i18n_.tr(loc, "log.new", {{"name", logName}}) + timerSuffixStart(loc, msg, id, true);
                }
                auto r = rows.front(); r.status = 0; st->update(r);
                setGroupSetting(msg, "activeLog", std::to_string(r.id));
                std::string resumedName = r.name.empty() ? ("log" + std::to_string(r.id)) : r.name;
                setGroupSetting(msg, "activeLogName", resumedName);
                std::string timerSuf = timerSuffixStart(loc, msg, r.id, false);   // 继续计时
                // Resuming a paused log: quote the previous .log off message and
                // say "paused here, recording resumed". Falls back to the plain
                // reply if we never recorded an off message (e.g. fresh .log on).
                std::string offId = getGroupSetting(msg, "lastLogOffMsgId");
                if (!offId.empty()) {
                    quoteOverride_ = offId;
                    setGroupSetting(msg, "lastLogOffMsgId", "");
                    return i18n_.tr(loc, "log.resumed", {{"name", resumedName}}) + timerSuf;
                }
                return i18n_.tr(loc, "log.on", {{"name", resumedName}}) + timerSuf;
            }
            if (sub == "off") {
                if (active <= 0) return i18n_.tr(loc, "log.not_recording");
                try { auto r = st->get<GameLogRow>(active); r.status = 1; st->update(r); } catch (...) {}
                setGroupSetting(msg, "activeLog", "");
                // Remember THIS .log off message so the next .log on can quote it.
                setGroupSetting(msg, "lastLogOffMsgId", msg.id);
                int cnt = (int)st->count<GameLogMessageRow>(orm::where(orm::c(&GameLogMessageRow::logId) == active));
                return i18n_.tr(loc, "log.off", {{"count", std::to_string(cnt)}}) + timerSuffixStop(loc, msg, active);
            }
            if (sub == "end") {
                // .log end works even when paused (off): end the active log if any,
                // otherwise the most recent not-yet-ended log for this group.
                int target = active;
                if (target <= 0) {
                    auto rows = st->get_all<GameLogRow>(
                        orm::where(orm::c(&GameLogRow::groupId) == msg.targetId
                                   and orm::c(&GameLogRow::status) != 2),
                        orm::order_by(&GameLogRow::id).desc(), orm::limit(1));
                    if (rows.empty()) return i18n_.tr(loc, "log.not_recording");
                    target = rows.front().id;
                }
                try { auto r = st->get<GameLogRow>(target); r.status = 2; st->update(r); } catch (...) {}
                std::string timerSuf = timerSuffixStop(loc, msg, target);   // 结算计时
                setGroupSetting(msg, "activeLog", "");
                setGroupSetting(msg, "activeLogName", "");
                int cnt = (int)st->count<GameLogMessageRow>(orm::where(orm::c(&GameLogMessageRow::logId) == target));
                shareLog(msg, target);   // auto-upload to log site + send txt to group file
                return i18n_.tr(loc, "log.ended", {{"count", std::to_string(cnt)}}) + timerSuf;
            }
            // C#98：.log type [txt|html] —— 群管设置 .log end 上传到群文件的格式
            //（html=自包含网页含内嵌图片；日志站始终收 txt）。无参查看当前格式。
            if (sub == "type") {
                std::string a2 = toLower(trim(name));
                if (a2.empty()) {
                    std::string cur = getGroupSetting(msg, "logUploadType");
                    if (cur.empty()) cur = "txt";
                    return i18n_.tr(loc, "log.type_show", {{"type", cur}});
                }
                if (a2 != "txt" && a2 != "html") return i18n_.tr(loc, "log.type_usage");
                if (groupTrustOf(msg) < 0) return i18n_.tr(loc, "gate.no_perm");   // 群管/信任以上
                setGroupSetting(msg, "logUploadType", a2);
                return i18n_.tr(loc, "log.type_set", {{"type", a2}});
            }
            // ── 海豹对齐子指令（halt/get/del/stat/export/masterget）────────
            // 按名称找某群日志（空名=最近一份）。返回 0=没有。
            auto findLog = [&](const std::string& gid, const std::string& nm) -> int {
                if (!nm.empty()) {
                    auto rows = st->get_all<GameLogRow>(
                        orm::where(orm::c(&GameLogRow::groupId) == gid
                                   and orm::c(&GameLogRow::name) == nm),
                        orm::order_by(&GameLogRow::id).desc(), orm::limit(1));
                    return rows.empty() ? 0 : rows.front().id;
                }
                auto rows = st->get_all<GameLogRow>(
                    orm::where(orm::c(&GameLogRow::groupId) == gid),
                    orm::order_by(&GameLogRow::id).desc(), orm::limit(1));
                return rows.empty() ? 0 : rows.front().id;
            };
            // .log halt：强行结束当前记录，不上传（海豹）。
            if (sub == "halt") {
                if (active <= 0) return i18n_.tr(loc, "log.not_recording");
                try { auto r = st->get<GameLogRow>(active); r.status = 2; st->update(r); } catch (...) {}
                std::string timerSuf = timerSuffixStop(loc, msg, active);
                setGroupSetting(msg, "activeLog", "");
                setGroupSetting(msg, "activeLogName", "");
                return i18n_.tr(loc, "log.halted") + timerSuf;
            }
            // .log get [名称]：重新上传并获取链接（不发群文件；海豹）。
            if (sub == "get") {
                int id = findLog(msg.targetId, trim(name));
                if (id <= 0) return i18n_.tr(loc, "log.not_found", {{"name", trim(name)}});
                shareLog(msg, id, false);
                return i18n_.tr(loc, "log.getting");
            }
            // .log del/rm <名称>：删除一份日志（连消息与缓存图；海豹）。
            if (sub == "del" || sub == "rm") {
                std::string nm = trim(name);
                if (nm.empty()) return i18n_.tr(loc, "log.del_usage");
                int id = findLog(msg.targetId, nm);
                if (id <= 0) return i18n_.tr(loc, "log.not_found", {{"name", nm}});
                if (id == active) { setGroupSetting(msg, "activeLog", ""); setGroupSetting(msg, "activeLogName", ""); }
                st->remove_all<GameLogMessageRow>(orm::where(orm::c(&GameLogMessageRow::logId) == id));
                st->remove_all<GameLogRow>(orm::where(orm::c(&GameLogRow::id) == id));
                try {   // 同步清缓存图 data/logs/images/log<id>_*
                    namespace fs = std::filesystem;
                    std::string prefix = "log" + std::to_string(id) + "_";
                    if (fs::exists("data/logs/images"))
                        for (auto& e : fs::directory_iterator("data/logs/images"))
                            if (e.is_regular_file()
                                && u8str(e.path().filename()).rfind(prefix, 0) == 0) {
                                std::error_code ec; fs::remove(e.path(), ec);
                            }
                } catch (...) {}
                return i18n_.tr(loc, "log.deleted", {{"name", nm}});
            }
            // .log stat [名称]：条数/时段/参与者摘要（简版统计；海豹为骰点统计）。
            if (sub == "stat") {
                int id = findLog(msg.targetId, trim(name));
                if (id <= 0) return i18n_.tr(loc, "log.not_found", {{"name", trim(name)}});
                auto msgs = st->get_all<GameLogMessageRow>(
                    orm::where(orm::c(&GameLogMessageRow::logId) == id),
                    orm::order_by(&GameLogMessageRow::id).asc());
                if (msgs.empty()) return i18n_.tr(loc, "log.stat_empty");
                std::map<std::string, int> per;
                for (auto& m : msgs) per[m.sender]++;
                std::vector<std::pair<std::string, int>> v(per.begin(), per.end());
                std::sort(v.begin(), v.end(), [](auto& x, auto& y) { return x.second > y.second; });
                std::string tops;
                int shown = 0;
                for (auto& [who, n] : v) { if (shown++ >= 5) break; tops += "\n  " + who + " × " + std::to_string(n); }
                std::string nm; try { nm = st->get<GameLogRow>(id).name; } catch (...) {}
                if (nm.empty()) nm = "log" + std::to_string(id);
                return i18n_.tr(loc, "log.stat", {{"name", nm},
                    {"count", std::to_string(msgs.size())},
                    {"players", std::to_string(per.size())},
                    {"from", logsvc::slashTime(msgs.front().createdAt)},
                    {"to", logsvc::slashTime(msgs.back().createdAt)},
                    {"tops", tops}});
            }
            // .log export [名称]：直接把日志文件发到群文件，不上传日志站（海豹）。
            if (sub == "export") {
                int id = findLog(msg.targetId, trim(name));
                if (id <= 0) return i18n_.tr(loc, "log.not_found", {{"name", trim(name)}});
                std::string txt = logsvc::renderSealdice(db_, id, &cfg_);
                if (txt.empty()) return i18n_.tr(loc, "log.stat_empty");
                std::string nm; try { nm = st->get<GameLogRow>(id).name; } catch (...) {}
                if (nm.empty()) nm = "log" + std::to_string(id);
                const bool asHtml = getGroupSetting(msg, "logUploadType") == "html";
                std::string body = txt;
                if (asHtml) { std::string h = logsvc::renderHtml(db_, id); if (!h.empty()) body = std::move(h); }
                std::string fn = logFileName(msg, nm, (asHtml && body != txt) ? "html" : "txt");
                std::string p;
                try {
                    std::filesystem::create_directories("data/logs");
                    p = u8str(std::filesystem::absolute(std::filesystem::path("data/logs") / fn));
                    std::ofstream f(p, std::ios::binary); f << body;
                } catch (...) { p.clear(); }
                if (auto a = adapters_.getAdapter(msg.adapterId)) a->uploadGroupFile(msg.targetId, fn, body, p);
                return i18n_.tr(loc, "log.exported", {{"name", nm}});
            }
            // .log masterget <群号> [名称]：骰主取任意群日志的链接（海豹）。
            if (sub == "masterget") {
                if (!isMaster(msg)) return i18n_.tr(loc, "gate.not_master");
                std::istringstream iss(name);
                std::string gidArg, nmArg;
                iss >> gidArg; std::getline(iss, nmArg); nmArg = trim(nmArg);
                if (gidArg.empty()) return i18n_.tr(loc, "log.masterget_usage");
                int id = findLog(gidArg, nmArg);
                if (id <= 0) return i18n_.tr(loc, "log.not_found", {{"name", nmArg.empty() ? gidArg : nmArg}});
                shareLog(msg, id, false);   // 链接回到当前窗口
                return i18n_.tr(loc, "log.getting");
            }
            // .log timer on/off：本群跑团计时开关（无参显示状态）。
            if (sub == "timer") {
                std::string a = toLower(trim(name));
                if (a == "off") { setGroupSetting(msg, "logTimerOff", "1"); return i18n_.tr(loc, "log.timer.disabled_set"); }
                if (a == "on")  { setGroupSetting(msg, "logTimerOff", "");  return i18n_.tr(loc, "log.timer.enabled_set"); }
                return i18n_.tr(loc, timerEnabled(msg) ? "log.timer.state_on" : "log.timer.state_off");
            }
            // .log time / .log 时长：查看当前（或最近）记录的累计时长。
            if (sub == "time" || sub == "\xe6\x97\xb6\xe9\x95\xbf" || sub == "\xe8\xae\xa1\xe6\x97\xb6") {
                if (!timerEnabled(msg)) return i18n_.tr(loc, "log.timer.is_off");
                int id = active; std::string nm = getGroupSetting(msg, "activeLogName");
                if (id <= 0) {
                    auto rows = st->get_all<GameLogRow>(orm::where(orm::c(&GameLogRow::groupId) == msg.targetId),
                                                        orm::order_by(&GameLogRow::id).desc(), orm::limit(1));
                    if (rows.empty()) return i18n_.tr(loc, "log.timer.show_none");
                    id = rows.front().id; nm = rows.front().name;
                }
                if (nm.empty()) nm = "log" + std::to_string(id);
                std::string sv = getGroupSetting(msg, "logTimerStart:" + std::to_string(id));
                std::string running = sv.empty() ? "" :
                    i18n_.tr(loc, "log.timer.running", {{"current", fmtDuration(loc, nowEpoch() - parseLongOr(sv, 0))}});
                return i18n_.tr(loc, "log.timer.show",
                    {{"name", nm}, {"total", fmtDuration(loc, timerTotal(msg, id))}, {"running", running}});
            }
            // status / get / empty
            if (active <= 0) {
                auto paused = st->get_all<GameLogRow>(
                    orm::where(orm::c(&GameLogRow::groupId) == msg.targetId
                        and orm::c(&GameLogRow::status) == 1),
                    orm::order_by(&GameLogRow::id).desc(), orm::limit(1));
                if (!paused.empty()) {
                    const auto& g = paused.front();
                    std::string nm = g.name.empty() ? ("log" + std::to_string(g.id)) : g.name;
                    return i18n_.tr(loc, "log.paused_status", {{"id", std::to_string(g.id)},
                        {"name", nm}, {"date", lastLogDate(g.id)}});
                }
                return i18n_.tr(loc, "log.idle");
            }
            int cnt = (int)st->count<GameLogMessageRow>(orm::where(orm::c(&GameLogMessageRow::logId) == active));
            std::string nm = getGroupSetting(msg, "activeLogName");
            if (nm.empty()) { try { nm = st->get<GameLogRow>(active).name; } catch (...) {} }
            if (nm.empty()) nm = "log" + std::to_string(active);
            return i18n_.tr(loc, "log.status", {{"id", std::to_string(active)}, {"name", nm}, {"count", std::to_string(cnt)}});
        } catch (const std::exception& e) {
            return i18n_.tr(loc, "log.usage");
        }
    }

public:
    /// Render a custom-reply template, substituting {variables}:
    ///   {nick}/{user}/{self}/{group}/{date}/{time}, {roll:EXPR} (dice total),
    ///   {draw:DECK} (deck draw), {$N} (regex capture group N), {a|b|c} (random).
    /// @p matchContent/@p type are used to extract regex capture groups.
    std::string renderReply(const Message& msg, const std::string& tmpl,
                            const std::string& matchContent, MatchType type) {
        std::vector<std::string> groups;
        if (type == MatchType::kRegex) {
            try {
                std::regex re(matchContent, std::regex::ECMAScript | std::regex::icase);
                std::smatch m;
                // Anchored at start, mirroring ReplyMatcher::matchRegex, so the
                // captured groups line up with what actually triggered the rule.
                if (std::regex_search(msg.content, m, re, std::regex_constants::match_continuous))
                    for (auto& g : m) groups.push_back(g.str());
            } catch (...) {}
        } else if (type == MatchType::kPrefix) {
            // Prefix match exposes everything after the prefix as the 1st (and only)
            // capture group {$1}; {$0} is the whole message (mirrors regex groups).
            groups.push_back(msg.content);
            if (msg.content.size() >= matchContent.size())
                groups.push_back(msg.content.substr(matchContent.size()));
            else
                groups.push_back("");
        }
        // C#28: Pre-process legacy variable aliases ({pc}→{nick}, {char}→{nick}, etc.)
        // so that original-Dice! flavor texts work with DiceNext's variable system.
        std::string processed = applyLegacyVarAliases(tmpl);
        std::string out; out.reserve(processed.size() + 32);
        for (size_t i = 0; i < processed.size();) {
            if (processed[i] == '{') {
                size_t end = processed.find('}', i + 1);
                if (end == std::string::npos) { out += processed[i++]; continue; }
                out += resolveReplyToken(msg, processed.substr(i + 1, end - i - 1), groups);
                i = end + 1;
            } else { out += processed[i++]; }
        }
        return out;
    }

private:
    std::string resolveReplyToken(const Message& msg, const std::string& tok,
                                  const std::vector<std::string>& groups) {
        // Random choice: {a|b|c}
        if (tok.find('|') != std::string::npos) {
            std::vector<std::string> opts;
            std::string cur;
            for (char c : tok) { if (c == '|') { opts.push_back(cur); cur.clear(); } else cur += c; }
            opts.push_back(cur);
            return opts.empty() ? std::string() : pickFrom(opts);
        }
        if (tok == "nick") {
            std::string n = resolveNickDisplay(msg);
            std::string pre = cfg_.get<std::string>("dice/nick_prefix", std::string("<"));
            std::string suf = cfg_.get<std::string>("dice/nick_suffix", std::string(">"));
            return pre + n + suf;
        }
        if (tok == "name") return displayName(msg);    // 人物卡名 > .nn > {nick}
        if (tok == "user") return msg.senderId;
        // 变量三来源 × 乾净/带包裹（用户可按需选择是否显示包裹符号）。
        if (tok == "qqnick")  return qqNickOf(msg);               // QQ昵称（乾净）
        if (tok == "card")    return groupCardOf(msg);            // 群名片（乾净，无则空）
        if (tok == "pcname")  return pcNameOf(msg);               // 人物卡名（乾净，无则空）
        if (tok == "qqnickw") return nickWrap(qqNickOf(msg));     // QQ昵称（带包裹）
        if (tok == "cardw")   return nickWrap(groupCardOf(msg));  // 群名片（带包裹）
        if (tok == "pcnamew") return nickWrap(pcNameOf(msg));     // 人物卡名（带包裹）
        if (tok == "self") return resolveSelfCall(msg);          // 自称 (原版 {self})
        if (tok == "strSelfName") return resolveSelfName(msg);
        if (tok == "strSelfCall") return resolveSelfCall(msg);
        if (tok == "selfId") return msg.selfId;                  // 机器人 QQ 号 (原 {self} 含义)
        if (tok == "group") return msg.targetId;
        if (tok == "date") { std::time_t t=std::time(nullptr); char b[16]; std::tm lt{};
#if defined(_WIN32)
            localtime_s(&lt,&t);
#else
            lt=*std::localtime(&t);
#endif
            std::strftime(b,sizeof(b),"%Y-%m-%d",&lt); return b; }
        if (tok == "time") { std::time_t t=std::time(nullptr); char b[16]; std::tm lt{};
#if defined(_WIN32)
            localtime_s(&lt,&t);
#else
            lt=*std::localtime(&t);
#endif
            std::strftime(b,sizeof(b),"%H:%M:%S",&lt); return b; }
        if (tok.rfind("roll:", 0) == 0) {
            auto r = engine_.roll(trim(tok.substr(5)));
            return r.ok() ? std::to_string(r.modifiedTotal) : std::string("?");
        }
        if (tok.rfind("draw:", 0) == 0) {
            std::string name = trim(tok.substr(5));
            if (!deck_.has(name)) return std::string("?");
            return deck_.drawFromDeck(name).value_or("");
        }
        if (tok.rfind("api:", 0) == 0) return fetchApi(trim(tok.substr(4)));   // {api:URL} 外部请求
        // C#29: {counter:name} — resolves to the current counter value (set by CausalRuleManager)
        if (tok.rfind("counter:", 0) == 0) {
            std::string cname = trim(tok.substr(8));
            auto it = counterContext_.find(cname);
            return it != counterContext_.end() ? it->second : std::string("0");
        }
        if (!tok.empty() && tok[0] == '$') {
            int n = parseIntOr(tok.substr(1), -1);
            if (n >= 0 && n < (int)groups.size()) return groups[n];
            return "";
        }
        return "{" + tok + "}";   // unknown → leave as-is
    }

    // ─── {api:URL} 外部 HTTP 请求 (#49) ──────────────────────
    // 默认关闭（config dice/api_enabled）。同步走 curl（与日志上传同路），带超时/
    // 体积上限；SSRF 防护：仅 http/https、拦截私网/环回/链路本地、可选白名单、拒绝
    // shell 不安全字符。结果裁剪后替换。失败/被拦截 → 空串。
    bool apiEnabled() const {
        try { json all = cfg_.getAll();
            if (all.contains("dice") && all["dice"].contains("api_enabled")) return all["dice"]["api_enabled"].get<bool>();
        } catch (...) {} return false;
    }
    int apiTimeout() const {
        try { json all = cfg_.getAll();
            if (all.contains("dice") && all["dice"].contains("api_timeout")) { int t = all["dice"]["api_timeout"].get<int>(); if (t > 0 && t <= 30) return t; }
        } catch (...) {} return 5;
    }
    std::vector<std::string> apiWhitelist() const {
        std::vector<std::string> v;
        try { json all = cfg_.getAll();
            if (all.contains("dice") && all["dice"].contains("api_whitelist") && all["dice"]["api_whitelist"].is_array())
                for (auto& e : all["dice"]["api_whitelist"]) if (e.is_string()) v.push_back(e.get<std::string>());
        } catch (...) {} return v;
    }
    // Scheme + shell-safety + SSRF host blocklist (localhost / LAN). No whitelist.
    bool isHostSafe(const std::string& url) const {
        std::string lo = toLower(url);
        if (lo.rfind("http://", 0) != 0 && lo.rfind("https://", 0) != 0) return false;
        for (unsigned char c : url) if (c <= 0x20 || c == '"' || c == '`' || c == '$' || c == '\\') return false;  // shell 安全
        size_t s = url.find("://"); if (s == std::string::npos) return false; s += 3;
        size_t e = url.find_first_of("/:?#", s);
        std::string host = toLower(url.substr(s, e == std::string::npos ? std::string::npos : e - s));
        if (host.empty()) return false;
        static const char* bad[] = {"localhost", "127.", "0.0.0.0", "::1", "[::1]", "10.", "192.168.", "169.254."};
        for (auto* b : bad) if (host.rfind(b, 0) == 0) return false;
        if (host.rfind("172.", 0) == 0) { int o2 = std::atoi(host.c_str() + 4); if (o2 >= 16 && o2 <= 31) return false; }
        if (host.size() >= 6 && host.compare(host.size() - 6, 6, ".local") == 0) return false;
        return true;
    }
    bool isApiUrlAllowed(const std::string& url) const {
        if (!isHostSafe(url)) return false;
        size_t s = url.find("://") + 3;
        size_t e = url.find_first_of("/:?#", s);
        std::string host = toLower(url.substr(s, e == std::string::npos ? std::string::npos : e - s));
        auto wl = apiWhitelist();
        if (!wl.empty()) {
            bool ok = false;
            for (auto& w : wl) if (!w.empty() && host.find(toLower(w)) != std::string::npos) { ok = true; break; }
            if (!ok) return false;
        }
        return true;
    }
    static std::string runCmdCapture(const std::string& cmd, size_t maxBytes = 65536) {
#if defined(_WIN32)
        FILE* p = _popen(cmd.c_str(), "r");
#else
        FILE* p = popen(cmd.c_str(), "r");
#endif
        if (!p) return "";
        std::string out; char buf[4096]; size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) { out.append(buf, n); if (out.size() > maxBytes) break; }
#if defined(_WIN32)
        _pclose(p);
#else
        pclose(p);
#endif
        return out;
    }
    std::string fetchApi(const std::string& url) {
        if (!apiEnabled() || !isApiUrlAllowed(url)) return "";
        std::string cmd = "curl -s --max-time " + std::to_string(apiTimeout()) +
                          " --max-filesize 65536 --proto =http,https \"" + url + "\"";
        std::string out = trim(runCmdCapture(cmd));
        // 裁剪：按码点近似截断到 ~600 字（避免刷屏）。
        if (out.size() > 600) { size_t cut = 600; while (cut < out.size() && (out[cut] & 0xC0) == 0x80) ++cut; out = out.substr(0, cut) + "\xe2\x80\xa6"; }
        return out;
    }

public:
    /// Return (and clear) any pending reply-quote override set during the last
    /// handleMessage(). Empty = quote the triggering message as usual. See #10.
    std::string takeQuoteOverride() { auto q = quoteOverride_; quoteOverride_.clear(); return q; }

    // C#29: Set/clear the counter context for {counter:name} resolution in renderReply.
    // CausalRuleManager calls setCounterContext() before rendering a causal reply,
    // then clearCounterContext() after.
    void setCounterContext(const std::map<std::string, std::string>& ctx) { counterContext_ = ctx; }
    void clearCounterContext() { counterContext_.clear(); }

private:
    /// C#28: Replace legacy original-Dice! variable placeholders with DiceNext
    /// equivalents so that flavor texts from GlobalVar.cpp render correctly.
    /// e.g. {pc}→{nick}, {char}→{nick}, {attr}→{$0}, {dice_exp}→{$0}
    static std::string applyLegacyVarAliases(const std::string& tmpl) {
        // Only process if the template contains a known legacy variable.
        // Simple string replacement (not regex) for performance and safety.
        if (tmpl.find('{') == std::string::npos) return tmpl;
        static const std::pair<std::string, std::string> aliases[] = {
            {"{pc}",       "{nick}"},
            {"{char}",     "{nick}"},
            {"{attr}",     "{$0}"},
            {"{dice_exp}", "{$0}"},
            {"{reason}",   "{$1}"},
            {"{turn}",     "{time}"},
        };
        std::string out = tmpl;
        for (auto& [from, to] : aliases) {
            size_t pos = 0;
            while ((pos = out.find(from, pos)) != std::string::npos) {
                out.replace(pos, from.size(), to);
                pos += to.size();
            }
        }
        return out;
    }

    std::map<std::string, std::string> counterContext_;  // C#29: {counter:name} → value

public:   // 以下方法供 main.cpp / api_service 调用（GLM 误插的 private: 把它们困住导致编译失败，恢复 public）
    /// Blacklist/trust ops for JS plugins' seal.ban.*  (op: ban/trust/remove).
    void jsBanOp(const std::string& op, const std::string& id, const std::string& reason) {
        if (id.empty()) return;
        if (op == "ban")        banlistAdd(0, 0, id, reason);      // user, blacklist
        else if (op == "trust") banlistAdd(0, 1, id, reason);      // user, whitelist(trust)
        else if (op == "remove") { banlistRemove(0, 0, id); banlistRemove(0, 1, id); }
    }

    /// Gated HTTP request for JS plugins' global fetch(). Reuses the external-API
    /// switch + SSRF/host blocklist + whitelist (#49). Method/headers/body are
    /// passed to curl via a config file (NOT the shell) so JSON bodies and header
    /// values can't inject. Returns the response body; @p status carries the HTTP
    /// code back (0 = blocked/failed). @p headerLines = "\n"-joined "K: V".
    std::string jsHttpFetch(const std::string& method, const std::string& url,
                            const std::string& headerLines, const std::string& body, int& status) {
        status = 0;
        if (!apiEnabled() || !isApiUrlAllowed(url)) return "";
        namespace fs = std::filesystem;
        auto esc = [](const std::string& s) {            // escape for curl config "..." values
            std::string o; o.reserve(s.size() + 8);
            for (char c : s) { if (c == '\\' || c == '"') o += '\\'; o += c; }
            return o;
        };
        std::string m;                                   // uppercase method
        for (char c : method) m += (char)std::toupper((unsigned char)c);
        if (m.empty()) m = "GET";

        static long long seq = 0;                        // unique temp names (called under jsMod mutex)
        long long id = ++seq;
        fs::path tmp = fs::temp_directory_path();
        fs::path cfgF  = tmp / ("dnfetch_" + std::to_string(id) + ".cfg");
        fs::path bodyF = tmp / ("dnfetch_" + std::to_string(id) + ".body");
        std::error_code ec;
        try {
            std::ofstream cf(cfgF, std::ios::binary);
            cf << "url = \"" << esc(url) << "\"\n";
            cf << "request = \"" << esc(m) << "\"\n";
            cf << "max-time = " << apiTimeout() << "\n";
            cf << "silent\nshow-error\nlocation\n";       // -sS -L
            cf << "max-filesize = 2097152\n";             // 2 MB cap
            cf << "proto = \"=http,https\"\n";
            cf << "write-out = \"\\n%{http_code}\"\n";     // append status on a new line
            std::istringstream hs(headerLines); std::string line;
            while (std::getline(hs, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) cf << "header = \"" << esc(line) << "\"\n";
            }
            if (!body.empty()) {
                std::ofstream bf(bodyF, std::ios::binary); bf << body;
                cf << "data-binary = \"@" << esc(bodyF.string()) << "\"\n";
            }
        } catch (...) { fs::remove(cfgF, ec); fs::remove(bodyF, ec); return ""; }

        std::string out = runCmdCapture("curl -K \"" + cfgF.string() + "\"", 2 * 1024 * 1024);
        fs::remove(cfgF, ec); fs::remove(bodyF, ec);

        auto nl = out.find_last_of('\n');                // split off the trailing status line
        if (nl != std::string::npos) {
            status = std::atoi(out.c_str() + nl + 1);
            out.erase(nl);
        }
        return out;
    }

    /// Gate-free GET for plugin update checks/downloads (admin-initiated from the
    /// panel). Enforces the SSRF host blocklist but NOT the api-enabled switch or
    /// whitelist (update URLs are arbitrary plugin homepages). @p status = HTTP code.
    std::string fetchPluginUrl(const std::string& url, int& status) {
        status = 0;
        if (!isHostSafe(url)) return "";
        std::string out = runCmdCapture(
            "curl -sSL --max-time 15 --max-filesize 2097152 --proto \"=http,https\" "
            "-w \"\\n%{http_code}\" \"" + url + "\"", 2 * 1024 * 1024);
        auto nl = out.find_last_of('\n');
        if (nl != std::string::npos) { status = std::atoi(out.c_str() + nl + 1); out.erase(nl); }
        return out;
    }

    /// Append a transcript line for every message while a group log is recording.
    /// Called from the message loop (records the user line and, if any, the reply).
    // ── C#3 日志内图片：提取 / 可选落地下载 ─────────────────────────
    bool saveLogImages() const { return cfg_.get<bool>("dice/save_log_images", false); }
    // 从原始消息提取图片引用（[CQ:image,url=/file=..] 与 海豹 [图片:..]/[图:..]）→ JSON 数组串。
    std::string extractImageRefs(const std::string& raw) const {
        std::vector<std::string> urls;
        auto unesc = [](std::string s) {
            auto rep = [&](const std::string& a, const std::string& b) {
                size_t p = 0; while ((p = s.find(a, p)) != std::string::npos) { s.replace(p, a.size(), b); p += b.size(); } };
            rep("&#44;", ","); rep("&#91;", "["); rep("&#93;", "]"); rep("&amp;", "&"); return s;
        };
        auto fieldOf = [&](const std::string& inner, const std::string& key) -> std::string {
            size_t k = inner.find(key + "=");
            if (k == std::string::npos) return "";
            k += key.size() + 1;
            size_t e = inner.find(',', k); if (e == std::string::npos) e = inner.size();
            return unesc(inner.substr(k, e - k));
        };
        // [CQ:image,..] 与 [img,..]（C#57 平台中立码，骰娘回复用）
        for (const std::string& tag : {std::string("[CQ:image"), std::string("[img,")}) {
            size_t p = 0;
            while ((p = raw.find(tag, p)) != std::string::npos) {
                size_t end = raw.find(']', p); if (end == std::string::npos) break;
                std::string inner = raw.substr(p, end - p);
                std::string u = fieldOf(inner, "url"); if (u.empty()) u = fieldOf(inner, "file");
                if (!u.empty()) urls.push_back(u);
                p = end + 1;
            }
        }
        for (const std::string& tag : {std::string("[\xe5\x9b\xbe\xe7\x89\x87:"), std::string("[\xe5\x9b\xbe:")}) {
            size_t q = 0;
            while ((q = raw.find(tag, q)) != std::string::npos) {
                size_t e = raw.find(']', q); if (e == std::string::npos) break;
                std::string u = raw.substr(q + tag.size(), e - q - tag.size());
                if (!u.empty()) urls.push_back(u);
                q = e + 1;
            }
        }
        if (urls.empty()) return "";
        nlohmann::json a = urls; return a.dump();
    }
    // 把内容里的图片码（[CQ:image,..]/[img,..]）替换成 [图片] 标签，转录里可读；
    // 实际图片引用另存 images 列，由导出/日志站按序回填。
    static std::string imageCodesToLabel(std::string s) {
        for (const std::string& tag : {std::string("[CQ:image"), std::string("[img,")}) {
            size_t p = 0;
            while ((p = s.find(tag, p)) != std::string::npos) {
                size_t end = s.find(']', p);
                if (end == std::string::npos) break;
                s.replace(p, end - p + 1, "[\xe5\x9b\xbe\xe7\x89\x87]");
                p += 8;   // strlen("[图片]")
            }
        }
        return s;
    }
    // 用 curl 把 url 下到 outPath（-K 配置文件传参，防 shell 注入）。返回是否成功。
    static bool curlDownload(const std::string& url, const std::string& outPath) {
        std::string cfgPath = outPath + ".curlcfg";
        { std::ofstream cf(cfgPath, std::ios::binary);
          cf << "url = \"" << url << "\"\noutput = \"" << outPath << "\"\n"
             << "--max-time 8\n--connect-timeout 4\n--silent\n--fail\n--location\n"; }
        std::string cmd = "curl -K \"" + cfgPath + "\"";
        int rc = std::system(cmd.c_str());
        std::error_code ec; std::filesystem::remove(cfgPath, ec);
        if (rc != 0) { std::filesystem::remove(outPath, ec); return false; }
        return std::filesystem::exists(outPath, ec);
    }
    // 把图片数组里的远端 url 落地到 data/logs/images/，成功的替换为本地文件名。
    std::string downloadLogImages(const std::string& jsonArr, int logId) const {
        nlohmann::json a;
        try { a = nlohmann::json::parse(jsonArr); } catch (...) { return jsonArr; }
        if (!a.is_array()) return jsonArr;
        std::error_code ec; std::filesystem::create_directories("data/logs/images", ec);
        nlohmann::json out = nlohmann::json::array();
        int n = 0;
        for (auto& e : a) {
            std::string u = e.is_string() ? e.get<std::string>() : "";
            if (u.rfind("http", 0) != 0) { out.push_back(u); continue; }   // 非 http（本地/已落地）原样
            std::string ext = "jpg";
            if (auto dot = u.find_last_of('.'); dot != std::string::npos && u.size() - dot <= 5) {
                ext = u.substr(dot + 1); if (auto qm = ext.find('?'); qm != std::string::npos) ext = ext.substr(0, qm);
            }
            std::string fname = "log" + std::to_string(logId) + "_" + std::to_string(nowEpoch()) + "_" + std::to_string(n++) + "." + ext;
            std::string path = "data/logs/images/" + fname;
            if (!curlDownload(u, path)) { out.push_back(u); continue; }   // 下载失败→保留临时 url
            // generic 图床：立即上传拿稳定 url（趁 QQ 链接还活着）；失败退回本地文件名。
            if (imghost::mode(cfg_) == "generic") {
                if (auto stable = imghost::uploadGeneric(cfg_, path)) { out.push_back(*stable); continue; }
            }
            out.push_back(fname);   // local / none / 上传失败 → 本地文件名
        }
        return out.dump();
    }

    void recordMessage(const Message& msg, const std::string& reply) {
        recordIncoming(msg);
        if (!reply.empty()) recordBotReply(msg, reply);
    }

    /// C#89：入站消息落游戏日志。与骰娘回复拆开——回复可能经 AI 后台线程润色/翻译
    /// 后才定稿，入站部分必须在消息线程即时记录且只记一次。
    void recordIncoming(const Message& msg) {
        if (msg.type == MessageType::kPrivate) return;
        int logId = activeLogId(msg);
        if (logId <= 0) return;
        auto* st = db_.getLogStorage();   // transcripts live in logs.db
        if (!st) return;
        try {
            // Prefer the human-readable form (images→[图片], 表情→[表情], @ in原顺序)
            // so the transcript / 日志站 上传 are legible; fall back to raw/clean.
            std::string logContent = !msg.displayContent.empty() ? msg.displayContent
                                   : (!msg.rawContent.empty() ? msg.rawContent : msg.content);
            if (!logContent.empty()) {
                GameLogMessageRow m;
                m.logId = logId; m.sender = displayName(msg); m.userId = msg.senderId;
                m.content = logContent; m.createdAt = nowIso();
                // C#3：从原始消息提取图片引用；骰主开启「保存图片」则落地到本地。
                std::string imgs = extractImageRefs(msg.rawContent.empty() ? msg.content : msg.rawContent);
                if (!imgs.empty()) m.images = saveLogImages() ? downloadLogImages(imgs, logId) : imgs;
                st->insert(m);
            }
        } catch (...) {}
    }

    /// C#89：骰娘回复落游戏日志（最终发送文本，含润色/翻译后的版本）。
    void recordBotReply(const Message& msg, const std::string& reply) {
        if (msg.type == MessageType::kPrivate || reply.empty()) return;
        int logId = activeLogId(msg);
        if (logId <= 0) return;
        auto* st = db_.getLogStorage();
        if (!st) return;
        try {
            GameLogMessageRow m;
            m.logId = logId; m.sender = i18n_.tr(localeForGroup(msg), "log.bot_name");
            m.userId = msg.selfId.empty() ? std::string("0") : msg.selfId;
            m.content = reply; m.createdAt = nowIso();
            // C#57：骰娘回复也可能带图（[img,file=..]/自定义回复的图码），一并提取，
            // 供导出 HTML 内嵌 / 日志站替换稳定链接。本地资产引用无需下载。
            std::string rimgs = extractImageRefs(reply);
            if (!rimgs.empty()) { m.images = rimgs; m.content = imageCodesToLabel(reply); }
            st->insert(m);
        } catch (...) {}
    }

    /// Auto-build / update a player's profile. Called for every processed
    /// message; @p didCommand bumps the command counter + last-command time.
    // C#53: total commands handled since this process started (non-empty reply = a command).
    static inline std::atomic<long> s_cmdCount{0};

    void recordPlayerActivity(const Message& msg, bool didCommand) {
        if (didCommand) s_cmdCount.fetch_add(1, std::memory_order_relaxed);
        if (msg.senderId.empty()) return;
        auto* st = db_.getStorage();
        if (!st) return;
        try {
            namespace orm = sqlite_orm;
            auto rows = st->get_all<PlayerProfileRow>(
                orm::where(orm::c(&PlayerProfileRow::platform) == msg.platform
                    and orm::c(&PlayerProfileRow::userId) == msg.senderId));
            if (rows.empty()) {
                PlayerProfileRow r;
                r.platform = msg.platform; r.userId = msg.senderId;
                r.nickname = msg.senderName;
                r.trustLevel = 0;
                r.cmdCount = didCommand ? 1 : 0;
                r.lastCmdAt = didCommand ? nowIso() : "";
                r.createdAt = nowIso();
                st->insert(r);
            } else {
                auto r = rows.front();
                if (!msg.senderName.empty()) r.nickname = msg.senderName;
                if (didCommand) { r.cmdCount += 1; r.lastCmdAt = nowIso(); }
                st->update(r);
            }
        } catch (...) {}
    }

    // ─── 调度 (#48 定时任务 / #47 不活跃自动退群) ─────────────
    static std::string todayYMD() {
        std::time_t t = std::time(nullptr); std::tm lt{};
#if defined(_WIN32)
        localtime_s(&lt, &t);
#else
        lt = *std::localtime(&t);
#endif
        char b[16]; std::strftime(b, sizeof(b), "%Y-%m-%d", &lt); return b;
    }
    /// Mark a group active "today" (called on each incoming group message).
    void markGroupActive(const std::string& platform, const std::string& gid) {
        if (gid.empty()) return;
        setGroupSettingFor(platform, gid, "lastActiveAt", todayYMD());
    }
    /// Groups whose last activity is older than @p days (0 → none). For #47.
    std::vector<std::pair<std::string, std::string>> inactiveGroups(int days) const {
        std::vector<std::pair<std::string, std::string>> out;
        if (days <= 0) return out;
        auto* st = db_.getStorage(); if (!st) return out;
        std::time_t now = std::time(nullptr);
        try {
            namespace orm = sqlite_orm;
            for (auto& r : st->get_all<GroupSettingRow>(
                    orm::where(orm::c(&GroupSettingRow::key) == std::string("lastActiveAt")))) {
                if (r.value.size() < 10) continue;
                std::tm tm{};
                try {
                    tm.tm_year = std::stoi(r.value.substr(0, 4)) - 1900;
                    tm.tm_mon  = std::stoi(r.value.substr(5, 2)) - 1;
                    tm.tm_mday = std::stoi(r.value.substr(8, 2));
                } catch (...) { continue; }
                tm.tm_hour = 12;
                std::time_t then = std::mktime(&tm);
                if (then <= 0) continue;
                if ((now - then) / 86400.0 > days) out.emplace_back(r.platform, r.groupId);
            }
        } catch (...) {}
        return out;
    }
    /// 距某群上次活跃的天数（按 lastActiveAt）。无记录 → 一个很大的数（视为长期不活跃）。
    int groupInactiveDays(const std::string& platform, const std::string& gid) const {
        std::string last = groupSettingValue(platform, gid, "lastActiveAt");
        if (last.size() < 10) return 100000;
        std::tm tm{};
        try { tm.tm_year = std::stoi(last.substr(0, 4)) - 1900;
              tm.tm_mon = std::stoi(last.substr(5, 2)) - 1;
              tm.tm_mday = std::stoi(last.substr(8, 2)); } catch (...) { return 100000; }
        tm.tm_hour = 12;
        std::time_t then = std::mktime(&tm);
        if (then <= 0) return 100000;
        int d = static_cast<int>((std::time(nullptr) - then) / 86400.0);
        return d < 0 ? 0 : d;
    }
    /// 因果条件求值（定时任务）。空 → 恒真。支持：
    ///   inactive>=N / inactive>N / inactive<N（本群 N 天无指令）；其它 → 暂当真（向前兼容）。
    bool evalScheduledCondition(const std::string& cond, const std::string& platform,
                                const std::string& type, const std::string& targetId) const {
        std::string c = trim(cond);
        if (c.empty()) return true;
        if (type == "private") return true;   // 不活跃判定只对群有意义
        // 解析 inactive<op>N
        std::string lc = toLower(c);
        if (lc.rfind("inactive", 0) == 0) {
            std::string rest = trim(lc.substr(8));
            std::string op; size_t i = 0;
            while (i < rest.size() && (rest[i] == '>' || rest[i] == '<' || rest[i] == '=')) op += rest[i++];
            int n = parseIntOr(trim(rest.substr(i)), 0);
            int days = groupInactiveDays(platform, targetId);
            if (op == ">=") return days >= n;
            if (op == ">")  return days > n;
            if (op == "<=") return days <= n;
            if (op == "<")  return days < n;
            if (op == "==" || op == "=") return days == n;
            return days >= n;   // 默认 >=
        }
        return true;   // 未知条件 → 不阻塞（保守放行）
    }

    /// Send one scheduled message to a target (定时任务). type: "group"|"private".
    /// 内容走 renderReply：支持 {self}{group}{date}{time}{roll:..}{draw:..}{a|b|c} 等变量/函数。
    void sendScheduled(const std::string& platform, const std::string& type,
                       const std::string& targetId, const std::string& content) {
        auto a = adapters_.getAdapter(std::string());
        if (!a) for (auto& x : adapters_.allAdapters()) if (x->isConnected()) { a = x; break; }
        if (!a || targetId.empty()) return;
        Message lm; lm.platform = platform; lm.targetId = targetId;
        lm.type = (type == "private") ? MessageType::kPrivate : MessageType::kGroup;
        std::string text = applySelf(lm, renderReply(lm, content, "", MatchType::kKeyword));
        if (type == "private") a->sendPrivateMessage(targetId, text);
        else a->sendGroupMessageCQ(targetId, text);
    }
    /// Leave a group with the given (already-rendered) reason. For #47.
    void leaveGroupWith(const std::string& platform, const std::string& gid, const std::string& reason) {
        auto a = adapters_.getAdapter(std::string());
        if (!a) for (auto& x : adapters_.allAdapters()) if (x->isConnected()) { a = x; break; }
        if (!a || gid.empty()) return;
        Message lm; lm.platform = platform; lm.targetId = gid; lm.type = MessageType::kGroup;
        if (!reason.empty()) a->sendGroupMessage(gid, applySelf(lm, reason));
        a->leaveGroup(gid);
        // B：退群通知骰主（定时任务退群/不活跃自动退群/网页退群等都走这里）。
        dice::notice::notify(cfg_, adapters_, dice::notice::kImportant,
            "\xe5\xb7\xb2\xe9\x80\x80\xe5\x87\xba\xe7\xbe\xa4 " + gid
                + (reason.empty() ? std::string() : ("\xef\xbc\x88" + reason + "\xef\xbc\x89")),
            platform, gid, "leave");
    }

private:
    Locale localeForGroup(const Message& msg) const { return resolver_.resolve(msg); }

    static std::string nowIso() {
        std::time_t t = std::time(nullptr);
        char buf[32];
#if defined(_WIN32)
        std::tm tmv{}; gmtime_s(&tmv, &t);
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv);
#else
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::gmtime(&t));
#endif
        return buf;
    }

    // ─── .text — template/expression test (SealDice 文本模板测试) ──
    /// Renders a template, evaluating every {token}: known variables (nick/user/
    /// group), {属性} → card value, otherwise {expr} → dice/arithmetic result.
    std::string handleText(Locale loc, const std::string& args, const Message& msg) {
        std::string tmpl = trim(args);
        if (tmpl.empty()) return i18n_.tr(loc, "fun.text.usage");
        std::string out; out.reserve(tmpl.size() + 16);
        for (size_t i = 0; i < tmpl.size();) {
            if (tmpl[i] == '{') {
                size_t end = tmpl.find('}', i + 1);
                if (end == std::string::npos) { out += tmpl[i++]; continue; }
                out += evalTextToken(msg, tmpl.substr(i + 1, end - i - 1));
                i = end + 1;
            } else out += tmpl[i++];
        }
        return out;
    }

    std::string evalTextToken(const Message& msg, const std::string& tok) {
        std::string t = trim(tok);
        if (t.empty()) return "";
        if (t == "nick" || t == "pc") return displayName(msg);
        if (t == "user") return msg.senderId;
        if (t == "group") return msg.targetId;
        // {属性} → character-card value, if it resolves.
        if (auto v = cards_.getAttr(msg.senderId, cardScope(msg), t)) return std::to_string(*v);
        // Otherwise treat as a dice/arithmetic expression ({1d16}, {2+3}…).
        auto r = engine_.roll(t);
        if (r.ok()) return std::to_string(r.modifiedTotal);
        return "{" + tok + "}";   // leave unrecognized tokens untouched
    }

    std::string handleReply(Locale loc, const std::string& args, const Message&) {
        auto lower = toLower(args);
        if (lower == "on")  return i18n_.tr(loc, "reply.on");
        if (lower == "off") return i18n_.tr(loc, "reply.off");
        return i18n_.tr(loc, "reply.usage");
    }

    // ─── .pc — multi-card management (new/tag/list/del/show) ─────

    /// Pick an @ target for proxy/card commands. When this bot is explicitly @'d,
    /// the other mentions are fellow bots rather than a proxy target: handle the
    /// command ourselves, even if QQ ordered the other bot before us in atList.
    std::string atTarget(const Message& msg) const {
        if (isAtSelf(msg)) return "";
        for (const auto& id : msg.atList) {
            if (id == "all") continue;
            if (!msg.selfId.empty() && id == msg.selfId) continue;
            if (isDiceBot(id)) continue;
            return id;
        }
        return "";
    }

    std::string cardLabel(Locale loc, const std::string& name) const {
        return name.empty() ? i18n_.tr(loc, "pc.default_name") : name;
    }

    /// Render one user's bound card in @p scope as "owner【name】: attrs".
    std::string renderCard(Locale loc, const std::string& user, const std::string& scope,
                           const std::string& owner) {
        std::string name = cards_.boundCard(user, scope);
        auto attrs = cards_.getAttrs(user, scope);
        if (attrs.empty())
            return i18n_.tr(loc, "pc.show_empty",
                {{"owner", owner}, {"name", cardLabel(loc, name)}});
        // D#06：折叠临时生命值（renderCard 无 msg 上下文，上限尽力从卡上显式上限属性取）。
        std::optional<int> hpMax;
        for (const auto& mn : explicitMaxNamesFor("\xe7\x94\x9f\xe5\x91\xbd\xe5\x80\xbc")) {
            auto it = attrs.find(CharacterCardStore::canonical(mn));
            if (it != attrs.end()) { hpMax = it->second; break; }
        }
        return i18n_.tr(loc, "pc.show",
            {{"owner", owner}, {"name", cardLabel(loc, name)}, {"detail", joinAttrsVital(attrs, hpMax)}});
    }

    // 「人」的称呼，区别于人物卡名：nn 优先，否则真实昵称，否则 userId。
    // 用于「谁的卡 / 谁在说话」这类上下文；人物卡名只用于跑团出目（displayName）。
    std::string personName(const Message& msg) const {
        std::string nn = getUserSetting(msg, "nick");
        if (!nn.empty()) return nn;
        return msg.senderName.empty() ? msg.senderId : msg.senderName;
    }
    // 读任意用户在某群的设置（查看他人时取对方 nn）。
    std::string getUserSettingOf(const std::string& userId, const std::string& group,
                                 const std::string& key) const {
        auto* st = db_.getStorage();
        if (!st) return "";
        try { namespace orm = sqlite_orm;
            auto rows = st->get_all<UserSettingRow>(
                orm::where(orm::c(&UserSettingRow::userId) == userId
                    and orm::c(&UserSettingRow::groupId) == group
                    and orm::c(&UserSettingRow::key) == key));
            if (!rows.empty()) return rows.front().value;
        } catch (...) {}
        return "";
    }
    // 任意用户的称呼：自己→personName；他人→nn > 群内真实昵称 > userId（不取人物卡名/名片）。
    std::string personNameOf(const Message& msg, const std::string& uid) {
        if (uid.empty() || uid == msg.senderId) return personName(msg);
        std::string nn = getUserSettingOf(uid, cardScope(msg), "nick");
        if (!nn.empty()) return nn;
        if (auto a = adapters_.getAdapter(msg.adapterId)) {
            json members = a->getMembers(msg.targetId);
            if (members.is_array()) for (auto& m : members) {
                std::string mid;
                if (m.contains("user_id")) { if (m["user_id"].is_string()) mid = m["user_id"].get<std::string>();
                    else if (m["user_id"].is_number()) mid = std::to_string(m["user_id"].get<int64_t>()); }
                if (mid != uid) continue;
                std::string nick = m.value("nickname", std::string());   // 真实昵称，不用名片(可能被 .sn 污染)
                if (!nick.empty()) return nick;
                break;
            }
        }
        return uid;
    }

    std::string listMyCards(Locale loc, const Message& msg) {
        const std::string user = msg.senderId;
        const std::string scope = cardScope(msg);
        const std::string nick = personName(msg);   // 列表标题用「人」的称呼，非人物卡名
        auto names = cards_.listCards(user);
        if (names.empty()) return i18n_.tr(loc, "pc.list_empty", {{"nick", nick}});
        std::string bound = cards_.boundCard(user, scope);
        std::ostringstream list;
        bool first = true;
        for (const auto& n : names) {
            if (!first) list << "\n";          // 一行一条
            first = false;
            list << (n == bound ? "\xe2\x98\x85" : "\xe3\x80\x80");  // ★当前绑定 / 全角空格占位
            list << cardLabel(loc, n) << "(" << cards_.attrCount(user, n) << ")";
        }
        return i18n_.tr(loc, "pc.list",
            {{"nick", nick}, {"count", std::to_string(names.size())}, {"list", list.str()}});
    }

    std::string handlePC(Locale loc, const std::string& args, const Message& msg) {
        const std::string user = msg.senderId;
        const std::string scope = cardScope(msg);
        const std::string nick = personName(msg);   // 「人」的称呼，非人物卡名

        // ".pc @某人" / ".pc"+@ → view that user's bound card in this group.
        std::string target = atTarget(msg);

        auto [subRaw, rest] = splitCommand(args);
        std::string sub = toLower(trim(subRaw));
        std::string name = trim(rest);

        // Bare ".pc": view the @'d user's card; otherwise show usage (use .pc list
        // to list your cards) — per user feedback, bare .pc returning help is clearer.
        if (sub.empty()) {
            if (!target.empty()) return renderCard(loc, target, scope, personNameOf(msg, target));
            return i18n_.tr(loc, "pc.usage");
        }

        if (sub == "list") return listMyCards(loc, msg);

        // ".pc rename <新名>"（重命名当前卡）或 ".pc rename <旧名> <新名>"。
        if (sub == "rename" || sub == "rn") {
            auto [a1, a2] = splitCommand(name);
            std::string oldName, newName;
            if (trim(a2).empty()) { oldName = cards_.boundCard(user, scope); newName = trim(a1); }
            else { oldName = trim(a1); newName = trim(a2); }
            if (newName.empty()) return i18n_.tr(loc, "pc.rename_usage");
            if (oldName.empty()) return i18n_.tr(loc, "pc.no_bound");
            if (!cards_.cardExists(user, oldName)) return i18n_.tr(loc, "pc.not_found", {{"name", oldName}});
            if (cards_.cardExists(user, newName)) return i18n_.tr(loc, "pc.new_exists", {{"name", newName}});
            cards_.renameCard(user, oldName, newName);
            if (cards_.boundCard(user, scope) == oldName) cards_.bindCard(user, scope, newName);
            return i18n_.tr(loc, "pc.renamed", {{"nick", nick}, {"old", oldName}, {"new", newName}});
        }

        // ".pc untag" / ".pc off"：解除本群当前绑定（不删卡）。
        if (sub == "untag" || sub == "off" || sub == "unbind") {
            std::string b = cards_.boundCard(user, scope);
            if (b.empty()) return i18n_.tr(loc, "pc.no_bound");
            cards_.bindCard(user, scope, "");
            return i18n_.tr(loc, "pc.unbound", {{"nick", nick}, {"name", b}});
        }

        if (sub == "new") {
            if (name.empty()) return i18n_.tr(loc, "pc.new_usage");
            if (!cards_.createCard(user, name))
                return i18n_.tr(loc, "pc.new_exists", {{"name", name}});
            cards_.bindCard(user, scope, name);
            return i18n_.tr(loc, "pc.new", {{"nick", nick}, {"name", name}});
        }

        if (sub == "tag") {
            if (name.empty()) return i18n_.tr(loc, "pc.tag_usage");
            if (!cards_.cardExists(user, name))
                return i18n_.tr(loc, "pc.not_found", {{"name", name}});
            cards_.bindCard(user, scope, name);
            return i18n_.tr(loc, "pc.bound", {{"nick", nick}, {"name", name}});
        }

        if (sub == "del" || sub == "rm") {
            if (name.empty()) return i18n_.tr(loc, "pc.del_usage");
            if (!cards_.deleteCard(user, name))
                return i18n_.tr(loc, "pc.not_found", {{"name", name}});
            if (cards_.boundCard(user, scope) == name) cards_.bindCard(user, scope, "");
            return i18n_.tr(loc, "pc.deleted", {{"nick", nick}, {"name", name}});
        }

        if (sub == "show" || sub == "stat") {
            // ".pc show" / ".pc show @某人" → view current (or target) card.
            if (!target.empty()) return renderCard(loc, target, scope, personNameOf(msg, target));
            return renderCard(loc, user, scope, nick);
        }

        // ── 原版 CardTemp 移植：.pc build / redo（build/buildv）+ lock / unlock ──
        if (sub == "build") return pcBuild(loc, msg, name, false);
        if (sub == "redo")  return pcBuild(loc, msg, name, true);
        if (sub == "lock" || sub == "unlock") {
            std::string key = toLower(name); if (key.empty()) key = "w";
            if (key != "w" && key != "r") return i18n_.tr(loc, "pc.lock_usage");
            bool on = (sub == "lock");
            bool ok = on ? cards_.lockCard(user, scope, key) : cards_.unlockCard(user, scope, key);
            if (!ok) return i18n_.tr(loc, on ? "pc.lock_already" : "pc.unlock_missing", {{"key", key}});
            return i18n_.tr(loc, on ? "pc.locked" : "pc.unlocked", {{"key", key}});
        }

        // ".pc <名字>" shorthand: bind an existing card by name.
        std::string bare = trim(args);
        if (cards_.cardExists(user, bare)) {
            cards_.bindCard(user, scope, bare);
            return i18n_.tr(loc, "pc.bound", {{"nick", nick}, {"name", bare}});
        }

        return i18n_.tr(loc, "pc.usage");
    }

    // ─── .npc — NPC 角色卡（D#06，DiceNext 原创）───────────────────────────
    // NPC 是「群共享」的非玩家角色卡：录入属性后可通过 `.npc <名> <掷骰指令>` 代骰调用
    // 其属性（如 .npc 哥布林 ra 侦查）。存储：每个 NPC 用一个伪用户 id
    // `npc:<群>:<名>` 的默认卡，名册存群设置 npcRoster（JSON 数组）。
    static std::string npcUser(const std::string& groupId, const std::string& name) {
        return "npc:" + groupId + ":" + name;
    }
    std::vector<std::string> npcRoster(const Message& msg) const {
        std::vector<std::string> v;
        std::string s = getGroupSetting(msg, "npcRoster");
        if (!s.empty()) try { auto j = json::parse(s); if (j.is_array())
            for (auto& e : j) if (e.is_string()) v.push_back(e.get<std::string>()); } catch (...) {}
        return v;
    }
    void npcRosterSave(const Message& msg, const std::vector<std::string>& v) {
        json a = json::array(); for (auto& s : v) a.push_back(s);
        setGroupSetting(msg, "npcRoster", a.dump());
    }
    bool npcRosterHas(const Message& msg, const std::string& name) const {
        for (auto& n : npcRoster(msg)) if (n == name) return true; return false;
    }
    void npcRosterAdd(const Message& msg, const std::string& name) {
        if (npcRosterHas(msg, name)) return;
        auto v = npcRoster(msg); v.push_back(name); npcRosterSave(msg, v);
    }

    std::string handleNpc(Locale loc, const std::string& args, const Message& msg) {
        if (msg.type == MessageType::kPrivate) return i18n_.tr(loc, "npc.group_only");
        const std::string g = msg.targetId;
        const std::string nick = personName(msg);
        auto [subRaw, rest] = splitCommand(args);
        std::string sub = trim(subRaw);
        std::string subL = toLower(sub);

        if (sub.empty()) return i18n_.tr(loc, "npc.usage");

        if (subL == "list") {
            auto v = npcRoster(msg);
            if (v.empty()) return i18n_.tr(loc, "npc.empty");
            std::string items; for (auto& n : v) { if (!items.empty()) items += "、"; items += n; }
            return i18n_.tr(loc, "npc.list", {{"count", std::to_string(v.size())}, {"items", items}});
        }
        if (subL == "new") {
            std::string name = trim(rest);
            if (name.empty()) return i18n_.tr(loc, "npc.new_usage");
            if (npcRosterHas(msg, name)) return i18n_.tr(loc, "npc.exists", {{"name", name}});
            npcRosterAdd(msg, name);
            return i18n_.tr(loc, "npc.new", {{"nick", nick}, {"name", name}});
        }
        if (subL == "del" || subL == "rm") {
            std::string name = trim(rest);
            if (name.empty()) return i18n_.tr(loc, "npc.del_usage");
            auto v = npcRoster(msg);
            auto it = std::find(v.begin(), v.end(), name);
            if (it == v.end()) return i18n_.tr(loc, "npc.not_found", {{"name", name}});
            v.erase(it); npcRosterSave(msg, v);
            cards_.clear(npcUser(g, name), g);   // 连带清卡
            return i18n_.tr(loc, "npc.deleted", {{"nick", nick}, {"name", name}});
        }
        if (subL == "clr") {
            std::string name = trim(rest);
            if (name.empty()) return i18n_.tr(loc, "npc.clr_usage");
            if (!npcRosterHas(msg, name)) return i18n_.tr(loc, "npc.not_found", {{"name", name}});
            cards_.clear(npcUser(g, name), g);
            return i18n_.tr(loc, "npc.cleared", {{"nick", nick}, {"name", name}});
        }
        if (subL == "show" || subL == "stat") {
            std::string name = trim(rest);
            if (name.empty()) return i18n_.tr(loc, "npc.show_usage");
            if (!npcRosterHas(msg, name)) return i18n_.tr(loc, "npc.not_found", {{"name", name}});
            return renderCard(loc, npcUser(g, name), g, name);
        }

        // ".npc <名>"（无子指令）→ 显示该 NPC；".npc <名> <子指令>" → 以该 NPC 身份代骰。
        std::string name = sub;
        std::string body = trim(rest);
        if (body.empty()) {
            if (!npcRosterHas(msg, name)) return i18n_.tr(loc, "npc.not_found", {{"name", name}});
            return renderCard(loc, npcUser(g, name), g, name);
        }
        // 防递归：子指令不得再是 npc。
        if (toLower(splitCommand(body).first) == "npc") return i18n_.tr(loc, "npc.no_nest");
        // 首次以属性/指令引用即自动登记入名册（.npc 哥布林 st 力量50 直接可用）。
        npcRosterAdd(msg, name);
        return npcProxy(loc, msg, name, body);
    }

    /// 以 NPC 身份执行一条子指令（代骰）：构造透视消息（发送者=NPC 伪用户，卡作用域=本群），
    /// 重新走命令分发，从而复用 .st/.ra/.rc/.rdc/.r 等全部处理器读写该 NPC 的卡。
    std::string npcProxy(Locale loc, const Message& msg, const std::string& name, const std::string& sub) {
        const std::string g = msg.targetId;
        Message pmsg = msg;
        pmsg.senderId = npcUser(g, name);
        pmsg.senderName = name;
        // 消息适配器会把操作者的群名片放在 extra.card；显示名解析时它优先于
        // senderName，若原样透传便会出现「NPC 标题 + 操作者人物卡」的混合回执。
        // 代理消息中的群名片应当就是该 NPC 本身。
        if (pmsg.extra.is_object()) pmsg.extra["card"] = name;
        pmsg.atList.clear();
        std::string prefix = commandPrefixes().empty() ? "." : commandPrefixes().front();
        pmsg.content = prefix + sub;
        std::string out = handleMessage(pmsg, loc);
        if (out.empty()) return i18n_.tr(loc, "npc.sub_empty", {{"name", name}, {"sub", sub}});
        // 冠以「（NPC:名）」前缀，明确这是代 NPC 掷的
        return i18n_.tr(loc, "npc.proxy_note", {{"name", name}}) + out;
    }

    // ─── 卡片模板注册表（原版 CardTemp presets 的可扩展版）────────────────
    // 内置 COC7 pc/bg（原版 BuildCOC7/COC7_BG）+ card-templates/*.json 与
    // data/card-templates/*.json 追加（规则包可随包分发；同名 preset 覆盖）。
    // 值语法：默认=Dicexp（3d6*5）；"{牌堆名}"=从牌堆抽文本；"js:代码"=JS 表达式
    //（代码里 card["属性"] 可读当前卡的数字属性）。
    struct CardPresetItem { std::string attr, src; int kind; };   // 0=dicexp 1=deck文本 2=js
public:
    /// 卡模板 "js:" 表达式求值钩子（main.cpp 注入 jsMod.evalString）。
    void setJsEval(std::function<std::optional<std::string>(const std::string&)> f) { jsEval_ = std::move(f); }
private:

    const std::map<std::string, std::vector<CardPresetItem>>& cardPresets() const {
        if (cardPresetsLoaded_) return cardPresets_;
        cardPresetsLoaded_ = true;
        auto classify = [](const std::string& attr, const std::string& src) -> CardPresetItem {
            if (src.rfind("js:", 0) == 0) return {attr, src.substr(3), 2};
            if (src.size() > 2 && src.front() == '{' && src.back() == '}') return {attr, src.substr(1, src.size() - 2), 1};
            return {attr, src, 0};
        };
        // 内置 COC7（原版 CharacterCard.cpp:92 BuildCOC7 / COC7_BG）
        cardPresets_["pc"] = {
            {"\xe5\x8a\x9b\xe9\x87\x8f", "3d6*5", 0}, {"\xe4\xbd\x93\xe8\xb4\xa8", "3d6*5", 0},
            {"\xe4\xbd\x93\xe5\x9e\x8b", "2d6*5+30", 0}, {"\xe6\x95\x8f\xe6\x8d\xb7", "3d6*5", 0},
            {"\xe5\xa4\x96\xe8\xb2\x8c", "3d6*5", 0}, {"\xe6\x99\xba\xe5\x8a\x9b", "2d6*5+30", 0},
            {"\xe6\x84\x8f\xe5\xbf\x97", "3d6*5", 0}, {"\xe6\x95\x99\xe8\x82\xb2", "2d6*5+30", 0},
            {"\xe5\xb9\xb8\xe8\xbf\x90", "3d6*5", 0},
        };
        cardPresets_["bg"] = {
            {"\xe6\x80\xa7\xe5\x88\xab", "\xe6\x80\xa7\xe5\x88\xab", 1},
            {"\xe5\xb9\xb4\xe9\xbe\x84", "7d6+8", 0},
            {"\xe8\x81\x8c\xe4\xb8\x9a", "\xe8\xb0\x83\xe6\x9f\xa5\xe5\x91\x98\xe8\x81\x8c\xe4\xb8\x9a", 1},
            {"\xe4\xb8\xaa\xe4\xba\xba\xe6\x8f\x8f\xe8\xbf\xb0", "\xe4\xb8\xaa\xe4\xba\xba\xe6\x8f\x8f\xe8\xbf\xb0", 1},
            {"\xe6\x80\x9d\xe6\x83\xb3\xe4\xbf\xa1\xe5\xbf\xb5", "\xe6\x80\x9d\xe6\x83\xb3\xe4\xbf\xa1\xe5\xbf\xb5", 1},
            {"\xe9\x87\x8d\xe8\xa6\x81\xe4\xb9\x8b\xe4\xba\xba", "\xe9\x87\x8d\xe8\xa6\x81\xe4\xb9\x8b\xe4\xba\xba", 1},
            {"\xe6\x84\x8f\xe4\xb9\x89\xe9\x9d\x9e\xe5\x87\xa1\xe4\xb9\x8b\xe5\x9c\xb0", "\xe6\x84\x8f\xe4\xb9\x89\xe9\x9d\x9e\xe5\x87\xa1\xe4\xb9\x8b\xe5\x9c\xb0", 1},
            {"\xe5\xae\x9d\xe8\xb4\xb5\xe4\xb9\x8b\xe7\x89\xa9", "\xe5\xae\x9d\xe8\xb4\xb5\xe4\xb9\x8b\xe7\x89\xa9", 1},
            {"\xe7\x89\xb9\xe8\xb4\xa8", "\xe8\xb0\x83\xe6\x9f\xa5\xe5\x91\x98\xe7\x89\xb9\xe7\x82\xb9", 1},
        };
        // 文件模板：{"presets": {"名字": {"属性": "值", ...}}}
        for (const char* dir : {"card-templates", "data/card-templates"}) {
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) continue;
            for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
                if (ec || !e.is_regular_file() || e.path().extension() != ".json") continue;
                try {
                    std::ifstream f(e.path(), std::ios::binary);
                    nlohmann::json j = nlohmann::json::parse(std::string(
                        (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()));
                    if (!j.contains("presets") || !j["presets"].is_object()) continue;
                    for (auto pit = j["presets"].begin(); pit != j["presets"].end(); ++pit) {
                        if (!pit.value().is_object()) continue;
                        std::vector<CardPresetItem> p;
                        for (auto ait = pit.value().begin(); ait != pit.value().end(); ++ait) {
                            if (ait.key().rfind("_", 0) == 0) continue;   // "_comment" 等注释键
                            if (ait.value().is_string()) p.push_back(classify(ait.key(), ait.value().get<std::string>()));
                        }
                        if (!p.empty()) cardPresets_[toLower(pit.key())] = std::move(p);
                    }
                    DICE_LOG_INFO("CardTemp: loaded presets from {}", u8str(e.path()));
                } catch (const std::exception& ex) {
                    DICE_LOG_WARN("CardTemp: parse {} failed: {}", u8str(e.path()), ex.what());
                }
            }
        }
        return cardPresets_;
    }

    // ─── .pc build/redo（原版 CharaCard::build/buildv，DiceEvent .pc build）────
    // 卡片模板 preset：Dicexp 属性（求值写卡，已有值跳过——原版语义）+ 文本属性（从
    // 牌堆抽取，存卡片 __meta.texts）+ JS 表达式。参数冒号分隔依次生成（原版 buildv），
    // 缺省 "pc"。redo = 先清卡再生成（原版 buildCard(reset=true)）。
    std::string pcBuild(Locale loc, const Message& msg, std::string para, bool redo) {
        const std::string user = msg.senderId, group = cardScope(msg), nick = displayName(msg);
        if (cards_.cardLocked(user, group, "w")) return i18n_.tr(loc, "card.locked_w");
        if (redo) { cards_.clear(user, group); for (auto& [k, v] : cards_.getTexts(user, group)) cards_.setText(user, group, k, ""); }
        // buildv：冒号分隔多参数依次生成。
        std::vector<std::string> parts;
        {
            std::string p = trim(para); size_t q = 0;
            while (q <= p.size()) {
                size_t c = p.find(':', q);
                std::string s = toLower(trim(p.substr(q, c == std::string::npos ? std::string::npos : c - q)));
                if (!s.empty()) parts.push_back(s);
                if (c == std::string::npos) break; q = c + 1;
            }
            if (parts.empty()) parts.push_back("pc");
        }
        auto texts = cards_.getTexts(user, group);
        auto& reg = cardPresets();
        for (auto& part : parts) {
            auto pit = reg.find(part);
            if (pit == reg.end()) return i18n_.tr(loc, "pc.build_unknown", {{"para", part}});
            for (auto& it : pit->second) {
                std::string canon = CharacterCardStore::canonical(it.attr);
                if (it.kind == 1) {          // 牌堆文本
                    if (texts.count(canon)) continue;                       // 已有值跳过
                    auto d = deck_.drawFromDeck(it.src);
                    if (!d || d->empty()) continue;                         // 无此牌堆则跳过
                    cards_.setText(user, group, canon, *d);
                    texts[canon] = *d;
                } else if (it.kind == 2) {   // JS 表达式（原版 TextType::JavaScript）
                    if (cards_.getAttr(user, group, canon) || texts.count(canon)) continue;
                    if (!jsEval_) continue;
                    // 把当前卡的数字属性作为 card 对象传入（card["力量"] 可引用）。
                    nlohmann::json cj = nlohmann::json::object();
                    for (auto& [k, v] : cards_.getAttrs(user, group)) cj[k] = v;
                    std::string script = "(function(card){ return (" + it.src + "); })(" + cj.dump() + ")";
                    auto r = jsEval_(script);
                    if (!r || r->empty() || *r == "undefined" || *r == "null") continue;
                    // 整数结果入数字属性，其余入文本属性。
                    bool isNum = !r->empty() && ((*r)[0] == '-' ? r->size() > 1 : true);
                    for (size_t ci = ((*r)[0] == '-' ? 1 : 0); ci < r->size(); ++ci)
                        if (!std::isdigit((unsigned char)(*r)[ci])) { isNum = false; break; }
                    if (isNum) cards_.setAttr(user, group, canon, parseIntOr(*r, 0));
                    else { cards_.setText(user, group, canon, *r); texts[canon] = *r; }
                } else {                     // Dicexp
                    if (cards_.getAttr(user, group, canon)) continue;       // 已有值跳过（原版）
                    int v = 0;
                    if (auto res = engine_.roll(it.src); res.ok()) v = res.modifiedTotal;
                    else { auto od = onedice::eval(it.src, 100); if (!od.ok) continue; v = (int)od.value; }
                    cards_.setAttr(user, group, canon, v);
                }
            }
        }
        // 生成后展示整卡（数字属性 + 文本属性），对齐原版 strPcCardBuild 的 show(true)。
        auto attrs = cards_.getAttrs(user, group);
        std::string detail = joinAttrs(attrs);
        for (auto& [k, v] : cards_.getTexts(user, group))
            detail += (detail.empty() ? "" : " ") + k + ":" + v;
        if (detail.empty()) detail = i18n_.tr(loc, "pc.build_empty");
        return i18n_.tr(loc, redo ? "pc.redo_done" : "pc.build_done", {{"nick", nick}, {"detail", detail}});
    }

    // ─── .draw / .deck / .gacha 牌堆 ─────────────────────────

    // ─── .rules 规则速查 (#46) ───────────────────────────────
    // 规则书存 rules/*.json：每文件一本，格式 {"name","aliases":[...],"entries":{词条:解释}}。
    // 懒加载并缓存。本地查询（原版是在线数据库，这里改为本地可扩展规则库）。
    void loadRules() const {
        if (rulesLoaded_) return;
        rulesLoaded_ = true;
        for (const char* dir : {"rules", "data/rules"}) {
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) continue;
            for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
                if (ec || !e.is_regular_file()) continue;
                if (e.path().extension() != ".json") continue;
                try {
                    std::ifstream f(e.path(), std::ios::binary);
                    std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                    json j = json::parse(body);
                    std::string book = j.value("name", u8str(e.path().stem()));
                    std::string bl = toLower(book);
                    if (j.contains("entries") && j["entries"].is_object())
                        for (auto& [k, v] : j["entries"].items())
                            if (v.is_string()) ruleBooks_[bl][k] = v.get<std::string>();
                    ruleAlias_[bl] = bl;
                    if (j.contains("aliases") && j["aliases"].is_array())
                        for (auto& a : j["aliases"]) if (a.is_string()) ruleAlias_[toLower(a.get<std::string>())] = bl;
                } catch (...) {}
            }
        }
    }
    /// Find an entry in a specific book (exact then substring). Returns {found, desc}.
    std::optional<std::string> lookupRule(const std::string& book, const std::string& item) const {
        auto bit = ruleBooks_.find(book);
        if (bit == ruleBooks_.end()) return std::nullopt;
        auto& entries = bit->second;
        auto eit = entries.find(item);
        if (eit != entries.end()) return eit->second;
        for (auto& [k, v] : entries) if (k.find(item) != std::string::npos) return v;   // 模糊
        return std::nullopt;
    }
    std::string handleRules(Locale loc, const std::string& args, const Message& msg) {
        loadRules();
        std::string a = trim(args);
        if (a.empty()) return i18n_.tr(loc, "rule.usage");
        if (ruleBooks_.empty()) return i18n_.tr(loc, "rule.no_books");
        // book:item 形式（支持全角冒号）。
        std::string book, item;
        size_t cpos = a.find(':');
        if (cpos == std::string::npos) { size_t fc = a.find("\xef\xbc\x9a"); if (fc != std::string::npos) { cpos = fc; } }
        if (cpos != std::string::npos) {
            size_t seplen = (a[cpos] == ':') ? 1 : 3;
            book = toLower(trim(a.substr(0, cpos)));
            item = trim(a.substr(cpos + seplen));
        } else {
            item = a;
        }
        if (item.empty()) return i18n_.tr(loc, "rule.usage");
        // 解析书别名 → 实际书名。
        std::string resolvedBook;
        if (!book.empty()) { auto it = ruleAlias_.find(book); resolvedBook = (it != ruleAlias_.end()) ? it->second : book; }
        // 指定书 → 只查该书；否则先查房间默认书，再查全部。
        std::vector<std::string> order;
        if (!resolvedBook.empty()) order.push_back(resolvedBook);
        else {
            std::string roomBook = toLower(getGroupSetting(msg, "ruleBook"));
            if (!roomBook.empty()) { auto it = ruleAlias_.find(roomBook); order.push_back(it != ruleAlias_.end() ? it->second : roomBook); }
            for (auto& [b, _] : ruleBooks_) if (std::find(order.begin(), order.end(), b) == order.end()) order.push_back(b);
        }
        for (auto& b : order)
            if (auto d = lookupRule(b, item))
                return i18n_.tr(loc, "rule.result", {{"book", b}, {"item", item}, {"desc", *d}});
        return i18n_.tr(loc, "rule.not_found", {{"item", item}});
    }
    std::string handleRuleSet(Locale loc, const std::string& args, const Message& msg) {
        if (msg.type == MessageType::kPrivate) return i18n_.tr(loc, "group.private");
        loadRules();
        std::string a = trim(args), al = toLower(a);
        if (a.empty() || al == "show") {
            std::string cur = getGroupSetting(msg, "ruleBook");
            return cur.empty() ? i18n_.tr(loc, "rule.book_none") : i18n_.tr(loc, "rule.book_show", {{"book", cur}});
        }
        if (al == "clr" || al == "clear") { setGroupSetting(msg, "ruleBook", ""); return i18n_.tr(loc, "rule.book_cleared"); }
        if (!senderIsGroupAdmin(msg)) return i18n_.tr(loc, "gate.no_perm");
        auto it = ruleAlias_.find(al);
        if (it == ruleAlias_.end()) return i18n_.tr(loc, "rule.book_unknown", {{"book", a}});
        setGroupSetting(msg, "ruleBook", it->second);
        return i18n_.tr(loc, "rule.book_set", {{"book", it->second}});
    }

    // ─── 临时牌堆 / 内联随机（KP 跑团用，无需管理员）──────────
    /// Split "甲/乙/丙" into choices (accepts ASCII '/' and full-width '／'/'｜'/'|').
    static std::vector<std::string> splitChoices(const std::string& s) {
        static const std::vector<std::string> seps = {"/", "\xef\xbc\x8f", "|", "\xef\xbd\x9c"};
        std::vector<std::string> out;
        std::string cur = s;
        // Normalize all separators to ASCII '/'.
        for (const auto& sep : seps) if (sep != "/") {
            size_t p; while ((p = cur.find(sep)) != std::string::npos) cur.replace(p, sep.size(), "/");
        }
        size_t pos = 0;
        while (pos <= cur.size()) {
            size_t c = cur.find('/', pos);
            std::string tok = trim(cur.substr(pos, c == std::string::npos ? std::string::npos : c - pos));
            if (!tok.empty()) out.push_back(tok);
            if (c == std::string::npos) break;
            pos = c + 1;
        }
        return out;
    }
    std::string pickChoice(const std::vector<std::string>& items) {
        if (items.empty()) return "";
        int idx = engine_.roll("1d" + std::to_string(items.size())).modifiedTotal - 1;
        if (idx < 0 || idx >= static_cast<int>(items.size())) idx = 0;
        return items[idx];
    }
    json getTempDecks(const Message& msg) const {
        std::string s = getGroupSetting(msg, "tempDecks");
        if (!s.empty()) try { auto j = json::parse(s); if (j.is_object()) return j; } catch (...) {}
        return json::object();
    }
    void saveTempDecks(const Message& msg, const json& j) { setGroupSetting(msg, "tempDecks", j.dump()); }
    /// Hide `_`-prefixed deck metadata keys in the list? config dice/deck_hide_underscore (default true).
    bool deckHideUnderscore() const {
        try {
            json all = cfg_.getAll();
            if (all.contains("dice") && all["dice"].contains("deck_hide_underscore"))
                return all["dice"]["deck_hide_underscore"].get<bool>();
        } catch (...) {}
        return true;
    }

    std::optional<std::string> tryHandleDraw(Locale loc, const Message& msg, const std::string& cmd) {
        if (toLower(cmd).rfind("draw", 0) != 0) return std::nullopt;
        std::string rest = trim(cmd.substr(4));
        // optional leading count: ".draw3 牌堆" or ".draw 牌堆"
        int times = 1;
        { size_t i = 0; std::string num; while (i < rest.size() && std::isdigit((unsigned char)rest[i])) num += rest[i++];
          if (!num.empty()) { times = parseIntOr(num, 1); rest = trim(rest.substr(i)); } }
        if (times < 1) times = 1; if (times > 10) times = 10;
        std::string name = trim(rest);

        // 内联随机：.draw 甲/乙/丙 → 直接随机选一（跑团临时随机，无需建堆）。
        if (name.find('/') != std::string::npos || name.find("\xef\xbc\x8f") != std::string::npos
            || name.find("\xef\xbd\x9c") != std::string::npos) {
            auto items = splitChoices(name);
            if (items.size() < 2) return i18n_.tr(loc, "deck.inline_usage");
            std::string res;
            for (int i = 0; i < times; ++i) { if (i) res += "\n"; res += pickChoice(items); }
            return i18n_.tr(loc, "deck.result",
                {{"nick", displayName(msg)}, {"name", i18n_.tr(loc, "deck.inline_name")}, {"res", res}});
        }

        if (name.empty()) {   // 无牌堆名 → 用本群默认牌堆（.deck set 设定）
            if (msg.type != MessageType::kPrivate) name = getGroupSetting(msg, "defaultDeck");
            if (name.empty()) return i18n_.tr(loc, "deck.usage");
        }

        // 本群临时牌堆优先于文件牌堆。
        if (msg.type != MessageType::kPrivate) {
            json td = getTempDecks(msg);
            if (td.contains(name) && td[name].is_array() && !td[name].empty()) {
                std::vector<std::string> items;
                for (auto& e : td[name]) if (e.is_string()) items.push_back(e.get<std::string>());
                std::string res;
                for (int i = 0; i < times; ++i) { if (i) res += "\n"; res += pickChoice(items); }
                return i18n_.tr(loc, "deck.result", {{"nick", displayName(msg)}, {"name", name}, {"res", res}});
            }
        }

        if (!deck_.has(name)) {
            // 模糊查找：在文件牌堆 + 本群临时牌堆名里找前缀/子串匹配。
            std::vector<std::string> cands = deck_.deckNames();
            if (msg.type != MessageType::kPrivate) {
                json td = getTempDecks(msg);
                if (td.is_object()) for (auto& [k, v] : td.items()) cands.push_back(k);
            }
            auto hits = fuzzyFind(name, cands);
            if (hits.size() == 1) {
                name = hits[0];   // 唯一匹配 → 用它继续往下抽
            } else if (hits.size() > 1) {
                std::string list;
                for (auto& c : hits) { if (!list.empty()) list += "\xe3\x80\x81"; list += c; }   // 、
                return i18n_.tr(loc, "deck.ambiguous", {{"name", name}, {"list", list}});
            } else {
                return i18n_.tr(loc, "deck.no_deck", {{"name", name}});
            }
            // 模糊命中的若是临时牌堆，走临时牌堆抽取。
            if (msg.type != MessageType::kPrivate) {
                json td = getTempDecks(msg);
                if (td.contains(name) && td[name].is_array() && !td[name].empty()) {
                    std::vector<std::string> items;
                    for (auto& e : td[name]) if (e.is_string()) items.push_back(e.get<std::string>());
                    std::string res;
                    for (int i = 0; i < times; ++i) { if (i) res += "\n"; res += pickChoice(items); }
                    return i18n_.tr(loc, "deck.result", {{"nick", displayName(msg)}, {"name", name}, {"res", res}});
                }
            }
        }

        std::string res;
        for (int i = 0; i < times; ++i) {
            auto card = deck_.drawFromDeck(name);
            if (i) res += "\n";
            res += card.value_or("");
        }
        const std::string nick = displayName(msg);
        return i18n_.tr(loc, "deck.result", {{"nick", nick}, {"name", name}, {"res", res}});
    }

    std::optional<std::string> tryHandleDeck(Locale loc, const Message& msg, const std::string& cmd) {
        if (toLower(cmd).rfind("deck", 0) != 0) return std::nullopt;
        std::string rest = trim(cmd.substr(4)), restL = toLower(rest);

        // .deck set/default <name> — 设置/查看本群默认牌堆（牌堆Pro：设默认）
        if (restL.rfind("set", 0) == 0 || restL.rfind("default", 0) == 0) {
            size_t kw = (restL.rfind("set", 0) == 0) ? 3 : 7;
            std::string name = trim(rest.substr(kw));
            if (name.empty()) {
                std::string cur = (msg.type != MessageType::kPrivate) ? getGroupSetting(msg, "defaultDeck") : "";
                return cur.empty() ? i18n_.tr(loc, "deck.default_none")
                                   : i18n_.tr(loc, "deck.default_show", {{"name", cur}});
            }
            if (msg.type == MessageType::kPrivate) return i18n_.tr(loc, "deck.default_group_only");
            if (!senderIsGroupAdmin(msg)) return i18n_.tr(loc, "gate.no_perm");
            if (!deck_.has(name)) return i18n_.tr(loc, "deck.no_deck", {{"name", name}});
            setGroupSetting(msg, "defaultDeck", name);
            return i18n_.tr(loc, "deck.default_set", {{"name", name}});
        }
        // .deck clr — 清除本群默认牌堆
        if (restL == "clr" || restL == "clear") {
            if (msg.type == MessageType::kPrivate) return i18n_.tr(loc, "deck.default_group_only");
            if (!senderIsGroupAdmin(msg)) return i18n_.tr(loc, "gate.no_perm");
            setGroupSetting(msg, "defaultDeck", "");
            return i18n_.tr(loc, "deck.default_cleared");
        }
        // .deck new <名> 项1/项2/… — 任何人（含 KP，无需管理员）可建本群临时牌堆
        if (restL.rfind("new", 0) == 0 || restL.rfind("add", 0) == 0) {
            if (msg.type == MessageType::kPrivate) return i18n_.tr(loc, "deck.default_group_only");
            auto [nm, content] = splitCommand(trim(rest.substr(3)));
            auto items = splitChoices(content);
            if (nm.empty() || items.empty()) return i18n_.tr(loc, "deck.temp_usage");
            json td = getTempDecks(msg);
            json arr = json::array(); for (auto& it : items) arr.push_back(it);
            td[nm] = arr; saveTempDecks(msg, td);
            return i18n_.tr(loc, "deck.temp_new", {{"name", nm}, {"count", std::to_string(items.size())}});
        }
        // .deck del <名> — 删除本群临时牌堆（建者或管理员）
        if (restL.rfind("del", 0) == 0 || restL.rfind("rm", 0) == 0) {
            if (msg.type == MessageType::kPrivate) return i18n_.tr(loc, "deck.default_group_only");
            size_t kw = (restL.rfind("del", 0) == 0) ? 3 : 2;
            std::string nm = trim(rest.substr(kw));
            json td = getTempDecks(msg);
            if (nm.empty() || !td.contains(nm)) return i18n_.tr(loc, "deck.no_deck", {{"name", nm}});
            td.erase(nm); saveTempDecks(msg, td);
            return i18n_.tr(loc, "deck.temp_del", {{"name", nm}});
        }

        // 列表：文件牌堆（可隐藏 `_` 元数据键）+ 本群临时牌堆。
        std::string list;
        size_t shown = 0, total = 0;
        bool hideUnderscore = deckHideUnderscore();
        for (const auto& n : deck_.deckNames()) {
            if (hideUnderscore && !n.empty() && n[0] == '_') continue;
            ++total;
            if (shown >= 40) continue;
            if (shown) list += "\n";          // 一行一条
            list += n; ++shown;
        }
        if (msg.type != MessageType::kPrivate) {
            json td = getTempDecks(msg);
            for (auto it = td.begin(); it != td.end(); ++it) {
                ++total;
                if (shown >= 40) continue;
                if (shown) list += "\n";
                list += it.key() + i18n_.tr(loc, "deck.temp_tag"); ++shown;
            }
        }
        if (shown >= 40) list += " …";
        if (total == 0) return i18n_.tr(loc, "deck.empty");
        return i18n_.tr(loc, "deck.list", {{"count", std::to_string(total)}, {"list", list}});
    }

    std::optional<std::string> tryHandleGacha(Locale loc, const Message& msg, const std::string& cmd) {
        if (toLower(cmd).rfind("gacha", 0) != 0) return std::nullopt;
        std::string name = trim(cmd.substr(5));
        if (name.empty()) name = "gacha";
        if (!deck_.has(name)) return i18n_.tr(loc, "deck.no_deck", {{"name", name}});
        auto card = deck_.drawFromDeck(name);
        return i18n_.tr(loc, "deck.result",
            {{"nick", displayName(msg)}, {"name", name}, {"res", card.value_or("")}});
    }

    std::string handleMod(Locale loc, const std::string&, const Message&) {
        return i18n_.tr(loc, "mod.wip");
    }


    // ─── User settings (.nn nickname, .set default dice) ─────

    std::string getUserSetting(const Message& msg, const std::string& key) const {
        auto* st = db_.getStorage();
        if (!st) return "";
        try {
            namespace orm = sqlite_orm;
            auto rows = st->get_all<UserSettingRow>(
                orm::where(orm::c(&UserSettingRow::userId) == msg.senderId
                    and orm::c(&UserSettingRow::groupId) == cardScope(msg)
                    and orm::c(&UserSettingRow::key) == key));
            if (!rows.empty()) return rows.front().value;
        } catch (...) {}
        return "";
    }
    void setUserSetting(const Message& msg, const std::string& key, const std::string& value) {
        auto* st = db_.getStorage();
        if (!st) return;
        try {
            namespace orm = sqlite_orm;
            auto rows = st->get_all<UserSettingRow>(
                orm::where(orm::c(&UserSettingRow::userId) == msg.senderId
                    and orm::c(&UserSettingRow::groupId) == cardScope(msg)
                    and orm::c(&UserSettingRow::key) == key));
            if (rows.empty()) {
                UserSettingRow r;
                r.userId = msg.senderId; r.groupId = cardScope(msg); r.key = key; r.value = value;
                st->insert(r);
            } else { auto r = rows.front(); r.value = value; st->update(r); }
        } catch (...) {}
    }
    void clearUserSetting(const Message& msg, const std::string& key) {
        auto* st = db_.getStorage();
        if (!st) return;
        try {
            namespace orm = sqlite_orm;
            st->remove_all<UserSettingRow>(
                orm::where(orm::c(&UserSettingRow::userId) == msg.senderId
                    and orm::c(&UserSettingRow::groupId) == cardScope(msg)
                    and orm::c(&UserSettingRow::key) == key));
        } catch (...) {}
    }

    /// Display name: bound character-card name → .nn override → platform
    /// nickname → id. Once a card is bound via .pc, the card's name is what
    /// shows on every roll (overriding QQ nickname / group card / .nn).
    /// Best-known display name for a user id (used for 代骰 perspective): the
    /// cached player-profile nickname, else the id itself.
public:   // 供 main.cpp 的戳一戳等事件解析昵称（getMembers 不可用时回退到记录的昵称）。
    std::string lookupNick(const std::string& platform, const std::string& userId) const {
        auto* st = db_.getStorage();
        if (st) {
            try {
                namespace orm = sqlite_orm;
                auto rows = st->get_all<PlayerProfileRow>(
                    orm::where(orm::c(&PlayerProfileRow::platform) == platform
                        and orm::c(&PlayerProfileRow::userId) == userId), orm::limit(1));
                if (!rows.empty() && !rows.front().nickname.empty()) return rows.front().nickname;
            } catch (...) {}
        }
        return userId;
    }

    // ─── Bot self-name / self-call (原版 strSelfName / strSelfCall) ──────
    // {self} 在回执中是「自称」，重定向链: {self} → strSelfCall → strSelfName。
    // strSelfName 为空时回落到适配器登录昵称，再回落到「骰娘」。系统级配置，存
    // config（dice/self_name, dice/self_call），可 .strSelfName/.strSelfCall 设定。
    std::string resolveSelfName(const Message& msg) const {
        std::string n = cfg_.get<std::string>("dice/self_name", std::string());
        if (!n.empty()) return n;
        if (auto a = adapters_.getAdapter(msg.adapterId)) {
            std::string ln = a->getLoginName();
            if (!ln.empty()) return ln;
        }
        return "\xe9\xaa\xb0\xe5\xa8\x98";   // 骰娘
    }
    std::string resolveSelfCall(const Message& msg) const {
        std::string c = cfg_.get<std::string>("dice/self_call", std::string());
        return c.empty() ? resolveSelfName(msg) : c;   // strSelfCall 默认 = &strSelfName
    }
public:
    /// Final-pass substitution of self tokens in a finished reply (原版 {self} 等）.
    // 分段发送：QQ 有单条文字上限，过长回复按宽度切成多段（ASCII=1/CJK=2，默认600=约300汉字，
    // 夹[100,1000]）。优先在换行处分段，尽量不把一行拆两段；单行超长才硬切（码点边界）。
    // 分段软断点：pos 处（刚消费完的字符之后）是否紧跟在句读/收尾符号后，适合在此断行，
    // 避免把一句话拦腰截断（#长文分段优化）。
    static bool isSoftBreak(const std::string& s, size_t pos) {
        if (pos == 0 || pos > s.size()) return false;
        char last = s[pos - 1];
        if (last == ' ' || last == '.' || last == '!' || last == '?' ||
            last == ';' || last == ',' || last == ')' || last == ']') return true;
        if (pos >= 3) {
            std::string ch = s.substr(pos - 3, 3);
            static const char* brk[] = {
                "\xe3\x80\x82", "\xe3\x80\x81", "\xe2\x80\xa6",                  // 。、…
                "\xef\xbc\x81", "\xef\xbc\x9f", "\xef\xbc\x9b", "\xef\xbc\x8c",  // ！？；，
                "\xef\xbc\x89", "\xef\xbc\x9a", "\xe3\x80\x8d", "\xe3\x80\x8f"   // ）：」』
            };
            for (auto* b : brk) if (ch == b) return true;
        }
        return false;
    }

    std::vector<std::string> segmentReply(const std::string& text) const {
        int maxW = cfg_.get<int>("dice/reply_segment_len", 600);
        if (maxW < 100) maxW = 100; if (maxW > 1000) maxW = 1000;
        auto cpLen = [](unsigned char c) -> int {
            return c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1; };
        auto width = [&](const std::string& s) {
            int w = 0; for (size_t i = 0; i < s.size();) { unsigned char c = s[i]; w += (c < 0x80 ? 1 : 2); i += cpLen(c); } return w; };
        if (width(text) <= maxW) return {text};
        std::vector<std::string> segs;
        std::string cur; int curW = 0;
        auto flush = [&] { if (!cur.empty()) { segs.push_back(cur); cur.clear(); curW = 0; } };
        size_t i = 0;
        while (i < text.size()) {
            size_t nl = text.find('\n', i);
            std::string line = text.substr(i, (nl == std::string::npos ? text.size() : nl) - i);
            i = (nl == std::string::npos ? text.size() : nl + 1);
            int lw = width(line);
            if (lw > maxW) {                 // 单行超长 → 软切：尽量在句读符号后断，别拦腰截断句子
                flush();
                size_t j = 0;
                while (j < line.size()) {
                    int cw = 0; size_t k = j, lastBreak = std::string::npos;
                    while (k < line.size()) {
                        unsigned char c = line[k]; int L = cpLen(c), w = (c < 0x80 ? 1 : 2);
                        if (cw + w > maxW) break;
                        cw += w; k += L;
                        if (isSoftBreak(line, k))           // k 处(即刚消费完的字符之后)是个好断点
                            lastBreak = k;
                    }
                    // 选断点：优先句读断点（且不能太靠前，避免碎片）；否则到 maxW 硬切。
                    size_t cut = k;
                    if (lastBreak != std::string::npos && lastBreak > j && (int)(width(line.substr(j, lastBreak - j))) >= maxW / 2)
                        cut = lastBreak;
                    if (cut <= j) cut = (k > j ? k : line.size());
                    std::string chunk = line.substr(j, cut - j);
                    j = cut;
                    if (j < line.size()) segs.push_back(chunk);   // 还有后续 → push；最后一块留给 cur
                    else { cur = chunk; curW = width(chunk); }
                }
                continue;
            }
            if (curW + lw > maxW && !cur.empty()) flush();
            if (!cur.empty()) cur += "\n";
            cur += line; curW += lw;
        }
        flush();
        return segs.empty() ? std::vector<std::string>{text} : segs;
    }

    /// 回复是否「引用」触发消息（默认 true=引用投掷对象的发言）。骰主可关
    /// （dice.quote_reply=false → 回复作为普通消息发出，不引用）。
    bool quoteReplyEnabled() const { return cfg_.get<bool>("dice/quote_reply", true); }

    // ─── 合并转发 (聊天记录形式) #6 ──────────────────────────
    // 开启后：多结果指令（.coc N / .dnd N）按「每条结果一个气泡」、过长会切多段的
    // 帮助文档等以合并转发发送。开关：dice.forward_long（默认关）。
    bool forwardEnabled() const { return cfg_.get<bool>("dice/forward_long", false); }
    // 字符数阈值（默认 1200）：任意回复的字符数超过它就强制走合并转发。仅在开关开时生效。
    int forwardThreshold() const { int n = cfg_.get<int>("dice/forward_threshold", 1200); return n < 1 ? 1 : n; }
    // 统计文本的「字符数」（UTF-8 码点数，CJK 也算 1），用于和阈值比较。
    static int textCharCount(const std::string& s) {
        int n = 0;
        for (size_t i = 0; i < s.size(); ) {
            unsigned char c = s[i];
            i += (c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1);
            ++n;
        }
        return n;
    }
    // 指定本次回复的合并转发节点（每个元素=一个聊天气泡）。空=不指定（main.cpp 会按
    // 分段结果决定是否转发）。consumed via takeForwardNodes()。
    void setForwardNodes(std::vector<std::string> nodes) { forwardNodes_ = std::move(nodes); }
    std::vector<std::string> takeForwardNodes() { auto v = std::move(forwardNodes_); forwardNodes_.clear(); return v; }

    std::string applySelf(const Message& msg, std::string text) const {
        if (text.find('{') == std::string::npos) return text;
        std::string call = resolveSelfCall(msg), name = resolveSelfName(msg);
        auto rep = [&](const char* tok, const std::string& val) {
            std::string t = tok; size_t p;
            while ((p = text.find(t)) != std::string::npos) text.replace(p, t.size(), val);
        };
        rep("{self}", call);
        rep("{strSelfCall}", call);
        rep("{strSelfName}", name);
        rep("{strSelfNick}", name);
        // Other global vars (only fill any that a handler/renderReply left behind).
        rep("{name}", displayName(msg));
        rep("{user}", msg.senderId);
        if (!msg.targetId.empty()) rep("{group}", msg.targetId);
        bool hasCard = msg.extra.is_object() && msg.extra.contains("card")
                       && msg.extra["card"].is_string() && !msg.extra["card"].get_ref<const std::string&>().empty();
        if (!msg.senderName.empty() || hasCard)
            rep("{nick}", resolveNickDisplay(msg));
        rep("{qqnick}",  qqNickOf(msg));
        rep("{card}",    groupCardOf(msg));
        rep("{pcname}",  pcNameOf(msg));
        rep("{qqnickw}", nickWrap(qqNickOf(msg)));
        rep("{cardw}",   nickWrap(groupCardOf(msg)));
        rep("{pcnamew}", nickWrap(pcNameOf(msg)));
        if (text.find("{date}") != std::string::npos || text.find("{time}") != std::string::npos) {
            std::time_t tt = std::time(nullptr); std::tm lt{};
#if defined(_WIN32)
            localtime_s(&lt, &tt);
#else
            lt = *std::localtime(&tt);
#endif
            char db[16], tb[16];
            std::strftime(db, sizeof(db), "%Y-%m-%d", &lt);
            std::strftime(tb, sizeof(tb), "%H:%M:%S", &lt);
            rep("{date}", db); rep("{time}", tb);
        }
        return text;
    }

private:
    /// `.strSelfName <名>` / `.strSelfCall <自称>` (Master)，复刻原版 .str<key>:
    /// 无参数/show 查看、reset/NULL 重置、否则设定。{self} 即引用这些。
    std::optional<std::string> tryHandleSelfText(Locale loc, const Message& msg, const std::string& cmd) {
        std::string lc = toLower(cmd);
        std::string field, label;
        if (lc.rfind("strselfname", 0) == 0)      { field = "self_name"; label = "strSelfName"; }
        else if (lc.rfind("strselfcall", 0) == 0) { field = "self_call"; label = "strSelfCall"; }
        else return std::nullopt;
        if (!isMaster(msg)) return std::string("");   // master-only, silent for others
        const std::string cfgKey = "dice/" + field;
        std::string val = trim(cmd.substr(11));       // both "strSelfName"/"strSelfCall" are 11 chars
        if (val.empty() || toLower(val) == "show") {
            std::string cur = cfg_.get<std::string>(cfgKey, std::string());
            return i18n_.tr(loc, "self.show",
                {{"key", label}, {"val", cur.empty() ? i18n_.tr(loc, "self.unset") : cur}});
        }
        if (toLower(val) == "reset" || val == "NULL") {
            cfg_.set<std::string>(cfgKey, std::string()); cfg_.save();
            return i18n_.tr(loc, "self.reset", {{"key", label}});
        }
        cfg_.set<std::string>(cfgKey, val); cfg_.save();
        return i18n_.tr(loc, "self.set", {{"key", label}, {"val", val}});
    }

    // 变量三来源（乾净值，无包裹符号）：QQ昵称 / 群名片 / 人物卡名。
    std::string qqNickOf(const Message& msg) const {
        return msg.senderName.empty() ? msg.senderId : msg.senderName;
    }
    std::string groupCardOf(const Message& msg) const {
        if (msg.extra.is_object()) {
            auto it = msg.extra.find("card");
            if (it != msg.extra.end() && it->is_string()) return it->get_ref<const std::string&>();
        }
        return "";
    }
    std::string pcNameOf(const Message& msg) const {
        return cards_.boundCard(msg.senderId, cardScope(msg));
    }
    // 包裹符号（dice/nick_prefix|suffix，默认 <>）；空值不加包裹避免留下孤零零的 <>。
    std::string nickWrap(const std::string& v) const {
        if (v.empty()) return v;
        return cfg_.get<std::string>("dice/nick_prefix", std::string("<")) + v
             + cfg_.get<std::string>("dice/nick_suffix", std::string(">"));
    }
    /// 原始显示名（人物卡名 > .nn 昵称 > 群名片/QQ）。用于设名片、先攻名等「存储/比较」场景。
    std::string displayNameRaw(const Message& msg) const {
        std::string card = cards_.boundCard(msg.senderId, cardScope(msg));
        if (!card.empty()) return card;                 // .pc 绑定的人物卡名（覆盖）
        std::string nn = getUserSetting(msg, "nick");
        if (!nn.empty()) return nn;                     // .nn 设置的昵称（覆盖）
        return resolveNickDisplay(msg);                 // 群名片 > QQ昵称 > QQ号（用户规范）
    }
    /// {nick} 变量专用：群名片 > QQ昵称 > senderId（不含人物卡名）。
    std::string resolveNickDisplay(const Message& msg) const {
        // 群名片优先（仅群内有效）
        if (msg.extra.is_object()) {
            auto it = msg.extra.find("card");
            if (it != msg.extra.end() && it->is_string() && !it->get_ref<const std::string&>().empty())
                return it->get_ref<const std::string&>();
        }
        // QQ 昵称
        if (!msg.senderName.empty()) return msg.senderName;
        // 最终回退到 QQ 号
        return msg.senderId;
    }
    /// 回复中使用的显示名：用可配置前后缀包裹（默认 <>，如 <希亚>）。骰主可改
    /// dice.nick_prefix/nick_suffix（留空=不包裹）。设名片/先攻名等用 displayNameRaw。
    std::string displayName(const Message& msg) const {
        std::string pre = cfg_.get<std::string>("dice/nick_prefix", std::string("<"));
        std::string suf = cfg_.get<std::string>("dice/nick_suffix", std::string(">"));
        return pre + displayNameRaw(msg) + suf;
    }

    // ─── .jrrp 今日人品 (deterministic per user per day) ──────

    std::optional<std::string> tryHandleJrrp(Locale loc, const Message& msg, const std::string& cmd) {
        // .jrrp 今日人品 / .mrrp 明日人品 / .zrrp 昨日人品。
        // 三者机制相同（hash(QQ+日期)），只是「确定性日期」不同：今天 / 明天 / 昨天。
        std::string lc = toLower(cmd);
        const char* key; long dayOffset;
        if (lc == "jrrp")      { dayOffset = 0;      key = "fun.jrrp"; }
        else if (lc == "mrrp") { dayOffset = 86400;  key = "fun.mrrp"; }   // 明日
        else if (lc == "zrrp") { dayOffset = -86400; key = "fun.zrrp"; }   // 昨日
        else return std::nullopt;
        const char* fmt = "%Y%m%d";

        std::time_t t = std::time(nullptr) + dayOffset;
        std::tm lt{};
#if defined(_WIN32)
        localtime_s(&lt, &t);
#else
        lt = *std::localtime(&t);
#endif
        char buf[16];
        std::strftime(buf, sizeof(buf), fmt, &lt);
        size_t h = std::hash<std::string>{}(msg.senderId + std::string(buf));
        int val = static_cast<int>(h % 100) + 1;   // 1..100
        return i18n_.tr(loc, key, {{"nick", displayName(msg)}, {"res", std::to_string(val)}});
    }

    // ─── .ti / .li 疯狂症状 (COC7 madness tables) ────────────

    std::optional<std::string> tryHandleInsane(Locale loc, const Message& msg, const std::string& cmd) {
        std::string lc = toLower(cmd);
        bool temp;
        if (lc == "ti") temp = true;
        else if (lc == "li") temp = false;
        else return std::nullopt;

        int sym = engine_.roll("1d10").modifiedTotal;   // 1..10
        int dur = engine_.roll("1d10").modifiedTotal;
        std::string symText = temp ? TempInsanity[sym] : LongInsanity[sym];
        std::string res = "1D10=" + std::to_string(sym) + "\n\xe7\x97\x87\xe7\x8a\xb6: " + symText; // 症状:

        I18n::Args a;
        a["pc"] = displayName(msg);
        a["nick"] = displayName(msg);
        a["dur"] = "1D10=" + std::to_string(dur);
        if (sym == 9) {
            int d = engine_.roll("1d100").modifiedTotal;
            a["detail_roll"] = "1D100=" + std::to_string(d);
            a["detail"] = strFear[d];
        } else if (sym == 10) {
            int d = engine_.roll("1d100").modifiedTotal;
            a["detail_roll"] = "1D100=" + std::to_string(d);
            a["detail"] = strPanic[d];
        }
        res = I18n::interpolate(res, a);
        return i18n_.tr(loc, temp ? "card.insane.temp" : "card.insane.long",
            {{"nick", displayName(msg)}, {"res", res}});
    }

    // ─── .name / .gn 随机名 ──────────────────────────────────

    std::string pickFrom(const std::vector<std::string>& v) {
        if (v.empty()) return "";
        int i = engine_.roll("1d" + std::to_string(v.size())).modifiedTotal - 1;
        if (i < 0 || i >= (int)v.size()) i = 0;
        return v[i];
    }
    std::string genName(const std::string& type) {
        if (type == "en") {
            static const std::vector<std::string> fn = {"James","Mary","John","Patricia","Robert",
                "Jennifer","Michael","Linda","William","Elizabeth","David","Susan","Richard","Karen"};
            static const std::vector<std::string> ln = {"Smith","Johnson","Williams","Brown","Jones",
                "Garcia","Miller","Davis","Wilson","Moore","Taylor","Anderson","Thomas","Harris"};
            return pickFrom(fn) + " " + pickFrom(ln);
        }
        if (type == "jp") {
            static const std::vector<std::string> sn = {"\xe4\xbd\x90\xe8\x97\xa4","\xe9\x93\x83\xe6\x9c\xa8",
                "\xe9\xab\x98\xe6\xa9\x8b","\xe7\x94\xb0\xe4\xb8\xad","\xe6\xb8\xa1\xe8\xbe\xb9","\xe4\xbc\x8a\xe8\x97\xa4",
                "\xe5\xb1\xb1\xe6\x9c\xac","\xe4\xb8\xad\xe6\x9d\x91","\xe5\xb0\x8f\xe6\x9e\x97","\xe5\x8a\xa0\xe8\x97\xa4"};
            static const std::vector<std::string> gn = {"\xe7\xbf\x94\xe5\xa4\xaa","\xe7\xb5\x90\xe8\xa1\xa3",
                "\xe5\xa4\xa7\xe8\xbc\x94","\xe7\xbe\x8e\xe5\x92\xb2","\xe5\x81\xa5\xe4\xb8\x80","\xe8\x91\xb5",
                "\xe7\x9b\xb4\xe6\xa8\xb9","\xe6\x84\x9b","\xe6\x8b\x93\xe6\xb5\xb7","\xe6\xb6\xbc"};
            return pickFrom(sn) + pickFrom(gn);
        }
        static const std::vector<std::string> xing = {"\xe8\xb5\xb5","\xe9\x92\xb1","\xe5\xad\x99","\xe6\x9d\x8e",
            "\xe5\x91\xa8","\xe5\x90\xb4","\xe9\x83\x91","\xe7\x8e\x8b","\xe5\x86\xaf","\xe9\x99\x88","\xe6\x9d\xa8",
            "\xe9\xbb\x84","\xe5\xbc\xa0","\xe5\x88\x98","\xe6\x9e\x97","\xe4\xbd\x95"};
        static const std::vector<std::string> ming = {"\xe4\xbc\x9f","\xe8\x8a\xb3","\xe5\xa8\x9c","\xe6\x95\x8f",
            "\xe9\x9d\x99","\xe4\xb8\xbd","\xe5\xbc\xba","\xe7\xa3\x8a","\xe5\x86\x9b","\xe6\xb4\x8b","\xe5\x8b\x87",
            "\xe8\x89\xb3","\xe6\x9d\xb0","\xe6\xb6\x9b","\xe6\x98\x8e","\xe8\xb6\x85","\xe9\x9c\x9e","\xe6\x80\xa1"};
        return pickFrom(xing) + pickFrom(ming);
    }
    std::optional<std::string> tryHandleName(Locale loc, const Message& msg, const std::string& cmd) {
        std::string lc = toLower(cmd);
        std::string rest;
        if (lc.rfind("name", 0) == 0) rest = trim(cmd.substr(4));
        else if (lc.rfind("gn", 0) == 0) rest = trim(cmd.substr(2));
        else return std::nullopt;

        std::string type;   // 空=未指定 → 每个名字随机国家（原版「随机姓名」混合牌堆语义）
        int n = 1;
        std::istringstream iss(rest);
        std::string tok;
        while (iss >> tok) {
            std::string tl = toLower(tok);
            if (tl == "cn" || tl == "zh" || tl == "en" || tl == "jp") type = (tl == "zh") ? "cn" : tl;
            else if (isAllDigits(tok)) n = parseIntOr(tok, 1);
        }
        if (n < 1) n = 1;
        if (n > 10) return i18n_.tr(loc, "fun.name.too_many");
        static const char* kTypes[] = {"cn", "en", "jp"};
        std::string res;
        for (int i = 0; i < n; ++i) {
            if (i) res += "  ";
            res += genName(type.empty() ? kTypes[engine_.roll("1d3").modifiedTotal - 1] : type);
        }
        return i18n_.tr(loc, "fun.name.result", {{"nick", displayName(msg)}, {"res", res}});
    }

    // ─── .en 技能成长 ────────────────────────────────────────

    std::optional<std::string> tryHandleEn(Locale loc, const Message& msg, const std::string& cmd) {
        if (toLower(cmd).rfind("en", 0) != 0) return std::nullopt;
        std::string rest = trim(cmd.substr(2));      // 支持 ".en 侦查" 与 ".en侦查"
        if (rest.empty()) return i18n_.tr(loc, "card.en.usage");

        // 批量：'|' 分隔；或多个「纯技能名」以空格分隔（海豹 .en 侦查 聆听）。
        std::vector<std::string> specs;
        if (rest.find('|') != std::string::npos) {
            size_t p = 0;
            while (p <= rest.size()) {
                size_t c = rest.find('|', p);
                std::string s = trim(rest.substr(p, c == std::string::npos ? std::string::npos : c - p));
                if (!s.empty()) specs.push_back(s);
                if (c == std::string::npos) break;
                p = c + 1;
            }
        } else {
            std::vector<std::string> toks;
            { std::istringstream iss(rest); std::string t; while (iss >> t) toks.push_back(t); }
            bool allNames = toks.size() >= 2;
            for (auto& t : toks)
                if (isAllDigits(t) || t[0] == '+' || t[0] == '-' || t.find('/') != std::string::npos) { allNames = false; break; }
            if (allNames) specs = toks;           // 批量技能
            else specs.push_back(rest);           // 单条（可带技能值 / 成长值）
        }

        std::string out;
        for (auto& s : specs) { if (!out.empty()) out += "\n"; out += runEnOne(loc, msg, s); }
        return out;
    }

    // 单条成长检定：.en 技能 [技能值] [([失败成长]/)成功成长]；成长值以 +/- 开头。
    std::string runEnOne(Locale loc, const Message& msg, const std::string& spec) {
        std::vector<std::string> toks;
        { std::istringstream iss(spec); std::string t; while (iss >> t) toks.push_back(t); }
        if (toks.empty()) return i18n_.tr(loc, "card.en.usage");
        std::string attr = toks[0];
        size_t i = 1;
        std::optional<int> explicitVal;
        if (i < toks.size() && isAllDigits(toks[i])) { explicitVal = parseIntOr(toks[i], 0); ++i; }
        std::string growthSpec;
        for (; i < toks.size(); ++i) if (toks[i][0] == '+' || toks[i][0] == '-') { growthSpec = toks[i]; break; }

        int skill;
        if (explicitVal) skill = *explicitVal;
        else {
            auto v = cards_.getAttr(msg.senderId, cardScope(msg), attr);
            if (!v) return i18n_.tr(loc, "dice.check.no_card", {{"attr", attr}});
            skill = *v;
        }
        int r = engine_.roll("1d100").modifiedTotal;
        const std::string nick = displayName(msg);
        const bool ok = (r > skill || r > 95);
        const char* cmp = ok ? ">" : "\xe2\x89\xa4";   // > / ≤
        std::string res = "1D100=" + std::to_string(r) + cmp + std::to_string(skill);

        // 解析成长值：先剥离符号，再按 '/' 拆成 失败/成功 两段。
        int sign = 1; std::string body = growthSpec;
        if (!body.empty() && (body[0] == '+' || body[0] == '-')) { sign = (body[0] == '-') ? -1 : 1; body = body.substr(1); }
        std::string failExpr, succExpr;
        if (body.empty()) { succExpr = "1d10"; }
        else if (auto sl = body.find('/'); sl != std::string::npos) { failExpr = body.substr(0, sl); succExpr = body.substr(sl + 1); }
        else { succExpr = body; }

        auto upper = [](std::string s) { for (auto& c : s) if (c == 'd') c = 'D'; return s; };
        auto grow = [&](const std::string& expr, const char* okKey) {
            int g = engine_.roll(expr.empty() ? "1d10" : expr).modifiedTotal * sign;
            int fin = skill + g;
            cards_.setAttr(msg.senderId, cardScope(msg), attr, fin);
            std::string change = (sign < 0 ? "-" : "") + upper(expr.empty() ? "1d10" : expr) + "=" + std::to_string(std::abs(g));
            return i18n_.tr(loc, okKey, {{"nick", nick}, {"attr", attr}, {"res", res},
                                        {"change", change}, {"final", std::to_string(fin)}});
        };
        if (ok) return grow(succExpr, "card.en.success");
        if (!failExpr.empty()) return grow(failExpr, "card.en.fail_grow");
        return i18n_.tr(loc, "card.en.fail", {{"nick", nick}, {"attr", attr}, {"res", res}});
    }

    // ─── .nn 改名 ────────────────────────────────────────────

    std::optional<std::string> tryHandleNN(Locale loc, const Message& msg, const std::string& cmd) {
        if (toLower(cmd).rfind("nn", 0) != 0) return std::nullopt;
        std::string name = trim(cmd.substr(2));
        std::string oldName = displayNameRaw(msg);   // 旧名用原始名，避免「<A>」双重包裹（新名 name 本就是原始输入）
        // If a character card is bound, .nn renames THAT card (so the new name
        // also shows everywhere the card name does, and they stay in sync).
        std::string bound = cards_.boundCard(msg.senderId, cardScope(msg));
        if (name.empty()) {
            clearUserSetting(msg, "nick");
            return i18n_.tr(loc, "fun.nn.clear", {{"old", oldName}});
        }
        if (name.size() > 50) return i18n_.tr(loc, "fun.nn.too_long");
        if (!bound.empty() && bound != name) cards_.renameCard(msg.senderId, bound, name);
        setUserSetting(msg, "nick", name);
        return i18n_.tr(loc, "fun.nn.set", {{"old", oldName}, {"new", name}});
    }

    // ─── .set 默认骰 ─────────────────────────────────────────

    std::optional<std::string> tryHandleSet(Locale loc, const Message& msg, const std::string& cmd) {
        if (toLower(cmd).rfind("set", 0) != 0) return std::nullopt;
        std::string rest = trim(cmd.substr(3));
        const std::string nick = displayName(msg);
        if (rest.empty()) {
            clearUserSetting(msg, "defaultDice");
            return i18n_.tr(loc, "fun.set.reset", {{"nick", nick}, {"default", "100"}});
        }
        // .set list/show → 列出可用规则系统 + 标记本群当前激活的（规则包深化）。
        if (std::string rl = toLower(rest); rl == "list" || rl == "show" || rl == "?") {
            std::string active = getGroupSetting(msg, "ruleSystem");
            std::string list; std::set<std::string> shownKeys;
            { std::shared_lock<std::shared_mutex> lk(rulesLock());
              for (auto& rp : rulePacks()) {
                  if (!rp.enabled || rp.setKeys.empty()) continue;
                  shownKeys.insert(toLower(rp.setKeys[0]));
                  if (!list.empty()) list += "\n";
                  bool act = (rp.name == active) || (toLower(rp.setKeys[0]) == toLower(active));
                  list += std::string(act ? "\xe2\x98\x85 " : "\xe3\x80\x80")   // ★ / 全角空格对齐
                        + rp.setKeys[0] + " — " + rp.fullName;
              }
              for (auto& b : rulePackBundles()) {   // 纯插件包（无 JSON 规则）声明的 setKey
                  if (!b.enabled || b.setKeys.empty() || shownKeys.count(toLower(b.setKeys[0]))) continue;
                  if (!list.empty()) list += "\n";
                  bool act = (toLower(b.setKeys[0]) == toLower(active));
                  list += std::string(act ? "\xe2\x98\x85 " : "\xe3\x80\x80") + b.setKeys[0] + " — " + b.name;
              } }
            if (list.empty()) list = i18n_.tr(loc, "fun.set.list_empty", {});
            return i18n_.tr(loc, "fun.set.list", {{"list", list},
                {"active", active.empty() ? i18n_.tr(loc, "fun.set.none", {}) : active}});
        }
        // .set <规则包key>（coc7/dnd/5e…）→ 切换本群规则系统 + 默认骰面数。仅群管可改（C#4）。
        if (auto rp = rulePackByKey(rest)) {
            if (msg.type == MessageType::kPrivate) return i18n_.tr(loc, "fun.set.group_only");
            if (!senderIsGroupAdmin(msg)) return i18n_.tr(loc, "fun.set.no_perm");
            setGroupSetting(msg, "ruleSystem", rp->name);
            if (rp->diceSides > 0) setGroupSetting(msg, "groupDefaultDice", std::to_string(rp->diceSides));
            return i18n_.tr(loc, "fun.set.rule",
                {{"nick", nick}, {"name", rp->fullName}, {"sides", std::to_string(rp->diceSides)}});
        }
        // 规则包深化：包(bundle)在 pack.json 声明的 setKey（规则逻辑由包内 lua/js 插件实现，无 JSON 规则）
        // → 也可激活（ruleSystem 存该 setKey，使包内插件按群 gating 生效）。
        {
            std::string rl = toLower(rest); std::string bname, bkey;
            { std::shared_lock<std::shared_mutex> lk(rulesLock());
              for (auto& b : rulePackBundles()) {
                  if (!b.enabled) continue;
                  for (auto& k : b.setKeys) if (toLower(k) == rl) { bname = b.name; bkey = b.setKeys[0]; break; }
                  if (!bname.empty()) break;
              } }
            if (!bname.empty()) {
                if (msg.type == MessageType::kPrivate) return i18n_.tr(loc, "fun.set.group_only");
                if (!senderIsGroupAdmin(msg)) return i18n_.tr(loc, "fun.set.no_perm");
                setGroupSetting(msg, "ruleSystem", bkey);
                return i18n_.tr(loc, "fun.set.rule", {{"nick", nick}, {"name", bname}, {"sides", "100"}});
            }
        }
        if (!isAllDigits(rest)) return i18n_.tr(loc, "fun.set.usage");
        int n = parseIntOr(rest, 100);
        if (n < 1 || n > 9999) return i18n_.tr(loc, "fun.set.invalid");
        setUserSetting(msg, "defaultDice", std::to_string(n));
        return i18n_.tr(loc, "fun.set.done", {{"nick", nick}, {"default", std::to_string(n)}});
    }

    // ─── .sleep 休息 ─────────────────────────────────────────

    std::optional<std::string> tryHandleSleep(Locale loc, const Message& msg, const std::string& cmd) {
        if (toLower(cmd) != "sleep") return std::nullopt;
        return i18n_.tr(loc, "fun.sleep", {{"nick", displayName(msg)}});
    }

    // ─── State ───────────────────────────────────────────────

    Database& db_;
    ConfigManager& cfg_;
    DiceEngine& engine_;
    I18n& i18n_;
    LocaleResolver& resolver_;
    CharacterCardStore& cards_;
    CardDeck& deck_;
    AdapterManager& adapters_;
    PersonaManager* personaMgr_ = nullptr;  // C#28-B: set via setPersonaManager()
    // 卡片模板（原版 CardTemp）：JS 求值钩子（main.cpp 注入 jsMod.evalString）+ preset 注册表缓存。
    std::function<std::optional<std::string>(const std::string&)> jsEval_;
    mutable std::map<std::string, std::vector<CardPresetItem>> cardPresets_;
    mutable bool cardPresetsLoaded_ = false;

    // When set during handleMessage, the message loop quotes THIS message id in
    // the reply instead of the triggering message (e.g. .log on quotes the prior
    // .log off). Consumed (and cleared) via takeQuoteOverride() after each call.
    std::string quoteOverride_;

    // #6：本次回复要以合并转发发送的节点（每元素=一个气泡）。由 .coc/.dnd 等多结果
    // 指令在 forwardEnabled() 时填充；main.cpp 取走后以 send_group_forward_msg 发出。
    std::vector<std::string> forwardNodes_;

    // C#10：插件帮助提供器（main.cpp 注入），供 .help 合并插件 cmd.help。
    HelpProviderFn helpProvider_;
    PluginListFn pluginProvider_;   // C#33

    // .rules 规则速查的懒加载缓存（rules/*.json）。mutable：在 const 查询里填充。
    mutable bool rulesLoaded_ = false;
    mutable std::map<std::string, std::map<std::string, std::string>> ruleBooks_;  // book → (entry → desc)
    mutable std::map<std::string, std::string> ruleAlias_;                          // alias → book
};

} // namespace dice
