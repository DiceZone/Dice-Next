#pragma once
#include "common/script_execution.h"
#include "common/log_context.h"
// ─── Dice!Next — Lua 插件子系统 ───────────────────────────────────
// 嵌入 Lua，并提供模块目录与全局函数接口：
//   data/mod/<模块名>/descriptor.json   模块元数据（title/ver/author/brief/helpdoc）
//   data/mod/<模块名>/reply/*.{lua,toml} msg_reply 因果回复
//   data/mod/<模块名>/script/*.lua       loadLua 调用的函数脚本
//   data/mod/<模块名>/speech/*.yaml      模板词条
//   data/mod/<模块名>/event/*.lua        cycle / clock / 安全 hook
//   data/mod/<模块名>/rulebook/*.yaml    分规则手册与 tape
// 目录资源可递归；脚本按全局命名空间登记，但不会自动执行资源 Lua。

#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <set>
#include <tuple>
#include <mutex>
#include <functional>
#include <optional>
#include <unordered_map>
#include <cstdint>
#include <atomic>
#include <filesystem>
#include <memory>
#include <deque>
#include <ctime>
#include <nlohmann/json_fwd.hpp>

struct lua_State;
struct sqlite3;

namespace dice {
namespace legacy_mod { struct Limits; }

class LuaPluginManager {
public:
    // Dice! named resources fold ASCII only. Keep UTF-8 bytes and display names.
    struct LegacyNameLess {
        using is_transparent = void;
        bool operator()(std::string_view a, std::string_view b) const {
            const auto fold = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
            const auto count = a.size() < b.size() ? a.size() : b.size();
            for (size_t i = 0; i < count; ++i) {
                const auto x = fold(static_cast<unsigned char>(a[i])), y = fold(static_cast<unsigned char>(b[i]));
                if (x != y) return x < y;
            }
            return a.size() < b.size();
        }
    };
    explicit LuaPluginManager(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000),
                              size_t memoryLimit = 256 * 1024 * 1024)
        : callbackState_(std::make_shared<CallbackState>()), mutex_(timeout), memoryLimit_(memoryLimit) {
        callbackState_->owner = this;
    }
    ~LuaPluginManager();

    bool init();                       // 创建 lua_State + 注册全局 + 打开配置库
    void freeRuntime();                // 关闭 lua_State（配置库另行管理）
    bool ready() const { std::lock_guard<ScriptMutex> lock(mutex_); return state_ != nullptr; }

    int  loadDir(const std::string& dir);   // 扫描 data/mod/<mod>/descriptor.json
    int  reload();                          // freeRuntime + init + loadDir(dir_)
    std::string modDir() const { return dir_; }
    // 除主目录外，额外扫描的 mod 目录（规则包 data/rulepacks/<包>/lua/）。reload 时一并加载。
    void setExtraDirs(std::vector<std::string> dirs) {
        std::lock_guard<ScriptMutex> lock(mutex_);
        extraDirs_ = std::move(dirs);
    }

    // 直接求值一段 Lua（供测试/脚本调用）。返回是否成功；err 写错误信息。
    bool eval(const std::string& code, std::string* err = nullptr);

    // ── 阶段二：reply 因果系统 ───────────────────────────────────
    void setSelfName(const std::string& n) { selfName_ = n; }
    void setBotId(const std::string& id) { botId_ = id; }   // getDiceQQ() 返回值
    using SelfIdLookupFn = std::function<std::string(const std::string& platform, const std::string& adapterId)>;
    void setSelfIdLookup(SelfIdLookupFn fn) { selfIdLookup_ = std::move(fn); }
    std::string botIdFor(const std::string& platform, const std::string& adapterId) const {
        // A known account with no resolved identity must never borrow another
        // bot's configured ID. The global value is only an unscoped fallback.
        if (selfIdLookup_ && (!platform.empty() || !adapterId.empty())) return selfIdLookup_(platform, adapterId);
        return botId_;
    }
    std::string botId() const { return botIdFor(activePlatform_, activeAdapter_); }
    // 对一条消息派发 msg_reply 因果回复：keyword 匹配 → echo() → 模板格式化。
    struct DispatchResult { bool matched = false; std::string reply; std::vector<ProcessingSource> sources; bool failed = false, pending = false; };
    ProcessingSource loggingSource(const std::string& mod, const std::string& entry = {}) const;
    DispatchResult dispatch(const std::string& text, const std::string& uid,
                            const std::string& gid, const std::string& nick,
                            const std::string& groupCard, bool isPrivate,
                            int trust = 0, const std::string& platform = "", const std::string& adapterId = "");
    // Side-effect-free exact trigger probe used before compact core parsers.
    // Search-pattern rules are intentionally excluded: ordinary chatter must not
    // claim a command route merely because it contains a keyword.
    bool hasCommandTrigger(const std::string& text, const std::string& uid,
                           const std::string& gid, const std::string& nick,
                           const std::string& groupCard, bool isPrivate,
                           int trust = 0, const std::string& platform = "", const std::string& adapterId = "");

