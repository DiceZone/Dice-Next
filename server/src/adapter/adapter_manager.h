#pragma once
// ─── Dice!Next v3.0.0 — Adapter Manager ──────────────────────
// Manages multiple adapters: load, start, stop, unload, routing.
// An adapter corresponds to one chat platform connection (e.g.,
// a QQ bot via OneBot v11, a Discord bot via gateway, etc.)

#include "adapter_interface.h"
#include "../storage/database.h"
#include "../core/identity/identity_binding.h"
#include "../common/logger.h"

#include <map>
#include <mutex>
#include <functional>
#include <ctime>

namespace dice {

class AdapterManager {
public:
    explicit AdapterManager(Database& db) : db_(db) { identity::BindingStore::instance().attach(db_); }

    // ─── Lifecycle ───────────────────────────────────────────

    /// Register a built-in or loaded adapter instance
    void registerAdapter(AdapterPtr adapter) {
        std::lock_guard lock(mutex_);
        adapters_[adapter->id()] = adapter;
        // Wire adapter's incoming messages → manager's routeMessage
        adapter->onMessage([this](const Message& msg) {
            routeMessage(msg);
        });
        // Wire adapter's non-message events (notice/request) → routeEvent
        adapter->onEvent([this](const BotEvent& ev) {
            routeEvent(ev);
        });
        DICE_LOG_INFO("AdapterManager: registered '{}' ({})", adapter->name(), adapter->id());
    }

    /// Start a specific adapter (connect to platform)
    bool startAdapter(const std::string& id) {
        auto a = getAdapter(id);
        if (!a) return false;
        if (a->start()) {
            DICE_LOG_INFO("AdapterManager: '{}' started", a->name());
            return true;
        }
        DICE_LOG_ERROR("AdapterManager: '{}' start failed: {}", a->name(), a->lastError());
        return false;
    }

    /// Stop a specific adapter (disconnect)
    void stopAdapter(const std::string& id) {
        auto a = getAdapter(id);
        if (!a) return;
        a->stop();
        DICE_LOG_INFO("AdapterManager: '{}' stopped", a->name());
    }

    /// Start all registered adapters
    void startAll() {
        for (auto& [id, a] : adapters_) {
            if (a->start()) {
                DICE_LOG_INFO("AdapterManager: '{}' online", a->name());
            } else {
                DICE_LOG_ERROR("AdapterManager: '{}' failed: {}", a->name(), a->lastError());
            }
        }
    }

    /// Stop all adapters
    void stopAll() {
        for (auto& [id, a] : adapters_) a->stop();
        DICE_LOG_INFO("AdapterManager: all adapters stopped");
    }

    /// Unregister (remove) an adapter
    void unregisterAdapter(const std::string& id) {
        std::lock_guard lock(mutex_);
        auto it = adapters_.find(id);
        if (it != adapters_.end()) {
            if (it->second->isConnected()) it->second->stop();
            adapters_.erase(it);
            DICE_LOG_INFO("AdapterManager: '{}' unregistered", id);
        }
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
        Message routed = msg;
        normalizeIdentity(routed);
        cacheOfficialGroupNickname(routed);
        ensureInboundGroup(routed);
        for (auto& handler : messageHandlers_) {
            handler(routed);
        }
    }

    /// Register a message handler (called for every incoming message)
    void onMessage(IAdapter::MessageCallback handler) {
        messageHandlers_.push_back(std::move(handler));
    }

    /// Route an incoming non-message event to all event handlers.
    void routeEvent(const BotEvent& ev) {
        for (auto& handler : eventHandlers_) handler(ev);
    }

    /// Register an event handler (called for every notice/request event).
    void onEvent(IAdapter::EventCallback handler) {
        eventHandlers_.push_back(std::move(handler));
    }

    /// Broadcast a reply to all connected adapters.
    /// Usually you'd want to reply through the same adapter that received the message.
    void broadcastToAll(const std::string& text) {
        for (auto& [id, a] : adapters_) {
            if (a->isConnected()) {
                // broadcast is adapter-specific
            }
        }
    }

private:
    static std::string profileTimestamp() {
        std::time_t now = std::time(nullptr); std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &now);
#else
        local = *std::localtime(&now);
#endif
        char out[32]{}; std::strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%SZ", &local);
        return out;
    }
    void cacheOfficialGroupNickname(const Message& msg) {
        if (msg.platform != "qq_official" || msg.type != MessageType::kGroup || msg.senderId.empty()
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
                auto row = profiles.front(); row.nickname = msg.senderName; st->update(row);
            }
        } catch (...) {}
    }
    void ensureInboundGroup(const Message& msg) {
        if (msg.type != MessageType::kGroup || msg.targetId.empty()) return;
        auto* st = db_.getStorage(); if (!st) return;
        try {
            auto rows = st->get_all<GroupSettingRow>(orm::where(
                orm::c(&GroupSettingRow::platform) == msg.platform and
                orm::c(&GroupSettingRow::groupId) == msg.targetId and
                orm::c(&GroupSettingRow::key) == std::string("enabled")), orm::limit(1));
            if (rows.empty()) { GroupSettingRow row; row.platform = msg.platform; row.groupId = msg.targetId; row.key = "enabled"; row.value = "1"; st->insert(row); }
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
            // QQ Official does not provide a private-chat nickname. If this
            // endpoint resolves to a known public identity (including one
            // joined by .bind), reuse its group-cached name instead of exposing
            // an OpenID or reserved virtual QQ number.
            if (msg.senderName.empty()) {
                std::string cachedName;
                if (auto* st = db_.getStorage()) {
                    try {
                        auto profiles = st->get_all<PlayerProfileRow>(orm::where(
                            orm::c(&PlayerProfileRow::platform) == std::string("qq_official") and
                            orm::c(&PlayerProfileRow::userId) == publicSender), orm::limit(1));
                        if (!profiles.empty()) cachedName = profiles.front().nickname;
                    } catch (...) {}
                }
                if (!cachedName.empty()) msg.senderName = cachedName;
                else {
                    msg.senderName = "用户";
                    msg.extra["__sender_name_fallback"] = true;
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
        if (msg.platform == "onebot_v11" && (msg.type == MessageType::kGroup || msg.type == MessageType::kPrivate)) {
            const std::string rawTarget = msg.targetId;
            const std::string rawSender = msg.senderId;
            const Kind scope = msg.type == MessageType::kPrivate ? Kind::User : Kind::Group;
            const std::string publicTarget = bindings.observeQQ(db_, "onebot_v11", msg.adapterId, rawTarget, scope);
            const std::string publicSender = bindings.observeQQ(db_, "onebot_v11", msg.adapterId, rawSender, Kind::User);
            msg.extra["__identity_transport"] = "onebot_v11";
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
};

} // namespace dice
