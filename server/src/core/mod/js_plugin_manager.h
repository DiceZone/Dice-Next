#pragma once
// ─── Dice!Next — JS 插件子系统 ─────────────────────────────────
// 嵌入 quickjs-ng，提供 `seal` / `console` 等全局对象，加载 plugins/js/*.js，
// 把海豹风格的 `seal.ext` 指令桥接到 Dice!Next 的消息流。
//
// 兼容目标（海豹扩展/指令模型）：
//   const ext = seal.ext.new(name, author, ver);
//   const cmd = seal.ext.newCmdItemInfo();
//   cmd.name='x'; cmd.help='...'; cmd.solve=(ctx,msg,cmdArgs)=>{ seal.replyToSender(ctx,msg,'hi'); return seal.ext.newCmdExecuteResult(true); };
//   ext.cmdMap['x'] = cmd; seal.ext.register(ext);

#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <set>
#include <cstdint>
#include <mutex>
#include <functional>
#include <optional>
#include <array>

#include <quickjs.h>

struct sqlite3;   // 持久化 KV 后端（data/plugins.db），仅在 .cpp 里用到完整定义

namespace dice {
struct Message;
struct BotEvent;

class JsPluginManager {
public:
    JsPluginManager() = default;
    ~JsPluginManager();

    bool init();                                   // 初始化运行时与全局对象
    int  loadDir(const std::string& dir);          // 加载目录下 *.js
    int  reload(const std::string& dir);           // 清空并重载
    bool ready() const { return ctx_ != nullptr; }

    /// 求值一段 JS 脚本并返回其字符串化结果（异常/未初始化返回 nullopt）。
    /// 供卡片模板的 "js:" 表达式等调用。
    std::optional<std::string> evalString(const std::string& script);

    struct Result { bool matched = false; std::string reply; };
    /// 用已注册 JS 指令处理一条消息；cmdLine = 去前缀后的文本（如 "seal ABC"）。
    /// Preferred SealDice-compatible entry points.  They preserve raw message
    /// identity, guild/channel IDs, segments, timestamps and adapter metadata.
    Result handle(const Message& msg, const std::string& cmdLine, int privilege = 0);
    Result handleNonCommand(const Message& msg, int privilege = 0);
    Result handleMessageReceived(const Message& msg, int privilege = 0);
    Result handleCommandReceived(const Message& msg, const std::string& cmdLine, int privilege = 0);
    void handleMessageSend(const Message& contextMessage, const Message& sentMessage,
                           const std::string& flag = "", int privilege = 0);

    void handleEvent(const BotEvent& event, int privilege = 0);

    Result handle(const std::string& platform, const std::string& userId,
                  const std::string& nickname, const std::string& groupId, const std::string& groupCard,
                  bool isPrivate, const std::string& cmdLine, int privilege = 0,
                  const std::vector<std::string>& atList = {});

    /// Legacy primitive overloads retained for callers/tests. New code should
    /// pass Message so SealDice message fields are not discarded.
    /// handleNonCommand dispatches onNotCommandReceived only; onMessageReceived
    /// is dispatched exactly once for every accepted message by the full pipeline.
    Result handleNonCommand(const std::string& platform, const std::string& userId,
                             const std::string& nickname, const std::string& groupId, const std::string& groupCard,
                            bool isPrivate, const std::string& fullText, int privilege = 0,
                            const std::vector<std::string>& atList = {});

    struct PluginMeta {
        std::string name, author, version, file, desc;
        std::string homepage, updateUrl, license;
        std::vector<std::string> commandList;   // 注册的指令名（live 枚举）
        int commands = 0;
        bool enabled = true;
        bool superseded = false;                 // 被同名更高版本顶替（不激活）
        std::string supersededBy;                // 顶替它的版本号
        bool ruleCompat = false;                 // 调用了 gameSystem/coc.registerRule → 「JS兼容规则」
        bool inMod = false;                      // 文件位于 data/mod（规则类插件目录）
    };
    std::vector<PluginMeta> plugins() const;
    // 枚举所有已注册指令的 help 文本（{插件名, 指令名, 帮助}），供帮助系统聚合。
    struct CmdHelp { std::string plugin, name, help; };
    std::vector<CmdHelp> commandHelps() const;
    // 比较两个版本号（点分数字，非数字段按字典序）；a<b 返回 <0。
    static int compareVersions(const std::string& a, const std::string& b);