    // 插件分群启停（地基）：派发前问宿主「此 mod 在该群是否启用」。id="lua:<mod名>"。
    using GroupGateFn = std::function<bool(const std::string& platform, const std::string& group, const std::string& pluginId, const std::string& adapterId)>;
    void setGroupGate(GroupGateFn f) { groupGate_ = std::move(f); }
    void setGroupGate(std::function<bool(const std::string&, const std::string&, const std::string&)> f) {
        groupGate_ = [f = std::move(f)](const std::string& p, const std::string& g,
                                      const std::string& id, const std::string&) { return f(p, g, id); };
    }
    // 旧 Dice! TOML reply 的 rule="BRP" 等条件：仅在当前 .game 团务规则匹配时生效。
    using RuleGateFn = std::function<bool(const std::string& platform, const std::string& group,
                                          const std::string& user, const std::string& rule,
                                          const std::string& adapterId)>;
    void setRuleGate(RuleGateFn f) { ruleGate_ = std::move(f); }
    using ReplyTypeGateFn = std::function<bool(const std::string&, const std::string&, const std::string&, const std::string&)>;
    void setReplyTypeGate(ReplyTypeGateFn f) { replyTypeGate_ = std::move(f); }
    // 旧 Dice! 模板的 {help:主题}：先查当前 Mod 的 helpdoc，再交给核心帮助系统。
    using HelpLookupFn = std::function<std::optional<std::string>(const std::string& topic)>;
    void setHelpLookup(HelpLookupFn f) { helpLookup_ = std::move(f); }
    // 原版 fmt->format：把 {key} 递归解析为 speech 别名/嵌套模板 / msg 变量 / 全局。
    std::string formatTemplate(const std::string& text,
                               const std::map<std::string, std::string>& vars, int depth = 0) const;
    // Help references additionally see the merged Mod help namespace, as in Dice!.
    std::string formatHelpTemplate(const std::string& text,
                                   const std::map<std::string, std::string>& vars) const;
    std::string formatHelpTopic(const std::string& topic,
                                const std::map<std::string, std::string>& vars) const;
    // &field（getUserConf 等）→ 经 speech 解析成实际字段名。
    std::string resolveAmp(const std::string& key) const;

    struct LuaMod {
        std::string name;       // 目录名 / 单文件名（唯一）
        std::string title, author, version, brief;
        std::string dir;        // 完整路径（目录型）或文件路径（单文件型）
        bool enabled = true;
        bool singleFile = false;   // 单文件 Lua 插件（msg_order 派发），非目录型 mod
        bool ruleCompat = false;   // 含 model/*.xml 属性模板 → 规则类（同 JS，在规则管理展示）
        int replies = 0;        // reply/*.{lua,toml} 数（目录）或 msg_order 条目数（单文件）
        int scripts = 0;        // script/*.lua 数
        int events = 0;         // 已注册的 event 条目数
        std::map<std::string, std::string, LegacyNameLess> helpdoc; // descriptor helpdoc, case-insensitive
        std::vector<std::string> permissions;   // descriptor 声明的能力（空=未声明）
        std::vector<std::string> risks;         // 静态预检命中的高危标签（仅告警不硬拦）
        std::vector<std::string> compatibilityWarnings; // Skipped legacy features, not permission warnings.
        bool permissionDeclared = false;
        bool ancestorDisabled = false;
        std::vector<std::string> managedPaths; // exact discovered paths, never reconstructed from an API name
    };
    std::vector<LuaMod> mods() const;

