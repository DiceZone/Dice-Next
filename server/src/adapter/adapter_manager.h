#pragma once
// ─── Dice!Next v3.0.0 — Adapter Manager ──────────────────────
// Manages multiple adapters: load, start, stop, unload, routing.
// An adapter corresponds to one chat platform connection (e.g.,
// a QQ bot via OneBot v11, a Discord bot via gateway, etc.)

#include "adapter_interface.h"
#include "../storage/database.h"
#include "../common/logger.h"

#include <map>
#include <mutex>
#include <functional>

namespace dice {

class AdapterManager {
public:
    explicit AdapterManager(Database& db) : db_(db) {}

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
        for (auto& handler : messageHandlers_) {
            handler(msg);
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
    Database& db_;
    std::map<std::string, AdapterPtr> adapters_;
    std::vector<IAdapter::MessageCallback> messageHandlers_;
    std::vector<IAdapter::EventCallback> eventHandlers_;
    mutable std::mutex mutex_;
};

} // namespace dice
