#pragma once
// ─── Dice!Next — Lua 插件子系统 ───────────────────────────────────
// 嵌入 Lua，并提供模块目录与全局函数接口：
//   data/mod/<模块名>/descriptor.json   模块元数据（title/ver/author/brief/helpdoc）
//   data/mod/<模块名>/reply/*.lua        msg_reply 因果回复（阶段二）
//   data/mod/<模块名>/script/*.lua       loadLua 调用的函数脚本
//   data/mod/<模块名>/speech/*.yaml      模板词条（阶段二）
//
// 阶段一（本轮）：引擎嵌入 + 模块发现(descriptor) + 核心 C API
//   (log/ranint/getUserConf/setUserConf/getGroupConf/setGroupConf/getUserToday/
//    setUserToday/getDiceDir/mkDirs/loadLua/sleepTime/drawDeck) + 配置持久化。
// 后续阶段：reply 因果系统 + speech 模板 + 人物卡属性 + sendMsg 派发 + WebUI。

#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <functional>
#include <filesystem>
#include <nlohmann/json_fwd.hpp>

struct lua_State;
struct sqlite3;

namespace dice {

class LuaPluginManager {
public:
    LuaPluginManager() = default;
    ~LuaPluginManager();

    bool init();                       // 创建 lua_State + 注册全局 + 打开配置库
    void freeRuntime();                // 关闭 lua_State（配置库另行管理）
    bool ready() const { return state_ != nullptr; }

    int  loadDir(const std::string& dir);   // 扫描 data/mod/<mod>/descriptor.json
    int  reload();                          // freeRuntime + init + loadDir(dir_)
    std::string modDir() const { return dir_; }
    // 除主目录外，额外扫描的 mod 目录（规则包 data/rulepacks/<包>/lua/）。reload 时一并加载。
    void setExtraDirs(std::vector<std::string> dirs) { extraDirs_ = std::move(dirs); }

    // 直接求值一段 Lua（供测试/脚本调用）。返回是否成功；err 写错误信息。
    bool eval(const std::string& code, std::string* err = nullptr);

    // ── 阶段二：reply 因果系统 ───────────────────────────────────
    void setSelfName(const std::string& n) { selfName_ = n; }
    void setBotId(const std::string& id) { botId_ = id; }   // getDiceQQ() 返回值
    std::string botId() const { return botId_; }
    // 对一条消息派发 msg_reply 因果回复：keyword 匹配 → echo() → 模板格式化。
    struct DispatchResult { bool matched = false; std::string reply; };
    DispatchResult dispatch(const std::string& text, const std::string& uid,
                            const std::string& gid, const std::string& nick,
                            const std::string& groupCard, bool isPrivate,
                            int trust = 0, const std::string& platform = "");

    // 插件分群启停（地基）：派发前问宿主「此 mod 在该群是否启用」。id="lua:<mod名>"。
    using GroupGateFn = std::function<bool(const std::string& platform, const std::string& group, const std::string& pluginId)>;
    void setGroupGate(GroupGateFn f) { groupGate_ = std::move(f); }
    // 原版 fmt->format：把 {key} 递归解析为 speech 别名/嵌套模板 / msg 变量 / 全局。
    std::string formatTemplate(const std::string& text,
                               const std::map<std::string, std::string>& vars, int depth = 0) const;
    // &field（getUserConf 等）→ 经 speech 解析成实际字段名。
    std::string resolveAmp(const std::string& key) const;

    struct LuaMod {
        std::string name;       // 目录名 / 单文件名（唯一）
        std::string title, author, version, brief;
        std::string dir;        // 完整路径（目录型）或文件路径（单文件型）
        bool enabled = true;
        bool singleFile = false;   // 单文件 Lua 插件（msg_order 派发），非目录型 mod
        bool ruleCompat = false;   // 含 model/*.xml 属性模板 → 规则类（同 JS，在规则管理展示）
        int replies = 0;        // reply/*.lua 数（目录）或 msg_order 条目数（单文件）
        int scripts = 0;        // script/*.lua 数
        std::map<std::string, std::string> helpdoc;   // descriptor.json helpdoc{主题:文本}
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