    // 帮助聚合：各 mod descriptor.helpdoc 的条目（{主题, 文本, mod}）。
    struct HelpItem { std::string topic, text, mod; };
    std::vector<HelpItem> helpEntries() const;

    // 某 mod 注册的「指令触发词」（供前端把指令与帮助词条区分展示）。
    //   kind="cmd"   = 前缀触发（msg_order 的 .xxx / keyword.prefix），触发词原样含/不含前缀符号
    //   kind="key"   = 精确关键词（msg_reply.match，如「{自称}好感」已解析自称）
    //   kind="search"= 包含匹配（keyword.search）
    struct ModCommand { std::string trigger, kind; };
    std::vector<ModCommand> commandsOf(const std::string& modName) const;

    // WebUI 管理：启停（重命名目录 ↔ <name>.disabled）/删除。返回是否成功。
    bool setModEnabled(const std::string& name, bool enabled);
    bool deleteMod(const std::string& name);

    // 上传导入（对齐原版 mod_dlpkg「整包解压进 mod 根目录」语义）：
    //   .lua → data/plugin/；.json（描述档/查询类 mod）→ data/mod/；
    //   .zip → 解压后把顶层 <名>.json、mod 目录（含仅 model/rulebook 的规则类）、.lua 归位，
    //          支持一包多 mod、json+目录成对、单层包装目录。
    // bytes=文件原始字节。成功返回 true；err 写失败原因；imported 写导入的 mod 名。不做 reload。
    bool importUpload(const std::string& filename, const std::string& bytes,
                      std::string* err = nullptr, std::vector<std::string>* imported = nullptr,
                      bool dryRun = false,
                      std::vector<std::string>* permissions = nullptr,
                      std::vector<std::string>* risks = nullptr);

    /// cpath 可选收窄开关（默认 false = 与 Dice! 一致；true = 仅插件/内置目录）。
    static void setCpathStrict(bool v) { s_cpathStrict = v; }

    // 注入：drawDeck(牌堆名) → 抽牌结果（main.cpp 接牌堆引擎）。
    using DeckDrawFn = std::function<std::string(const std::string&)>;
    void setDeckDraw(DeckDrawFn f) { deckDraw_ = std::move(f); }
    using ScopedDeckDrawFn = std::function<std::string(const std::string& platform, const std::string& adapterId,
        const std::string& group, const std::string& user, const std::string& name)>;
    void setScopedDeckDraw(ScopedDeckDrawFn fn) { scopedDeckDraw_ = std::move(fn); }
    std::string drawDeck(const std::string& name, const std::string& group = {}, const std::string& user = {}) const {
        if (scopedDeckDraw_) return scopedDeckDraw_(activePlatform_, activeAdapter_, group, user, name);
        return deckDraw_ ? deckDraw_(name) : std::string();
    }

    // 配置存取（供 C 函数与外部用）。scope 形如 "u:<uid>" / "g:<gid>"。
    static bool s_cpathStrict;
    std::string confGet(const std::string& scope, const std::string& key) const;
    bool        confHas(const std::string& scope, const std::string& key) const;
    void        confSet(const std::string& scope, const std::string& key, const std::string& val);
    /// Bulk upsert/delete configuration values in one SQLite transaction.
    /// Intended for migrations; regular plugin calls should keep using confSet().
    bool        confSetBatch(const std::vector<std::tuple<std::string, std::string, std::string>>& values);
    // 枚举某 key 在所有 u:<uid> 作用域的值（rank_user 用 getUserConf(nil,field)）。
    std::vector<std::pair<std::string, std::string>> confAllUsers(const std::string& key) const;
    // 枚举某作用域全部键值（玩家管理详情页查看/编辑该用户的插件变量）。
    std::vector<std::pair<std::string, std::string>> confAllOf(const std::string& scope) const;
    // 人物卡（Lua mod 专用，JSON blob，可存嵌套结构如背包）。scope=gid。
    std::string cardLoad(const std::string& uid, const std::string& scope) const;   // JSON 对象串（无则"{}"）
    void        cardSave(const std::string& uid, const std::string& scope, const std::string& jsonObj);
    // 枚举/删除某用户的全部 Lua 卡片数据（scope→data JSON）。
    std::vector<std::pair<std::string, std::string>> cardAllOf(const std::string& uid) const;
    void        cardDel(const std::string& uid, const std::string& scope);

