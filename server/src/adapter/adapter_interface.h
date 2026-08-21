#pragma once
// ─── Dice!Next v3.0.0 — Adapter Plugin Interface ─────────────
// Protocol-agnostic abstract interface for chat platform adapters.
// Replace the old DD:: namespace (hardcoded QQAPI) with a
// plugin-based architecture — OneBot v11, Milky, Discord, Kook, etc.
#include "../common/content_format.h"

#include <string>
#include <functional>
#include <memory>
#include <vector>
#include <utility>
#include <atomic>
#include <nlohmann/json.hpp>

namespace dice {

using json = nlohmann::json;

// ═══════════════════════════════════════════════════════════════
// Message Envelope — platform-agnostic message representation
// ═══════════════════════════════════════════════════════════════

enum class MessageType {
    kPrivate,   // 私聊
    kGroup,     // 群聊
    kChannel,   // 频道/子频道 (Discord/Kook)
};

struct Message {
    std::string id;           // 消息唯一ID
    std::string platform;     // 来源平台（"onebot_v11" / "discord" ...），用于 i18n 语言解析
    std::string content;      // 纯文本内容（已去除CQ码和@后的文本，用于指令解析）
    std::string rawContent;   // 原始消息（含平台特定格式，CQ码原样：[CQ:image,..]）
    std::string displayContent; // 给人看的可读形式（按原顺序：文字/@xxx/[图片]/[表情]/[语音]），用于日志/转录/模拟聊天
    std::string senderId;     // 发送者ID
    std::string senderName;   // 发送者昵称
    std::string targetId;     // 目标（群号/频道号/私聊对方QQ）
    std::string selfId;       // 接收此消息的机器人自身账号（用于多骰娘群指定账号）
    std::string adapterId;    // 接收此消息的适配器 id（用于回执/退群等平台操作定位适配器）
    std::vector<std::string> atList;  // 被@的账号列表（"all" 表示@全体），用于账号定向
    MessageType type = MessageType::kGroup;
    int64_t timestamp = 0;
    bool fromSelf = false;    // 本消息由骰娘账号自身发出（post_type=message_sent，自控开关开启时才进管线）
    json extra;               // 平台特定附加数据（CQ码、附件等）
};

// ═══════════════════════════════════════════════════════════════
// Event Envelope — non-message platform events (notice / request),
// abstracted so the engine / custom-reply / plugins can react to them
// uniformly regardless of platform.
// ═══════════════════════════════════════════════════════════════

enum class EventType {
    kGroupIncrease,   // 有人入群（含机器人自己入群）
    kGroupDecrease,   // 有人退群/被踢
    kFriendAdd,       // 被添加为好友（已通过）
    kFriendRequest,   // 收到加好友请求（待处理）
    kGroupRequest,    // 收到加群/邀请请求（待处理）
    kPoke,            // 戳一戳
    kGroupRecall,     // 群消息撤回（extra.message_id = 被撤回消息）
    kGroupHistory,    // 历史消息拉取结果（extra.messages = OneBot 消息数组）
    kGroupUpload,     // 群文件上传（extra.file = {id,name,size,busid}）
    kOther,
};

struct BotEvent {
    EventType type = EventType::kOther;
    std::string platform;     // 来源平台
    std::string adapterId;    // 接收此事件的适配器 id
    std::string selfId;       // 机器人自身账号
    std::string groupId;      // 群事件的群号（私聊/好友事件为空）
    std::string userId;       // 事件主体用户（入群者/请求者/新好友/被戳者）
    std::string operatorId;   // 触发者（邀请人/操作者/戳人者）
    std::string comment;      // 请求验证信息
    std::string flag;         // 请求标识（用于同意/拒绝）
    std::string subType;      // 请求子类型（add 申请 / invite 邀请）
    int64_t timestamp = 0;
    json extra;               // 平台原始事件
};

// ═══════════════════════════════════════════════════════════════
// Abstract Adapter Interface
// ═══════════════════════════════════════════════════════════════

class IAdapter {
public:
    virtual ~IAdapter() = default;