    // A configurable item a plugin registered (register*Config). Captured so the
    // WebUI can render an editable form (input/switch/select/textarea by type).
    struct ConfigItem {
        std::string file, ext, key, type, def, description, optionsJson;
    };
    // Called by the register*Config bridges during load to record metadata.
    void addConfig(const std::string& ext, const std::string& key, const std::string& type,
                   const std::string& def, const std::string& description,
                   const std::string& optionsJson = "");
    std::vector<ConfigItem> configs() const;                 // all registered items (metadata)
    std::string configKey(const std::string& ext, const std::string& key) const { return "cfg:" + ext + ":" + key; }
    void setConfig(const std::string& ext, const std::string& key, const std::string& value) { kvSet(configKey(ext, key), value); }

    // Update support: fetch a plugin's declared @updateUrl, compare versions, and
    // (on request) overwrite the file. Fetch is injected (gate-free, SSRF-blocked).
    using UpdateFetchFn = std::function<std::string(const std::string& url, int& status)>;
    void setUpdateFetch(UpdateFetchFn f) { updateFetch_ = std::move(f); }
    struct UpdateInfo { bool ok = false; bool hasUpdate = false;
                        std::string current, latest, updateUrl, error; };
    UpdateInfo checkUpdate(const std::string& file) const;     // does NOT reload
    bool updatePlugin(const std::string& file, std::string& err);
    /// 删除插件：删文件 + 清理该插件的配置命名空间（ext:<name>:*），防持久残留。
    bool deletePlugin(const std::string& file, std::string& err);  // overwrites file; caller reloads
    /// List ALL plugin files in the plugin dir — loaded (enabled) ones plus any
    /// `*.js.disabled` files (shown disabled). For the WebUI management page.
    std::vector<PluginMeta> listAll() const;
    /// The directory passed to the last loadDir/reload (so the WebUI can write /
    /// remove / toggle plugin files there and then reload).
    std::string pluginDir() const { return dir_; }
    /// 规则类 JS 插件目录（data/mod，由主插件目录推导）。loadDir 会一并扫描它。
    std::string modDir() const { return modDir_; }
    /// 标记「当前正在加载的插件」为规则类（gameSystem/coc.registerRule 桥接调用）。
    void markCurrentRulePlugin() { if (!loadingFile_.empty()) rulePluginFiles_.insert(loadingFile_); }
    /// seal.gameSystem.newTemplate(ByYaml) 的模板原文（JSON/YAML）收集，供宿主解析属性别名/衍生。
    void addGameSystemTemplate(std::string s) { if (!s.empty()) gameSystemTemplates_.push_back(std::move(s)); }
    const std::vector<std::string>& gameSystemTemplates() const { return gameSystemTemplates_; }
    /// 返回某插件文件（foo.js / foo.js.disabled）实际所在目录（主目录或 data/mod），
    /// 找不到则回退主目录。供 WebUI 的启停/删除按文件定位。
    std::string dirForFile(const std::string& file) const;

    // Timers: real setTimeout/setInterval, fired on the host event loop. Scheduler
    // is injected (main.cpp → drogon loop->runAfter); replies from a fired timer are
    // delivered via the injected sender (a timer runs outside a message turn).
    using ScheduleFn = std::function<void(double delaySec, std::function<void()>)>;
    void setScheduler(ScheduleFn f) { scheduler_ = std::move(f); }

