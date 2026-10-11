#pragma once
#include "script_execution.h"
#include <quickjs.h>
#include <cstring>
#include <vector>

namespace dice {
enum class RegexMode { Search, Prefix, Full };
struct RegexResult {
    bool matched = false;
    std::vector<std::string> groups;
    std::string error;
};

// std::regex has no cancellation and a short pattern can still backtrack for
// hours. Use the existing, interruptible ECMAScript engine, isolated from all
// user plugin globals. Each HTTP/message thread owns a finite regex cache.
class BoundedRegexEngine {
public:
    BoundedRegexEngine() {
        runtime_ = JS_NewRuntime();
        if (!runtime_) return;
        JS_SetMemoryLimit(runtime_, 16 * 1024 * 1024);
        JS_SetMaxStackSize(runtime_, 512 * 1024);
        context_ = JS_NewContext(runtime_);
        if (!context_) return;
        constexpr const char* source = R"JS((() => {
            const cache = new Map();
            return (pattern, text, mode, validate) => {
                let re = cache.get(pattern);
                if (!re) {
                    re = new RegExp(pattern, 'i');
                    if (cache.size >= 256) cache.clear();
                    cache.set(pattern, re);
                }
                if (validate) return true;
                const match = re.exec(text);
                if (!match || (mode > 0 && match.index !== 0) ||
                    (mode === 2 && match[0].length !== text.length)) return null;
                return match;
            };
        })())JS";
        function_ = JS_Eval(context_, source, std::strlen(source), "<bounded-regex>", JS_EVAL_TYPE_GLOBAL);
        // Bootstrap is trusted constant code; don't leave this thread's engine
        // permanently uninitialized if the caller's plugin budget just expired.
        JS_SetInterruptHandler(runtime_, [](JSRuntime*, void*) { return ScriptExecution::expired() ? 1 : 0; }, nullptr);
    }
    ~BoundedRegexEngine() {
        if (context_) { JS_FreeValue(context_, function_); JS_FreeContext(context_); }
        if (runtime_) JS_FreeRuntime(runtime_);
    }
    RegexResult match(const std::string& pattern, const std::string& text, RegexMode mode, bool validate) {
        RegexResult result;
        if (pattern.size() > 4096 || text.size() > 64 * 1024) {
            result.error = "正则表达式或输入超过长度上限";
            return result;
        }
        if (blocked_.count(pattern)) { result.error = "此正则表达式已因执行超时暂停，请修改表达式"; return result; }
        if (!context_ || !JS_IsFunction(context_, function_)) { result.error = "正则引擎不可用"; return result; }
        ScriptExecution execution(std::chrono::milliseconds(100));
        JS_UpdateStackTop(runtime_);
        JSValue arguments[] = {JS_NewStringLen(context_, pattern.data(), pattern.size()),
            JS_NewStringLen(context_, text.data(), text.size()), JS_NewInt32(context_, static_cast<int>(mode)),
            JS_NewBool(context_, validate)};
        JSValue value = JS_EXCEPTION;
        if (std::none_of(std::begin(arguments), std::end(arguments), [](JSValue value) { return JS_IsException(value); }))
            value = JS_Call(context_, function_, JS_UNDEFINED, 4, arguments);
        for (auto argument : arguments) JS_FreeValue(context_, argument);
        if (JS_IsException(value)) {
            JSValue error = JS_GetException(context_);
            if (ScriptExecution::expired()) {
                result.error = "正则表达式执行超时，已暂停匹配";
                if (blocked_.size() >= 256) blocked_.clear();
                blocked_.insert(pattern);
            } else {
                const char* message = JS_ToCString(context_, error);
                result.error = message ? message : "正则表达式无效或内存不足";
                if (message) JS_FreeCString(context_, message);
            }
            JS_FreeValue(context_, error);
        } else if (validate) result.matched = JS_ToBool(context_, value);
        else if (JS_IsObject(value)) {
            result.matched = true;
            JSValue length = JS_GetPropertyStr(context_, value, "length");
            uint32_t count = 0;
            JS_ToUint32(context_, &count, length);
            JS_FreeValue(context_, length);
            for (uint32_t i = 0; i < count && i < 1024; ++i) {
                JSValue group = JS_GetPropertyUint32(context_, value, i);
                const char* textValue = JS_IsUndefined(group) ? nullptr : JS_ToCString(context_, group);
                result.groups.emplace_back(textValue ? textValue : "");
                if (textValue) JS_FreeCString(context_, textValue);
                JS_FreeValue(context_, group);
            }
        }
        JS_FreeValue(context_, value);
        return result;
    }
private:
    JSRuntime* runtime_ = nullptr;
    JSContext* context_ = nullptr;
    JSValue function_ = JS_UNDEFINED;
    std::set<std::string> blocked_;
};
inline RegexResult boundedRegex(const std::string& pattern, const std::string& text,
                                RegexMode mode = RegexMode::Search, bool validate = false) {
    thread_local BoundedRegexEngine engine;
    return engine.match(pattern, text, mode, validate);
}
} // namespace dice
