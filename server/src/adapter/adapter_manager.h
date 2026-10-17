#pragma once
// ─── Dice!Next v3.0.0 — Adapter Manager ──────────────────────
// Manages multiple adapters: load, start, stop, unload, routing.
// An adapter corresponds to one chat platform connection (e.g.,
// a QQ bot via OneBot v11, a Discord bot via gateway, etc.)

#include "adapter_interface.h"
#include "../storage/database.h"
#include "../storage/group_account_settings.h"
#include "../core/identity/identity_binding.h"
#include "../common/logger.h"
#include "../common/utils.h"
#include "../common/log_context.h"

#include <map>
#include <mutex>
#include <functional>
#include <set>
#include <vector>
#include <ctime>

namespace dice {

class AdapterManager {
public:
    explicit AdapterManager(Database& db) : db_(db) { identity::BindingStore::instance().attach(db_); }

    // ─── Lifecycle ───────────────────────────────────────────

    /// Register a built-in or loaded adapter instance
    void registerAdapter(AdapterPtr adapter) {
        AdapterPtr replaced;
        {
            std::lock_guard lock(mutex_);
            auto it = adapters_.find(adapter->id());
            if (it != adapters_.end()) replaced = it->second;
        }
        if (replaced) forgetStarted(replaced.get());
        std::lock_guard lock(mutex_);
        adapter->attachDatabase(db_);
        adapters_[adapter->id()] = adapter;
        adapter->plainTextResolver = [this](const Message& m) {
            auto* st = db_.getStorage();
            if (!st || m.targetId.empty()) return false;
            const std::string key = m.type == MessageType::kPrivate ? "replyTextPrivate" : "replyTextGroup";
            namespace orm = sqlite_orm;
            try {
                auto rows = st->get_all<GroupAccountSettingRow>(orm::where(
                    orm::c(&GroupAccountSettingRow::adapterId) == m.adapterId
                    and orm::c(&GroupAccountSettingRow::key) == key
                    and (orm::c(&GroupAccountSettingRow::groupId) == m.targetId
                        or orm::c(&GroupAccountSettingRow::endpointId) == m.targetId)));
                if (!rows.empty()) return rows.front().value == "1";
            } catch (...) {}
            return false;
        };
        // Wire adapter's incoming messages → manager's routeMessage
        adapter->onMessage([this](const Message& msg) {
            routeMessage(msg);
        });
        // Wire adapter's non-message events (notice/request) → routeEvent
        adapter->onEvent([this](const BotEvent& ev) {
            routeEvent(ev);
        });
        DICE_LOG_INFO("已注册适配器「{}」（{}）。", adapter->name(), adapter->id());
    }

    /// Start a specific adapter (connect to platform)
    bool startAdapter(const std::string& id) {
        auto a = getAdapter(id);
        if (!a) return false;
        std::lock_guard startLock(startMutex_);
        if (a->start()) {
            started_.insert(a.get());
            DICE_LOG_INFO("适配器「{}」启动请求已提交，连接结果由适配器报告。", a->name());
            return true;
        }
        DICE_LOG_ERROR("适配器「{}」启动失败：{}", a->name(), a->lastError());
        return false;
    }

    /// Stop a specific adapter (disconnect)
    void stopAdapter(const std::string& id) {
        auto a = getAdapter(id);
        if (!a) return;
        a->stop();
        forgetStarted(a.get());
        DICE_LOG_INFO("适配器「{}」已停止。", a->name());
    }

    /// Start all registered adapters
    ///
    /// 启动后 0.5 秒才跑这一趟。这半秒里管理接口可能已经建好并启动了适配器（BDC
    /// 托管开通就在这个窗口里调 POST /api/adapters），再启动一次会重建反向 WS 的
    /// TcpServer，trantor 直接中止进程。已经启动过的同一个适配器对象跳过。
    void startAll() {
        std::vector<AdapterPtr> pending;
        {
            std::lock_guard lock(mutex_);
            for (auto& [id, a] : adapters_) pending.push_back(a);
        }
        std::lock_guard startLock(startMutex_);
        for (auto& a : pending) {
            if (started_.count(a.get())) continue;
            if (a->start()) {
                started_.insert(a.get());
                DICE_LOG_INFO("适配器「{}」启动请求已提交，尚不代表平台已经连接。", a->name());
            } else {
                DICE_LOG_ERROR("适配器「{}」启动失败：{}", a->name(), a->lastError());
            }
        }
    }

    /// Stop all adapters
    void stopAll() {
        for (auto& [id, a] : adapters_) a->stop();
        {
            std::lock_guard startLock(startMutex_);
            started_.clear();
        }
        DICE_LOG_INFO("所有适配器已停止。");
    }

