#pragma once
#include "logger.h"
#include <algorithm>
#include <initializer_list>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <utility>

namespace dice {
// Captured at ingress and carried with asynchronous plugin/output work. This is
// diagnostic identity only, never an authorization or routing decision.
struct ProcessingOrigin {
    std::string platform, adapterId, accountId, accountName;
    std::string userId, userName, groupId, groupName, messageId, entry;
    std::string targetId, targetName, targetType;
    nlohmann::json json() const {
        return {{"platform", platform}, {"adapter_id", adapterId}, {"account_id", accountId},
                {"account_name", accountName}, {"user_id", userId}, {"user_name", userName},
                {"group_id", groupId}, {"group_name", groupName}, {"message_id", messageId}, {"entry", entry},
                {"target_id", targetId}, {"target_name", targetName}, {"target_type", targetType}};
    }
};
inline thread_local ProcessingOrigin activeProcessingOrigin;
struct ProcessingSource {
    std::string kind, name, file, entry;
    std::string label() const {
        return kind + (name.empty() ? "" : "「" + name + "」")
            + (entry.empty() ? "" : "（" + entry + "）");
    }
    nlohmann::json json() const {
        return {{"kind", kind}, {"name", name}, {"file", file}, {"entry", entry}};
    }
};
struct ProcessingContext {
    std::vector<ProcessingSource> sources;
    std::string trigger;
    // Active plugin sends are requests, not proof the whole invocation succeeded.
    std::string result = "请求发送（处理仍在执行）";
    ProcessingOrigin origin = activeProcessingOrigin;
    std::string label() const {
        std::string text;
        for (const auto& source : sources) {
            if (!text.empty()) text += "、";
            text += source.label();
        }
        return text.empty() ? "系统发送接口（未提供处理来源）" : text;
    }
    nlohmann::json json() const {
        auto items = nlohmann::json::array();
        for (const auto& source : sources) items.push_back(source.json());
        return {{"sources", items}, {"trigger", trigger}, {"result", result}, {"origin", origin.json()}};
    }
};
// Structured records carry this snapshot across transport callbacks. It is
// diagnostic data only; never use it for routing or permission decisions.
inline ProcessingContext processingContextFromJson(const nlohmann::json &value) {
    ProcessingContext result;
    result.origin = {};
    auto read = [](const nlohmann::json &object, const char *key) {
        const auto it = object.is_object() ? object.find(key) : object.end();
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    if (!value.is_object())
        return result;
    result.trigger = read(value, "trigger");
    result.result = read(value, "result");
    const auto sources = value.find("sources");
    if (sources != value.end() && sources->is_array())
        for (const auto &source : *sources) {
            if (result.sources.size() >= 16)
                break;
            if (source.is_object())
                result.sources.push_back({read(source, "kind"), read(source, "name"), read(source, "file"),
                                          read(source, "entry")});
        }
    const auto origin = value.find("origin");
    if (origin != value.end() && origin->is_object()) {
        for (const auto &[key, member] :
             std::initializer_list<std::pair<const char *, std::string ProcessingOrigin::*>>{
                 {"platform", &ProcessingOrigin::platform},
                 {"adapter_id", &ProcessingOrigin::adapterId},
                 {"account_id", &ProcessingOrigin::accountId},
                 {"account_name", &ProcessingOrigin::accountName},
                 {"user_id", &ProcessingOrigin::userId},
                 {"user_name", &ProcessingOrigin::userName},
                 {"group_id", &ProcessingOrigin::groupId},
                 {"group_name", &ProcessingOrigin::groupName},
                 {"message_id", &ProcessingOrigin::messageId},
                 {"entry", &ProcessingOrigin::entry},
                 {"target_id", &ProcessingOrigin::targetId},
                 {"target_name", &ProcessingOrigin::targetName},
                 {"target_type", &ProcessingOrigin::targetType}})
            result.origin.*member = read(*origin, key);
    }
    return result;
}
inline bool isNotificationProcessing(const ProcessingContext &context) {
    for (const auto &source : context.sources)
        if (source.kind == "通知")
            return true;
    return false;
}
inline thread_local ProcessingContext activeProcessingContext;
class ProcessingScope {
public:
    explicit ProcessingScope(ProcessingContext context)
        : previous_(std::exchange(activeProcessingContext, std::move(context))),
          previousOrigin_(std::exchange(activeProcessingOrigin, activeProcessingContext.origin)) {}
    ProcessingScope(ProcessingSource source, std::string trigger = {})
        : ProcessingScope(ProcessingContext{{std::move(source)},
            trigger.empty() ? activeProcessingContext.trigger
                : activeProcessingContext.trigger.empty() || activeProcessingContext.trigger.find(trigger) != std::string::npos
                    ? (activeProcessingContext.trigger.empty() ? trigger : activeProcessingContext.trigger)
                    : activeProcessingContext.trigger + " → " + trigger}) {}
    ~ProcessingScope() {
        activeProcessingContext = std::move(previous_);
        activeProcessingOrigin = std::move(previousOrigin_);
    }
    ProcessingScope(const ProcessingScope&) = delete;
private:
    ProcessingContext previous_;
    ProcessingOrigin previousOrigin_;
};
inline void addProcessingSource(std::vector<ProcessingSource>& sources, const ProcessingSource& item) {
    for (const auto& old : sources)
        if (old.kind == item.kind && old.name == item.name && old.file == item.file && old.entry == item.entry) return;
    sources.push_back(item);
}

// A prepared logical output survives asynchronous formatting and retries.
// Platform-generated message IDs remain separate from this internal msgID.
struct PreparedOutput {
    std::string id, target, proposed, adapterLabel;
    ProcessingContext processing;
    PreparedOutput(std::string destination, std::string text,
                   ProcessingContext context = activeProcessingContext, std::string transport = {})
        : id(newLogCorrelationId()), target(std::move(destination)), proposed(std::move(text)),
          adapterLabel(std::move(transport)), processing(std::move(context)) {
        writeLog({}, LogLevel::Info, "处理", fmt::format(
            "{}；结果：{}；触发：{}；拟发送至：{}；拟发送内容：{} (msgID:{})",
            processing.label(), processing.result,
            processing.trigger.empty() ? "未提供" : processing.trigger,
            target, proposed, id),
            {{"msg_id", id}, {"target", target}, {"processing", processing.json()}});
    }
};

// Repeated notification delivery failures must not fill local logs either.
// No scheduled work or configuration mutations: the next real failure can
// report the accumulated count after the existing one-minute window.
inline std::pair<bool, size_t>
admitNotificationFailure(const std::string &key,
                         std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
    struct Entry {
        std::chrono::steady_clock::time_point at;
        size_t suppressed = 0;
    };
    static std::mutex mutex;
    static std::map<std::string, Entry> entries;
    std::lock_guard lock(mutex);
    auto it = entries.find(key);
    if (it != entries.end() && now >= it->second.at && now - it->second.at < std::chrono::seconds(60)) {
        ++it->second.suppressed;
        return {false, 0};
    }
    const auto suppressed = it == entries.end() ? size_t(0) : it->second.suppressed;
    if (entries.size() >= 256 && it == entries.end()) {
        auto oldest = std::min_element(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
            return a.second.at < b.second.at;
        });
        entries.erase(oldest);
    }
    entries[key] = {now, 0};
    return {true, suppressed};
}
struct OutboundContext { std::shared_ptr<const PreparedOutput> output; size_t part = 0, parts = 0; };
inline thread_local OutboundContext activeOutboundContext;
class OutboundScope {
public:
    explicit OutboundScope(std::shared_ptr<const PreparedOutput> output, size_t part = 0, size_t parts = 0)
        : previous_(std::exchange(activeOutboundContext, OutboundContext{std::move(output), part, parts})) {}
    ~OutboundScope() { activeOutboundContext = std::move(previous_); }
    OutboundScope(const OutboundScope&) = delete;
private:
    OutboundContext previous_;
};

// Capture at the transport boundary, after Markdown/media conversion. Copy this
// object into asynchronous callbacks; never read thread-local origins there.
struct OutboundAttempt {
    OutboundContext context;
    std::string adapter, target, content, module = "消息";
    static OutboundAttempt prepare(std::string adapter, std::string target, std::string actual, std::string module = "消息") {
        auto context = activeOutboundContext;
        if (!context.output) context.output = std::make_shared<PreparedOutput>(target, actual);
        if (isNotificationProcessing(context.output->processing)) {
            target = context.output->target;
            if (!context.output->adapterLabel.empty())
                adapter = context.output->adapterLabel;
        }
        return {std::move(context), std::move(adapter), std::move(target), std::move(actual), std::move(module)};
    }
    std::string suffix() const {
        return context.parts > 1 ? fmt::format(" (msgID:{}，分段:{}/{})", context.output->id, context.part, context.parts)
                                 : fmt::format(" (msgID:{})", context.output->id);
    }
    nlohmann::json details() const {
        return {{"msg_id", context.output->id},
                {"adapter", adapter},
                {"target", target},
                {"part", context.part},
                {"parts", context.parts},
                {"processing", context.output->processing.json()},
                {"notification_delivery", isNotificationProcessing(context.output->processing)}};
    }
    void submitted() const {
        writeLog({}, LogLevel::Info, module, fmt::format("发↪ [{}] [{}] 骰娘：{}；已提交发送，等待平台结果{}",
            adapter, target, content, suffix()), details());
    }
    void accepted(std::string_view platformId = {}) const {
        writeLog({}, LogLevel::Debug, module, fmt::format("[{}] 平台已确认接受本次发送；平台消息 ID：{}{}",
            adapter, platformId.empty() ? "未提供" : platformId, suffix()), details());
    }
    void failed(std::string_view reason) const {
        const bool notice = isNotificationProcessing(context.output->processing);
        size_t suppressed = 0;
        if (notice) {
            const auto admitted = admitNotificationFailure(
                nlohmann::json::array({adapter, target, std::string(reason.substr(0, 1000))})
                    .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
            if (!admitted.first)
                return;
            suppressed = admitted.second;
        }
        auto data = details();
        data["operation"] = notice ? "发送通知" : module == "事件" ? "执行事件" : "发送消息";
        data["error"] = reason;
        data["actual_content"] = content;
        data["suppressed"] = suppressed;
        std::string message =
            notice ? fmt::format("向{}发送通知失败，请检查适配器 {} 是否可触达该通知窗口。", target, adapter)
            : module == "事件" ? fmt::format("{}失败（{}，适配器 {}）：{}", content, target, adapter, reason)
                               : fmt::format("向{}发送消息失败（适配器 {}）：{}", target, adapter, reason);
        if (suppressed)
            message += fmt::format("（此前相同错误另出现 {} 次）", suppressed);
        writeLog({}, LogLevel::Error, notice ? "通知" : module, message, data);
    }
};
inline void logPluginText(LogLevel level, std::string_view text) {
    writeLog({}, level, "插件",
             (activeProcessingContext.sources.empty() ? std::string("插件（未识别来源）")
                                                      : activeProcessingContext.label()) +
                 "：" + std::string(text),
             {{"processing", activeProcessingContext.json()}, {"plugin_text", text}});
}
// The operation is known by the caller. The interpreter's error is evidence,
// not a reason to guess a more specific cause or suggest a speculative fix.
inline void logPluginFailure(std::string_view operation, std::string_view error) {
    const ProcessingSource *plugin = nullptr;
    for (const auto &source : activeProcessingContext.sources)
        if (source.kind.find("插件") != std::string::npos)
            plugin = &source;
    const auto label = plugin && !plugin->name.empty() ? "插件「" + plugin->name + "」" : "插件";
    writeLog({}, LogLevel::Error, plugin ? plugin->kind : "插件",
             label + std::string(operation) + "失败：" + std::string(error),
             {{"operation", operation}, {"error", error}, {"processing", activeProcessingContext.json()}});
}
inline void logProcessingResult(const ProcessingSource& source, std::string_view result, std::string_view trigger = {}) {
    writeLog({}, LogLevel::Info, "处理", source.label() + "；结果：" + std::string(result),
             {{"processing", ProcessingContext{{source}, std::string(trigger), std::string(result)}.json()}});
}
} // namespace dice