    // G2: WebSocket（drogon 客户端 + quickjs 事件回调；浏览器兼容 WebSocket API）。
    void wsConnect(JSContext* ctx, JSValue obj, const std::string& url);
    void wsSend(int64_t id, const std::string& data);
    void wsClose(int64_t id);
    void wsCleanup();
    using SenderFn = std::function<void(const std::string& platform, bool isPrivate,
                        const std::string& groupId, const std::string& userId, const std::string& text)>;
    void setSender(SenderFn f) { sender_ = std::move(f); }
    // 群管（禁言/踢/改名片，按 platform 路由到适配器）+ 黑名单（addBan/addTrust/remove）。
    using GroupAdminFn = std::function<void(const std::string& platform, const std::string& op,
        const std::string& groupId, const std::string& userId, int64_t num, const std::string& text)>;
    void setGroupAdmin(GroupAdminFn f) { groupAdmin_ = std::move(f); }
    void groupAdmin(const std::string& platform, const std::string& op, const std::string& groupId,
                    const std::string& userId, int64_t num, const std::string& text) const {
        if (groupAdmin_) groupAdmin_(platform, op, groupId, userId, num, text);
    }
    using BanFn = std::function<void(const std::string& op, const std::string& id, const std::string& reason)>;
    void setBanOp(BanFn f) { banOp_ = std::move(f); }
    void banOp(const std::string& op, const std::string& id, const std::string& reason) const {
        if (banOp_) banOp_(op, id, reason);
    }
    // 黑白名单查询（seal.ban.getList/getUser）。op="list"→JSON 数组；op="user"→JSON 对象/null。
    using BanQueryFn = std::function<std::string(const std::string& op, const std::string& id)>;
    void setBanQuery(BanQueryFn f) { banQuery_ = std::move(f); }
    std::string banQuery(const std::string& op, const std::string& id) const {
        return banQuery_ ? banQuery_(op, id) : std::string();
    }
    int64_t addTimer(JSValue cb, double delaySec, double intervalSec);  // caller holds mutex_
    void clearTimer(int64_t id);                                        // caller holds mutex_
    void fireTimer(int64_t id);                                         // locks mutex_ (loop cb)
    bool capturing() const { return capturing_; }                      // true during a message turn
    void routeReply(const std::string& platform, bool isPrivate,
                    const std::string& groupId, const std::string& userId, const std::string& text);
    JSValue findExt(const std::string& name) const;                    // seal.ext.find
    bool hasCommand(const std::string& word);                          // is `word` a registered JS cmd?
    bool hasCommand(const Message& message, const std::string& word);  // same, respecting per-group enable state

    // 供内嵌的 C 函数回调使用（经 JS_GetContextOpaque 取到 this）。
    void appendReply(const std::string& s) { if (!pendingReply_.empty()) pendingReply_ += "\n"; pendingReply_ += s; }
    void markSideEffectReply() { sideEffectReply_ = true; }
    void registerExt(JSValueConst ext);            // 登记 ext.cmdMap 里的指令
    // 持久化 KV（seal.vars / ext.storage），存 data/plugins.db 的 js_kv 表。
    // key 仍是扁平全 key（命中按完整 key，跨插件共享语义不变）；ns 列仅供
    // 「按插件查看/导出/删除」用（ext:<插件>/cfg:<插件>/shared/misc）。
    std::string kvGet(const std::string& key, const std::string& def = "") const;
    void        kvSet(const std::string& key, const std::string& val);
    // WebUI 用：取某命名空间下的全部 (key,value)（如 "ext:钓鱼"）。
    std::vector<std::pair<std::string, std::string>> kvByNamespace(const std::string& ns) const;
    // WebUI 用：清空某命名空间的全部数据（如卸载/重置插件存储）。返回删除条数。
    int kvClearNamespace(const std::string& ns);
    // WebUI 用：某插件文件注册的 ext 名（storage 按 ext.name 命名空间，可能 ≠ @name 显示名）。
    std::vector<std::string> extNamesForFile(const std::string& file) const;