    /// Unregister (remove) an adapter
    void unregisterAdapter(const std::string& id) {
        AdapterPtr removed;
        {
            std::lock_guard lock(mutex_);
            auto it = adapters_.find(id);
            if (it == adapters_.end()) return;
            removed = it->second;
            if (removed->isConnected()) removed->stop();
            adapters_.erase(it);
        }
        forgetStarted(removed.get());
        DICE_LOG_INFO("适配器「{}」已移除。", id);
    }

    // ─── Query ───────────────────────────────────────────────

    AdapterPtr getAdapter(const std::string& id) const {
        std::lock_guard lock(mutex_);
        auto it = adapters_.find(id);
        return (it != adapters_.end()) ? it->second : nullptr;
    }

    std::vector<AdapterPtr> allAdapters() const {
        std::lock_guard lock(mutex_);
        std::vector<AdapterPtr> result;
        for (auto& [id, a] : adapters_) result.push_back(a);
        return result;
    }

    // ─── Message Routing ─────────────────────────────────────

    /// Route an incoming message to all message handlers.
    /// Called by adapters when they receive a message.
    void routeMessage(const Message& msg) {
        markActive(msg.adapterId);
        Message routed = msg;
        normalizeIdentity(routed);
        cacheOfficialNickname(routed);
        ensureInboundGroup(routed);
        const auto adapter = getAdapter(routed.adapterId);
        const std::string adapterName = adapter ? adapter->name() : routed.adapterId;
        const bool privateMessage = routed.type == MessageType::kPrivate;
        const std::string groupName = !privateMessage && adapter ? adapter->getGroupName(routed.targetId) : "";
        writeLog(DICE_LOG_SOURCE, LogLevel::Info, "消息", fmt::format("收↩ [{}「{}」] [{}{}({})] {}({})：{}",
            routed.platform, adapterName, privateMessage ? "私聊 " : "群 ", groupName, routed.targetId,
            routed.senderName.empty() ? routed.senderId : routed.senderName, routed.senderId,
            routed.displayContent.empty() ? routed.content : routed.displayContent),
            {{"adapter", routed.adapterId}, {"platform", routed.platform}, {"platform_msg_id", routed.id},
             {"sender_name_source", routed.extra.is_object() ? routed.extra.value("__sender_name_source", std::string("platform")) : "platform"}});
        ProcessingScope incoming(ProcessingContext{{}, "用户消息：" + routed.content, "正在处理用户消息"});
        for (auto& handler : messageHandlers_) {
            try { handler(routed); }
            catch (...) { DICE_LOG_EXCEPTION("消息处理未完成；适配器 " + routed.adapterId + "，平台 " + routed.platform); }
        }
    }

    /// Register a message handler (called for every incoming message)
    void onMessage(IAdapter::MessageCallback handler) {
        messageHandlers_.push_back(std::move(handler));
    }

    /// Route an incoming non-message event to all event handlers.
    void routeEvent(const BotEvent& ev) {
        markActive(ev.adapterId);
        BotEvent routed = ev;
        // QQ 官方事件携带的是 OpenID，而消息路径已把群号映射为公共群号。
        // 这里做同样的归一化，避免事件写出的 left/__removed/inviter 挂在
        // “OpenID 幽灵群记录”上（WebUI 显示已退群、消息也无法重建该记录）。
        if (routed.platform == "qq_official") {
            const std::string botId = routed.extra.value("official_bot_id", std::string());
            if (!botId.empty()) {
                if (!routed.groupId.empty()) {
                    routed.extra["__identity_native_target"] = routed.groupId;
                    const std::string publicId = identity::BindingStore::instance()
                        .observeOfficial(db_, botId, routed.groupId, identity::Kind::Group);
                    if (!publicId.empty()) routed.groupId = publicId;
                }
                // Friend/member/operator events must use the same public user
                // IDs as messages, while the transport IDs and opaque request
                // flags remain available for outgoing API calls.
                if (!routed.userId.empty() && routed.userId != routed.selfId) {
                    routed.extra["__identity_native_sender"] = routed.userId;
                    routed.userId = identity::BindingStore::instance().observeOfficial(db_, botId, routed.userId, identity::Kind::User);
                }
                if (!routed.operatorId.empty() && routed.operatorId != routed.selfId) {
                    routed.extra["__identity_native_operator"] = routed.operatorId;
                    routed.operatorId = identity::BindingStore::instance().observeOfficial(db_, botId, routed.operatorId, identity::Kind::User);
                }
                if (const auto adapter = getAdapter(routed.adapterId)) {
                    if (!routed.userId.empty() && routed.userId != routed.selfId) {
                        auto name = routed.extra.value("user_name", std::string());
                        if (name.empty()) name = adapter->getUserName(routed.userId, routed.groupId);
                        if (!name.empty()) routed.extra["user_name"] = name;
                    }
                    if (!routed.operatorId.empty()) {
                        auto name = routed.extra.value("operator_name", std::string());
                        if (name.empty()) name = adapter->getUserName(routed.operatorId, routed.groupId);
                        if (!name.empty()) routed.extra["operator_name"] = name;
                    }
                }
            }
        }
        for (auto& handler : eventHandlers_) {
            try { handler(routed); }
            catch (...) { DICE_LOG_EXCEPTION("event handler adapter=" + routed.adapterId + " platform=" + routed.platform); }
        }
    }