    // 旧版 Lua 的 getPlayerCard* 直接操作骰娘人物卡；第二参数为字符串时表示卡名，
    // 为数字/空值时表示群作用域并读取该群绑定卡。Lua 自己的 lua_card 表仍只供插件私有 JSON 数据使用。
    using PlayerCardReadFn = std::function<bool(const std::string& uid, const std::string& selector,
                                                bool byName, const std::string& key, nlohmann::json& out)>;
    using PlayerCardWriteFn = std::function<bool(const std::string& uid, const std::string& selector,
                                                 bool byName, const std::string& key, const nlohmann::json& value)>;
    using PlayerCardLockFn = std::function<bool(const std::string& uid, const std::string& selector,
                                                bool byName, const std::string& key, bool on)>;
    using PlayerCardLockedFn = std::function<bool(const std::string& uid, const std::string& selector,
                                                  bool byName, const std::string& key)>;
    void setPlayerCardBridge(PlayerCardReadFn read, PlayerCardWriteFn write,
                             PlayerCardLockFn lock, PlayerCardLockedFn locked) {
        playerCardRead_ = std::move(read); playerCardWrite_ = std::move(write);
        playerCardLock_ = std::move(lock); playerCardLocked_ = std::move(locked);
    }
    bool hasPlayerCardBridge() const { return static_cast<bool>(playerCardRead_) && static_cast<bool>(playerCardWrite_); }
    bool playerCardRead(const std::string& uid, const std::string& selector, bool byName,
                        const std::string& key, nlohmann::json& out) const;
    bool playerCardWrite(const std::string& uid, const std::string& selector, bool byName,
                         const std::string& key, const nlohmann::json& value) const;
    bool playerCardLock(const std::string& uid, const std::string& selector, bool byName,
                        const std::string& key, bool on) const;
    bool playerCardLocked(const std::string& uid, const std::string& selector, bool byName,
                          const std::string& key) const;

    // 好感度桥接：与内置 .favor 共用存储。未显式传 platform 时，使用当前消息平台。
    using FavorGetFn = std::function<int(const std::string& platform, const std::string& uid)>;
    using FavorSetFn = std::function<int(const std::string& platform, const std::string& uid, int value)>;
    using FavorGrowFn = std::function<std::pair<int, int>(const std::string& platform, const std::string& uid)>;
    void setFavorBridge(FavorGetFn get, FavorSetFn set, FavorSetFn add, FavorGrowFn grow) {
        favorGet_ = std::move(get); favorSet_ = std::move(set);
        favorAdd_ = std::move(add); favorGrow_ = std::move(grow);
    }
    std::string favorPlatform(const std::string& explicitPlatform) const {
        return explicitPlatform.empty() ? activePlatform_ : explicitPlatform;
    }
    bool hasFavorBridge() const { return favorGet_ && favorSet_ && favorAdd_ && favorGrow_; }
    int favorGet(const std::string& platform, const std::string& uid) const { return favorGet_ ? favorGet_(platform, uid) : 0; }
    int favorSet(const std::string& platform, const std::string& uid, int value) const { return favorSet_ ? favorSet_(platform, uid, value) : 0; }
    int favorAdd(const std::string& platform, const std::string& uid, int delta) const { return favorAdd_ ? favorAdd_(platform, uid, delta) : 0; }
    std::pair<int, int> favorGrow(const std::string& platform, const std::string& uid) const {
        return favorGrow_ ? favorGrow_(platform, uid) : std::make_pair(-1, 0);
    }

    // 卡片锁定桥接（原版 CharaCard::lock/unlock）：作用在「真人物卡」（.st 那套）上，
    // 由 main.cpp 注入。key="w" 锁写 / "r" 锁读；on=true 加锁。返回是否发生变化。
    using CardLockFn = std::function<bool(const std::string& uid, const std::string& scope,
                                          const std::string& key, bool on)>;
    void setCardLock(CardLockFn f) { cardLockFn_ = std::move(f); }
    bool cardLock(const std::string& uid, const std::string& scope,
                  const std::string& key, bool on) const {
        return cardLockFn_ ? cardLockFn_(uid, scope, key, on) : false;
    }