    // 牌堆抽取注入（seal.deck.draw）。由 main.cpp 接到 CardDeck。
    void setSelfInfo(const std::string& id, const std::string& nick) { selfId_ = id; selfNick_ = nick; }   // ctx.endPoint.userId/nickname
    void setDeckDraw(std::function<std::string(const std::string&, bool)> f) { deckDraw_ = std::move(f); }
    std::string drawDeck(const std::string& name, bool shuffle) const { return deckDraw_ ? deckDraw_(name, shuffle) : std::string(); }
    // 骰子表达式求值注入（seal.format 里的 {表达式}）。接到 DiceEngine。
    void setDiceEval(std::function<std::string(const std::string&)> f) { diceEval_ = std::move(f); }
    std::string evalDice(const std::string& expr) const { return diceEval_ ? diceEval_(expr) : expr; }

    // 插件分群启停（地基）：派发前问宿主「此插件(按源文件)在该群是否启用」。id="js:<文件>"。
    using GroupGateFn = std::function<bool(const std::string& platform, const std::string& group, const std::string& pluginId)>;
    void setGroupGate(GroupGateFn f) { groupGate_ = std::move(f); }

    // seal.getEndPoints(): live adapter snapshots in SealDice's public shape.
    struct EndpointInfo {
        std::string id, nickname, userId, platform, protocolType;
        int state = 0;
        bool enable = true;
        int64_t groupNum = 0;
        int64_t cmdExecutedNum = 0;
        int64_t cmdExecutedLastTime = 0;
        int64_t onlineTotalTime = 0;
    };
    using EndpointProviderFn = std::function<std::vector<EndpointInfo>()>;
    void setEndpointProvider(EndpointProviderFn f) { endpointProvider_ = std::move(f); }
    std::vector<EndpointInfo> endpointInfos() const {
        return endpointProvider_ ? endpointProvider_() : std::vector<EndpointInfo>{};
    }

    // seal.vars 人物卡桥接：海豹 gameSystem 插件用「无 $ 前缀」的属性名读写玩家人物卡
    // （= .st/.ra 用的同一份卡）。注入后，seal.vars.intGet/intSet 的无前缀名直达人物卡，
    // 这样自订规则插件（如最终物语）的检定/状态指令才能读到 .st 录入的属性。
    using CardGetFn = std::function<bool(const std::string& platform, const std::string& userId,
                                         const std::string& groupId, const std::string& attr, long long& out)>;
    using CardSetFn = std::function<void(const std::string& platform, const std::string& userId,
                                         const std::string& groupId, const std::string& attr, long long val)>;
    void setCardBridge(CardGetFn g, CardSetFn s) { cardGet_ = std::move(g); cardSet_ = std::move(s); }
    // 读取「关联/表达式属性」(.st 物防='dex+1' 存的原文)，供 seal.vars.strGet 读到。
    using CardGetStrFn = std::function<bool(const std::string& platform, const std::string& userId,
                                            const std::string& groupId, const std::string& attr, std::string& out)>;
    void setCardStrBridge(CardGetStrFn g) { cardGetStr_ = std::move(g); }
    // 群名片解析器（platform, groupId, userId → 群名片）。规则包 JS 读
    // msg.sender.card / ctx.player.card / ctx.player.name(显示名) 靠它；未设则回退 QQ 昵称。
    using CardNameFn = std::function<std::string(const std::string&, const std::string&, const std::string&)>;
    void setCardNameResolver(CardNameFn f) { cardNameResolver_ = std::move(f); }
    // 当前群日志状态，供 ctx.group.logOn / ctx.group.logCurName 兼容海豹字段。
    using LogStateFn = std::function<std::pair<bool, std::string>(const std::string&, const std::string&)>;
    void setLogStateResolver(LogStateFn f) { logStateResolver_ = std::move(f); }
    using GroupNameFn = std::function<std::string(const std::string&, const std::string&, const std::string&)>;
    void setGroupNameResolver(GroupNameFn f) { groupNameResolver_ = std::move(f); }
    using GroupSystemFn = std::function<std::string(const std::string&, const std::string&, const std::string&)>;
    void setGroupSystemResolver(GroupSystemFn f) { groupSystemResolver_ = std::move(f); }
    using DiceSidesFn = std::function<std::array<int, 3>(const Message&)>;
    void setDiceSidesResolver(DiceSidesFn f) { diceSidesResolver_ = std::move(f); }
    bool cardGetStr(const std::string& p, const std::string& u, const std::string& g, const std::string& a, std::string& out) const {
        return cardGetStr_ ? cardGetStr_(p, u, g, a, out) : false;
    }
    bool hasCardStrBridge() const { return (bool)cardGetStr_; }
    bool cardGet(const std::string& p, const std::string& u, const std::string& g, const std::string& a, long long& out) const {
        return cardGet_ ? cardGet_(p, u, g, a, out) : false;
    }
    void cardSet(const std::string& p, const std::string& u, const std::string& g, const std::string& a, long long v) const {
        if (cardSet_) cardSet_(p, u, g, a, v);
    }
    bool hasCardBridge() const { return (bool)cardGet_; }