    /// Global outbound presentation selected by the dice owner.  Adapters that
    /// have no rich-message equivalent simply keep sending traditional text.
    static void setCardMessageMode(bool enabled) noexcept { cardMessageMode_.store(enabled); }
    static bool cardMessageMode() noexcept { return cardMessageMode_.load(); }
    /// 解析适配器级 message_format：""=跟随全局, "traditional"=传统文本, "card"=卡片。
    static int parseFormatOverride(const std::string& v) {
        return v == "card" ? 1 : v == "traditional" ? 0 : -1;
    }
    /// 每个适配器可单独覆盖出站消息形式（-1=跟随全局，0=传统文本，1=卡片）。
    void setMessageFormatOverride(int mode) noexcept { messageFormatOverride_.store(mode); }
    bool effectiveCardMode() const noexcept {
        const int v = messageFormatOverride_.load();
        return v < 0 ? cardMessageMode() : v > 0;
    }

    /// Unique adapter identifier (e.g. "onebot-v11-1")
    virtual std::string id() const = 0;

    /// Human-readable name (e.g. "我的QQ机器人")
    virtual std::string name() const = 0;

    /// Platform name (e.g. "onebot_v11", "discord", "kook", "milky")
    virtual std::string platform() const = 0;

    /// Adapter version string
    virtual std::string version() const = 0;

    // ─── Lifecycle ───────────────────────────────────────────

    /// Configure the adapter from JSON (endpoint, token, etc.)
    virtual bool configure(const json& config) = 0;

    /// Connect to the platform
    virtual bool start() = 0;

    /// Disconnect gracefully
    virtual void stop() = 0;

    /// Whether currently connected
    virtual bool isConnected() const = 0;

    /// Last error message
    virtual std::string lastError() const = 0;

    /// fine-grained connection state for the web UI.
    /// Default just maps isConnected()→"connected"/"disconnected"; adapters with a
    /// reconnect backoff (e.g. OneBot) also report "timeout" when retries are paused.
    virtual std::string connectionStatus() const { return isConnected() ? "connected" : "disconnected"; }

    /// manually resume a paused/timed-out connection (resets backoff). Default = start().
    virtual void resumeConnection() { start(); }

    // ─── Messaging ───────────────────────────────────────────

    /// Send a message to the platform
    virtual void sendMessage(const Message& msg) = 0;

    /// Reply to a specific message (platform-specific quoting)
    virtual void sendReply(const Message& original, const std::string& replyText) = 0;
    /// Format-aware path used by the core reply pipeline. Existing plugin and
    /// adapter calls remain plain text through sendReply for compatibility.
    virtual void sendReplyFormatted(const Message& original, const std::string& replyText,
                                    ContentFormat /*format*/) {
        sendReply(original, replyText);
    }

    // ─── Event Callbacks ─────────────────────────────────────

    /// Called when a new message arrives from the platform.
    /// The Dice engine should register its handler here.
    using MessageCallback = std::function<void(const Message&)>;
    virtual void onMessage(MessageCallback cb) = 0;

    /// Called when a non-message event (notice/request) arrives. Default no-op
    /// so adapters that don't surface events still compile.
    using EventCallback = std::function<void(const BotEvent&)>;
    virtual void onEvent(EventCallback /*cb*/) {}

    /// Approve/deny a pending friend request (by its flag). Default no-op.
    virtual void setFriendRequest(const std::string& /*flag*/, bool /*approve*/,
                                  const std::string& /*remark*/ = "") {}
    /// Approve/deny a pending group join/invite request. Default no-op.
    virtual void setGroupRequest(const std::string& /*flag*/, const std::string& /*subType*/,
                                 bool /*approve*/, const std::string& /*reason*/ = "") {}