    // 注入：drawDeck(牌堆名) → 抽牌结果（main.cpp 接牌堆引擎）。
    using DeckDrawFn = std::function<std::string(const std::string&)>;
    void setDeckDraw(DeckDrawFn f) { deckDraw_ = std::move(f); }

    // 配置存取（供 C 函数与外部用）。scope 形如 "u:<uid>" / "g:<gid>"。
    std::string confGet(const std::string& scope, const std::string& key) const;
    bool        confHas(const std::string& scope, const std::string& key) const;
    void        confSet(const std::string& scope, const std::string& key, const std::string& val);
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

    // eventMsg(text, gid, uid)：把 text 当作 uid 在 gid 群的消息跑完整回复管线并发出
    //（复刻原版 DiceLua.cpp::eventMsg → DiceEvent::virtualCall）。gid 空=私聊。
    using EventFn = std::function<void(const std::string& text, const std::string& gid, const std::string& uid)>;
    void setEventMsg(EventFn f) { eventMsg_ = std::move(f); }

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
    SendFn      sender_;
    EventFn     eventMsg_;
    HttpFetchFn httpFetch_;
    GroupGateFn groupGate_;   // 分群启停 gate（地基）
    CardLockFn  cardLockFn_;  // 卡片锁定桥接（真人物卡）
    PlayerCardReadFn playerCardRead_;
    PlayerCardWriteFn playerCardWrite_;
    PlayerCardLockFn playerCardLock_;
    PlayerCardLockedFn playerCardLocked_;
    RollFn      roller_;      // 掷骰引擎桥（pc:rollDice）
    ExtraFn     askExtra_;    // 平台扩展查询（askExtra）

private:
    void registerGlobals();
    void openConfStore();
    int  loadDirLocked(const std::string& dir);
    void loadModReplies(const LuaMod& mod);   // 载入 speech/*.yaml + reply/*.lua
    void loadModFile(const std::filesystem::path& path, bool enabled);   // 单文件 Lua 插件（msg_order）
    std::string valueOf(const std::string& key,
                        const std::map<std::string, std::string>& vars, int depth) const;

    // 一条 msg_reply 因果规则（来自 reply/*.lua）。
    struct ReplyRule {
        std::string name, modName, modDir;
        std::vector<std::string> matchPatterns;    // keyword.match（精确等于）
        std::vector<std::string> prefixPatterns;   // keyword.prefix（前缀，余下为 suffix）
        std::vector<std::string> searchPatterns;   // keyword.search（包含即命中）
        int cdUser = 0, cdGrp = 0;           // limit.cd（秒）
        int trustAtLeast = 0;                // limit.user_var.trust.at_least
        bool groupOnly = false;              // limit.grp_id → 仅群聊
        int  echoRef = 0;                    // echo 为函数时的 registry 引用
        std::string echoScript;              // echo={lua="name"} 时的脚本名（跑 script/<name>.lua）
    };

    lua_State* state_ = nullptr;
    mutable std::recursive_mutex mutex_;
    std::string dir_;
    std::vector<std::string> extraDirs_;   // 规则包附加 mod 目录
    std::vector<LuaMod> mods_;
    std::map<std::string, std::string> speech_;   // 全局 speech 词条（各 mod 合并）
    std::vector<ReplyRule> replyRules_;
    std::string selfName_ = "\xe9\xaa\xb0\xe5\xa8\x98";   // 骰娘（默认自称，main 注入覆盖）
    std::string botId_;                                   // 机器人账号（getDiceQQ）

    sqlite3* confDb_ = nullptr;        // data/lua_mod.db：lua_conf(scope,k,v)
    mutable std::mutex confMutex_;
};

}  // namespace dice