    // seal.favor：与内置 .favor 共用 player_profiles.favor。
    // grow 返回 {本次增长值(-1=未成长), 当前值}。
    using FavorGetFn = std::function<int(const std::string& platform, const std::string& userId)>;
    using FavorSetFn = std::function<int(const std::string& platform, const std::string& userId, int value)>;
    using FavorGrowFn = std::function<std::pair<int, int>(const std::string& platform, const std::string& userId)>;
    void setFavorBridge(FavorGetFn get, FavorSetFn set, FavorSetFn add, FavorGrowFn grow) {
        favorGet_ = std::move(get); favorSet_ = std::move(set);
        favorAdd_ = std::move(add); favorGrow_ = std::move(grow);
    }
    bool hasFavorBridge() const { return favorGet_ && favorSet_ && favorAdd_ && favorGrow_; }
    int favorGet(const std::string& p, const std::string& u) const { return favorGet_ ? favorGet_(p, u) : 0; }
    int favorSet(const std::string& p, const std::string& u, int v) const { return favorSet_ ? favorSet_(p, u, v) : 0; }
    int favorAdd(const std::string& p, const std::string& u, int v) const { return favorAdd_ ? favorAdd_(p, u, v) : 0; }
    std::pair<int, int> favorGrow(const std::string& p, const std::string& u) const {
        return favorGrow_ ? favorGrow_(p, u) : std::make_pair(-1, 0);
    }

    // 除主目录外，额外扫描的 js 插件目录（规则包 data/rulepacks/<包>/js/）。reload 时一并加载。
    void setExtraDirs(std::vector<std::string> dirs) { extraDirs_ = std::move(dirs); }

    // HTTP 注入（全局 fetch）。接到 CommandRouter 的受控 curl（白名单 + 开关 + SSRF 防护）。
    // 返回响应体；headerLines = "\n" 分隔的 "K: V"；status 出参带回 HTTP 状态码（0=失败/被拒）。
    using HttpFetchFn = std::function<std::string(const std::string& method, const std::string& url,
                          const std::string& headerLines, const std::string& body, int& status)>;
    void setHttpFetch(HttpFetchFn f) { httpFetch_ = std::move(f); }
    std::string httpFetch(const std::string& method, const std::string& url,
                          const std::string& headerLines, const std::string& body, int& status) const {
        if (httpFetch_) return httpFetch_(method, url, headerLines, body, status);
        status = 0; return "";
    }

private:
    void installGlobals();
    void freeRuntime();
    int  loadDirLocked(const std::string& dir);   // loadDir body; caller holds mutex_
    void wsCleanupLocked();                        // caller already holds mutex_
    void openKvStore();                           // 打开 data/plugins.db + 建表 + 导入旧 json + 装载缓存
    static std::string kvNamespace(const std::string& key);  // 从扁平 key 推导 ns 列
    // Drain the JS microtask/job queue (resolves promises, e.g. awaited fetch),
    // so an async solve() driven by synchronous (blocking) fetch runs to completion.
    void drainJobs();
    // 构造 ctx / msg 两个 JS 对象（caller 负责 JS_FreeValue）。
    void buildCtxMsg(const Message& msg, int privilege, JSValue& outCtx, JSValue& outMsg);
    JSValue buildCmdArgs(const Message& msg, const std::string& cmdLine,
                         const std::string& command, const std::string& rest);
    void dispatchCommandHookLocked(const Message& msg, JSValueConst jctx,
                                   JSValueConst jmsg, JSValueConst jargs);
    Result dispatchMessageHook(const Message& msg, int privilege, const char* hook);
    static PluginMeta parseMeta(const std::string& src);