    // sendMsg(text, groupId, userId)：插件主动发消息（主机按 platform 路由适配器）。
    using SendFn = std::function<void(const std::string& text, const std::string& gid, const std::string& uid)>;
    void setSender(SendFn f) { sender_ = std::move(f); }
    using ScopedSendFn = std::function<void(const std::string& platform, const std::string& adapterId,
                                           const std::string& text, const std::string& gid, const std::string& uid)>;
    void setScopedSender(ScopedSendFn f) { scopedSender_ = std::move(f); }
    void setScopedAsyncReply(ScopedSendFn f) { scopedAsyncReply_ = std::move(f); }
    bool hasSender() const { return scopedSender_ || sender_; }
    void send(const std::string& text, const std::string& group, const std::string& user) const {
        if (scopedSender_) scopedSender_(activePlatform_, activeAdapter_, text, group, user);
        else if (sender_) sender_(text, group, user);
    }

    // eventMsg(text, gid, uid)：把 text 当作 uid 在 gid 群的消息跑完整回复管线并发出
    //（复刻原版 DiceLua.cpp::eventMsg → DiceEvent::virtualCall）。gid 空=私聊。
    using EventFn = std::function<void(const std::string& text, const std::string& gid, const std::string& uid)>;
    void setEventMsg(EventFn f) { eventMsg_ = std::move(f); }

    // 旧 Dice! sleepTime(ms)：在 Lua 协程让出后由宿主事件循环续跑，避免阻塞消息线程。
    using ScheduleFn = std::function<void(double seconds, std::function<void()> callback)>;
    using AsyncReplyFn = std::function<void(const std::string& platform, const std::string& gid,
                                             const std::string& uid, const std::string& text)>;
    void setScheduler(ScheduleFn f) { scheduler_ = std::move(f); }
    using ClockFn = std::function<std::time_t()>;
    void setClock(ClockFn f) { clock_ = std::move(f); }
    using DiceLookupFn = std::function<bool(const std::string&, const std::string&, const std::string&)>;
    void setDiceLookup(DiceLookupFn f) { diceLookup_ = std::move(f); }
    using TrustLookupFn = std::function<int(const std::string&, const std::string&, const std::string&, const std::string&, bool)>;
    void setTrustLookup(TrustLookupFn f) { trustLookup_ = std::move(f); }
    void setAsyncReply(AsyncReplyFn f) { asyncReply_ = std::move(f); }

    // Safe old event/tape bridge. The host retains blacklist, disable and approval policy.
    bool dispatchHook(const std::string& hook, nlohmann::json& context, std::vector<ProcessingSource>* sources = nullptr);
    bool hasHook(const std::string& hook) const;
    void startEventHooks(); // actual host startup only, not import preflight
    std::optional<std::string> ruleManual(const std::string& topic, const std::string& activeRule = {}) const;
    DispatchResult dispatchTape(const std::string& rule, const std::string& tape, nlohmann::json& context);
    std::filesystem::path resolveScript(const std::string& name, const std::string& ownerDir) const;

    // 单文件插件 task_call{name="globalFunction"} 兼容桥。
    std::vector<std::string> taskNames() const;
    bool hasTask(const std::string& name) const;
    bool runTask(const std::string& name, std::string* error = nullptr);

    // http.get/post → 受控 fetch（外置API开关 + SSRF 黑名单）。method/url/headerLines/body → body，写 status。
    using HttpFetchFn = std::function<std::string(const std::string& method, const std::string& url,
                                                  const std::string& headerLines, const std::string& body, int& status)>;
    void setHttpFetch(HttpFetchFn f) { httpFetch_ = std::move(f); }

    // pc:rollDice(expr) → 掷骰引擎桥（原版 Actor_rollDice）。ok=false 时 err 为错误码/信息。
    struct RollOut { bool ok = false; long long sum = 0; std::string expr, expansion, err; };
    using RollFn = std::function<RollOut(const std::string& expr, int defaultFace)>;
    void setRoller(RollFn f) { roller_ = std::move(f); }

    // askExtra(json) → 平台扩展查询（原版 DD::getExtra）。返回 JSON 串；空=失败。
    using ExtraFn = std::function<std::string(const std::string& dataJson)>;
    void setAskExtra(ExtraFn f) { askExtra_ = std::move(f); }