    // ─── Platform Operations ─────────────────────────────────

    /// Get current login identity
    virtual std::string getLoginId() const = 0;
    virtual std::string getLoginName() const = 0;

    /// Group management
    virtual std::string getGroupName(const std::string& groupId) const = 0;
    virtual std::vector<std::string> getGroupMemberList(const std::string& groupId) const = 0;
    virtual bool isGroupAdmin(const std::string& groupId, const std::string& userId) const = 0;
    virtual bool isGroupOwner(const std::string& groupId, const std::string& userId) const = 0;

    /// Group operations
    virtual void setGroupKick(const std::string& groupId, const std::string& userId) = 0;
    virtual void setGroupBan(const std::string& groupId, const std::string& userId, int durationSec) = 0;

    /// Leave a group (.dismiss). Default no-op for platforms that don't support it.
    virtual void leaveGroup(const std::string& /*groupId*/) {}

    /// Set a member's group card / nickname (web group-management). For the bot's
    /// own card pass userId = getLoginId(). Default no-op.
    virtual void setGroupCard(const std::string& /*groupId*/, const std::string& /*userId*/,
                              const std::string& /*card*/) {}

    /// Set the group's name/title (web group-management). Default no-op.
    virtual void setGroupName(const std::string& /*groupId*/, const std::string& /*name*/) {}

    /// Grant a member a special title (group-owner only). Default no-op.
    virtual void setGroupSpecialTitle(const std::string& /*groupId*/, const std::string& /*userId*/,
                                      const std::string& /*title*/) {}

    /// Joined-group list (id, name) cached from the platform. Default empty.
    virtual std::vector<std::pair<std::string, std::string>> getGroupList() const { return {}; }

    /// number of QQ friends the bot has (-1 = unknown / not synced yet). Default -1.
    virtual int getFriendCount() const { return -1; }

    /// ask the platform (e.g. NapCat get_group_msg_history) for recent group
    /// history. Result arrives asynchronously as a kGroupHistory event. Default no-op.
    virtual void requestGroupHistory(const std::string& /*groupId*/, int /*count*/) {}

    /// poke a group member (NapCat group_poke / 戳一戳). Default no-op.
    virtual void sendGroupPoke(const std::string& /*groupId*/, const std::string& /*userId*/) {}

    /// friend uid list (synced with get_friend_list; empty = unknown). Default empty.
    virtual std::vector<std::string> getFriendList() const { return {}; }

    /// delete a friend (NapCat delete_friend). Default no-op.
    virtual void deleteFriend(const std::string& /*userId*/) {}
    /// Ask the platform to refresh the joined-group list (async). Default no-op.
    virtual void refreshGroupList() {}

    /// Cached member count for a joined group (from the group list). Default 0.
    virtual int getGroupMemberCount(const std::string& /*groupId*/) const { return 0; }

    /// The bot's own role in a group: "owner" | "admin" | "member" | "". Default "".
    virtual std::string getSelfRole(const std::string& /*groupId*/) const { return ""; }
    /// Ask the platform for ONLY the bot's role in a group (cheap; no full member list).
    virtual void refreshSelfRole(const std::string& /*groupId*/) {}
    /// Ask the platform to refresh a group's member list + the bot's role (async).
    virtual void refreshMembers(const std::string& /*groupId*/) {}
    /// Ask the platform for ONE member's role (owner/admin/member) when a
    /// message did not carry it. Async; default no-op.
    virtual void refreshMemberRole(const std::string& /*groupId*/, const std::string& /*userId*/) {}
    /// Cached member list of a group (platform-native objects). Default empty array.
    virtual json getMembers(const std::string& /*groupId*/) const { return json::array(); }

    /// 同步调用平台 API 并等待响应（群文件列表/下载链等请求-响应式接口）。
    /// 阻塞至响应或超时（返回 null）。⚠️ 不得在适配器接收线程上调用（会死锁），
    /// 供 WebUI 的 drogon 处理线程使用。默认不支持。
    virtual json invokeAction(const std::string& /*action*/, const json& /*params*/,
                              int /*timeoutMs*/ = 8000) { return json(); }