    /// Register an event handler (called for every notice/request event).
    void onEvent(IAdapter::EventCallback handler) {
        eventHandlers_.push_back(std::move(handler));
    }

    /// 该适配器最近一次收到消息/事件的 UTC ISO 时间；从未活跃时返回空串。
    std::string lastActiveAt(const std::string& adapterId) const {
        std::lock_guard lock(mutex_);
        auto it = lastActiveAt_.find(adapterId);
        return it == lastActiveAt_.end() ? std::string() : it->second;
    }

    /// Broadcast a reply to all connected adapters.
    /// Usually you'd want to reply through the same adapter that received the message.
    void broadcastToAll(const std::string& /*text*/) {
        for (auto& [id, a] : adapters_) {
            if (a->isConnected()) {
                // broadcast is adapter-specific
            }
        }
    }

private:
    void markActive(const std::string& adapterId) {
        if (adapterId.empty()) return;
        std::lock_guard lock(mutex_);
        lastActiveAt_[adapterId] = utils::nowIso8601();
    }
    static std::string profileTimestamp() {
        return utils::formatTimeInTimezone(std::time(nullptr), "%Y-%m-%dT%H:%M:%S");
    }
    void cacheOfficialNickname(const Message& msg) {
        if (msg.platform != "qq_official" || msg.senderId.empty()
            || msg.senderName.empty() || (msg.extra.is_object() && msg.extra.value("__sender_name_fallback", false))) return;
        auto* st = db_.getStorage(); if (!st) return;
        try {
            auto profiles = st->get_all<PlayerProfileRow>(orm::where(
                orm::c(&PlayerProfileRow::platform) == std::string("qq_official") and
                orm::c(&PlayerProfileRow::userId) == msg.senderId), orm::limit(1));
            if (profiles.empty()) {
                PlayerProfileRow row; row.platform = "qq_official"; row.userId = msg.senderId;
                row.nickname = msg.senderName; row.createdAt = profileTimestamp(); st->insert(row);
            } else if (profiles.front().nickname != msg.senderName) {
                st->update_all(orm::set(orm::assign(&PlayerProfileRow::nickname, msg.senderName)),
                    orm::where(orm::c(&PlayerProfileRow::id) == profiles.front().id));
            }
        } catch (...) {}
    }
    void ensureInboundGroup(const Message& msg) {
        if (msg.type != MessageType::kGroup || msg.targetId.empty()) return;
        auto* st = db_.getStorage(); if (!st) return;
        try {
            // A message from the group proves that the bot is still present.
            // Restore a record hidden by a previous WebUI deletion; otherwise a
            // QQ Official group could remain invisible forever because that
            // platform has no separate re-join event in this path.
            if (!msg.adapterId.empty()) {
                st->remove_all<GroupAccountSettingRow>(orm::where(
                    orm::c(&GroupAccountSettingRow::adapterId) == msg.adapterId and
                    orm::c(&GroupAccountSettingRow::groupId) == msg.targetId and
                    orm::c(&GroupAccountSettingRow::key) == std::string("__removed")));
                // 兼容旧数据：早期“删除记录”的墓碑在共享表，消息证明骰子仍在
                // 群时必须一并清掉，否则 WebUI 永远看不到这条群记录。
                st->remove_all<GroupSettingRow>(orm::where(
                    orm::c(&GroupSettingRow::platform) == msg.platform and
                    orm::c(&GroupSettingRow::groupId) == msg.targetId and
                    orm::c(&GroupSettingRow::key) == std::string("__removed")));
                st->remove_all<GroupSettingRow>(orm::where(
                    orm::c(&GroupSettingRow::platform) == msg.platform and
                    orm::c(&GroupSettingRow::groupId) == msg.targetId and
                    orm::c(&GroupSettingRow::key) == std::string("left")));
                st->remove_all<GroupSettingRow>(orm::where(
                    orm::c(&GroupSettingRow::platform) == msg.platform and
                    orm::c(&GroupSettingRow::groupId) == msg.targetId and
                    orm::c(&GroupSettingRow::key) == std::string("leaving")));
                auto rows = st->get_all<GroupAccountSettingRow>(orm::where(
                    orm::c(&GroupAccountSettingRow::adapterId) == msg.adapterId and
                    orm::c(&GroupAccountSettingRow::groupId) == msg.targetId and
                    orm::c(&GroupAccountSettingRow::key) == std::string("enabled")), orm::limit(1));
                if (rows.empty()) {
                    GroupAccountSettingRow row; row.adapterId = msg.adapterId;
                    row.platform = msg.platform; row.groupId = msg.targetId;
                    row.endpointId = msg.extra.value("__identity_native_target", msg.targetId);
                    // Receiving a message proves presence, not permission to
                    // enable the bot. Inherit the imported shared preference.
                    row.key = "enabled";
                    row.value = accountGroupSetting(*st, "", msg.platform, msg.targetId, "enabled");
                    if (row.value.empty()) row.value = "1";
                    st->insert(row);
                }
                auto setPresence = [&](const std::string& key, const std::string& value) {
                    auto found = st->get_all<GroupAccountSettingRow>(orm::where(
                        orm::c(&GroupAccountSettingRow::adapterId) == msg.adapterId and
                        orm::c(&GroupAccountSettingRow::groupId) == msg.targetId and
                        orm::c(&GroupAccountSettingRow::key) == key), orm::limit(1));
                    if (found.empty()) {
                        GroupAccountSettingRow row; row.adapterId = msg.adapterId; row.platform = msg.platform;
                        row.groupId = msg.targetId; row.endpointId = msg.extra.value("__identity_native_target", msg.targetId);
                        row.key = key; row.value = value; st->insert(row);
                    } else { auto row = found.front(); row.value = value; st->update(row); }
                };
                setPresence("left", "0");
                setPresence("leaving", "0");
            } else {
                st->remove_all<GroupSettingRow>(orm::where(
                    orm::c(&GroupSettingRow::platform) == msg.platform and
                    orm::c(&GroupSettingRow::groupId) == msg.targetId and
                    orm::c(&GroupSettingRow::key) == std::string("__removed")));
                auto rows = st->get_all<GroupSettingRow>(orm::where(
                    orm::c(&GroupSettingRow::platform) == msg.platform and
                    orm::c(&GroupSettingRow::groupId) == msg.targetId and
                    orm::c(&GroupSettingRow::key) == std::string("enabled")), orm::limit(1));
                if (rows.empty()) { GroupSettingRow row; row.platform = msg.platform; row.groupId = msg.targetId; row.key = "enabled"; row.value = "1"; st->insert(row); }
            }
        } catch (...) {}
    }
    void normalizeIdentity(Message& msg) {
        using identity::BindingStore;
        using identity::Kind;
        auto& bindings = BindingStore::instance();
        if (msg.platform == "qq_official") {
            const std::string botId = msg.extra.value("official_bot_id", std::string());
            if (botId.empty()) return;
            const Kind scope = msg.type == MessageType::kPrivate ? Kind::User : Kind::Group;
            const std::string rawTarget = msg.targetId;
            const std::string rawSender = msg.senderId;
            const std::string localTarget = BindingStore::officialId(botId, rawTarget);
            const std::string localSender = BindingStore::officialId(botId, rawSender);
            const std::string publicTarget = bindings.observeOfficial(db_, botId, rawTarget, scope);
            const std::string publicSender = bindings.observeOfficial(db_, botId, rawSender, Kind::User);
            msg.extra["__identity_transport"] = "qq_official";
            msg.extra["__identity_native_target"] = rawTarget;
            msg.extra["__identity_native_sender"] = rawSender;
            msg.extra["__identity_local_target"] = localTarget;
            msg.extra["__identity_local_sender"] = localSender;
            msg.extra["__identity_qualified_target"] = BindingStore::qualified(scope, publicTarget);
            msg.extra["__identity_qualified_sender"] = BindingStore::qualified(Kind::User, publicSender);
            msg.targetId = publicTarget;
            msg.senderId = publicSender;
            // New events can carry username. Empty values use the adapter's
            // persisted OpenID/union-name cache, then a bound public profile.
            // A display fallback must never become a cached platform nickname.
            if (msg.senderName.empty()) {
                std::string cachedName;
                if (const auto adapter = getAdapter(msg.adapterId)) cachedName = adapter->getUserName(publicSender,
                    msg.type == MessageType::kGroup ? msg.targetId : std::string());
                if (auto* st = db_.getStorage()) {
                    try {
                        auto profiles = st->get_all<PlayerProfileRow>(orm::where(
                            orm::c(&PlayerProfileRow::platform) == std::string("qq_official") and
                            orm::c(&PlayerProfileRow::userId) == publicSender), orm::limit(1));
                        if (cachedName.empty() && !profiles.empty() && profiles.front().nickname != "用户"
                            && profiles.front().nickname != rawSender && profiles.front().nickname != publicSender) cachedName = profiles.front().nickname;
                    } catch (...) {}
                }
                if (!cachedName.empty()) { msg.senderName = cachedName; msg.extra["__sender_name_source"] = "cache"; }
                else {
                    msg.senderName = "用户";
                    msg.extra["__sender_name_fallback"] = true;
                    msg.extra["__sender_name_source"] = "missing";
                }
            }
            for (auto& at : msg.atList) {
                if (at == msg.selfId) continue;
                at = bindings.observeOfficial(db_, botId, at, Kind::User);
            }
            return;
        }
        if (msg.platform == "discord" || msg.platform == "kook") {
            // Discord/KOOK 原生 id 可能与 QQ 号数字碰撞（KOOK 用户 id 纯数字），
            // 一律经身份系统映射为公共号（未绑定=虚拟号，绑定后=真实 QQ）。
            const Kind scope = msg.type == MessageType::kPrivate ? Kind::User : Kind::Group;
            const std::string rawTarget = msg.targetId;
            const std::string rawSender = msg.senderId;
            const std::string publicTarget = bindings.observeVirtual(db_, msg.platform, msg.adapterId, rawTarget, scope);
            const std::string publicSender = bindings.observeVirtual(db_, msg.platform, msg.adapterId, rawSender, Kind::User);
            msg.extra["__identity_transport"] = msg.platform;
            msg.extra["__identity_native_target"] = rawTarget;
            msg.extra["__identity_native_sender"] = rawSender;
            msg.extra["__identity_qualified_target"] = BindingStore::qualified(scope, publicTarget);
            msg.extra["__identity_qualified_sender"] = BindingStore::qualified(Kind::User, publicSender);
            if (!publicTarget.empty()) msg.targetId = publicTarget;
            if (!publicSender.empty()) msg.senderId = publicSender;
            for (auto& at : msg.atList) {
                if (at == msg.selfId || at == "all") continue;
                const auto pub = bindings.observeVirtual(db_, msg.platform, msg.adapterId, at, Kind::User);
                if (!pub.empty()) at = pub;
            }
            return;
        }
        if ((msg.platform == "onebot_v11" || msg.platform == "milky") &&
            (msg.type == MessageType::kGroup || msg.type == MessageType::kPrivate)) {
            const std::string rawTarget = msg.targetId;
            const std::string rawSender = msg.senderId;
            const Kind scope = msg.type == MessageType::kPrivate ? Kind::User : Kind::Group;
            const std::string transport = msg.platform;
            const std::string publicTarget = bindings.observeQQ(db_, transport, msg.adapterId, rawTarget, scope);
            const std::string publicSender = bindings.observeQQ(db_, transport, msg.adapterId, rawSender, Kind::User);
            msg.extra["__identity_transport"] = transport;
            msg.extra["__identity_native_target"] = rawTarget;
            msg.extra["__identity_native_sender"] = rawSender;
            msg.extra["__identity_qualified_target"] = BindingStore::qualified(scope, publicTarget);
            msg.extra["__identity_qualified_sender"] = BindingStore::qualified(Kind::User, publicSender);
            msg.targetId = publicTarget;
            msg.senderId = publicSender;
        }
    }
    Database& db_;
    std::map<std::string, AdapterPtr> adapters_;
    std::vector<IAdapter::MessageCallback> messageHandlers_;
    std::vector<IAdapter::EventCallback> eventHandlers_;
    mutable std::mutex mutex_;
    //: 由 startAdapter / startAll 启动过的适配器对象。按对象记而不是按 id：改配置会
    //: 换一个新对象注册到同一个 id 上，新对象必须照常启动。
    std::mutex startMutex_;
    std::set<const IAdapter*> started_;

    void forgetStarted(const IAdapter* adapter) {
        std::lock_guard startLock(startMutex_);
        started_.erase(adapter);
    }
    std::map<std::string, std::string> lastActiveAt_;   // adapterId → UTC ISO（受 mutex_ 保护）
};

} // namespace dice