    // 当前正加载的模块目录（loadLua 解析相对 script 路径用）。
    std::string loadingModDir_;
    DeckDrawFn  deckDraw_;
    ScopedDeckDrawFn scopedDeckDraw_;
    SelfIdLookupFn selfIdLookup_;
    SendFn      sender_;
    EventFn     eventMsg_;
    ScheduleFn  scheduler_;
    AsyncReplyFn asyncReply_;
    ScopedSendFn scopedSender_, scopedAsyncReply_;
    HttpFetchFn httpFetch_;
    GroupGateFn groupGate_;   // 分群启停 gate（地基）
    RuleGateFn ruleGate_;     // 旧 TOML reply 的 rule 条件
    ReplyTypeGateFn replyTypeGate_;
    HelpLookupFn helpLookup_; // {help:主题} 核心帮助查询桥
    CardLockFn  cardLockFn_;  // 卡片锁定桥接（真人物卡）
    PlayerCardReadFn playerCardRead_;
    PlayerCardWriteFn playerCardWrite_;
    PlayerCardLockFn playerCardLock_;
    PlayerCardLockedFn playerCardLocked_;
    FavorGetFn favorGet_;
    FavorSetFn favorSet_, favorAdd_;
    FavorGrowFn favorGrow_;
    std::string activePlatform_;  // 仅在 Lua echo/msg_order 调用栈内设置
    std::string activeAdapter_;
    RollFn      roller_;      // 掷骰引擎桥（pc:rollDice）
    ExtraFn     askExtra_;    // 平台扩展查询（askExtra）

private:
    struct CallbackState { std::recursive_mutex mutex; LuaPluginManager* owner = nullptr; };
    std::shared_ptr<CallbackState> callbackState_;
    ClockFn clock_;
    DiceLookupFn diceLookup_;
    TrustLookupFn trustLookup_;
    std::time_t nowSeconds() const { return clock_ ? clock_() : std::time(nullptr); }
    bool postCallback(double delay, std::function<void(LuaPluginManager&)> callback);
    void registerGlobals();
    void openConfStore();
    int  loadDirLocked(const std::string& dir);
    void loadModReplies(LuaMod& mod);   // 载入 speech + reply(.lua/.toml) + event(.lua)
    void loadModFile(const std::filesystem::path& path, bool enabled);   // 单文件 Lua 插件（msg_order）
    struct TemplateBudget { size_t steps = 10000, bytes = 32 * 1024 * 1024; bool exhausted = false; };
    std::string valueOf(const std::string& key, const std::map<std::string, std::string>& vars,
                        int depth, bool helpScope, TemplateBudget& budget) const;
    std::string formatTemplateImpl(const std::string& text, const std::map<std::string, std::string>& vars,
                                  int depth, bool helpScope, TemplateBudget& budget) const;
    std::string formatHelpTemplateImpl(const std::string& text, const std::map<std::string, std::string>& vars,
                                      int depth, TemplateBudget& budget) const;

    // 一条 msg_reply 因果规则（来自 reply/*.lua 或 reply/*.toml）。
    struct ReplyRule {
        std::string name, modName, modDir, type = "reply";
        std::vector<std::string> matchPatterns;    // keyword.match（精确等于）
        std::vector<std::string> prefixPatterns;   // keyword.prefix（前缀，余下为 suffix）
        std::vector<std::string> searchPatterns;   // keyword.search（包含即命中）
        std::vector<std::string> regexPatterns;    // keyword.regex/Regex（正则）
        bool requiresGame = false;            // type="Game"：需已开团且发送者为参与者
        std::string requiredRule;             // rule="BRP"：仅匹配该群规则
        int  echoRef = 0;                    // echo 为函数时的 registry 引用
        bool echoTakesMessage = true;
        std::string echoScript;              // echo={lua="name"} 时的脚本名（跑 script/<name>.lua）
        std::string staticEcho;               // TOML echo="..." 静态回复
        bool staticEchoDefined = false;
        std::string echoDeckJson;
        std::shared_ptr<const legacy_mod::Limits> limits;
        std::string legacySourceFile;         // data/plugin 单文件：每次调用重新执行源码
        std::string legacyFunctionName;       // msg_order 指向的全局函数名（位于独立 _ENV）
    };
    bool matchesLimits(const ReplyRule& rule, const std::string& uid, const std::string& gid,
                       bool isPrivate, int trust, const std::string& platform, const std::string& adapterId) const;
    std::string limitScopeKey(int scope, const std::string& uid, const std::string& gid,
                              bool isPrivate, const std::string& platform, const std::string& adapterId) const;
    bool consumeLimits(const ReplyRule& rule, const std::string& uid, const std::string& gid,
                       bool isPrivate, const std::string& platform, const std::string& adapterId,
                       std::string& notice, std::vector<std::string>& locks, bool& busy);
    void releaseLegacyLocks(const std::vector<std::string>& locks);
    struct LimitLease {
        LuaPluginManager& owner; std::vector<std::string> keys; bool retain = false;
        ~LimitLease() { if (!retain) owner.releaseLegacyLocks(keys); }
    };
    struct QueuedDispatch {
        std::string text, uid, gid, nick, card, platform, adapterId, ruleKey;
        bool isPrivate = false; int trust = 0, depth = 0;
        uint64_t generation = 0;
        std::vector<std::string> locks;
        ProcessingContext context;
    };
    std::set<std::string> activeLegacyLocks_;
    std::deque<QueuedDispatch> queuedDispatches_;
    bool lockDrainScheduled_ = false;
    void drainLegacyLocks(uint64_t generation);

