#pragma once
// ─── Dice!Next — 崩溃诊断兜底（Windows）────────────────────────
// 背景：用户在 Server 2012/2016 上报「已停止工作」(BEX64 @ ucrtbase)，本地无法复现。
// 此模块把「死因」落成 data/logs/crash_*.txt：异常码/地址/所属模块+偏移/启动阶段/
// 未捕获 C++ 异常的 what()/CRT invalid parameter——下一个版本用户一跑即可精确定位。
//
// 覆盖四类死法：
//   1) SEH（访问违例/DEP 等）        → SetUnhandledExceptionFilter
//   2) 未捕获 C++ 异常 → terminate    → std::set_terminate（能拿到 what()！）
//   3) CRT 无效参数（fail-fast 前）   → _set_invalid_parameter_handler
//   4) abort()                        → signal(SIGABRT)
//
// 崩溃处理上下文中只用 CRT 基础 IO（fopen/fprintf），不碰 iostream/堆分配大对象。

#include <atomic>

#if defined(_WIN32)
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <ctime>
#include <exception>

namespace dice::crashdiag {

// 启动阶段标记：main 各阶段更新；崩溃时写入报告，即使拿不到堆栈也知道死在哪一步。
inline std::atomic<const char*> g_phase{"pre-main"};
inline void setPhase(const char* p) { g_phase.store(p, std::memory_order_relaxed); }

inline const char* g_buildTag = "";   // main 里注入版本/build 号

namespace detail {

inline void writeHeader(FILE* f, const char* kind) {
    std::time_t t = std::time(nullptr);
    std::tm tm{}; localtime_s(&tm, &t);
    char ts[32]; std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    std::fprintf(f, "==== Dice!Next crash report ====\n");
    std::fprintf(f, "time   : %s\n", ts);
    std::fprintf(f, "kind   : %s\n", kind);
    std::fprintf(f, "build  : %s\n", g_buildTag && *g_buildTag ? g_buildTag : "?");
    std::fprintf(f, "phase  : %s\n", g_phase.load(std::memory_order_relaxed));
}

inline FILE* openReport() {
    CreateDirectoryA("data", nullptr);
    CreateDirectoryA("data\\logs", nullptr);
    char name[128];
    std::snprintf(name, sizeof name, "data\\logs\\crash_%lld.txt", (long long)std::time(nullptr));
    FILE* f = nullptr;
    if (fopen_s(&f, name, "w") != 0 || !f) { fopen_s(&f, "crash.txt", "w"); }
    return f;
}

// 把地址翻译成「模块名+偏移」（对照 ucrtbase.dll+0x6e00e 这类问题签名）。
inline void writeAddr(FILE* f, const char* tag, const void* addr) {
    MEMORY_BASIC_INFORMATION mbi{};
    char mod[MAX_PATH] = "?";
    unsigned long long off = 0;
    if (VirtualQuery(addr, &mbi, sizeof mbi) && mbi.AllocationBase) {
        GetModuleFileNameA((HMODULE)mbi.AllocationBase, mod, MAX_PATH);
        off = (unsigned long long)addr - (unsigned long long)mbi.AllocationBase;
    }
    std::fprintf(f, "%s: %p  (%s + 0x%llx)\n", tag, addr, mod, off);
}

inline LONG WINAPI sehFilter(EXCEPTION_POINTERS* ep) {
    if (FILE* f = openReport()) {
        writeHeader(f, "SEH exception");
        if (ep && ep->ExceptionRecord) {
            auto* er = ep->ExceptionRecord;
            std::fprintf(f, "code   : 0x%08lX\n", er->ExceptionCode);
            writeAddr(f, "at     ", er->ExceptionAddress);
            if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
                std::fprintf(f, "access : %s %p\n",
                             er->ExceptionInformation[0] ? "write" : "read",
                             (void*)er->ExceptionInformation[1]);
        }
        std::fclose(f);
    }
    return EXCEPTION_EXECUTE_HANDLER;   // 让进程退出（已留报告）
}

[[noreturn]] inline void onTerminate() {
    if (FILE* f = openReport()) {
        writeHeader(f, "std::terminate (uncaught C++ exception)");
        if (auto ex = std::current_exception()) {
            try { std::rethrow_exception(ex); }
            catch (const std::exception& e) { std::fprintf(f, "what   : %s\n", e.what()); }
            catch (...) { std::fprintf(f, "what   : (non-std exception)\n"); }
        } else {
            std::fprintf(f, "what   : (terminate without active exception)\n");
        }
        std::fclose(f);
    }
    std::abort();
}

inline void onInvalidParameter(const wchar_t* expr, const wchar_t* func,
                               const wchar_t* file, unsigned line, uintptr_t) {
    if (FILE* f = openReport()) {
        writeHeader(f, "CRT invalid parameter");
        // Release 下参数多为 null；有则写出。
        std::fprintf(f, "func   : %ls\nexpr   : %ls\nfile   : %ls:%u\n",
                     func ? func : L"?", expr ? expr : L"?", file ? file : L"?", line);
        std::fclose(f);
    }
    std::abort();
}

inline void onAbort(int) {
    if (FILE* f = openReport()) {
        writeHeader(f, "abort()");
        std::fclose(f);
    }
    // 交还默认处理（fail-fast）
}

}  // namespace detail

/// 手动落一份致命错误报告（main 的整体 catch 用：能拿到 what()）。
inline void reportFatal(const char* kind, const char* what) {
    if (FILE* f = detail::openReport()) {
        detail::writeHeader(f, kind);
        std::fprintf(f, "what   : %s\n", what ? what : "?");
        std::fclose(f);
    }
}

/// main() 最先调用。@p buildTag 形如 "3.0.0(591)"。
inline void install(const char* buildTag) {
    g_buildTag = buildTag;
    SetUnhandledExceptionFilter(detail::sehFilter);
    std::set_terminate(detail::onTerminate);
    _set_invalid_parameter_handler(detail::onInvalidParameter);
    std::signal(SIGABRT, detail::onAbort);
}

}  // namespace dice::crashdiag

#else   // non-Windows: no-op
namespace dice::crashdiag {
inline void setPhase(const char*) {}
inline void install(const char*) {}
inline void reportFatal(const char*, const char*) {}
}
#endif
