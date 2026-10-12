#pragma once
#include "logger.h"
#include <nlohmann/json.hpp>
#include <utility>

namespace dice {
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
        return {{"sources", items}, {"trigger", trigger}, {"result", result}};
    }
};
inline thread_local ProcessingContext activeProcessingContext;
class ProcessingScope {
public:
    explicit ProcessingScope(ProcessingContext context)
        : previous_(std::exchange(activeProcessingContext, std::move(context))) {}
    ProcessingScope(ProcessingSource source, std::string trigger = {})
        : ProcessingScope(ProcessingContext{{std::move(source)},
            trigger.empty() ? activeProcessingContext.trigger
                : activeProcessingContext.trigger.empty() || activeProcessingContext.trigger.find(trigger) != std::string::npos
                    ? (activeProcessingContext.trigger.empty() ? trigger : activeProcessingContext.trigger)
                    : activeProcessingContext.trigger + " → " + trigger}) {}
    ~ProcessingScope() { activeProcessingContext = std::move(previous_); }
    ProcessingScope(const ProcessingScope&) = delete;
private:
    ProcessingContext previous_;
};
inline void addProcessingSource(std::vector<ProcessingSource>& sources, const ProcessingSource& item) {
    for (const auto& old : sources)
        if (old.kind == item.kind && old.name == item.name && old.file == item.file && old.entry == item.entry) return;
    sources.push_back(item);
}

// A prepared logical output survives asynchronous formatting and retries.
// Platform-generated message IDs remain separate from this internal msgID.
struct PreparedOutput {
    std::string id, target, proposed;
    ProcessingContext processing;
    PreparedOutput(std::string destination, std::string text,
                   ProcessingContext context = activeProcessingContext)
        : id(newLogCorrelationId()), target(std::move(destination)), proposed(std::move(text)),
          processing(std::move(context)) {
        writeLog({}, LogLevel::Info, "处理", fmt::format(
            "{}；结果：{}；触发：{}；拟发送至：{}；拟发送内容：{} (msgID:{})",
            processing.label(), processing.result,
            processing.trigger.empty() ? "未提供" : processing.trigger,
            target, proposed, id),
            {{"msg_id", id}, {"target", target}, {"processing", processing.json()}});
    }
};
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
        return {std::move(context), std::move(adapter), std::move(target), std::move(actual), std::move(module)};
    }
    std::string suffix() const {
        return context.parts > 1 ? fmt::format(" (msgID:{}，分段:{}/{})", context.output->id, context.part, context.parts)
                                 : fmt::format(" (msgID:{})", context.output->id);
    }
    nlohmann::json details() const {
        return {{"msg_id", context.output->id}, {"adapter", adapter}, {"target", target},
                {"part", context.part}, {"parts", context.parts}, {"processing", context.output->processing.json()}};
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
        writeLog({}, LogLevel::Error, module, fmt::format("[{}] 向 [{}] 发送失败：{}；实际内容：{}{}",
            adapter, target, reason, content, suffix()), details());
    }
};
inline void logPluginText(LogLevel level, std::string_view text) {
    writeLog({}, level, "插件", (activeProcessingContext.sources.empty() ? std::string("插件（未识别来源）") : activeProcessingContext.label()) + "：" + std::string(text),
             {{"processing", activeProcessingContext.json()}});
}
inline void logProcessingResult(const ProcessingSource& source, std::string_view result, std::string_view trigger = {}) {
    writeLog({}, LogLevel::Info, "处理", source.label() + "；结果：" + std::string(result),
             {{"processing", ProcessingContext{{source}, std::string(trigger), std::string(result)}.json()}});
}
} // namespace dice