    struct LegacyCycleEvent {
        std::string id, modName, modDir, actionLua;
        std::string actionText;
        std::string defaultsJson;
        int actionRef = 0;
        double intervalSeconds = 0.0;
        std::vector<std::pair<int, int>> clocks;
        std::vector<std::string> hooks;
    };
    void scheduleCycleEvent(const std::string& id, bool runNow);
    void runCycleEvent(const std::string& id, uint64_t generation);
    bool runLegacyEvent(const LegacyCycleEvent& event, nlohmann::json& context, bool mayYield);
    void scheduleClockTick();
    void runClockTick(uint64_t generation);
    std::time_t lastClockTime_ = 0;
    bool hostStarted_ = false;
    struct RuleBook {
        std::map<std::string, std::string> manual;
        std::map<std::string, LegacyCycleEvent> tapes;
    };
    std::map<std::string, RuleBook> ruleBooks_;
    struct ScriptEntry { std::filesystem::path path; std::string modName; };
    std::map<std::string, ScriptEntry, LegacyNameLess> scriptRegistry_;
    std::map<std::string, HelpItem, LegacyNameLess> legacyHelp_;

    struct LegacyTask {
        std::string name, modName, modDir;
        std::string sourceFile, functionName;
    };
    struct PendingCoroutine {
        uint64_t generation = 0;
        int threadRef = 0;
        int messageRef = 0;
        std::string ruleName, modName, modDir, platform, adapterId, gid, uid;
        std::map<std::string, std::string> vars;
        ProcessingContext logContext;
        std::vector<std::string> locks;
        bool event = false;
    };
    void scheduleCoroutineResume(int threadRef, double delaySeconds);
    void resumeCoroutine(int threadRef);
    std::string finishCoroutine(PendingCoroutine& pending, lua_State* coroutine, int resultCount);
    static double yieldedDelaySeconds(lua_State* coroutine, int resultCount);

    lua_State* state_ = nullptr;
    mutable ScriptMutex mutex_;
    size_t memoryLimit_;
    size_t allocatedBytes_ = 0;
    std::set<std::string> suspendedMods_;
    std::string dir_;
    std::vector<std::string> extraDirs_;   // 规则包附加 mod 目录
    std::vector<LuaMod> mods_;
    // Original Dice! speech keys ignore ASCII case; scalar or uniform choices.
    std::map<std::string, std::vector<std::string>> speech_;
    std::vector<ReplyRule> replyRules_;
    std::map<std::string, LegacyCycleEvent> cycleEvents_;
    std::map<std::string, LegacyTask> taskCalls_;
    std::unordered_map<int, PendingCoroutine> pendingCoroutines_;
    std::atomic<uint64_t> runtimeGeneration_{0};
    int lastLoadErrors_ = 0;
    std::string selfName_ = "\xe9\xaa\xb0\xe5\xa8\x98";   // 骰娘（默认自称，main 注入覆盖）
    std::string botId_;                                   // 机器人账号（getDiceQQ）

    sqlite3* confDb_ = nullptr;        // data/lua_mod.db：lua_conf(scope,k,v)
    mutable std::mutex confMutex_;
};

}  // namespace dice