    JSRuntime* rt_ = nullptr;
    JSContext* ctx_ = nullptr;
    mutable std::mutex mutex_;

    // 保存已注册的 ext 对象（Dup 持有）；派发时实时读 ext.cmdMap，
    // 与「先 register 后加指令」的插件顺序无关。
    std::vector<std::pair<JSValue, std::string>> exts_;   // {ext 对象, 来源文件}
    std::vector<PluginMeta> plugins_;
    std::vector<ConfigItem> configs_;   // 插件注册的配置项元数据（每次 (re)load 重建）

    std::string pendingReply_;     // solve 期间 replyToSender 累积
    bool sideEffectReply_ = false; // replyPerson 等已直接投递的回复
    std::string loadingFile_;      // 当前加载文件名
    std::string selfId_, selfNick_;   // 机器人自身账号/昵称（ctx.endPoint.userId/nickname）
    std::string dir_;              // 最近一次 loadDir/reload 的主目录（供 WebUI 管理）
    std::string modDir_;           // 规则类插件目录 data/mod（从 dir_ 推导）
    std::set<std::string> rulePluginFiles_;  // 本次加载中被标记为规则类的文件名
    std::vector<std::string> gameSystemTemplates_;  // seal.gameSystem 模板原文

    std::unordered_map<std::string, std::string> kv_;   // 内存读缓存（启动时从 plugins.db 装载）
    sqlite3*            kvDb_ = nullptr;                 // 持久化后端：data/plugins.db
    mutable std::mutex  kvMutex_;                        // 守护 kv_ 与 kvDb_（与 mutex_ 不嵌套冲突，恒为最内层）
    std::function<std::string(const std::string&, bool)> deckDraw_;
    std::function<std::string(const std::string&)> diceEval_;
    HttpFetchFn httpFetch_;
    UpdateFetchFn updateFetch_;

    ScheduleFn scheduler_;
    SenderFn sender_;
    GroupGateFn groupGate_;   // 分群启停 gate（地基）
    EndpointProviderFn endpointProvider_;
    CardGetFn cardGet_;       // seal.vars ↔ 人物卡桥接
    CardSetFn cardSet_;
    CardGetStrFn cardGetStr_; // 关联/表达式属性读取
    FavorGetFn favorGet_;
    FavorSetFn favorSet_, favorAdd_;
    FavorGrowFn favorGrow_;
    CardNameFn cardNameResolver_;   // 群名片/显示名解析
    LogStateFn logStateResolver_;   // 群日志状态/当前名称
    GroupNameFn groupNameResolver_;
    GroupSystemFn groupSystemResolver_;
    DiceSidesFn diceSidesResolver_;
    std::vector<std::string> extraDirs_;   // 规则包附加 js 目录
    GroupAdminFn groupAdmin_;
    BanFn banOp_;
    BanQueryFn banQuery_;
    void wsFire(int64_t id, const char* evt, const std::string& data);   // drogon loop 线程调用
    std::map<int64_t, JSValue> wsObjs_;   // 连接 id → JS 对象引用（持 mutex_）
    int64_t wsSeq_ = 0;

    struct Timer { JSValue cb; double intervalSec; };
    std::map<int64_t, Timer> timers_;     // active setTimeout/setInterval callbacks
    int64_t timerSeq_ = 0;
    bool capturing_ = false;              // inside a message turn → replies accumulate
};

} // namespace dice