    /// 异步平台操作。HTTP 型适配器可覆盖此接口，避免在 Web 事件循环中阻塞等待。
    using ActionCallback = std::function<void(json)>;
    virtual void invokeActionAsync(const std::string& action, const json& params, ActionCallback cb) {
        cb(invokeAction(action, params));
    }

    /// Send a plain text message to a group/user from the web admin. Default no-op.
    virtual void sendGroupMessage(const std::string& /*groupId*/, const std::string& /*text*/) {}
    virtual void sendGroupMessageFormatted(const std::string& groupId, const std::string& text,
                                           ContentFormat /*format*/) {
        sendGroupMessage(groupId, text);
    }

    /// Send a group message whose text may contain platform codes (CQ codes like
    /// [CQ:at,qq=..] / [CQ:image,..]) that the platform should parse. Default
    /// falls back to plain send. Default routes to sendGroupMessage.
    virtual void sendGroupMessageCQ(const std::string& groupId, const std::string& cqText) {
        sendGroupMessage(groupId, cqText);
    }

    /// Send a private message to a user. Default no-op.
    virtual void sendPrivateMessage(const std::string& /*userId*/, const std::string& /*text*/) {}
    virtual void sendPrivateMessageFormatted(const std::string& userId, const std::string& text,
                                             ContentFormat /*format*/) {
        sendPrivateMessage(userId, text);
    }

    /// Send a "merged forward" (合并转发/聊天记录) to a group: each entry in @p nodes
    /// becomes one chat bubble inside a single forwarded record. Returns true if it
    /// was sent as a real forward; false if the platform doesn't support it (the
    /// caller should then fall back to plain segmented messages). Default: unsupported.
    virtual bool sendGroupForwardMsg(const std::string& /*groupId*/,
                                     const std::vector<std::string>& /*nodes*/) { return false; }
    virtual bool sendGroupForwardMsgFormatted(const std::string& groupId,
                                              const std::vector<std::string>& nodes,
                                              ContentFormat /*format*/) {
        return sendGroupForwardMsg(groupId, nodes);
    }

    /// Upload a file to a group's files (.log end → txt). @p content is the raw
    /// file bytes (so the platform client needn't share a filesystem with us —
    /// remote/containered OneBot can't stat our local path); @p localPath is an
    /// optional on-disk copy for impls that require a path. Default no-op.
    virtual void uploadGroupFile(const std::string& /*groupId*/, const std::string& /*name*/,
                                 const std::string& /*content*/, const std::string& /*localPath*/ = "") {}

    /// 平台能力声明：上层功能（好友清理/群管按钮/戳一戳/合并转发/群文件等）
    /// 一律按能力位开关，禁止用 platform 字符串猜。缺省全 false，各适配器如实申报。
    /// keys: friends / friend_delete / kick / ban(禁言) / poke / forward(合并转发) / group_file /
    ///       group_card(名片) / member_list / group_leave
    virtual json capabilities() const {
        return {
            {"friends", false}, {"friend_delete", false}, {"kick", false}, {"ban", false}, {"poke", false},
            {"forward", false}, {"group_file", false}, {"group_card", false},
            {"member_list", false}, {"group_leave", false},
        };
    }

    /// Get status info for WebUI
    virtual json getStatus() const {
        return {
            {"id", id()},
            {"name", name()},
            {"platform", platform()},
            {"version", version()},
            {"connected", isConnected()},
            {"loginId", getLoginId()},
            {"loginName", getLoginName()}
        };
    }

private:
    inline static std::atomic_bool cardMessageMode_{false};
    std::atomic<int> messageFormatOverride_{-1};
};

using AdapterPtr = std::shared_ptr<IAdapter>;

} // namespace dice
