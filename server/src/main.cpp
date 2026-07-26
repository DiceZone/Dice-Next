#include "common/logger.h"
#include "common/types.h"
#include "common/errors.h"
#include "common/utils.h"
#include "common/hot_reload.h"
#include "config/config_manager.h"
#include "storage/database.h"
#include "storage/migration.h"
#include "storage/legacy_importer.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include "service/api_service.h"
#include "service/web_auth.h"          // C#34 WebUI 登录鉴权
#include "service/chat_image.h"        // C#65 模拟聊天图片本地化
#include "service/ai_polish.h"         // C#68 AI 回复润色
#include "service/ai_translate.h"      // C#68 AI 回复翻译（.lang 自定义语言）
#include "service/ai_chat.h"           // 智能化阶段A：AI 对话回复
#include "service/ai_memory.h"         // 智能化阶段B/C：群聊滚动摘要 + 长期事实记忆
#include "service/ai_tools.h"          // 智能化阶段D：AI 工具调用（掷骰/抽牌/查卡）
#include "service/ai_npc.h"            // 智能化阶段E：NPC 扮演
#include "service/ai_vision.h"         // C#85：多模态图像识别
#include "service/ai_worker.h"         // C#89：AI 后台线程（AI 调用不再阻塞消息管线）
#include "platform/instance_guard.h"   // 必须在 tray_win.h(<windows.h>) 之前：先引 winsock2.h
#include "platform/autostart_win.h"    // Windows 注册表开机自启；其他平台提供 no-op 接口。
#include "platform/tray_win.h"
#include "platform/crash_diag_win.h"
#include "adapter/adapter_interface.h"
#include "adapter/adapter_manager.h"
#include "adapter/onebot_v11_adapter.h"
#include "adapter/qq_official_adapter.h"
#include "core/command_router.h"
#include "core/causal/causal_rule_manager.h"
#include "core/causal/cooldown_manager.h"
#include "core/causal/counter_store.h"
#include "core/mod/js_plugin_manager.h"
#include "core/mod/lua_plugin_manager.h"
#include "i18n/i18n.h"
#include "i18n/locale_resolver.h"

#include <optional>

#include <iostream>
#include <string>
#include <fstream>
#include <csignal>
#include <atomic>
#include <clocale>
#include <thread>
#include <chrono>
#include <vector>
#include <filesystem>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

/// 按 UTF-8 构造文件系统路径：Windows 的 narrow ifstream/ofstream 把路径当系统码页
/// (中文系统=GBK)，用它打开 UTF-8 的中文文件名会失败；用 u8string 构造的 path 才正确
/// (Windows 内部转 wide)。用于一切「按用户给的名字开文件」的场景（C#35 文件名编码）。
inline std::filesystem::path u8path(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

// 路径 → UTF-8 窄串：Windows 上 path::string() 走 ANSI 代码页，文件名含 GBK 无映射
// 字符（emoji 等）会抛 system_error（Server 2012/2016 启动崩溃根因）。u8string 永不抛。
static inline std::string dnx_u8str(const std::filesystem::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

/// 重启本程序（C#34 改 IP/端口后用）：派生一个分离的 cmd——等本进程退出(2秒，让单
/// 实例锁与端口释放)→ 重新拉起同一个 exe(继承当前工作目录)，随后本进程优雅退出。
inline void relaunchSelf() {
#if defined(_WIN32) || defined(_WIN64)
    wchar_t exePath[MAX_PATH] = {0};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        std::wstring cmd = L"cmd.exe /c timeout /t 2 /nobreak >nul & start \"\" \"";
        cmd += exePath; cmd += L"\"";
        STARTUPINFOW si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
        if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        }
    }
#endif
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        drogon::app().quit();   // 解阻塞 app.run() → 进程退出 → 重启脚本随后拉起新实例
    }).detach();
}

/// Set console to UTF-8 mode (cross-platform)
void setupConsole() {
#if defined(_WIN32) || defined(_WIN64)
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    // Enable ANSI escape sequences on Windows 10+
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode)) {
            mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, mode);
        }
    }
#endif
    // Set C locale to user's preferred locale (UTF-8 on modern systems)
    std::setlocale(LC_ALL, ".utf8");
    // Also try common fallback names
    if (std::setlocale(LC_ALL, nullptr) == nullptr) {
        std::setlocale(LC_ALL, "en_US.UTF-8");
    }
}

std::atomic<bool> g_running{true};

void signalHandler(int signal) {
    DICE_LOG_INFO("Received signal {}, shutting down...", signal);
    g_running.store(false);
}

// A message that @s another account is normally treated as "for another bot" and
// ignored — but if it's one of OUR JS plugin commands, the @ is a 代骰/arg target,
// so let it through (the plugin resolves the @ via getCtxProxy).
static bool jsCommandMatches(dice::JsPluginManager& jsMod, dice::CommandRouter& cmdRouter,
                             const dice::Message& msg) {
    if (!jsMod.ready()) return false;
    auto body = cmdRouter.commandBody(msg.content);
    if (!body || body->empty()) return false;
    std::string w = *body;
    if (auto sp = w.find_first_of(" \t"); sp != std::string::npos) w = w.substr(0, sp);
    return jsMod.hasCommand(w);
}

void printBanner() {
    const char* banner = R"(
  ____  _            _ 
 |  _ \(_) ___ ___  | |
 | | | | |/ __/ _ \ | |
 | |_| | | (_|  __/ |_|
 |____/|_|\___\___| (_)
                      
   Dice!Next v3.0.0
   REST API + WebSocket + React Management Panel
)";
    std::cout << banner << std::endl;
}

void printStartupInfo(const dice::ConfigManager& configMgr,
                      const dice::Database& db,
                      const dice::HotReloadMonitor* hotReload) {
    auto config = configMgr.getAll();

    std::string host   = configMgr.get<std::string>("server/host", "0.0.0.0");
    int         port   = configMgr.get<int>("server/port", 18088);
    std::string apiKey = configMgr.get<std::string>("server/api_key", "");
    std::string dbPath = configMgr.get<std::string>("server/db_path", "./data/dice.db");

    std::cout << "\n";
    std::cout << "  ┌─────────────────────────────────────────────┐\n";
    std::cout << "  │  Dice!Next — System Ready              │\n";
    std::cout << "  ├─────────────────────────────────────────────┤\n";
    std::cout << "  │  HTTP Server : " << host << ":" << port;
    for (int i = 0; i < 22 - static_cast<int>(std::to_string(port).size()); ++i)
        std::cout << " ";
    std::cout << "│\n";

    std::cout << "  │  API Key     : " << apiKey;
    for (int i = 0; i < 22 - static_cast<int>(apiKey.size()); ++i)
        std::cout << " ";
    std::cout << "│\n";

    std::cout << "  │  Database    : " << dbPath;
    for (int i = 0; i < 22 - static_cast<int>(dbPath.size()); ++i)
        std::cout << " ";
    std::cout << "│\n";

    std::cout << "  │  Hot Reload  : ";
    if (hotReload && hotReload->isRunning()) {
        std::cout << "ENABLED (watching config/, data/)";
        for (int i = 0; i < 4; ++i) std::cout << " ";
    } else {
        std::cout << "DISABLED";
        for (int i = 0; i < 16; ++i) std::cout << " ";
    }
    std::cout << "│\n";

    std::cout << "  └─────────────────────────────────────────────┘\n";
    std::cout << "\n  Press Ctrl+C to stop.\n\n";
}

}  // anonymous namespace

static int realMain(int argc, char* argv[]) {
    // ── 崩溃诊断兜底：Server 2012/2016 上「已停止工作」(BEX64 @ ucrtbase) 无法本地
    // 复现 → 任何一种死法（SEH/未捕获C++异常/CRT invalid parameter/abort）都会把
    // 死因写到 data/logs/crash_*.txt（异常码+模块偏移+启动阶段+what()）。
    static std::string s_buildTag = dice::versionString() + "(" + std::to_string(dice::buildNumber()) + ")";
    dice::crashdiag::install(s_buildTag.c_str());
    // 诊断自测（隐藏参数）：--crash-test=throw|abort|av 触发对应死法，验证 crash_*.txt 生成。
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--crash-test=throw") { dice::crashdiag::setPhase("crash-test"); throw std::runtime_error("crash-test: uncaught"); }
        if (a == "--crash-test=abort") { dice::crashdiag::setPhase("crash-test"); std::abort(); }
        if (a == "--crash-test=av")    { dice::crashdiag::setPhase("crash-test"); volatile int* p = nullptr; *p = 1; }
    }
    dice::crashdiag::setPhase("main:chdir");
    // ── 0a. chdir 到 exe 所在目录 ────────────────────────────
    // 让相对路径(data/ config/ web/dist 等)始终基于程序目录，无论从哪启动——
    // 这样「开机自启(Run 键，cwd=system32)」「双击/从别处运行」都能找到数据。
#ifdef _WIN32
    {
        wchar_t b[MAX_PATH]; DWORD n = GetModuleFileNameW(nullptr, b, MAX_PATH);
        if (n > 0) { std::wstring p(b, n); auto s = p.find_last_of(L"\\/");
            if (s != std::wstring::npos) {
                std::wstring dir = p.substr(0, s);
                // 仅当 exe 目录含 web/（发行包才有 web/dist）才切——避免开发时 exe 在
                // build/Release（可能因旧运行残留 config/）被误切走。
                if (GetFileAttributesW((dir + L"\\web").c_str()) != INVALID_FILE_ATTRIBUTES)
                    SetCurrentDirectoryW(dir.c_str());
            }
        }
    }
#endif

    // ── 0. Setup console for UTF-8 output ───────────────────
    setupConsole();

    // ── 1. Initialize logger ─────────────────────────────────
    dice::crashdiag::setPhase("logger");
    dice::initLogger("info");
    DICE_LOG_INFO("Dice!Next starting...");

    // ── 2. Register signal handlers ──────────────────────────
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // ── 3. Load configuration ────────────────────────────────
    dice::crashdiag::setPhase("config");
    std::string configPath = "config/default_config.json";
    if (argc > 1) {
        configPath = argv[1];
    }

    dice::ConfigManager configMgr(configPath);
    if (!configMgr.load()) {
        DICE_LOG_ERROR("Failed to load configuration from '{}'", configPath);
        return 1;
    }
    DICE_LOG_INFO("Configuration loaded from '{}'", configPath);

    // ── 3.4. 单实例守卫（同一套数据只允许一个进程）────────────
    {
        namespace fs = std::filesystem;
        std::string dbp = configMgr.get<std::string>("server/db_path", "./data/dice.db");
        fs::path dataDir = fs::path(dbp).parent_path();
        if (dataDir.empty()) dataDir = ".";
        std::error_code ec; fs::create_directories(dataDir, ec);
        std::string lockPath = (dataDir / ".instance.lock").string();
        if (!dice::acquireInstanceLock(lockPath)) {
            DICE_LOG_ERROR("同一数据目录 '{}' 已有一个 Dice!Next 实例在运行，本次启动取消。", dataDir.string());
            std::cerr << "\n[Dice!Next] 检测到同一套数据已有实例在运行（数据目录: " << dataDir.string()
                      << "）。\n  同一套数据不能同时运行多个进程。\n  若确认没有其他实例，请删除 "
                      << lockPath << " 后重试。\n" << std::endl;
            return 1;
        }
    }

    // ── 3.45. 解析可用的 WebUI 端口（被占用则自动 +1 并写回配置）──
    {
        int wantPort = configMgr.get<int>("server/port", 18088);
        uint16_t got = dice::findAvailablePort(static_cast<uint16_t>(wantPort));
        if (got != wantPort) {
            DICE_LOG_WARN("WebUI 端口 {} 已被占用，自动改用 {} 并写入配置文件。", wantPort, got);
            configMgr.set<int>("server/port", static_cast<int>(got));
            configMgr.save();
        }
    }

    // ── 3.5. Initialize i18n translation engine ──────────────
    std::string i18nDir = configMgr.get<std::string>("i18n/resource_dir", "i18n");
    std::string defaultLocaleCode = configMgr.get<std::string>("i18n/default_locale", "zh-Hans");
    dice::I18n i18n(i18nDir, dice::localeFromString(defaultLocaleCode));
    if (!i18n.load()) {
        DICE_LOG_WARN("i18n: no translation bundles loaded — replies may show raw keys");
    } else {
        DICE_LOG_INFO("i18n: ready (default locale '{}', {} bundle(s) loaded)",
            defaultLocaleCode, i18n.availableLocales().size());
    }
    // C#28-B: Persona system initialized later (after DB open + migration)

    // ── 4. Open database ─────────────────────────────────────
    dice::crashdiag::setPhase("database");
    std::string dbPath = configMgr.get<std::string>("server/db_path", "./data/dice.db");
    dice::Database db;
    if (!db.open(dbPath)) {
        DICE_LOG_ERROR("Failed to open database at '{}'", dbPath);
        return 1;
    }

    // ── 5. Run migration ────────────────────────────────────
    dice::crashdiag::setPhase("migration");
    dice::Migration migration(db);
    if (!migration.migrate()) {
        DICE_LOG_ERROR("Database migration failed");
        return 1;
    }

    // Load editable-template overrides from DB into the i18n engine.
    if (auto* st0 = db.getStorage()) {
        try {
            int n = 0;
            for (auto& r : st0->get_all<dice::I18nOverrideRow>()) {
                i18n.setOverride(dice::localeFromString(r.locale), r.key, r.value); ++n;
            }
            if (n) DICE_LOG_INFO("i18n: loaded {} template override(s)", n);
        } catch (...) {}
    }

    // ── 6. Check for legacy data ─────────────────────────────
    // Legacy data path: check config for explicit path, fall back to ./data/
    std::string legacyPath = configMgr.get<std::string>("migration/legacy_path",
        "./data/legacy");
    if (migration.needsLegacyImport(legacyPath)) {
        DICE_LOG_INFO("Legacy data detected at '{}'", legacyPath);
        dice::LegacyImporter legacyImporter(db, legacyPath);
        auto report = legacyImporter.import(legacyPath);
        DICE_LOG_INFO("Legacy import: {} imported, {} errors, {} warnings, {} skipped",
            report.totalImported, report.errors,
            report.warnings.size(), report.skippedItems.size());

        // Log warnings if any
        for (const auto& warn : report.warnings) {
            DICE_LOG_WARN("Legacy import warning: {}", warn);
        }
    } else {
        DICE_LOG_INFO("No legacy data detected at '{}', skipping import", legacyPath);
    }

    // ── 7. Start hot reload monitor ──────────────────────────
    dice::HotReloadMonitor* hotReload = nullptr;
    bool hotReloadEnabled = configMgr.get<bool>("hot_reload/enabled", true);
    int  hotReloadDebounce = configMgr.get<int>("hot_reload/debounce_ms", 500);

    if (hotReloadEnabled) {
        hotReload = new dice::HotReloadMonitor();
        // Only watch config directory, NOT data/ (DB writes trigger unnecessary reloads)
        hotReload->start("config", hotReloadDebounce, [&configMgr](const std::string& changedFile) {
            configMgr.reload();  // writing_ guard inside will skip self-saves
        });

        DICE_LOG_INFO("Hot reload monitor started (debounce: {}ms, watching . recursively)", hotReloadDebounce);
    }

    // ── 8. Print banner and startup information ──────────────
    printBanner();
    printStartupInfo(configMgr, db, hotReload);

    // ── 8.5. Initialize adapter layer and command router ──────
    dice::crashdiag::setPhase("adapters+router");
    dice::AdapterManager adapterMgr(db);
    dice::DiceEngine engine(configMgr);
    dice::LocaleResolver localeResolver(db, configMgr);
    dice::CharacterCardStore cardStore(db);
    // 规则包（rules/*.json）的属性别名并入全局同义词表（P1：全局合并，行为同现状）。
    // 必须在开始服务前完成（canonical() 服务期无锁并发读）。
    {
        int packN = dice::CommandRouter::loadRulePacks("rules");
        packN += dice::CommandRouter::loadRulePacks("data/rules");
        int bundleN = dice::CommandRouter::loadRulePackBundles();   // C#27：data/rulepacks/<包>/
        int aliasN = dice::CommandRouter::loadModelTemplates();     // 规则 mod 的 model/*.xml 属性别名 → .st/.ra
        DICE_LOG_INFO("规则包：加载 {} 个规则 + {} 个规则包(bundle) + {} 个属性模板别名", packN, bundleN, aliasN);
    }
    dice::CommandRouter::loadHelpFiles();   // C#10：加载 data/help/*.md 帮助文档
    dice::CommandRouter::loadHelpDocs();    // C#26：加载 data/helpdoc/**/*.json 结构化帮助文档（海豹兼容+随包速查）
    dice::CardDeck cardDeck;
    cardDeck.setDiceEval([&engine](const std::string& e) {
        auto r = engine.roll(e);
        return r.ok() ? static_cast<long long>(r.modifiedTotal) : 0LL;
    });
    // Decks load from two places: bundled defaults in ./decks (shipped, replaced
    // on upgrade) and user decks in ./data/decks (uploads/edits — same-named user
    // decks override bundled ones). All persistent user data lives under data/.
    int deckCount = cardDeck.loadDir("decks");
    deckCount += cardDeck.loadDir("data/decks");
    DICE_LOG_INFO("CardDeck: {} total decks ({} loaded from decks/ + data/decks/)",
        cardDeck.deckCount(), deckCount);
    dice::CommandRouter cmdRouter(db, configMgr, engine, i18n, localeResolver,
        cardStore, cardDeck, adapterMgr);
    dice::ReplyManager replyManager(db, configMgr);
    replyManager.loadRules();

    // C#29: Causal rule engine — managers instantiated here, wired into the
    // message pipeline below (checked before regular ReplyManager).
    dice::CooldownManager cooldownMgr;
    dice::CounterStore counterStore(db);
    dice::CausalRuleManager causalMgr(db, configMgr, cooldownMgr, counterStore);
    causalMgr.loadRules();

    // C#28-B: Persona switching system
    dice::PersonaManager personaMgr(db, i18n, configMgr);
    personaMgr.loadStartupPersona();
    cmdRouter.setPersonaManager(&personaMgr);

    // 初始化 JS 插件子系统并从 plugins/js 加载插件。
    dice::JsPluginManager jsMod;
    jsMod.init();
    jsMod.setDeckDraw([&cardDeck](const std::string& name, bool /*shuffle*/) -> std::string {
        return cardDeck.has(name) ? cardDeck.drawFromDeck(name).value_or("") : std::string();
    });
    jsMod.setDiceEval([&engine](const std::string& expr) -> std::string {
        auto r = engine.roll(expr);
        return r.ok() ? std::to_string(r.modifiedTotal) : expr;   // 求值失败则原样返回
    });
    // 全局 fetch → 走命令路由的受控 HTTP（外置API开关 + 白名单 + SSRF 防护）。
    jsMod.setHttpFetch([&cmdRouter](const std::string& method, const std::string& url,
                                    const std::string& headers, const std::string& body, int& status) {
        return cmdRouter.jsHttpFetch(method, url, headers, body, status);
    });
    // 插件更新检测/下载（面板管理操作，免外置API开关，仅 SSRF 防护）。
    jsMod.setUpdateFetch([&cmdRouter](const std::string& url, int& status) {
        return cmdRouter.fetchPluginUrl(url, status);
    });
    // 真计时器：把 JS setTimeout/setInterval 排到 drogon 事件循环。
    jsMod.setScheduler([](double sec, std::function<void()> cb) {
        drogon::app().getLoop()->runAfter(sec, std::move(cb));
    });
    // 插件分群启停（C#27 地基）：JS 指令派发前问「该群是否启用此插件（按源文件）」。
    jsMod.setGroupGate([&cmdRouter](const std::string& platform, const std::string& group, const std::string& pluginId) {
        return cmdRouter.isPluginEnabledInGroup(platform, group, pluginId);
    });
    // 溯洄引用：牌堆里的 {词条} 若不是牌堆名，就查帮助词条并展开（#帮助/牌堆溯洄函数）。
    cardDeck.setHelpLookup([&cmdRouter](const std::string& name) { return cmdRouter.helpEntryContent(name); });
    // seal.vars ↔ 人物卡桥接：让海豹 gameSystem 插件用无$前缀属性名读写 .st 录入的卡。
    jsMod.setCardBridge(
        [&cmdRouter](const std::string& p, const std::string& u, const std::string& g, const std::string& a, long long& out) {
            return cmdRouter.jsCardGet(p, u, g, a, out);
        },
        [&cmdRouter](const std::string& p, const std::string& u, const std::string& g, const std::string& a, long long v) {
            cmdRouter.jsCardSet(p, u, g, a, v);
        });
    jsMod.setCardStrBridge(
        [&cmdRouter](const std::string& p, const std::string& u, const std::string& g, const std::string& a, std::string& out) {
            return cmdRouter.jsCardGetStr(p, u, g, a, out);
        });
    // D#01：群名片解析器 —— JS 规则包读 msg.sender.card / ctx.player.name（显示名）。
    jsMod.setCardNameResolver([&adapterMgr](const std::string& platform, const std::string& groupId,
                                            const std::string& userId) -> std::string {
        if (groupId.empty() || userId.empty()) return "";
        for (auto& a : adapterMgr.allAdapters()) {
            if (a->platform() != platform || !a->isConnected()) continue;
            try {
                nlohmann::json ms = a->getMembers(groupId);
                for (auto& mm : ms) {
                    if (!mm.is_object() || !mm.contains("user_id")) continue;
                    std::string mid = mm["user_id"].is_string() ? mm["user_id"].get<std::string>()
                        : mm["user_id"].is_number() ? std::to_string(mm["user_id"].get<long long>()) : "";
                    if (mid == userId) return mm.value("card", std::string());
                }
            } catch (...) {}
            break;
        }
        return "";
    });
    // 日志暂停后 logOn=false，logCurName 保留到结束或清理。
    jsMod.setLogStateResolver([&cmdRouter](const std::string& platform, const std::string& groupId) {
        std::string id = cmdRouter.getGroupSettingFor(platform, groupId, "activeLog");
        std::string name = cmdRouter.getGroupSettingFor(platform, groupId, "activeLogName");
        if (!id.empty() && name.empty()) name = "log" + id;
        return std::make_pair(!id.empty(), name);
    });
    // 计时器/异步回调里的 replyToSender → 通过对应平台适配器直接发出。
    jsMod.setSender([&adapterMgr](const std::string& platform, bool isPrivate,
                                  const std::string& groupId, const std::string& userId, const std::string& text) {
        auto send = [&](const auto& a) {
            if (isPrivate) a->sendPrivateMessage(userId, text);
            else           a->sendGroupMessage(groupId, text);
        };
        for (auto& a : adapterMgr.allAdapters())
            if (a->isConnected() && a->platform() == platform) { send(a); return; }
        for (auto& a : adapterMgr.allAdapters())
            if (a->isConnected()) { send(a); return; }   // fallback: first connected
    });
    // 群管：禁言/踢/改名片（按 platform 找适配器）。
    jsMod.setGroupAdmin([&adapterMgr](const std::string& platform, const std::string& op,
                                      const std::string& groupId, const std::string& userId,
                                      int64_t num, const std::string& text) {
        for (auto& a : adapterMgr.allAdapters()) {
            if (!a->isConnected() || a->platform() != platform) continue;
            if (op == "ban")       a->setGroupBan(groupId, userId, (int)num);
            else if (op == "kick") a->setGroupKick(groupId, userId);
            else if (op == "card") a->setGroupCard(groupId, userId, text);
            return;
        }
    });
    // 黑名单：seal.ban.addBan/addTrust/remove → 命令路由的黑白名单系统。
    jsMod.setBanOp([&cmdRouter](const std::string& op, const std::string& id, const std::string& reason) {
        cmdRouter.jsBanOp(op, id, reason);
    });
    // seal.ban.getList/getUser → 读我方黑白名单（rank: 黑名单 -30 / 白名单 +30，仿海豹 BanRankInfo）。
    jsMod.setBanQuery([&cmdRouter](const std::string& op, const std::string& id) -> std::string {
        auto entryJson = [](const dice::BanlistRow& r) {
            return nlohmann::json{{"id", r.targetId}, {"name", ""}, {"score", 0},
                {"rank", r.listType == 0 ? -30 : 30},
                {"reasons", nlohmann::json::array({r.reason})}, {"banTime", 0}};
        };
        if (op == "list") {
            nlohmann::json arr = nlohmann::json::array();
            for (auto& r : cmdRouter.banlistAll()) arr.push_back(entryJson(r));
            return arr.dump();
        }
        for (auto& r : cmdRouter.banlistAll())
            if (r.targetId == id) return entryJson(r).dump();
        return "null";
    });
    // C#27：规则包 data/rulepacks/<包>/js 附加加载（按群激活 gating）。
    { std::vector<std::string> luaDirs, jsDirs; dice::CommandRouter::packPluginDirs(luaDirs, jsDirs); jsMod.setExtraDirs(jsDirs); }
    // 插件目录优先 data/plugins/js（数据迁移 C#2），旧部署回退到根 plugins/js。
    dice::crashdiag::setPhase("js-plugins");
    jsMod.loadDir(std::filesystem::exists("data/plugins/js") ? "data/plugins/js" : "plugins/js");
    // JS 规则插件(seal.gameSystem)的属性模板：把模板原文交给 CommandRouter 解析（别名/衍生 → .st/.ra）。
    dice::CommandRouter::jsGameSystemTemplates() = jsMod.gameSystemTemplates();
    dice::CommandRouter::loadJsGameSystems();

    // 初始化 Lua 插件子系统：引擎、模块发现和核心 API。
    dice::LuaPluginManager luaMod;
    luaMod.init();
    luaMod.setDeckDraw([&cardDeck](const std::string& name) -> std::string {
        return cardDeck.has(name) ? cardDeck.drawFromDeck(name).value_or("") : std::string();
    });
    luaMod.setSelfName(configMgr.get<std::string>("dice/self_name", std::string("\xe9\xaa\xb0\xe5\xa8\x98")));
    luaMod.setBotId(configMgr.get<std::string>("dice/self_qq", std::string()));   // getDiceQQ()
    // Lua 插件可锁定人物卡，.st 的读写会遵守锁定状态。
    luaMod.setCardLock([&cardStore](const std::string& uid, const std::string& scope,
                                    const std::string& key, bool on) {
        return on ? cardStore.lockCard(uid, scope, key) : cardStore.unlockCard(uid, scope, key);
    });
    // 旧版 Lua 的 getPlayerCard* 与 .st/.pc 共用 CharacterCardStore：数字群号取该群绑定卡，
    // 字符串第二参数取同名卡。不要再落入 lua_mod.db 的插件私有 lua_card 表，否则 Lua 改卡
    // 与骰点/前端会形成两套互不可见的数据。
    luaMod.setPlayerCardBridge(
        [&cardStore, &cmdRouter](const std::string& uid, const std::string& selector, bool byName,
                                 const std::string& key, nlohmann::json& out) {
            if (uid.empty() || key.empty()) return false;
            const std::string card = byName ? selector : cardStore.boundCard(uid, selector);
            if (key == "__Name") { out = card; return true; }
            if (auto value = cardStore.getAttrByName(uid, card, key)) { out = *value; return true; }
            auto texts = cardStore.getTextsByName(uid, card);
            if (auto it = texts.find(key); it != texts.end()) { out = it->second; return true; }
            // .st 的关联/表达式属性按群作用域保存；旧 Lua 读取普通属性失败后也会
            // 回退对应的 &属性 字段，因此活动卡场景返回原始表达式。
            if (!byName) {
                std::string expr;
                if (cmdRouter.jsCardGetStr("", uid, selector, key, expr)) { out = expr; return true; }
            }
            return false;
        },
        [&cardStore](const std::string& uid, const std::string& selector, bool byName,
                     const std::string& key, const nlohmann::json& value) {
            if (uid.empty() || key.empty()) return false;
            const std::string card = byName ? selector : cardStore.boundCard(uid, selector);
            if (key == "__Name") {
                if (!value.is_string()) return false;
                const std::string newName = value.get<std::string>();
                if (newName == card) return true;
                if (!cardStore.cardExists(uid, card) && !cardStore.createCard(uid, card)) return false;
                return cardStore.renameCard(uid, card, newName);
            }
            if (value.is_null()) {
                cardStore.eraseAttrByName(uid, card, key);
                cardStore.setTextByName(uid, card, key, "");
                return true;
            }
            if (value.is_number()) {
                cardStore.setAttrByName(uid, card, key, static_cast<int>(value.get<double>()));
                cardStore.setTextByName(uid, card, key, "");
                return true;
            }
            cardStore.eraseAttrByName(uid, card, key);
            cardStore.setTextByName(uid, card, key,
                                    value.is_string() ? value.get<std::string>() : value.dump());
            return true;
        },
        [&cardStore](const std::string& uid, const std::string& selector, bool byName,
                     const std::string& key, bool on) {
            if (uid.empty() || key.empty()) return false;
            const std::string card = byName ? selector : cardStore.boundCard(uid, selector);
            return on ? cardStore.lockCardByName(uid, card, key)
                      : cardStore.unlockCardByName(uid, card, key);
        },
        [&cardStore](const std::string& uid, const std::string& selector, bool byName,
                     const std::string& key) {
            if (uid.empty() || key.empty()) return false;
            return cardStore.cardLockedByName(uid, byName ? selector : cardStore.boundCard(uid, selector), key);
        });
    // 卡片模板中的 "js:" 值由 QuickJS 求值。
    cmdRouter.setJsEval([&jsMod](const std::string& script) { return jsMod.evalString(script); });
    // Lua 插件 http.get/post → 走命令路由的受控 HTTP（外置API开关 + 白名单 + SSRF 防护）。
    luaMod.setHttpFetch([&cmdRouter](const std::string& method, const std::string& url,
                                     const std::string& headers, const std::string& body, int& status) {
        return cmdRouter.jsHttpFetch(method, url, headers, body, status);
    });
    // C#107：.game 团务与 Lua msg.game/GameTable 同源存储（lua_mod.db conf "game:<gid>"）。
    cmdRouter.setGameConf({
        [&luaMod](const std::string& scope, const std::string& key) { return luaMod.confGet(scope, key); },
        [&luaMod](const std::string& scope, const std::string& key, const std::string& val) { luaMod.confSet(scope, key, val); },
        [&luaMod](const std::string& scope) { return luaMod.confAllOf(scope); },
    });
    // pc:rollDice(expr) 连接到掷骰引擎。
    luaMod.setRoller([&engine](const std::string& expr, int defaultFace) {
        dice::LuaPluginManager::RollOut out;
        std::string e = expr.empty() ? ("1d" + std::to_string(defaultFace > 0 ? defaultFace : 100)) : expr;
        out.expr = e;
        auto r = engine.roll(e);
        if (r.ok()) {
            out.ok = true;
            out.sum = r.modifiedTotal;
            out.expansion = r.detail.empty() ? std::to_string(r.modifiedTotal) : r.detail;
        } else {
            out.err = r.error;
        }
        return out;
    });
    // askExtra(json) 透传 {action, params} 到平台扩展查询。
    // OneBot invokeAction（如 get_group_member_list），其余格式返回失败。
    luaMod.setAskExtra([&adapterMgr](const std::string& dataJson) -> std::string {
        try {
            auto j = nlohmann::json::parse(dataJson, nullptr, false);
            if (!j.is_object() || !j.contains("action")) return {};
            std::string action = j.value("action", std::string());
            nlohmann::json params = j.contains("params") && j["params"].is_object()
                ? j["params"] : nlohmann::json::object();
            for (auto& a : adapterMgr.allAdapters()) {
                if (!a || !a->isConnected()) continue;
                auto resp = a->invokeAction(action, params);
                if (resp.is_object() && resp.contains("data") && !resp["data"].is_null())
                    return resp["data"].dump();
                return {};
            }
        } catch (...) {}
        return {};
    });
    // Lua 插件 sendMsg(text,gid,uid) → 按已连适配器发群/私聊。
    luaMod.setSender([&adapterMgr](const std::string& text, const std::string& gid, const std::string& uid) {
        for (auto& a : adapterMgr.allAdapters()) {
            if (!a->isConnected()) continue;
            if (!gid.empty()) a->sendGroupMessage(gid, text);
            else if (!uid.empty()) a->sendPrivateMessage(uid, text);
            return;
        }
    });
    // Lua 插件 eventMsg(text,gid,uid) 将 text 作为消息走完整回复管线。
    // 复用 poke_command 的管线（内置指令→JS插件→Lua因果→自定义回复→applySelf→发送）。
    // 用线程局部深度计数防 eventMsg 递归风暴（eventMsg 的回复又触发 eventMsg）。
    luaMod.setEventMsg([&adapterMgr, &cmdRouter, &replyManager, &jsMod, &luaMod](
            const std::string& text, const std::string& gid, const std::string& uid) {
        static thread_local int depth = 0;
        if (depth >= 4) { DICE_LOG_INFO("eventMsg recursion capped at depth 4: '{}'", text); return; }
        struct Guard { int& d; Guard(int& x):d(x){ ++d; } ~Guard(){ --d; } } guard(depth);
        std::string platform;
        for (auto& a : adapterMgr.allAdapters()) if (a->isConnected()) { platform = a->platform(); break; }
        dice::Message pm;
        pm.platform = platform;
        pm.senderId = uid;
        pm.senderName = cmdRouter.lookupNick(platform, uid);
        pm.content = text; pm.rawContent = text; pm.displayContent = text;
        bool pv = gid.empty();
        pm.type = pv ? dice::MessageType::kPrivate : dice::MessageType::kGroup;
        pm.targetId = pv ? uid : gid;
        std::string reply = cmdRouter.handleMessage(pm);
        if (reply.empty() && !cmdRouter.isGroupDisabled(pm)) {
            if (jsMod.ready()) {
                if (auto body = cmdRouter.commandBody(pm.content); body && !body->empty()) {
                    auto jr = jsMod.handle(pm.platform, pm.senderId, pm.senderName, pm.targetId, "", pv, *body,
                                           cmdRouter.jsPrivilegeLevel(pm), pm.atList);
                    if (jr.matched && !jr.reply.empty()) reply = jr.reply;
                }
            }
            if (reply.empty() && luaMod.ready()) {
                int trust = cmdRouter.jsPrivilegeLevel(pm) >= 70 ? 4 : 0;
                auto lr = luaMod.dispatch(pm.content, pm.senderId, pv ? "" : pm.targetId, pm.senderName, "", pv, trust);
                if (lr.matched && !lr.reply.empty()) reply = lr.reply;
            }
            if (reply.empty()) {
                auto matches = replyManager.matchMessage(pm.content);
                if (!matches.empty()) { const auto& r = matches.front();
                    reply = cmdRouter.renderReply(pm, replyManager.pickResult(r), r.matchContent, r.matchType); }
            }
        }
        if (reply.empty()) return;
        reply = cmdRouter.applySelf(pm, reply);
        for (auto& a : adapterMgr.allAdapters()) {
            if (!a->isConnected()) continue;
            if (!platform.empty() && a->platform() != platform) continue;
            if (pv) a->sendPrivateMessage(uid, reply);
            else    a->sendGroupMessageCQ(gid, reply);
            return;
        }
    });
    // 插件分群启停（C#27 地基）：Lua mod 派发前问「该群是否启用此 mod」。
    luaMod.setGroupGate([&cmdRouter](const std::string& platform, const std::string& group, const std::string& pluginId) {
        return cmdRouter.isPluginEnabledInGroup(platform, group, pluginId);
    });
    { std::vector<std::string> luaDirs, jsDirs; dice::CommandRouter::packPluginDirs(luaDirs, jsDirs); luaMod.setExtraDirs(luaDirs); }   // C#27：规则包 lua 附加加载
    dice::crashdiag::setPhase("lua-mods");
    luaMod.loadDir("data/mod");   // 与 C#7 的 JS 规则插件共用 data/mod（Lua mod=目录，JS=文件）

    // C#10：把 JS 插件 cmd.help + Lua mod descriptor.helpdoc 喂给 .help 帮助系统
    //（解耦：router 不直接依赖各引擎）。
    cmdRouter.setHelpProvider([&jsMod, &luaMod]() {
        std::vector<std::pair<std::string, std::string>> v;
        for (auto& ch : jsMod.commandHelps()) if (!ch.help.empty()) v.push_back({ch.name, ch.help});
        for (auto& h : luaMod.helpEntries()) if (!h.text.empty()) v.push_back({h.topic, h.text});
        return v;
    });

    // C#33：.plugin 指令的插件清单（lua mods + js 插件），与 /api/groups/plugins 同源。
    cmdRouter.setPluginProvider([&jsMod, &luaMod]() {
        std::vector<dice::CommandRouter::PluginEntry> v;
        for (auto& m : luaMod.mods())
            v.push_back({"lua:" + m.name, m.title.empty() ? m.name : m.title, "lua", m.enabled});
        for (auto& p : jsMod.listAll()) {
            std::string file = p.file;
            const std::string sfx = ".disabled";
            if (file.size() > sfx.size() && file.substr(file.size() - sfx.size()) == sfx)
                file = file.substr(0, file.size() - sfx.size());
            v.push_back({"js:" + file, p.name.empty() ? file : p.name, "js", p.enabled});
        }
        return v;
    });

    // 智能化阶段D：AI 工具执行器工厂 —— 给定一条消息，返回一个 ToolExec 回调，能真的
    // 掷骰（骰子引擎）/抽牌（牌堆）/查属性（发送者人物卡）。供 AI 对话 function-calling 用。
    auto makeAiTool = [&engine, &cardDeck, &cmdRouter](dice::Message m) -> dice::aitools::ToolExec {
        return [&engine, &cardDeck, &cmdRouter, m](const std::string& tn, const nlohmann::json& ta) -> std::string {
            if (tn == "roll_dice") {
                std::string expr = ta.value("expression", std::string("d100"));
                auto res = engine.roll(expr);
                if (res.ok()) return res.formattedOutput;
                auto od = onedice::eval(expr, 100);
                return od.ok ? od.detail : (std::string("\xe8\xa1\xa8\xe8\xbe\xbe\xe5\xbc\x8f\xe6\x97\xa0\xe6\x95\x88: ") + res.error);  // 表达式无效:
            }
            if (tn == "draw_deck") {
                std::string dn = ta.value("name", std::string());
                auto d = cardDeck.drawFromDeck(dn);
                return d ? *d : (std::string("\xe6\xb2\xa1\xe6\x9c\x89\xe7\x89\x8c\xe5\xa0\x86\xef\xbc\x9a") + dn);  // 没有牌堆：
            }
            if (tn == "get_attr") {
                std::string an = ta.value("name", std::string());
                dice::Message mm = m;
                auto v = cmdRouter.aiGetAttr(mm, an);
                return v ? (an + "=" + std::to_string(*v))
                         : (std::string("\xe4\xba\xba\xe7\x89\xa9\xe5\x8d\xa1\xe6\x97\xa0\xe6\xad\xa4\xe5\xb1\x9e\xe6\x80\xa7\xef\xbc\x9a") + an);  // 人物卡无此属性：
            }
            // AI 深化：写卡 —— 改发送者本人卡的一个属性（绝对值或 +/- 相对增减）。
            if (tn == "set_attr") {
                std::string an = ta.value("name", std::string());
                std::string vs;
                if (ta.contains("value")) { auto& jv = ta["value"]; vs = jv.is_string() ? jv.get<std::string>() : jv.dump(); }
                if (an.empty() || vs.empty()) return "(\xe7\xbc\xba\xe5\xb0\x91\xe5\xb1\x9e\xe6\x80\xa7\xe5\x90\x8d\xe6\x88\x96\xe5\x80\xbc)";  // (缺少属性名或值)
                dice::Message mm = m;
                bool rel = (vs[0] == '+' || vs[0] == '-');
                int val = 0; try { val = std::stoi(vs); } catch (...) { return std::string("(\xe6\x97\xa0\xe6\x95\x88\xe7\x9a\x84\xe5\x80\xbc: ") + vs + ")"; }  // (无效的值)
                int oldv = cmdRouter.aiGetAttr(mm, an).value_or(0);
                int newv = rel ? (oldv + val) : val;
                if (!cmdRouter.aiSetAttr(mm, an, newv)) return std::string("(\xe6\x97\xa0\xe6\xb3\x95\xe8\xae\xbe\xe7\xbd\xae): ") + an;  // (无法设置)
                if (rel) return an + "\xef\xbc\x9a" + std::to_string(oldv) + " \xe2\x86\x92 " + std::to_string(newv);  // 属性：old → new
                return an + " \xe5\xb7\xb2\xe8\xae\xbe\xe4\xb8\xba " + std::to_string(newv);  // 已设为 N
            }
            // C#83：执行任意指令 / 帮助搜索 —— 以发送者身份走 command_router，权限=该用户。
            if (tn == "run_command" || tn == "search_help") {
                std::string cmd = (tn == "search_help")
                    ? (".helpdoc " + ta.value("query", std::string()))
                    : ta.value("command", std::string());
                if (cmd.empty()) return "(\xe7\xa9\xba\xe6\x8c\x87\xe4\xbb\xa4)";  // (空指令)
                unsigned char f0 = (unsigned char)cmd[0];
                if (f0 != '.' && f0 != '!' && f0 != 0xE3 && f0 != 0xEF) cmd = "." + cmd;  // 补默认前缀（. 。 ! ！）
                dice::Message cm = m;
                cm.content = cmd; cm.rawContent = cmd; cm.displayContent = cmd;
                std::string rep = cmdRouter.handleMessage(cm);
                return rep.empty()
                    ? std::string("(\xe8\xaf\xa5\xe6\x8c\x87\xe4\xbb\xa4\xe6\x97\xa0\xe8\xbe\x93\xe5\x87\xba)")  // (该指令无输出)
                    : rep;
            }
            return std::string("(unknown tool: ") + tn + ")";
        };
    };

    // Wire: incoming messages → command router; if no command matched, fall back
    // to the custom-reply word library; then send via the originating adapter.
    adapterMgr.onMessage([&adapterMgr, &cmdRouter, &replyManager, &jsMod, &luaMod, &causalMgr, &db, &configMgr, &engine, &cardDeck, &makeAiTool](const dice::Message& msg) {
        // Multi-bot群: if another bot is @'d and not us, stay silent.
        if (dice::CommandRouter::isForAnotherBot(msg) && !jsCommandMatches(jsMod, cmdRouter, msg)) return;
        // Black/white-list: ignore blacklisted users/groups (and non-whitelisted in whitelist mode).
        if (cmdRouter.isBlocked(msg)) return;
        // C#58 群自动化：消息命中「自动踢出/禁言」关键字则执行并跳过后续处理。
        {
            std::string act = cmdRouter.applyGroupAutoModeration(msg);
            if (!act.empty()) {
                DICE_LOG_INFO("event: C#58 群 {} 自动{} 用户 {}（命中关键字）", msg.targetId,
                              act == "kick" ? "踢出" : "禁言", msg.senderId);
                return;
            }
        }
        bool disabled = cmdRouter.isGroupDisabled(msg);
        // .bot off is bypassed only by an explicit @ to us; a hard web-admin lock
        // remains absolute. This also lets @-addressed plugin commands reach their
        // normal fallback path when no built-in command matched.
        bool forcedByAt = dice::CommandRouter::isAtSelf(msg) && !cmdRouter.isGroupLocked(msg);
        auto reply = cmdRouter.handleMessage(msg);
        // C#68 阶段3：回复来源分类（builtin/plugin/reply），供 AI 翻译按范围过滤。
        std::string replySrc = "builtin";
        // No command matched → custom replies, unless the group is disabled or has
        // custom replies turned off (.group +禁用回复).
        bool replyOff = msg.type == dice::MessageType::kGroup && !msg.targetId.empty()
                        && cmdRouter.isReplyDisabledFor(msg.platform, msg.targetId);
        // C#69 自控：操作者用骰娘账号手打的消息走**完整管线**（内置/插件/自定义回复）；
        // 骰娘自己的回复回声已在适配器层被自回声去重丢弃，不会到这里，故无需在此限制。
        if (reply.empty() && (!disabled || forcedByAt) && !replyOff) {
            // JS 插件指令（海豹兼容）优先于自定义回复。
            if (jsMod.ready()) {
                if (auto body = cmdRouter.commandBody(msg.content); body && !body->empty()) {
                    auto jr = jsMod.handle(msg.platform, msg.senderId,
                        msg.senderName.empty() ? msg.senderId : msg.senderName,
                        msg.targetId, msg.extra.value("card", std::string()), msg.type == dice::MessageType::kPrivate, *body,
                        cmdRouter.jsPrivilegeLevel(msg), msg.atList);
                    if (jr.matched && !jr.reply.empty()) { reply = jr.reply; replySrc = "plugin"; }
                }
            }
            // Lua 模块的因果回复。
            if (reply.empty() && luaMod.ready()) {
                bool priv = msg.type == dice::MessageType::kPrivate;
                int trust = cmdRouter.jsPrivilegeLevel(msg) >= 70 ? 4 : 0;   // master/信任≥4 → trust4
                auto lr = luaMod.dispatch(msg.content, msg.senderId, priv ? "" : msg.targetId,
                    msg.senderName.empty() ? msg.senderId : msg.senderName, msg.extra.value("card", std::string()), priv, trust, msg.platform);
                if (lr.matched && !lr.reply.empty()) { reply = lr.reply; replySrc = "plugin"; }
            }
            // C#29: 因果规则匹配（优先于普通自定义回复）。
            if (reply.empty()) {
                std::string nick = msg.senderName.empty() ? msg.senderId : msg.senderName;
                std::string groupId = msg.type == dice::MessageType::kPrivate ? "" : msg.targetId;
                auto cr = causalMgr.matchAndExecute(msg.content, msg.senderId, groupId, nick);
                if (cr.matched && !cr.reply.empty()) {
                    // Build counter context for {counter:name} resolution in renderReply
                    std::map<std::string, std::string> counterCtx;
                    for (auto& cc : cr.counterChanges) counterCtx[cc.name] = std::to_string(cc.newValue);
                    cmdRouter.setCounterContext(counterCtx);
                    reply = cmdRouter.renderReply(msg, cr.reply, "", dice::MatchType::kKeyword);
                    cmdRouter.clearCounterContext();
                    replySrc = "reply";
                }
            }
            if (reply.empty()) {
                auto matches = replyManager.matchMessage(msg.content);
                if (!matches.empty()) {
                    const auto& r = matches.front();
                    reply = cmdRouter.renderReply(msg, replyManager.pickResult(r), r.matchContent, r.matchType);
                    if (!reply.empty()) replySrc = "reply";
                }
            }
            // 仍无回复 → JS 插件的非指令消息钩子（自动回复 / 随机抓话等）。
            if (reply.empty() && jsMod.ready()) {
                auto nc = jsMod.handleNonCommand(msg.platform, msg.senderId,
                    msg.senderName.empty() ? msg.senderId : msg.senderName,
                    msg.targetId, msg.extra.value("card", std::string()), msg.type == dice::MessageType::kPrivate, msg.content,
                    cmdRouter.jsPrivilegeLevel(msg));
                if (nc.matched && !nc.reply.empty()) { reply = nc.reply; replySrc = "plugin"; }
            }
        }
        // ── C#89：先在消息线程消费本条消息的一次性路由状态 ─────────────
        // 回复投递可能转入 AI 后台线程，这些状态晚取会被下一条消息拿走/污染。
        std::string aiCat = (replySrc == "plugin") ? "plugin"
                          : (replySrc == "reply")  ? "custom"
                          : cmdRouter.lastReplyCategory();
        std::string quoteId = cmdRouter.takeQuoteOverride();
        std::vector<std::string> fwdNodes = cmdRouter.takeForwardNodes();

        // 本群被链接时，将入站消息带来源前缀转发。
        // 即时转发到目标窗口；骰娘回复（对齐 logEcho）在 finishReply 定稿后再转发。
        // .link 指令本身不转发（防环）。
        bool linkReplyOk = false;
        if (msg.type == dice::MessageType::kGroup && !msg.targetId.empty() && !msg.fromSelf) {
            auto aims = cmdRouter.linkAimsFor(msg.platform, msg.targetId);
            if (!aims.empty()) {
                std::string body = msg.content;
                size_t st = body.find_first_not_of(" \t");
                bool isLinkCmd = false;
                if (st != std::string::npos) {
                    std::string t = body.substr(st);
                    if (!t.empty() && (t[0] == '.' || t[0] == '!' || (unsigned char)t[0] >= 0xE0)) {   // 前缀 . ! 。 ！
                        size_t k = ((unsigned char)t[0] >= 0xE0) ? 3 : 1;   // CJK 前缀 3 字节
                        if (t.size() > k && t.compare(k, 4, "link") == 0) isLinkCmd = true;
                    }
                }
                if (!isLinkCmd) {
                    linkReplyOk = true;
                    std::string who = (msg.senderName.empty() ? msg.senderId : msg.senderName) + "(" + msg.senderId + ")";
                    std::string base = msg.rawContent.empty() ? msg.content : msg.rawContent;
                    for (auto& aim : aims)
                        cmdRouter.linkForward(msg, aim, "[" + who + "] " + base);
                }
            }
        }

        // ── C#89：统一的回复投递（润色→翻译→link转发→日志→发送）────────
        // AI 网关是同步 curl（最长 30s），此前润色/翻译/对话都直接跑在适配器消息线程
        // 上，一次超时全部指令失效 30 秒。需要 AI 后处理的回复把这一整段投给
        // aiwork::Worker 后台执行；不需要 AI 时原地执行，行为与旧版完全一致。
        auto finishReply = [&adapterMgr, &cmdRouter, &configMgr, &db](
            const dice::Message& msg, std::string reply, std::string broadcast,
            const std::string& aiCat, const std::string& quoteId,
            std::vector<std::string> fwdNodes, bool recordLog, bool linkReplyOk) {
            // Resolve self tokens ({self}/{strSelfName}/{strSelfCall}) in the final
            // text — works for both command replies and custom replies.
            if (!reply.empty())     reply = cmdRouter.applySelf(msg, reply);
            if (!broadcast.empty()) broadcast = cmdRouter.applySelf(msg, broadcast);
            // C#68 阶段2：AI 润色 —— 类别在覆盖范围内 + 总开关+润色开关开启。失败/超时/
            // 破坏数字一律回退原文，绝不影响掷骰结果。
            if (!reply.empty() && !msg.fromSelf
                && dice::aipolish::enabled(configMgr) && dice::aipolish::covers(configMgr, aiCat)) {
                reply = dice::aipolish::polish(configMgr, msg.content, reply);
            }
            // C#68 阶段3：AI 翻译 —— 本群/本用户 .lang 切到骰主自定义语言时，回复先按正常
            // 语言生成，发送前大模型翻译成目标语言（带缓存）。按覆盖范围过滤。
            if (!reply.empty() && !msg.fromSelf && dice::aitrans::enabled(configMgr)) {
                std::string tgt = cmdRouter.aiLangFor(msg);
                if (!tgt.empty() && dice::aitrans::covers(configMgr, aiCat))
                    reply = dice::aitrans::translate(configMgr, tgt, reply);
            }
            if (reply.empty() && broadcast.empty()) return;
            // .link：骰娘回复按最终文本转发到链接目标。
            if (linkReplyOk && !reply.empty()
                && msg.type == dice::MessageType::kGroup && !msg.targetId.empty() && !msg.fromSelf) {
                for (auto& aim : cmdRouter.linkAimsFor(msg.platform, msg.targetId))
                    cmdRouter.linkForward(msg, aim, reply);
            }
            // .log 游戏日志：骰娘回复按最终发送文本记录（入站已在消息线程记过）。
            if (recordLog && !reply.empty()) cmdRouter.recordBotReply(msg, reply);
            // 模拟聊天窗 + chat.db 持久化（骰娘侧）。私聊使用稳定的
            // private:<用户号> 作用域，避免不同私聊混在旧的空 groupId 中。
            if ((msg.type == dice::MessageType::kGroup && !msg.targetId.empty())
                || (msg.type == dice::MessageType::kPrivate && !msg.senderId.empty())) {
                std::string chatScope = msg.type == dice::MessageType::kPrivate
                    ? "private:" + msg.senderId : msg.targetId;
                std::string key = msg.platform + ":" + chatScope;
                // C#82：骰娘自己的消息也显示名字（QQ 昵称，缺失回退「骰娘」）。
                std::string botName;
                if (auto ba = adapterMgr.getAdapter(msg.adapterId)) botName = ba->getLoginName();
                if (botName.empty()) botName = "\xe9\xaa\xb0\xe5\xa8\x98";   // 骰娘
                if (!reply.empty()) dice::GroupChatLog::instance().add(key, botName, msg.selfId, reply, true);
                if (!broadcast.empty()) dice::GroupChatLog::instance().add(key, "\xF0\x9F\x93\xA2", msg.selfId, broadcast, true);
                if (!reply.empty()) {
                    if (auto* cst = db.getChatStorage()) {
                        try {
                            dice::ChatMsgRow rout;
                            rout.platform = msg.platform; rout.groupId = chatScope;
                            rout.userId = msg.selfId; rout.sender = botName;
                            rout.content = reply; rout.self = 1;
                            rout.time = static_cast<int64_t>(std::time(nullptr));
                            cst->insert(rout);
                        } catch (...) {}
                    }
                }
            }
            if (!reply.empty()) {
                // Log the bot's OUTGOING reply too, so the dashboard log shows 收 + 发.
                std::string where = (msg.type == dice::MessageType::kGroup && !msg.targetId.empty())
                    ? "\xe7\xbe\xa4 " + msg.targetId : std::string("\xe7\xa7\x81\xe8\x81\x8a");  // 群 / 私聊
                // Flatten CR/LF: a reply may echo user-supplied text ({$1}/.text), so keep
                // it on one physical log line to prevent fake-log-entry injection.
                std::string replyFlat; replyFlat.reserve(reply.size());
                for (char ch : reply) { if (ch == '\n') replyFlat += "\xe2\x8f\x8e"; else if (ch != '\r') replyFlat += ch; }
                DICE_LOG_INFO("\xe5\x8f\x91\xe2\x86\xaa [{}] \xe9\xaa\xb0\xe5\xa8\x98: {}", where, replyFlat);  // 发↪ 骰娘
                // A handler may ask to quote a DIFFERENT message (e.g. .log on quotes
                // the previous .log off). Override the quoted id on a copy of msg.
                dice::Message replyMsg = msg;
                if (!quoteId.empty()) replyMsg.id = quoteId;
                // 分段发送：超长回复切成多段；首段引用回复，其余作为普通消息（避免 N 次引用）。
                // 引用开关：骰主关「引用投掷对象发言」后，首段也不引用（但 .log on 等指定引用
                // 的 quoteOverride 仍生效——那是刻意的定向引用）。
                bool quote = cmdRouter.quoteReplyEnabled() || !quoteId.empty();
                auto segs = cmdRouter.segmentReply(reply);
                bool priv = msg.type == dice::MessageType::kPrivate;
                // #6 合并转发(聊天记录)：仅群消息、开关开启、且**回复字符数超过阈值**(默认1200，
                // 应用于所有回复内容)时强制转发。节点也遵守分段设置：有显式节点(.coc/.dnd 每条
                // 结果)就逐个再按分段切，否则整段按分段切。适配器不支持则回退普通分段发送。
                bool wantForward = !priv && !msg.targetId.empty() && cmdRouter.forwardEnabled()
                    && dice::CommandRouter::textCharCount(reply) > cmdRouter.forwardThreshold();
                if (wantForward) {
                    if (!fwdNodes.empty()) {
                        std::vector<std::string> nodes;
                        for (auto& n : fwdNodes) for (auto& s : cmdRouter.segmentReply(n)) nodes.push_back(s);
                        fwdNodes = std::move(nodes);
                    } else {
                        fwdNodes = segs;   // segs 已是 segmentReply(reply)，天然遵守分段设置
                    }
                }
                // 多段时不混用「引用回复 + 普通消息」：QQ 客户端会把引用消息排到普通消息之后，
                // 造成分段 A/B 倒序显示（.draw 长结果尤其明显）。只有单段才引用；多段全部按
                // 普通消息「顺序」发，保证 A→B 阅读顺序一致。
                bool quoteFirst = quote && segs.size() == 1;
                auto sendSegs = [&](const auto& a) {
                    if (wantForward && !fwdNodes.empty() && a->sendGroupForwardMsg(msg.targetId, fwdNodes)) return;
                    for (size_t k = 0; k < segs.size(); ++k) {
                        if (k == 0 && quoteFirst) a->sendReply(replyMsg, segs[0]);
                        // C#69：私聊回复发到 targetId（普通私聊=对方=senderId；自身消息自控时
                        // =对话对方，避免回复发给骰娘自己）。sendReply 亦用 targetId，一致。
                        else if (priv) a->sendPrivateMessage(msg.targetId, segs[k]);
                        else a->sendGroupMessage(msg.targetId, segs[k]);
                    }
                };
                // 只通过消息来源的那个适配器回复，避免多账号/多适配器时串台。
                if (auto a = adapterMgr.getAdapter(msg.adapterId)) {
                    if (a->isConnected()) sendSegs(a);
                } else {
                    for (auto& a : adapterMgr.allAdapters())   // 回退：来源不明时发首个已连接
                        if (a->isConnected()) { sendSegs(a); break; }
                }
            }
            // Send the broadcast as its own plain message to the triggering group,
            // via the originating adapter (after the command reply).
            if (!broadcast.empty()) {
                DICE_LOG_INFO("[\xe7\xbe\xa4 {}] \xf0\x9f\x93\xa2 \xe5\xb9\xbf\xe6\x92\xad: {}", msg.targetId, broadcast);  // 群 ... 📢 广播
                if (auto a = adapterMgr.getAdapter(msg.adapterId))
                    a->sendGroupMessage(msg.targetId, "\xF0\x9F\x93\xA2 " + broadcast);  // 📢
            }
        };

        // 智能化阶段A：AI 对话回复 —— 前面都没回复时，被 @骰娘 / 命中关键词 / 概率待机
        // 触发 → 用「人设 + chat.db 近期上下文」生成一条对话回复。强限频防刷屏；仅群聊。
        // 阶段E：若命中某 NPC（名字/触发词），优先以该 NPC 身份回复（人设/模型覆盖）。
        // C#89：触发判定留在消息线程（廉价），上下文/记忆检索/图像识别/生成/工具调用整段
        // 投给 AI 后台线程 —— 大模型再慢也不影响其他指令；完成后经 finishReply 发送。
        if (reply.empty() && !disabled && !replyOff && !msg.fromSelf
            && msg.type == dice::MessageType::kGroup && !msg.targetId.empty()
            && (dice::aichat::enabled(configMgr) || dice::ainpc::enabled(configMgr))
            && cmdRouter.aiEnabledForGroup(msg.platform, msg.targetId)      // C#84：本群 AI 开关
            && cmdRouter.aiWhitelistOk(msg.platform, msg.targetId, true)) { // AI 白名单模式
            bool atMe = !msg.selfId.empty()
                && std::find(msg.atList.begin(), msg.atList.end(), msg.selfId) != msg.atList.end();
            std::string trigText = msg.displayContent.empty() ? msg.content : msg.displayContent;
            std::string gkey = msg.platform + ":" + msg.targetId;
            dice::ainpc::Npc npc;
            bool npcHit = dice::ainpc::enabled(configMgr)
                && dice::ainpc::match(configMgr, gkey, dice::aichat::cleanForAi(configMgr, trigText), npc);
            dice::aichat::Trigger tkind = dice::aichat::enabled(configMgr)
                ? dice::aichat::triggerKind(configMgr, trigText, atMe) : dice::aichat::Trigger::None;
            bool defaultHit = !npcHit && tkind != dice::aichat::Trigger::None;
            // C#87：被@/命中关键词（Strong）或 NPC 命中 → 必回，无视冷却；待机搭话（Standby）受冷却。
            bool bypassCd = npcHit || tkind == dice::aichat::Trigger::Strong;
            if ((npcHit || defaultHit) && (bypassCd || dice::aichat::cooldownOk(configMgr, gkey))) {
                std::string senderNick = msg.senderName.empty() ? msg.senderId : msg.senderName;
                dice::aitools::ToolExec toolExec = dice::aitools::enabled(configMgr) ? makeAiTool(msg) : nullptr;
                bool rec = !disabled, lnk = linkReplyOk;
                dice::Message msgC = msg;
                auto job = [&configMgr, &db, finishReply, msgC, gkey, npc, npcHit,
                            senderNick, trigText, toolExec, rec, lnk]() {
                    std::string ctx;
                    if (auto* cst = db.getChatStorage()) {
                        try {
                            namespace orm = sqlite_orm;
                            int rounds = dice::aichat::contextRounds(configMgr);
                            if (rounds > 0) {
                                auto rows = cst->get_all<dice::ChatMsgRow>(
                                    orm::where(orm::c(&dice::ChatMsgRow::platform) == msgC.platform
                                        and orm::c(&dice::ChatMsgRow::groupId) == msgC.targetId),
                                    orm::order_by(&dice::ChatMsgRow::id).desc(), orm::limit(rounds));
                                for (auto it = rows.rbegin(); it != rows.rend(); ++it)
                                    ctx += it->sender + "\xef\xbc\x9a" + dice::aichat::cleanForAi(configMgr, it->content) + "\n";
                            }
                        } catch (...) {}
                    }
                    // 阶段B：注入本群滚动摘要（快速读）；阶段C：按当前消息向量检索相关持久事实
                    //（embedding 同样是网络调用，必须在后台线程）。
                    std::string memBg;
                    if (dice::aimemory::shortEnabled(configMgr))
                        memBg = dice::aimemory::currentSummary(db.getChatStorage(), "group", gkey);
                    if (dice::aimemory::longEnabled(configMgr)) {
                        auto facts = dice::aimemory::retrieveFacts(configMgr, db.getChatStorage(), "group", gkey,
                            dice::aichat::cleanForAi(configMgr, trigText));
                        if (!facts.empty()) {
                            std::string fb = "\xe7\x9b\xb8\xe5\x85\xb3\xe8\xae\xb0\xe5\xbf\x86\xef\xbc\x9a\n";  // 相关记忆：
                            for (auto& f : facts) fb += "- " + f + "\n";
                            memBg = memBg.empty() ? fb : (memBg + "\n" + fb);
                        }
                    }
                    // 阶段E：NPC 命中时用其人设/模型覆盖，否则走默认骰娘对话。
                    // A1：NPC 开了情绪记忆时，注入对该玩家的当前关系（好感/情绪）。
                    nlohmann::json npcMood = nlohmann::json::object();
                    if (npcHit && npc.moodEnabled)
                        npcMood = dice::ainpc::getMood(db.getChatStorage(), gkey, npc.id, msgC.senderId);
                    std::string sysOv = npcHit ? dice::ainpc::systemPrompt(npc, npcMood, senderNick) : std::string();
                    std::string modelOv = npcHit ? npc.modelId : std::string();
                    // C#85：多模态 —— 消息带图且开启图像识别时，识别图片内容并注入当前消息。
                    std::string curText = dice::aichat::cleanForAi(configMgr, trigText);
                    if (dice::aivision::enabled(configMgr) && !msgC.rawContent.empty()) {
                        std::string vdesc = dice::aivision::describe(configMgr, msgC.rawContent);
                        if (!vdesc.empty())
                            curText += "\n\xef\xbc\x88\xe6\x88\x91\xe5\x8f\x91\xe7\x9a\x84\xe5\x9b\xbe\xe7\x89\x87\xe5\x86\x85\xe5\xae\xb9\xef\xbc\x9a" + vdesc + "\xef\xbc\x89";  // （我发的图片内容：...）
                    }
                    std::string aiReply = dice::aichat::generate(configMgr, ctx, senderNick,
                        curText, memBg, toolExec, sysOv, modelOv);
                    if (aiReply.empty()) return;
                    std::string rep = dice::aichat::replyAt(configMgr)
                        ? ("[CQ:at,qq=" + msgC.senderId + "] " + aiReply) : aiReply;
                    // AI 对话回复不带一次性路由状态（无引用覆写/转发节点），类别为空 →
                    // finishReply 内的润色/翻译自然跳过（本就是 AI 生成，无需再加工）。
                    finishReply(msgC, rep, "", "", "", {}, rec, lnk);
                    // A1：互动结束后评估好感变化并写回（已在后台线程，先发后评不拖回复）。
                    if (npcHit && npc.moodEnabled)
                        dice::ainpc::updateMood(configMgr, db.getChatStorage(), npc, gkey,
                            msgC.senderId, senderNick, curText, aiReply);
                };
                if (!dice::aiwork::Worker::instance().post(std::move(job)))
                    DICE_LOG_INFO("C#89: AI \xe9\x98\x9f\xe5\x88\x97\xe5\xb7\xb2\xe6\xbb\xa1\xef\xbc\x8c\xe4\xb8\xa2\xe5\xbc\x83\xe6\x9c\xac\xe6\xac\xa1 AI \xe5\xaf\xb9\xe8\xaf\x9d\xe8\xa7\xa6\xe5\x8f\x91 group {}", msg.targetId);  // 队列已满，丢弃本次 AI 对话触发
            }
        }

        // Triggered broadcast: a pending announcement, delivered ONCE per group as
        // a SEPARATE plain group message (not a quoted reply), only when the group
        // actually triggered the bot. Not mass-sent → avoids spam/ban.
        std::string broadcast;
        if (!disabled && !reply.empty() && !msg.fromSelf && msg.type == dice::MessageType::kGroup && !msg.targetId.empty())
            broadcast = dice::BroadcastManager::instance().takeFor(msg.platform + ":" + msg.targetId);

        // Auto-build the player's profile (every processed message; a non-empty
        // reply counts as a command for the activity counter). C#69：自身消息不建档。
        if (!msg.fromSelf) cmdRouter.recordPlayerActivity(msg, !reply.empty());
        // “活跃” = 本群最近用过指令（非空回复即一次指令，与 recordPlayerActivity 同口径）。
        // 这样定时任务的 inactive>=N 条件表示“N 天无指令”，纯聊天不计入，符合“无指令退群”语义。
        if (msg.type == dice::MessageType::kGroup && !msg.targetId.empty() && !reply.empty())
            cmdRouter.markGroupActive(msg.platform, msg.targetId);   // #47 群活跃度（按指令）
        // .log transcript recording (skipped for disabled groups). C#69：操作者手打的
        // 自控消息（fromSelf 且已过自回声去重）视同正常消息记录；骰娘自己的回复回声不会到这里。
        // C#89：只记入站；骰娘回复待润色/翻译定稿后在 finishReply 里记（recordBotReply）。
        if (!disabled) cmdRouter.recordIncoming(msg);
        // Feed the web "模拟聊天" live window (incoming line + bot reply + broadcast).
        if ((msg.type == dice::MessageType::kGroup && !msg.targetId.empty())
            || (msg.type == dice::MessageType::kPrivate && !msg.senderId.empty())) {
            std::string chatScope = msg.type == dice::MessageType::kPrivate
                ? "private:" + msg.senderId : msg.targetId;
            std::string key = msg.platform + ":" + chatScope;
            // 喂 CQ 原文(msg.rawContent，含 [CQ:image,file=URL])给模拟聊天，前端据此渲染真图片；
            // content 是去掉 CQ 的纯指令文本(无图)，rawContent 才保留图片链接。带 userId 作头像/标识 (C#32)。
            std::string chatContent = !msg.rawContent.empty() ? msg.rawContent
                : (msg.displayContent.empty() ? msg.content : msg.displayContent);
            dice::GroupChatLog::instance().add(key,
                msg.senderName.empty() ? msg.senderId : msg.senderName, msg.senderId,
                chatContent, false);
            // C#89：骰娘回复/广播的模拟聊天与 chat.db 写入移到 finishReply（定稿后）。
            // C#44：同步持久化到 chat.db（带 msgId，供撤回标注与保留期管理）。
            if (auto* cst = db.getChatStorage()) {
                try {
                    int64_t now = static_cast<int64_t>(std::time(nullptr));
                    dice::ChatMsgRow rin;
                    rin.platform = msg.platform; rin.groupId = chatScope; rin.msgId = msg.id;
                    rin.userId = msg.senderId;
                    // C#82：群员显示群名片(群昵称)优先，其次 QQ 昵称，最后 QQ 号。
                    std::string sndCard = (msg.extra.contains("card") && msg.extra["card"].is_string())
                                        ? msg.extra["card"].get<std::string>() : std::string();
                    rin.sender = !sndCard.empty() ? sndCard
                               : (msg.senderName.empty() ? msg.senderId : msg.senderName);
                    rin.content = chatContent; rin.self = 0; rin.time = now;
                    int64_t inId = cst->insert(rin);
                    // C#65：入站消息若含远端(NTQQ)图片，趁 rkey 新鲜后台下载到本地并
                    // 回写行内容为 /api/chat/images/<名>（避免网页直连 QQ 图床 400 裂开）。
                    if (inId > 0 && dice::chatimg::hasRemoteImage(chatContent)) {
                        std::string cap = chatContent;
                        std::thread([&db, inId, cap]() {
                            std::string local = dice::chatimg::localize(cap);
                            if (local == cap) return;
                            if (auto* c = db.getChatStorage()) {
                                try { auto row = c->get<dice::ChatMsgRow>(inId); row.content = local; c->update(row); }
                                catch (...) {}
                            }
                        }).detach();
                    }
                } catch (...) {}
            }
            // 阶段B/C：后台折叠 —— 存完消息后异步更新本群记忆（滚动摘要 + 抽取持久事实，
            // 攒够一批才真正调模型，天然限频）。放后台线程避免阻塞消息处理。默认全关。
            if ((dice::aimemory::shortEnabled(configMgr) || dice::aimemory::longEnabled(configMgr))
                && cmdRouter.aiEnabledForGroup(msg.platform, msg.targetId)      // C#84：本群关 AI 则不建记忆
                && cmdRouter.aiWhitelistOk(msg.platform, msg.targetId, true)) { // AI 白名单模式
                std::string plat = msg.platform, gid = msg.targetId;
                std::thread([&configMgr, &db, plat, gid]() {
                    dice::aimemory::maybeFold(configMgr, db.getChatStorage(), plat, gid);
                }).detach();
            }
        }
        // ── C#89：投递回复 ── 需要 AI 后处理（润色/翻译命中覆盖范围）时走后台线程，
        // 消息线程立即空出来处理下一条；否则原地执行（无 AI 时零行为差异）。
        if (!reply.empty() || !broadcast.empty()) {
            bool needBg = !msg.fromSelf && !reply.empty()
                && ((dice::aipolish::enabled(configMgr) && dice::aipolish::covers(configMgr, aiCat))
                 || (dice::aitrans::enabled(configMgr) && dice::aitrans::covers(configMgr, aiCat)
                     && !cmdRouter.aiLangFor(msg).empty()));
            if (needBg) {
                bool rec = !disabled, lnk = linkReplyOk;
                auto job = [finishReply, msg, reply, broadcast, aiCat, quoteId, fwdNodes, rec, lnk]() {
                    finishReply(msg, reply, broadcast, aiCat, quoteId, fwdNodes, rec, lnk);
                };
                if (!dice::aiwork::Worker::instance().post(std::move(job))) {
                    // AI 队列堵死（持续超时）→ 跳过润色/翻译（类别传空即跳过）直接发，
                    // 保证指令回复永远送达。
                    DICE_LOG_INFO("C#89: AI \xe9\x98\x9f\xe5\x88\x97\xe5\xb7\xb2\xe6\xbb\xa1\xef\xbc\x8c\xe8\xb7\xb3\xe8\xbf\x87\xe6\xb6\xa6\xe8\x89\xb2/\xe7\xbf\xbb\xe8\xaf\x91\xe7\x9b\xb4\xe6\x8e\xa5\xe5\x8f\x91\xe9\x80\x81");  // 队列已满，跳过润色/翻译直接发送
                    finishReply(msg, reply, broadcast, "", quoteId, fwdNodes, !disabled, linkReplyOk);
                }
            } else {
                finishReply(msg, reply, broadcast, aiCat, quoteId, fwdNodes, !disabled, linkReplyOk);
            }
        }
    });

    // Non-message events: 入群欢迎词、被加好友欢迎、加好友/加群条件自动同意。
    adapterMgr.onEvent([&adapterMgr, &configMgr, &i18n, &localeResolver, &cmdRouter, &jsMod, &luaMod, &replyManager, &db](const dice::BotEvent& e) {
        using ET = dice::EventType;
        nlohmann::json cfgAll = configMgr.getAll();
        nlohmann::json ev = (cfgAll.contains("events") && cfgAll["events"].is_object())
                                ? cfgAll["events"] : nlohmann::json::object();
        // 全局开关（系统设置页 dice/listen_*）控制各类事件是否响应。
        auto diceFlag = [&](const char* k, bool def) {
            if (cfgAll.contains("dice") && cfgAll["dice"].contains(k) && cfgAll["dice"][k].is_boolean())
                return cfgAll["dice"][k].get<bool>();
            return def;
        };
        auto diceStr = [&](const char* k) -> std::string {
            if (cfgAll.contains("dice") && cfgAll["dice"].contains(k) && cfgAll["dice"][k].is_string())
                return cfgAll["dice"][k].get<std::string>();
            return {};
        };
        auto a = adapterMgr.getAdapter(e.adapterId);
        if (!a) return;

        // 从适配器成员缓存取用户显示名（群名片 > QQ昵称 > userid）。
        // 解析昵称：群名片 > QQ昵称 > 记录的昵称 > QQ号（用户规范）。类型安全：user_id
        // 在 OneBot 里可能是数字或字符串，统一转字符串比较，避免 json value 类型不匹配抛异常。
        auto resolveUserNick = [&](const std::string& uid, const std::string& gid) -> std::string {
            if (!gid.empty() && a) {
                try {
                    nlohmann::json members = a->getMembers(gid);
                    for (auto& m : members) {
                        if (!m.is_object() || !m.contains("user_id")) continue;
                        std::string mid = m["user_id"].is_string() ? m["user_id"].get<std::string>()
                                        : m["user_id"].is_number() ? std::to_string(m["user_id"].get<long long>()) : "";
                        if (mid != uid) continue;
                        std::string c = m.value("card", std::string());      // 群名片
                        if (!c.empty()) return c;
                        std::string nn = m.value("nickname", std::string());  // QQ昵称
                        if (!nn.empty()) return nn;
                        break;
                    }
                } catch (...) {}
            }
            return cmdRouter.lookupNick(e.platform, uid);   // 记录的昵称，最终回退 QQ号
        };
        // C#100：通知里群/人显示「名字(号码)」；名字未知（未入群/无档案）回退纯号码。
        auto groupLabel = [&](const std::string& gid) -> std::string {
            if (gid.empty()) return gid;
            std::string n = a ? a->getGroupName(gid) : gid;
            return (n.empty() || n == gid) ? gid : (n + "(" + gid + ")");
        };
        auto userLabel = [&](const std::string& uid) -> std::string {
            if (uid.empty()) return uid;
            std::string n = resolveUserNick(uid, e.groupId);
            return (n.empty() || n == uid) ? uid : (n + "(" + uid + ")");
        };

        // Locale for this group/user (for default greetings).
        dice::Message lm;
        lm.platform = e.platform;
        lm.type = e.groupId.empty() ? dice::MessageType::kPrivate : dice::MessageType::kGroup;
        lm.targetId = e.groupId.empty() ? e.userId : e.groupId;
        lm.senderId = e.userId;
        dice::Locale loc = localeResolver.resolve(lm);

        // ── C#44：聊天记录持久化 —— 撤回标注 / 历史消息回流入库 ──
        if (e.type == ET::kGroupUpload) {
            // C#99：群文件上传 → 记入模拟聊天 + chat.db（[CQ:file,...] 供前端渲染
            // 成文件条目，点击经 /api/groups/.../file-url 取下载链）。
            if (e.groupId.empty()) return;
            nlohmann::json f = e.extra.contains("file") && e.extra["file"].is_object()
                ? e.extra["file"] : nlohmann::json::object();
            std::string line = "[CQ:file,name=" + f.value("name", std::string())
                + ",id=" + f.value("id", std::string())
                + ",busid=" + f.value("busid", std::string())
                + ",size=" + f.value("size", std::string()) + "]";
            std::string who = e.userId.empty() ? "?" : e.userId;
            std::string key = e.platform + ":" + e.groupId;
            dice::GroupChatLog::instance().add(key, who, e.userId, line, false);
            if (auto* cst = db.getChatStorage()) {
                try {
                    dice::ChatMsgRow r;
                    r.platform = e.platform; r.groupId = e.groupId; r.userId = e.userId;
                    r.sender = who; r.content = line; r.self = 0;
                    r.time = static_cast<int64_t>(std::time(nullptr));
                    cst->insert(r);
                } catch (...) {}
            }
            DICE_LOG_INFO("event: C#99 群 {} 文件上传 {} by {}", e.groupId, f.value("name", std::string()), e.userId);
            return;
        }
        if (e.type == ET::kGroupRecall) {
            auto* cst = db.getChatStorage();
            if (!cst) return;
            std::string mid;
            try { mid = e.extra.value("message_id", std::string()); } catch (...) {}
            if (mid.empty() || e.groupId.empty()) return;
            try {
                namespace orm = sqlite_orm;
                auto rows = cst->get_all<dice::ChatMsgRow>(orm::where(
                    orm::c(&dice::ChatMsgRow::platform) == e.platform and
                    orm::c(&dice::ChatMsgRow::groupId) == e.groupId and
                    orm::c(&dice::ChatMsgRow::msgId) == mid));
                for (auto r : rows) { r.recalled = 1; cst->update(r); }
                if (!rows.empty())
                    DICE_LOG_INFO("event: 群 {} 消息 {} 已标注撤回（内容保留）", e.groupId, mid);
            } catch (...) {}
            return;
        }
        if (e.type == ET::kGroupHistory) {
            auto* cst = db.getChatStorage();
            if (!cst) return;
            int added = 0;
            try {
                namespace orm = sqlite_orm;
                const auto& msgs = e.extra["messages"];
                for (const auto& m : msgs) {
                    if (!m.is_object()) continue;
                    auto sfield = [&](const char* k) -> std::string {
                        if (!m.contains(k) || m[k].is_null()) return "";
                        if (m[k].is_string()) return m[k].get<std::string>();
                        if (m[k].is_number_integer()) return std::to_string(m[k].get<int64_t>());
                        return "";
                    };
                    std::string mid = sfield("message_id");
                    if (mid.empty()) continue;
                    // 去重：该消息已入库（含实时记录的）则跳过。
                    int dup = (int)cst->count<dice::ChatMsgRow>(orm::where(
                        orm::c(&dice::ChatMsgRow::platform) == e.platform and
                        orm::c(&dice::ChatMsgRow::groupId) == e.groupId and
                        orm::c(&dice::ChatMsgRow::msgId) == mid));
                    if (dup > 0) continue;
                    dice::ChatMsgRow r;
                    r.platform = e.platform; r.groupId = e.groupId; r.msgId = mid;
                    r.userId = sfield("user_id");
                    if (m.contains("sender") && m["sender"].is_object()) {
                        r.sender = m["sender"].value("card", std::string());
                        if (r.sender.empty()) r.sender = m["sender"].value("nickname", std::string());
                    }
                    if (r.sender.empty()) r.sender = r.userId;
                    // raw_message 保留 CQ 码（前端渲染图片）；缺失时退 message 的字符串形式
                    r.content = m.value("raw_message", std::string());
                    if (r.content.empty() && m.contains("message") && m["message"].is_string())
                        r.content = m["message"].get<std::string>();
                    r.self = (!e.selfId.empty() && r.userId == e.selfId) ? 1 : 0;
                    r.time = m.value("time", (int64_t)0);
                    if (r.time == 0) r.time = static_cast<int64_t>(std::time(nullptr));
                    int64_t hid = cst->insert(r);
                    // C#65：历史消息里的远端图片也本地化（拉历史时 rkey 通常仍新鲜）。
                    if (hid > 0 && dice::chatimg::hasRemoteImage(r.content)) {
                        std::string cap = r.content;
                        std::thread([&db, hid, cap]() {
                            std::string local = dice::chatimg::localize(cap);
                            if (local == cap) return;
                            if (auto* c = db.getChatStorage()) {
                                try { auto row = c->get<dice::ChatMsgRow>(hid); row.content = local; c->update(row); }
                                catch (...) {}
                            }
                        }).detach();
                    }
                    ++added;
                }
            } catch (...) {}
            DICE_LOG_INFO("event: 群 {} 历史消息入库 {} 条", e.groupId, added);
            return;
        }

        if (e.type == ET::kGroupIncrease) {
            if (e.userId == e.selfId) {
                // 云黑群组: bot was pulled into a blacklisted group → say so and leave.
                if (cmdRouter.isGroupBanned(e.groupId)) {
                    a->sendGroupMessage(e.groupId, cmdRouter.applySelf(lm, i18n.tr(loc, "event.blacklist_group")));
                    a->leaveGroup(e.groupId);
                    DICE_LOG_INFO("event: leaving blacklisted group {} on join", e.groupId);
                    cmdRouter.notifyMasters(dice::notice::kImportant,
                        "\xe8\xa2\xab\xe6\x8b\x89\xe5\x85\xa5\xe9\xbb\x91\xe5\x90\x8d\xe5\x8d\x95\xe7\xbe\xa4 " + groupLabel(e.groupId) + "\xef\xbc\x8c\xe5\xb7\xb2\xe8\x87\xaa\xe5\x8a\xa8\xe9\x80\x80\xe5\x87\xba", "blacklist_leave");
                    return;
                }
                // 非好友强拉兜底：人少的群可不经审批直接把 bot 拉进来（无 request 事件），
                // 开启「拒绝非好友邀请」时按拉人者(operator，缺失则回退已记录的邀请人)判定，
                // 非好友 → 提示后立即退群。Master/白名单豁免；好友列表未同步时放行。
                if (ev.value("group_invite_reject_nonfriend", false)) {
                    std::string puller = e.operatorId;
                    if (puller.empty())
                        puller = cmdRouter.groupSettingValue(e.platform, e.groupId, "inviter");
                    if (!puller.empty() && puller != e.selfId
                        && !cmdRouter.isMasterUser(e.platform, puller)
                        && !cmdRouter.isUserWhitelisted(puller)) {
                        auto fl = a->getFriendList();
                        bool isFriend = fl.empty();   // 空=未知 → 放行
                        for (const auto& f : fl) if (f == puller) { isFriend = true; break; }
                        if (!isFriend) {
                            a->sendGroupMessage(e.groupId,
                                cmdRouter.applySelf(lm, i18n.tr(loc, "event.nonfriend_group_leave")));
                            a->leaveGroup(e.groupId);
                            DICE_LOG_INFO("event: 非好友 {} 强拉入群 {} → 自动退群", puller, e.groupId);
                            cmdRouter.notifyMasters(dice::notice::kImportant,
                                "\xe8\xa2\xab\xe9\x9d\x9e\xe5\xa5\xbd\xe5\x8f\x8b " + userLabel(puller) + " \xe6\x8b\x89\xe5\x85\xa5\xe7\xbe\xa4 " + groupLabel(e.groupId) + "\xef\xbc\x8c\xe5\xb7\xb2\xe8\x87\xaa\xe5\x8a\xa8\xe9\x80\x80\xe5\x87\xba", "nonfriend_leave");
                            return;
                        }
                    }
                }
                // C#60：曾「删除记录」过的群重新加回来 → 清墓碑标记，否则群组管理
                // 的自动发现会一直跳过它，刷不出这个群。
                if (cmdRouter.groupSettingValue(e.platform, e.groupId, "__removed") == "1") {
                    cmdRouter.setGroupSettingFor(e.platform, e.groupId, "__removed", "0");
                    cmdRouter.setGroupSettingFor(e.platform, e.groupId, "enabled", "1");
                    DICE_LOG_INFO("event: C#60 群 {} 重新加入，已清除移除标记（记录可重建）", e.groupId);
                }
                // C#62：曾指令退群的群重新加回来 → 清「已退群/退群中」状态。
                if (cmdRouter.groupSettingValue(e.platform, e.groupId, "left") == "1" ||
                    cmdRouter.groupSettingValue(e.platform, e.groupId, "leaving") == "1") {
                    cmdRouter.setGroupSettingFor(e.platform, e.groupId, "left", "0");
                    cmdRouter.setGroupSettingFor(e.platform, e.groupId, "leaving", "0");
                }
                // C#47：直接被拉进群（无邀请事件）时，operator 即邀请人
                // 已有邀请人记录时不覆盖。
                if (!e.operatorId.empty() &&
                    cmdRouter.groupSettingValue(e.platform, e.groupId, "inviter").empty())
                    cmdRouter.setGroupSettingFor(e.platform, e.groupId, "inviter", e.operatorId);
                // 群名关键词自动退群：邀请/入群事件不含群名，入群后靠 C#100 反查暖缓存，
                // 延迟数秒按群名判关键词（空白分隔多词，任一命中）→ 提示 + 退群 + 通知骰主。
                {
                    std::string kws = ev.value("group_name_keyword_leave", std::string());
                    if (!kws.empty()) {
                        auto aw = std::weak_ptr<dice::IAdapter>(a);
                        std::string gid = e.groupId, plat = e.platform;
                        drogon::app().getLoop()->runAfter(5.0,
                            [aw, gid, plat, kws, loc, &cmdRouter, &i18n]() {
                            auto ad = aw.lock(); if (!ad || !ad->isConnected()) return;
                            std::string gname = ad->getGroupName(gid);
                            if (gname.empty() || gname == gid) {
                                // 缓存未暖 → 此处跑在 drogon 主 loop，不是 WS 线程，
                                // 同步 invokeAction 安全（echo 由 WS 线程兑现）。
                                long long g = 0; try { g = std::stoll(gid); } catch (...) {}
                                auto r = ad->invokeAction("get_group_info",
                                    {{"group_id", g}, {"no_cache", true}}, 5000);
                                if (r.is_object() && r.contains("data") && r["data"].is_object())
                                    gname = r["data"].value("group_name", std::string());
                            }
                            if (gname.empty() || gname == gid) return;   // 名字拿不到 → 放行
                            std::istringstream ss(kws); std::string kw, hitKw;
                            while (ss >> kw)
                                if (!kw.empty() && gname.find(kw) != std::string::npos) { hitKw = kw; break; }
                            if (hitKw.empty()) return;
                            dice::Message lm2;
                            lm2.platform = plat; lm2.type = dice::MessageType::kGroup; lm2.targetId = gid;
                            ad->sendGroupMessage(gid, cmdRouter.applySelf(lm2,
                                i18n.tr(loc, "event.keyword_group_leave")));
                            ad->leaveGroup(gid);
                            DICE_LOG_INFO("event: 群 {}({}) 名称含关键词「{}」→ 自动退群", gname, gid, hitKw);
                            cmdRouter.notifyMasters(dice::notice::kImportant,
                                "\xe7\xbe\xa4\xe5\x90\x8d\xe3\x80\x8c" + gname + "\xe3\x80\x8d(" + gid
                                    + ") \xe5\x90\xab\xe5\x85\xb3\xe9\x94\xae\xe8\xaf\x8d\xe3\x80\x8c" + hitKw
                                    + "\xe3\x80\x8d\xef\xbc\x8c\xe5\xb7\xb2\xe8\x87\xaa\xe5\x8a\xa8\xe9\x80\x80\xe5\x87\xba", "keyword_leave");
                        });
                    }
                }
                // Bot itself was added to a group → greet + refresh group cache.
                a->refreshGroupList();
                std::string g = ev.value("group_joined", std::string());
                if (g.empty()) g = i18n.tr(loc, "event.group_joined");
                if (!g.empty()) a->sendGroupMessage(e.groupId, g);
                return;
            }
            // Blacklist-quit: if a blacklisted user joins and the group's level is
            // "member" (any member triggers退群), leave the group. ("admin" level
            // only triggers for owner-level offenders, which a joiner never is.)
            if (cmdRouter.isUserBlacklisted(e.userId) &&
                cmdRouter.blacklistQuitLevel(e.platform, e.groupId) == "member") {
                a->sendGroupMessage(e.groupId, i18n.tr(loc, "event.blacklist_quit"));
                a->leaveGroup(e.groupId);
                DICE_LOG_INFO("event: leaving group {} — blacklisted user {} joined", e.groupId, e.userId);
                cmdRouter.notifyMasters(dice::notice::kImportant,
                    "\xe9\xbb\x91\xe5\x90\x8d\xe5\x8d\x95\xe7\x94\xa8\xe6\x88\xb7 " + userLabel(e.userId) + " \xe5\x8a\xa0\xe5\x85\xa5\xe7\xbe\xa4 " + groupLabel(e.groupId) + "\xef\xbc\x8c\xe5\xb7\xb2\xe8\x87\xaa\xe5\x8a\xa8\xe9\x80\x80\xe5\x87\xba", "blacklist_leave");
                return;
            }
            // ── C#51：必须加入用户群 —— 挂靠黑名单检查点（入群事件）。
            // 若本群所有成员中无一人在配置的「用户群」里 → 自动退群。
            // 成员缓存异步刷新，故先请求两边刷新、延时后再比对。
            {
                std::string ug = diceStr("user_group");
                bool enforce  = diceFlag("user_group_enforce", false);
                if (enforce && !ug.empty() && e.groupId != ug) {
                    a->refreshMembers(e.groupId);
                    a->refreshMembers(ug);
                    std::string gid2 = e.groupId, ug2 = ug;
                    auto aw = std::weak_ptr<dice::IAdapter>(a);
                    std::string leaveText = cmdRouter.applySelf(lm, i18n.tr(loc, "event.usergroup_leave", {{"group", ug}}));
                    drogon::app().getLoop()->runAfter(8.0, [aw, gid2, ug2, leaveText]() {
                        auto ad = aw.lock(); if (!ad || !ad->isConnected()) return;
                        auto ids = [&](const std::string& g) {
                            std::set<std::string> s;
                            try {
                                nlohmann::json ms = ad->getMembers(g);
                                for (auto& m : ms) {
                                    if (!m.is_object() || !m.contains("user_id")) continue;
                                    s.insert(m["user_id"].is_string() ? m["user_id"].get<std::string>()
                                        : m["user_id"].is_number() ? std::to_string(m["user_id"].get<long long>()) : "");
                                }
                            } catch (...) {}
                            return s;
                        };
                        auto gm = ids(gid2), um = ids(ug2);
                        if (gm.empty() || um.empty()) return;   // 缓存未就绪 → 本轮跳过（下次入群再查）
                        for (const auto& id : gm) if (um.count(id)) return;   // 有交集 → 合规
                        if (!leaveText.empty()) ad->sendGroupMessage(gid2, leaveText);
                        ad->leaveGroup(gid2);
                        DICE_LOG_INFO("event: C#51 群 {} 无成员在用户群 {} → 自动退群", gid2, ug2);
                    });
                }
            }
            // 入群反馈开关。
            if (!diceFlag("listen_group_add", true)) return;
            // A member joined → send the group's .welcome text if configured.
            // C#76: welcome with delay + cooldown
            std::string welcome = cmdRouter.groupSettingValue(e.platform, e.groupId, "welcome");
            if (welcome.empty()) return;
            int welcomeDelay = 0;
            try { auto d = cmdRouter.groupSettingValue(e.platform, e.groupId, "welcome_delay"); if (!d.empty()) welcomeDelay = std::stoi(d); } catch (...) {}
            int welcomeCooldown = 0;
            try { auto c = cmdRouter.groupSettingValue(e.platform, e.groupId, "welcome_cooldown"); if (!c.empty()) welcomeCooldown = std::stoi(c); } catch (...) {}
            auto doWelcome = [=](const std::string& uid) {
                std::string w = welcome;
                auto rep = [&](const std::string& tok, const std::string& val) {
                    size_t p; while ((p = w.find(tok)) != std::string::npos) w.replace(p, tok.size(), val);
                };
                rep("{at}", "[CQ:at,qq=" + uid + "]");
                rep("{user}", uid);
                rep("{nick}", resolveUserNick(uid, e.groupId));
                a->sendGroupMessageCQ(e.groupId, w);
            };
{
                // C#76 v2: welcome with debounce + cooldown + proper timer cancellation
                static std::mutex wcMtx;
                static std::map<std::string, std::chrono::steady_clock::time_point> lastW;
                static std::map<std::string, std::vector<std::string>> pendU;
                static std::map<std::string, trantor::TimerId> pendT;
                static std::map<std::string, std::shared_ptr<std::atomic<bool>>> pendCancel;
                std::string key = e.platform + ":" + e.groupId;
                auto now = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> lk(wcMtx);

                // Cancel any previously pending debounce timer
                if (auto cit = pendCancel.find(key); cit != pendCancel.end())
                    cit->second->store(true);
                if (auto tit = pendT.find(key); tit != pendT.end())
                    drogon::app().getLoop()->invalidateTimer(tit->second);

                if (welcomeDelay <= 0) {
                    // No debounce delay: check cooldown then send immediately or schedule
                    double cdRemain = 0;
                    if (welcomeCooldown > 0) {
                        auto it = lastW.find(key);
                        if (it != lastW.end()) {
                            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
                            if (elapsed < welcomeCooldown) cdRemain = welcomeCooldown - elapsed;
                        }
                    }
                    if (cdRemain <= 0) {
                        doWelcome(e.userId);
                        lastW[key] = now;
                    } else {
                        // Schedule: send after remaining cooldown
                        auto cancelled = std::make_shared<std::atomic<bool>>(false);
                        pendCancel[key] = cancelled;
                        auto self = a; auto gid = e.groupId; auto plat = e.platform; auto uid = e.userId;
                        pendT[key] = drogon::app().getLoop()->runAfter(cdRemain, [self, gid, plat, uid, key, cancelled, welcome, &cmdRouter]() {
                            if (cancelled->load()) return;
                            std::string w = welcome;
                            { size_t pos; auto rep=[&](const std::string&t,const std::string&v){ while((pos=w.find(t))!=std::string::npos) w.replace(pos,t.size(),v); };
                              rep("{at}","[CQ:at,qq="+uid+"]"); rep("{user}",uid); rep("{nick}",cmdRouter.lookupNick(plat,uid)); }
                            self->sendGroupMessageCQ(gid, w);
                            { std::lock_guard<std::mutex> lk2(wcMtx); lastW[key]=std::chrono::steady_clock::now(); pendT.erase(key); pendCancel.erase(key); }
                        });
                    }
                } else {
                    // Debounce: buffer user, schedule after max(welcomeDelay, remainingCooldown)
                    pendU[key].push_back(e.userId);
                    double delay = static_cast<double>(welcomeDelay);
                    if (welcomeCooldown > 0) {
                        auto it = lastW.find(key);
                        if (it != lastW.end()) {
                            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
                            if (elapsed < welcomeCooldown)
                                delay = (std::max)(delay, static_cast<double>(welcomeCooldown - elapsed));
                        }
                    }
                    auto cancelled = std::make_shared<std::atomic<bool>>(false);
                    pendCancel[key] = cancelled;
                    auto users = pendU[key];
                    auto self = a; auto gid = e.groupId; auto plat = e.platform;
                    pendT[key] = drogon::app().getLoop()->runAfter(delay, [self, gid, plat, key, users, cancelled, welcome, &cmdRouter]() {
                        if (cancelled->load()) return;
                        std::string w = welcome;
                        { size_t pos; auto rep=[&](const std::string&t,const std::string&v){ while((pos=w.find(t))!=std::string::npos) w.replace(pos,t.size(),v); };
                          auto uid = users.back();
                          rep("{at}","[CQ:at,qq="+uid+"]"); rep("{user}",uid); rep("{nick}",cmdRouter.lookupNick(plat,uid)); }
                        self->sendGroupMessageCQ(gid, w);
                        { std::lock_guard<std::mutex> lk2(wcMtx); lastW[key]=std::chrono::steady_clock::now(); pendU.erase(key); pendT.erase(key); pendCancel.erase(key); }
                    });
                }
}
        } else if (e.type == ET::kGroupDecrease) {
            // B：骰娘被移出群（或自身退群回执）→ 标记已退群 + 通知骰主。
            if (!e.selfId.empty() && e.userId == e.selfId && !e.groupId.empty()) {
                cmdRouter.setGroupSettingFor(e.platform, e.groupId, "left", "1");
                std::string who = (!e.operatorId.empty() && e.operatorId != e.selfId)
                    ? ("\xef\xbc\x88\xe6\x93\x8d\xe4\xbd\x9c\xe8\x80\x85 " + userLabel(e.operatorId) + "\xef\xbc\x89") : std::string();
                cmdRouter.notifyMasters(dice::notice::kImportant,
                    "\xe9\xaa\xb0\xe5\xa8\x98\xe5\xb7\xb2\xe7\xa6\xbb\xe5\xbc\x80\xe7\xbe\xa4 " + groupLabel(e.groupId) + who, "group_left");
            }
        } else if (e.type == ET::kPoke) {
            if (!ev.value("poke_enabled", true)) return;   // C#70：戳一戳回复总开关（默认开）
            // 戳一戳: only react when WE were the one poked (target == self).
            if (e.selfId.empty() || e.userId != e.selfId) return;
            // 映射：events.poke_command 设了就把它当「被戳者发的消息」跑完整回复管线
            //（内置指令 / JS插件 / Lua插件 / 自定义回复），实现戳一戳→任意功能。
            std::string runMsg = ev.value("poke_command", std::string());
            if (!runMsg.empty()) {
                dice::Message pm;
                pm.platform = e.platform; pm.adapterId = e.adapterId; pm.selfId = e.selfId;
                pm.senderId = e.operatorId;
                pm.senderName = resolveUserNick(e.operatorId, e.groupId);   // 群名片>QQ昵称>记录>QQ号
                pm.content = runMsg; pm.rawContent = runMsg; pm.displayContent = runMsg;
                bool pv = e.groupId.empty();
                pm.type = pv ? dice::MessageType::kPrivate : dice::MessageType::kGroup;
                pm.targetId = pv ? e.operatorId : e.groupId;
                std::string reply = cmdRouter.handleMessage(pm);
                if (reply.empty() && !cmdRouter.isGroupDisabled(pm)) {
                    if (jsMod.ready()) {
                        if (auto body = cmdRouter.commandBody(pm.content); body && !body->empty()) {
                            auto jr = jsMod.handle(pm.platform, pm.senderId, pm.senderName, pm.targetId, "", pv, *body,
                                                   cmdRouter.jsPrivilegeLevel(pm), pm.atList);
                            if (jr.matched && !jr.reply.empty()) reply = jr.reply;
                        }
                    }
                    if (reply.empty() && luaMod.ready()) {
                        int trust = cmdRouter.jsPrivilegeLevel(pm) >= 70 ? 4 : 0;
                        auto lr = luaMod.dispatch(pm.content, pm.senderId, pv ? "" : pm.targetId, pm.senderName, "", pv, trust, pm.platform);
                        if (lr.matched && !lr.reply.empty()) reply = lr.reply;
                    }
                    if (reply.empty()) {
                        auto matches = replyManager.matchMessage(pm.content);
                        if (!matches.empty()) { const auto& r = matches.front();
                            reply = cmdRouter.renderReply(pm, replyManager.pickResult(r), r.matchContent, r.matchType); }
                    }
                }
                if (!reply.empty()) {
                    reply = cmdRouter.applySelf(pm, reply);
                    if (pv) a->sendPrivateMessage(e.operatorId, reply);
                    else a->sendGroupMessageCQ(e.groupId, reply);
                }
                DICE_LOG_INFO("event: poke->run '{}' by {} in group {}", runMsg, e.operatorId, e.groupId);
                return;
            }
            // 回退：发一段文本（原行为）。{at} 戳回操作者；其他变量走 applySelf。
            std::string poke = ev.value("poke", std::string());
            if (poke.empty()) poke = i18n.tr(loc, "event.poke");
            if (poke.empty()) return;
            {
                auto rep = [&](const std::string& tok, const std::string& val) {
                    size_t p; while ((p = poke.find(tok)) != std::string::npos) poke.replace(p, tok.size(), val);
                };
                rep("{at}", "[CQ:at,qq=" + e.operatorId + "]");
            }
            // 用 applySelf 统一处理 {name}/{nick}/{user}/{group} 等变量
            {
                dice::Message pm2;
                pm2.platform = e.platform; pm2.adapterId = e.adapterId; pm2.selfId = e.selfId;
                pm2.senderId = e.operatorId;
                pm2.senderName = resolveUserNick(e.operatorId, e.groupId);
                bool pv2 = e.groupId.empty();
                pm2.type = pv2 ? dice::MessageType::kPrivate : dice::MessageType::kGroup;
                pm2.targetId = pv2 ? e.operatorId : e.groupId;
                poke = cmdRouter.applySelf(pm2, poke);
            }
            if (e.groupId.empty()) a->sendPrivateMessage(e.operatorId, poke);
            else a->sendGroupMessageCQ(e.groupId, poke);
            DICE_LOG_INFO("event: poked by {} in group {}", e.operatorId, e.groupId);
        } else if (e.type == ET::kFriendAdd) {
            a->refreshGroupList();   // 同步刷新好友列表缓存（非好友邀请判定依赖它）
            if (!diceFlag("listen_friend_add", true)) return;       // 新好友事件开关
            // B：新好友通过通知骰主。
            cmdRouter.notifyMasters(dice::notice::kImportant,
                "\xe6\x96\xb0\xe5\xa5\xbd\xe5\x8f\x8b\xe5\xb7\xb2\xe6\xb7\xbb\xe5\x8a\xa0\xef\xbc\x9a" + userLabel(e.userId), "friend_add");
            std::string fw = ev.value("friend_welcome", std::string());
            if (fw.empty()) fw = i18n.tr(loc, "event.friend_welcome");
            if (!fw.empty()) a->sendPrivateMessage(e.userId, fw);
            // ── C#51：新好友不在用户群 → 私聊发送用户群邀请（群号+引导文本）。
            // 说明：OneBot v11 无标准「发送群邀请卡片」API，先以私聊文本邀请。
            {
                std::string ug = diceStr("user_group");
                if (!ug.empty() && diceFlag("user_group_invite", true)) {
                    a->refreshMembers(ug);
                    std::string uid2 = e.userId, ug2 = ug;
                    auto aw = std::weak_ptr<dice::IAdapter>(a);
                    std::string inviteText = cmdRouter.applySelf(lm,
                        i18n.tr(loc, "event.usergroup_invite", {{"group", ug}}));
                    drogon::app().getLoop()->runAfter(6.0, [aw, uid2, ug2, inviteText]() {
                        auto ad = aw.lock(); if (!ad || !ad->isConnected()) return;
                        try {
                            nlohmann::json ms = ad->getMembers(ug2);
                            for (auto& m : ms) {
                                if (!m.is_object() || !m.contains("user_id")) continue;
                                std::string mid = m["user_id"].is_string() ? m["user_id"].get<std::string>()
                                    : m["user_id"].is_number() ? std::to_string(m["user_id"].get<long long>()) : "";
                                if (mid == uid2) return;   // 已在用户群 → 不打扰
                            }
                        } catch (...) {}
                        if (!inviteText.empty()) ad->sendPrivateMessage(uid2, inviteText);
                        DICE_LOG_INFO("event: C#51 新好友 {} 不在用户群 {} → 已私聊邀请", uid2, ug2);
                    });
                }
            }
        } else if (e.type == ET::kFriendRequest) {
            if (!diceFlag("listen_friend_request", true)) return;    // 好友请求事件开关
            // B：通知骰主收到好友申请（含附言），便于人工处理。
            cmdRouter.notifyMasters(dice::notice::kImportant,
                "\xe6\x94\xb6\xe5\x88\xb0\xe5\xa5\xbd\xe5\x8f\x8b\xe7\x94\xb3\xe8\xaf\xb7\xef\xbc\x9a" + userLabel(e.userId)
                    + (e.comment.empty() ? "" : "\xef\xbc\x88\xe9\x99\x84\xe8\xa8\x80\xef\xbc\x9a" + e.comment + "\xef\xbc\x89"), "friend_req");
            // 策略：all=任意通过 / keyword=含关键词才通过(其余留人工) / reject=禁止任何人添加 /
            //       manual=不自动处理。未设 friend_policy 时由旧 auto_approve_friend 派生。
            std::string pol = ev.value("friend_policy", std::string());
            if (pol.empty()) pol = ev.value("auto_approve_friend", false)
                ? (ev.value("friend_keyword", std::string()).empty() ? "all" : "keyword") : "manual";
            if (pol == "all") {
                a->setFriendRequest(e.flag, true);
                DICE_LOG_INFO("event: 好友申请自动通过(任意) from {}", e.userId);
            } else if (pol == "keyword") {
                std::string kw = ev.value("friend_keyword", std::string());
                if (!kw.empty() && e.comment.find(kw) != std::string::npos) {
                    a->setFriendRequest(e.flag, true);
                    DICE_LOG_INFO("event: 好友申请含关键词自动通过 from {}", e.userId);
                }   // 不含关键词 → 留人工，不处理
            } else if (pol == "reject") {
                a->setFriendRequest(e.flag, false);
                DICE_LOG_INFO("event: 好友申请自动拒绝(禁止添加) from {}", e.userId);
            }   // manual → 不处理
        } else if (e.type == ET::kGroupRequest) {
            if (!diceFlag("listen_group_request", true)) return;     // 群请求事件开关
            if (e.subType == "invite") {
                // 记录群邀请人。
                // 无论走哪条审批路径（自动/人工）都先记下，邀请人视同群管理（canRoomHost）。
                if (!e.userId.empty() && !e.groupId.empty())
                    cmdRouter.setGroupSettingFor(e.platform, e.groupId, "inviter", e.userId);
                // B：通知骰主收到加群邀请（含邀请人），便于人工处理。
                cmdRouter.notifyMasters(dice::notice::kImportant,
                    "\xe6\x94\xb6\xe5\x88\xb0\xe5\x8a\xa0\xe7\xbe\xa4\xe9\x82\x80\xe8\xaf\xb7\xef\xbc\x9a\xe7\xbe\xa4 " + groupLabel(e.groupId)
                        + " \xef\xbc\x88\xe9\x82\x80\xe8\xaf\xb7\xe4\xba\xba " + userLabel(e.userId) + "\xef\xbc\x89", "group_invite");
                // 加群邀请（机器人被邀请进群）。注意：邀请事件不含群名称，故「群名含
                // 非法关键词」无法在此判定 → 归入「进群后自动退群」规则（另行规划）。
                // 叠加拒绝：黑名单群邀请永远先拒。
                if (ev.value("group_invite_reject_blacklist", true) && cmdRouter.isGroupBanned(e.groupId)) {
                    a->setGroupRequest(e.flag, e.subType, false, "blacklisted group");
                    DICE_LOG_INFO("event: 黑名单群邀请自动拒绝 group {}", e.groupId);
                    return;
                }
                // 叠加拒绝：非好友邀请（广告号常批量拉群成员进其他群）。Master/白名单豁免；
                // 好友列表未同步（空）时放行，避免误杀。
                if (ev.value("group_invite_reject_nonfriend", false)
                    && !cmdRouter.isMasterUser(e.platform, e.userId)
                    && !cmdRouter.isUserWhitelisted(e.userId)) {
                    auto fl = a->getFriendList();
                    bool isFriend = fl.empty();   // 空=未知 → 视同好友放行
                    for (const auto& f : fl) if (f == e.userId) { isFriend = true; break; }
                    if (!isFriend) {
                        a->setGroupRequest(e.flag, e.subType, false, "");
                        DICE_LOG_INFO("event: 非好友 {} 的群邀请自动拒绝 group {}", e.userId, e.groupId);
                        cmdRouter.notifyMasters(dice::notice::kImportant,
                            "\xe9\x9d\x9e\xe5\xa5\xbd\xe5\x8f\x8b " + userLabel(e.userId) + " \xe9\x82\x80\xe8\xaf\xb7\xe5\x8a\xa0\xe7\xbe\xa4 " + groupLabel(e.groupId) + "\xef\xbc\x8c\xe5\xb7\xb2\xe8\x87\xaa\xe5\x8a\xa8\xe6\x8b\x92\xe7\xbb\x9d", "nonfriend_leave");
                        return;
                    }
                }
                std::string pol = ev.value("group_invite_policy", std::string());
                if (pol.empty()) pol = ev.value("auto_approve_group", false) ? "all" : "manual";
                if (pol == "all") {
                    a->setGroupRequest(e.flag, e.subType, true);
                    DICE_LOG_INFO("event: 群邀请自动通过(任意) group {}", e.groupId);
                } else if (pol == "whitelist") {
                    if (cmdRouter.isMasterUser(e.platform, e.userId) || cmdRouter.isUserWhitelisted(e.userId)) {
                        a->setGroupRequest(e.flag, e.subType, true);
                        DICE_LOG_INFO("event: 白名单/Master 群邀请自动通过 group {} by {}", e.groupId, e.userId);
                    }   // 非白名单 → 留人工
                } else if (pol == "reject") {
                    a->setGroupRequest(e.flag, e.subType, false, "");
                    DICE_LOG_INFO("event: 群邀请自动拒绝 group {}", e.groupId);
                }   // ignore / manual → 不处理（忽视所有邀请）
            } else {
                // sub_type=add：有人申请加入机器人作为管理员的群。
                // C#58：优先看本群 .group auto pass 设置（all=全过 / 关键字=验证消息含关键字才过）。
                std::string ap = cmdRouter.groupSettingValue(e.platform, e.groupId, "autoPass");
                if (!ap.empty()) {
                    if (ap == "all" || (!e.comment.empty() && e.comment.find(ap) != std::string::npos)) {
                        a->setGroupRequest(e.flag, e.subType, true);
                        DICE_LOG_INFO("event: C#58 群 {} 加群申请自动通过(auto pass) from {}", e.groupId, e.userId);
                    }   // 不含关键字 → 留人工
                    return;   // 本群已配 autoPass，以它为准，不再走旧全局行为
                }
                // 无 per-group 设置 → 回退旧全局行为。
                if (!ev.value("auto_approve_group", false)) return;
                std::string kw = ev.value("group_keyword", std::string());
                if (!kw.empty() && e.comment.find(kw) == std::string::npos) return;
                a->setGroupRequest(e.flag, e.subType, true);
                DICE_LOG_INFO("event: auto-approved group join request for group {}", e.groupId);
            }
        } else if (e.type == ET::kPoke) {
            if (!ev.value("poke_enabled", true)) return;   // C#70：戳一戳回复总开关（默认开）
            // Only react when the BOT itself is poked, in a group.
            if (e.userId != e.selfId || e.groupId.empty()) return;
            if (cmdRouter.isGroupDisabledFor(e.platform, e.groupId)) return;
            a->sendGroupMessage(e.groupId, i18n.tr(loc, "event.poke"));
        }
    });

    // Register adapters from DB (don't start yet — wait for event loop)
    auto* st = db.getStorage();
    if (st) {
        // First-run bootstrap: if the adapters table is empty, seed it from the
        // config file. Without this, a freshly-built bot has zero adapters in the
        // DB and connects to nothing — the web UI then becomes the source of truth.
        if (st->count<dice::AdapterRow>() == 0) {
            nlohmann::json cfgAll = configMgr.getAll();
            if (cfgAll.contains("adapters") && cfgAll["adapters"].is_array()) {
                for (auto& a : cfgAll["adapters"]) {
                    dice::AdapterRow row;
                    row.name           = a.value("name", std::string("Bot"));
                    row.type           = static_cast<int>(dice::adapterTypeFromString(
                                             a.value("type", std::string("onebot_v11"))));
                    row.connectionMode = static_cast<int>(dice::connectionModeFromString(
                                             a.value("connection_mode", std::string("forward_ws"))));
                    row.endpoint       = a.value("endpoint", std::string(""));
                    row.accessToken    = a.value("access_token", std::string(""));
                    row.enabled        = a.value("enabled", false);
                    row.config         = (row.type == static_cast<int>(dice::AdapterType::kQQOfficial))
                        ? nlohmann::json{{"appId", a.value("app_id", a.value("appId", std::string()))},
                                         {"appSecret", a.value("app_secret", a.value("appSecret", std::string()))}}.dump()
                        : "{}";
                    st->insert(row);
                }
                DICE_LOG_INFO("Seeded {} adapter(s) from config into database",
                    cfgAll["adapters"].size());
            }
        }

        auto adapters = st->get_all<dice::AdapterRow>();
        for (auto& row : adapters) {
            if (row.enabled) {
                dice::AdapterPtr adapter;
                if (row.type == static_cast<int>(dice::AdapterType::kQQOfficial)) {
                    auto c = nlohmann::json::parse(row.config, nullptr, false);
                    auto qq = std::make_shared<dice::QQOfficialAdapter>(std::to_string(row.id));
                    qq->configure({{"name", row.name}, {"appId", c.value("appId", std::string())},
                                   {"appSecret", c.value("appSecret", std::string())}});
                    adapter = qq;
                } else {
                    std::string mode = (row.connectionMode == 1) ? "reverse_ws" : (row.connectionMode == 2) ? "http" : "forward_ws";
                    auto onebot = std::make_shared<dice::OneBotV11Adapter>(std::to_string(row.id));
                    onebot->configure({{"name", row.name}, {"endpoint", row.endpoint},
                        {"accessToken", row.accessToken}, {"connectionMode", mode}});
                    adapter = onebot;
                }
                adapterMgr.registerAdapter(adapter);
            }
        }
    }

    DICE_LOG_INFO("Adapter framework initialized");

    // 运行报错推送：挂接 spdlog 错误 sink，
    // ERROR 级日志推送给订阅了「错误」级别的骰主通知窗口（60s 节流 + 防递归）。
    dice::getLogger()->sinks().push_back(std::make_shared<dice::notice::ErrorNotifySink>(
        [&configMgr, &adapterMgr](const std::string& m) {
            dice::notice::notify(configMgr, adapterMgr, dice::notice::kError,
                "\xe8\xbf\x90\xe8\xa1\x8c\xe9\x94\x99\xe8\xaf\xaf\xef\xbc\x9a" + m, "", "", "error");
        }));

    // ── 9. Start drogon HTTP server ──────────────────────────
    int port = configMgr.get<int>("server/port", 18088);
    std::string host = configMgr.get<std::string>("server/host", "0.0.0.0");

    DICE_LOG_INFO("Starting drogon HTTP server on {}:{}", host, port);

    auto& app = drogon::app();
    app.setLogLevel(trantor::Logger::kWarn);
    app.addListener(host, port);
    app.setThreadNum(4);
    // 上传体积上限：规则包(含 lua/js mod)、牌堆、图片都走 base64 塞进 JSON body，
    // drogon 默认 client_max_body_size 仅 1MB，稍大的规则包就会被拒（报 "string too long"）。
    // 放宽到 128MB，并让大 body 直接驻内存（getBody() 可直接读，不落临时文件）。
    app.setClientMaxBodySize(128 * 1024 * 1024);
    app.setClientMaxMemoryBodySize(128 * 1024 * 1024);

    // ── C#34 WebUI 登录鉴权 ───────────────────────────────────
    dice::WebAuth::instance().setPassword(configMgr.get<std::string>("webui/password", ""));
    // 前置拦截：设了口令时，/api/* 需有效会话 Cookie（放行登录/状态查询与静态文件）。
    app.registerPreHandlingAdvice(
        [](const drogon::HttpRequestPtr& req,
           drogon::AdviceCallback&& stop, drogon::AdviceChainCallback&& next) {
            const std::string& path = req->path();
            if (path.rfind("/api/", 0) != 0 ||                       // 静态文件
                path == "/api/auth/login" || path == "/api/auth/status") { next(); return; }
            if (!dice::WebAuth::instance().hasPassword()) { next(); return; }   // 未设口令=不鉴权
            if (dice::WebAuth::instance().validToken(req->getCookie("dice_session"))) { next(); return; }
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k401Unauthorized);
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody("{\"code\":401,\"message\":\"unauthorized\"}");
            stop(resp);
        });
    {
        auto jResp = [](const nlohmann::json& out, drogon::HttpStatusCode code = drogon::k200OK) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(code);
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(out.dump());
            return resp;
        };
        // 是否需要登录 + 当前会话是否已登录。
        app.registerHandler("/api/auth/status",
            [jResp](const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                bool required = dice::WebAuth::instance().hasPassword();
                bool authed = !required || dice::WebAuth::instance().validToken(req->getCookie("dice_session"));
                cb(jResp({{"code", 0}, {"message", "ok"}, {"data", {{"required", required}, {"authed", authed}}}}));
            }, {drogon::Get});
        // 登录：校验口令 → 颁发 token，写 Cookie。
        app.registerHandler("/api/auth/login",
            [jResp](const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                try {
                    auto j = nlohmann::json::parse(req->getBody());
                    std::string pw = j.value("password", "");
                    if (!dice::WebAuth::instance().hasPassword() || dice::WebAuth::instance().checkPassword(pw)) {
                        std::string token = dice::WebAuth::instance().issueToken();
                        auto resp = jResp({{"code", 0}, {"message", "ok"}, {"data", {{"authed", true}}}});
                        drogon::Cookie ck("dice_session", token);
                        ck.setPath("/"); ck.setHttpOnly(true); ck.setMaxAge(7 * 24 * 3600);
                        resp->addCookie(ck);
                        cb(resp);
                    } else {
                        cb(jResp({{"code", 401}, {"message", "密码错误"}}, drogon::k401Unauthorized));
                    }
                } catch (const std::exception& e) { cb(jResp({{"code", 1}, {"message", e.what()}})); }
            }, {drogon::Post});
        // 退出：吊销当前 token。
        app.registerHandler("/api/auth/logout",
            [jResp](const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                dice::WebAuth::instance().revoke(req->getCookie("dice_session"));
                cb(jResp({{"code", 0}, {"message", "ok"}}));
            }, {drogon::Post});
        // 设置/修改/清除 WebUI 口令（已登录态下才可达——前置拦截已保证）。空=关闭鉴权。
        app.registerHandler("/api/system/webui-auth",
            [jResp, &configMgr](const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                try {
                    if (req->method() == drogon::Put) {
                        auto j = nlohmann::json::parse(req->getBody());
                        std::string pw = j.value("password", "");
                        configMgr.set<std::string>("webui/password", pw);
                        configMgr.save();
                        dice::WebAuth::instance().setPassword(pw);
                    }
                    cb(jResp({{"code", 0}, {"message", "ok"},
                              {"data", {{"enabled", dice::WebAuth::instance().hasPassword()}}}}));
                } catch (const std::exception& e) { cb(jResp({{"code", 1}, {"message", e.what()}})); }
            }, {drogon::Get, drogon::Put});
        // 运行 IP/端口：读取/保存。⚠️ HTTP 监听在启动时绑定，改了**需重启**才生效（drogon
        // 无法运行时换监听）；故返回 restart_required=true。可配合 /api/system/restart 自动重启。
        app.registerHandler("/api/system/server-config",
            [jResp, &configMgr](const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                try {
                    bool changed = false;
                    if (req->method() == drogon::Put) {
                        auto j = nlohmann::json::parse(req->getBody());
                        if (j.contains("host") && j["host"].is_string())
                            { configMgr.set<std::string>("server/host", j["host"].get<std::string>()); changed = true; }
                        if (j.contains("port") && j["port"].is_number()) {
                            int p = j["port"].get<int>();
                            if (p >= 1 && p <= 65535) { configMgr.set<int>("server/port", p); changed = true; }
                        }
                        if (changed) configMgr.save();
                    }
                    cb(jResp({{"code", 0}, {"message", "ok"}, {"data", {
                        {"host", configMgr.get<std::string>("server/host", "0.0.0.0")},
                        {"port", configMgr.get<int>("server/port", 18088)},
                        {"restart_required", changed}}}}));
                } catch (const std::exception& e) { cb(jResp({{"code", 1}, {"message", e.what()}})); }
            }, {drogon::Get, drogon::Put});
        // 重启程序：派生一个分离的小批处理(等本进程退出→重启 exe)，然后优雅退出。
        app.registerHandler("/api/system/restart",
            [jResp](const drogon::HttpRequestPtr&,
                    std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                cb(jResp({{"code", 0}, {"message", "restarting"}}));
                relaunchSelf();
            }, {drogon::Post});
    }

    // Static file serving — serve web/dist/ as document root
    namespace fs = std::filesystem;
    std::string webRoot = "./web/dist";
    // Also try relative to executable path
    if (!fs::exists(webRoot)) {
        webRoot = "../web/dist";
    }
    if (fs::exists(webRoot)) {
        app.setDocumentRoot(webRoot);
        app.setStaticFilesCacheTime(0);
        DICE_LOG_INFO("Serving static frontend from '{}'", webRoot);
    } else {
        DICE_LOG_WARN("Frontend not found at '{}'", webRoot);
    }

    // (Removed dead SPA-fallback route table: the frontend uses hash routing,
    // so the browser only ever requests "/" — those per-path handlers never fired
    // and were already out of sync with the real routes.)

    // Console log mode: human-readable by default; raw JSON dump if configured.
    dice::OneBotV11Adapter::s_rawEventLog = configMgr.get<bool>("log/raw_events", false);
    // C#69 自响应（用骰娘账号自身发指令自控）：默认关。
    dice::OneBotV11Adapter::s_respondSelf = configMgr.get<bool>("dice/respond_self", false);

    // C#56：图片发送方式（[img,file=..] 发送期解析要读 dice/image_send 配置）。
    dice::imgsend::init(configMgr);

    // ── Register real REST API endpoints ─────────────────────
    dice::utils::setStartupEpoch();
    dice::api::registerApiRoutes(db, configMgr, adapterMgr, cardDeck, replyManager, i18n, jsMod, luaMod,
                                 causalMgr, cooldownMgr, counterStore, personaMgr);
    DICE_LOG_INFO("REST API routes registered");

    // ── Playground test harness ──────────────────────────────
    // The chat UI lives in the React app (sidebar → 测试台). This is the
    // backend it calls: run a fake message through the command router.
    app.registerHandler("/api/test/message",
        [&cmdRouter, &replyManager, &jsMod, &luaMod, &configMgr, &db, &makeAiTool](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            nlohmann::json out;
            try {
                auto body = nlohmann::json::parse(req->getBody());
                dice::Message msg;
                msg.id         = "playground";
                msg.platform   = body.value("platform", std::string("onebot_v11"));
                msg.content    = body.value("text", std::string(""));
                msg.rawContent = body.value("rawContent", msg.content);   // C#85：可带 CQ 图码测识图
                msg.displayContent = msg.content;
                msg.senderId   = body.value("userId", std::string("10001"));
                msg.senderName = body.value("nickname", std::string("\xe6\xb5\x8b\xe8\xaf\x95\xe5\x91\x98"));
                if (body.value("messageType", std::string("group")) == "private") {
                    msg.type     = dice::MessageType::kPrivate;
                    msg.targetId = msg.senderId;
                } else {
                    msg.type     = dice::MessageType::kGroup;
                    msg.targetId = body.value("groupId", std::string("100001"));
                }
                // Optional multi-bot targeting fields for testing.
                msg.selfId = body.value("selfId", std::string(""));
                if (body.contains("role")) msg.extra["role"] = body.value("role", std::string(""));
                if (body.contains("card")) msg.extra["card"] = body.value("card", std::string(""));   // 测试台注入群名片，验证 {card}/{nick}
                if (body.contains("atList") && body["atList"].is_array())
                    for (const auto& a : body["atList"])
                        if (a.is_string()) msg.atList.push_back(a.get<std::string>());

                std::optional<dice::Locale> forced;
                if (body.contains("locale") && body["locale"].is_string()) {
                    std::string lc = body["locale"].get<std::string>();
                    if (!lc.empty()) forced = dice::localeFromString(lc);
                }
                std::string reply;
                if ((!dice::CommandRouter::isForAnotherBot(msg) || jsCommandMatches(jsMod, cmdRouter, msg))
                    && !cmdRouter.isBlocked(msg)) {
                    bool disabled = cmdRouter.isGroupDisabled(msg);
                    bool forcedByAt = dice::CommandRouter::isAtSelf(msg) && !cmdRouter.isGroupLocked(msg);
                    bool replyOff = msg.type == dice::MessageType::kGroup && !msg.targetId.empty()
                                    && cmdRouter.isReplyDisabledFor(msg.platform, msg.targetId);
                    reply = cmdRouter.handleMessage(msg, forced);
                    std::string replySrc = "builtin";   // C#68：来源分类（同 live 管线）
                    if (reply.empty() && (!disabled || forcedByAt) && !replyOff) {
                        if (jsMod.ready()) {
                            if (auto bd = cmdRouter.commandBody(msg.content); bd && !bd->empty()) {
                                auto jr = jsMod.handle(msg.platform, msg.senderId,
                                    msg.senderName.empty() ? msg.senderId : msg.senderName,
                                    msg.targetId, msg.extra.value("card", std::string()), msg.type == dice::MessageType::kPrivate, *bd,
                                    cmdRouter.jsPrivilegeLevel(msg), msg.atList);
                                if (jr.matched && !jr.reply.empty()) { reply = jr.reply; replySrc = "plugin"; }
                            }
                        }
                        if (reply.empty() && luaMod.ready()) {
                            bool priv = msg.type == dice::MessageType::kPrivate;
                            int trust = cmdRouter.jsPrivilegeLevel(msg) >= 70 ? 4 : 0;
                            auto lr = luaMod.dispatch(msg.content, msg.senderId, priv ? "" : msg.targetId,
                                msg.senderName.empty() ? msg.senderId : msg.senderName, msg.extra.value("card", std::string()), priv, trust, msg.platform);
                            if (lr.matched && !lr.reply.empty()) { reply = lr.reply; replySrc = "plugin"; }
                        }
                        if (reply.empty()) {
                            auto matches = replyManager.matchMessage(msg.content);
                            if (!matches.empty()) {
                                const auto& r = matches.front();
                                reply = cmdRouter.renderReply(msg, replyManager.pickResult(r), r.matchContent, r.matchType);
                                if (!reply.empty()) replySrc = "reply";
                            }
                        }
                        if (reply.empty() && jsMod.ready()) {
                            auto nc = jsMod.handleNonCommand(msg.platform, msg.senderId,
                                msg.senderName.empty() ? msg.senderId : msg.senderName,
                                msg.targetId, msg.extra.value("card", std::string()), msg.type == dice::MessageType::kPrivate, msg.content,
                                cmdRouter.jsPrivilegeLevel(msg), msg.atList);
                            if (nc.matched && !nc.reply.empty()) { reply = nc.reply; replySrc = "plugin"; }
                        }
                    }
                    // 智能化阶段A：测试台也走 AI 对话（无其它回复+触发时），方便骰主预览。
                    // 测试台不读 chat.db 上下文（用空上下文），仅验证触发+生成+发送。
                    if (reply.empty() && !disabled && (dice::aichat::enabled(configMgr) || dice::ainpc::enabled(configMgr))
                        && (msg.targetId.empty() || (cmdRouter.aiEnabledForGroup(msg.platform, msg.targetId)
                            && cmdRouter.aiWhitelistOk(msg.platform, msg.targetId, true)))) {  // C#84 开关 + AI 白名单
                        bool atMe = !msg.selfId.empty()
                            && std::find(msg.atList.begin(), msg.atList.end(), msg.selfId) != msg.atList.end();
                        std::string gkNpc = msg.platform + ":" + msg.targetId;
                        dice::ainpc::Npc pnpc;
                        bool pNpcHit = dice::ainpc::enabled(configMgr)
                            && dice::ainpc::match(configMgr, gkNpc, dice::aichat::cleanForAi(configMgr, msg.content), pnpc);
                        bool pDefaultHit = !pNpcHit && dice::aichat::enabled(configMgr)
                            && dice::aichat::shouldTrigger(configMgr, msg.content, atMe);
                        if (pNpcHit || pDefaultHit) {
                            std::string sn = msg.senderName.empty() ? msg.senderId : msg.senderName;
                            // 阶段B/C：测试台也注入本群摘要 + 检索到的持久事实，便于预览记忆效果。
                            std::string memBg;
                            std::string gk = msg.platform + ":" + msg.targetId;
                            if (dice::aimemory::shortEnabled(configMgr) && !msg.targetId.empty())
                                memBg = dice::aimemory::currentSummary(db.getChatStorage(), "group", gk);
                            if (dice::aimemory::longEnabled(configMgr) && !msg.targetId.empty()) {
                                auto facts = dice::aimemory::retrieveFacts(configMgr, db.getChatStorage(), "group", gk,
                                    dice::aichat::cleanForAi(configMgr, msg.content));
                                if (!facts.empty()) {
                                    std::string fb = "\xe7\x9b\xb8\xe5\x85\xb3\xe8\xae\xb0\xe5\xbf\x86\xef\xbc\x9a\n";
                                    for (auto& f : facts) fb += "- " + f + "\n";
                                    memBg = memBg.empty() ? fb : (memBg + "\n" + fb);
                                }
                            }
                            dice::aitools::ToolExec texec = dice::aitools::enabled(configMgr) ? makeAiTool(msg) : nullptr;
                            // A1：测试台注入 NPC 情绪记忆（只读预览，不更新好感）。
                            nlohmann::json pMood = nlohmann::json::object();
                            if (pNpcHit && pnpc.moodEnabled)
                                pMood = dice::ainpc::getMood(db.getChatStorage(), gkNpc, pnpc.id, msg.senderId);
                            std::string sysOv = pNpcHit ? dice::ainpc::systemPrompt(pnpc, pMood, sn) : std::string();
                            std::string modelOv = pNpcHit ? pnpc.modelId : std::string();
                            // C#85：测试台也走图像识别注入（便于骰主预览）。
                            std::string pcur = dice::aichat::cleanForAi(configMgr, msg.content);
                            if (dice::aivision::enabled(configMgr) && !msg.rawContent.empty()) {
                                std::string vd = dice::aivision::describe(configMgr, msg.rawContent);
                                if (!vd.empty())
                                    pcur += "\n\xef\xbc\x88\xe6\x88\x91\xe5\x8f\x91\xe7\x9a\x84\xe5\x9b\xbe\xe7\x89\x87\xe5\x86\x85\xe5\xae\xb9\xef\xbc\x9a" + vd + "\xef\xbc\x89";
                            }
                            std::string ar = dice::aichat::generate(configMgr, "", sn, pcur, memBg, texec, sysOv, modelOv);
                            if (!ar.empty()) reply = ar;
                        }
                    }
                    // Note: triggered broadcast is NOT applied here — the playground
                    // shouldn't consume a real broadcast push; it only fires on live
                    // group messages (see adapterMgr.onMessage above).
                    if (!reply.empty()) reply = cmdRouter.applySelf(msg, reply);
                    // C#68/C#78：测试台同样走 AI 润色 + 翻译（与 live 一致），方便骰主在
                    // 「指令测试」页直接预览效果；失败/破坏数字回退原文。
                    std::string aiCat = (replySrc == "plugin") ? "plugin"
                                      : (replySrc == "reply")  ? "custom"
                                      : cmdRouter.lastReplyCategory();
                    if (!reply.empty()
                        && dice::aipolish::enabled(configMgr) && dice::aipolish::covers(configMgr, aiCat)) {
                        reply = dice::aipolish::polish(configMgr, msg.content, reply);
                    }
                    if (!reply.empty() && dice::aitrans::enabled(configMgr)) {
                        std::string tgt = cmdRouter.aiLangFor(msg);
                        if (!tgt.empty() && dice::aitrans::covers(configMgr, aiCat))
                            reply = dice::aitrans::translate(configMgr, tgt, reply);
                    }
                    if (!disabled) cmdRouter.recordMessage(msg, reply);
                    cmdRouter.recordPlayerActivity(msg, !reply.empty());
                }
                out["reply"]   = reply;
                out["matched"] = !reply.empty();
                if (!reply.empty()) out["segments"] = cmdRouter.segmentReply(reply);   // 分段预览
            } catch (const std::exception& e) {
                out["error"] = e.what();
            }
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(out.dump());
            cb(resp);
        }, {drogon::Post});

    // ── C#12 可视化生成器：对未保存的规则包 JSON 实时测试一条指令 ──────────
    app.registerHandler("/api/rules/test",
        [&cmdRouter](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            nlohmann::json out;
            try {
                auto body = nlohmann::json::parse(req->getBody());
                std::string content = body.value("content", std::string(""));
                std::string command = body.value("command", std::string(""));
                std::string nick    = body.value("nick", std::string(""));
                std::map<std::string, int> attrs;
                if (body.contains("attrs") && body["attrs"].is_object())
                    for (auto& [k, v] : body["attrs"].items())
                        if (v.is_number()) attrs[k] = v.get<int>();
                auto r = cmdRouter.testRulePackCommand(content, command, attrs, nick);
                out["code"] = 0; out["message"] = "ok";
                out["data"] = { {"matched", r.matched}, {"reply", r.reply}, {"status", r.status} };
            } catch (const std::exception& e) {
                out["code"] = 1; out["message"] = e.what();
            }
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(out.dump());
            cb(resp);
        }, {drogon::Post});

    // ── C#6 Lua mod 管理（列表 / 启停 / 删除 / 重载）──────────────────
    {
        auto jsonResp = [](const nlohmann::json& out) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(out.dump());
            return resp;
        };
        auto luaList = [&luaMod]() {
            nlohmann::json arr = nlohmann::json::array();
            for (auto& m : luaMod.mods()) {
                nlohmann::json help = nlohmann::json::array();
                for (auto& [topic, text] : m.helpdoc) help.push_back(topic);
                // 真实指令触发词（与帮助词条区分）：{trigger, kind} 列表。
                nlohmann::json cmds = nlohmann::json::array();
                for (auto& c : luaMod.commandsOf(m.name))
                    cmds.push_back({{"trigger", c.trigger}, {"kind", c.kind}});
                arr.push_back({{"name", m.name}, {"title", m.title}, {"author", m.author},
                               {"version", m.version}, {"brief", m.brief}, {"enabled", m.enabled},
                               {"replies", m.replies}, {"scripts", m.scripts}, {"helpTopics", help},
                               {"commands", cmds},
                               {"singleFile", m.singleFile}, {"ruleCompat", m.ruleCompat}});
            }
            return arr;
        };
        app.registerHandler("/api/mod/lua",
            [jsonResp, luaList](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                cb(jsonResp({{"code", 0}, {"message", "ok"}, {"data", luaList()}}));
            }, {drogon::Get});
        app.registerHandler("/api/mod/lua/toggle",
            [jsonResp, luaList, &luaMod](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                nlohmann::json out;
                try {
                    auto j = nlohmann::json::parse(req->getBody());
                    std::string name = j.value("name", std::string()); bool en = j.value("enabled", true);
                    if (name.empty()) { cb(jsonResp({{"code", 1}, {"message", "name required"}})); return; }
                    luaMod.setModEnabled(name, en); luaMod.reload();
                    out = {{"code", 0}, {"message", "ok"}, {"data", luaList()}};
                } catch (const std::exception& e) { out = {{"code", 1}, {"message", e.what()}}; }
                cb(jsonResp(out));
            }, {drogon::Post});
        app.registerHandler("/api/mod/lua/delete",
            [jsonResp, luaList, &luaMod](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                nlohmann::json out;
                try {
                    auto j = nlohmann::json::parse(req->getBody());
                    std::string name = j.value("name", std::string());
                    if (name.empty()) { cb(jsonResp({{"code", 1}, {"message", "name required"}})); return; }
                    luaMod.deleteMod(name); luaMod.reload();
                    out = {{"code", 0}, {"message", "ok"}, {"data", luaList()}};
                } catch (const std::exception& e) { out = {{"code", 1}, {"message", e.what()}}; }
                cb(jsonResp(out));
            }, {drogon::Post});
        app.registerHandler("/api/mod/lua/reload",
            [jsonResp, luaList, &luaMod](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                luaMod.reload();
                cb(jsonResp({{"code", 0}, {"message", "ok"}, {"data", luaList()}}));
            }, {drogon::Post});
        // 导入 Lua mod：上传 zip（base64），解压到 data/mod/<模块名> 后重载。
        app.registerHandler("/api/mod/lua/upload",
            [jsonResp, luaList, &luaMod](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                namespace fs = std::filesystem;
                try {
                    auto j = nlohmann::json::parse(req->getBody());
                    std::string filename = j.value("filename", std::string("mod.zip"));
                    std::string content = j.value("content", std::string());
                    // dataURL 前缀（data:...;base64,）剥离
                    if (auto comma = content.find(",base64,"); comma != std::string::npos) content = content.substr(comma + 8);
                    else if (auto c2 = content.find(','); c2 != std::string::npos && content.rfind("data:", 0) == 0) content = content.substr(c2 + 1);
                    std::string bytes = drogon::utils::base64Decode(content);
                    if (bytes.empty()) { cb(jsonResp({{"code", 1}, {"message", "empty/invalid file"}})); return; }
                    fs::create_directories("data/mod");
                    // 单文件 Lua 插件（msg_order）→ data/plugin/；扫描器同时识别两个目录，
                    // 归位让目录不再混乱）。目录型 mod（zip）仍进 data/mod/。
                    if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".lua") {
                        std::string base = filename;
                        if (auto p = base.find_last_of("/\\"); p != std::string::npos) base = base.substr(p + 1);
                        fs::create_directories("data/plugin");
                        { std::ofstream f(fs::path("data/plugin") / base, std::ios::binary); f.write(bytes.data(), (std::streamsize)bytes.size()); }
                        luaMod.reload();
                        cb(jsonResp({{"code", 0}, {"message", "ok"}, {"data", luaList()}}));
                        return;
                    }
                    std::string tag = std::to_string((long long)std::time(nullptr));
                    fs::path zipPath = fs::path("data/mod") / ("_imp_" + tag + ".zip");
                    fs::path tmpDir  = fs::path("data/mod") / ("_imp_" + tag);
                    { std::ofstream f(zipPath, std::ios::binary); f.write(bytes.data(), (std::streamsize)bytes.size()); }
                    fs::create_directories(tmpDir);
                    // 解压：Win10+ 自带 tar(bsdtar 可解 zip)；POSIX 用 unzip。
                    std::error_code ec;
#if defined(_WIN32)
                    std::string cmd = "tar -xf \"" + zipPath.string() + "\" -C \"" + tmpDir.string() + "\"";
#else
                    std::string cmd = "unzip -o -q \"" + zipPath.string() + "\" -d \"" + tmpDir.string() + "\"";
#endif
                    std::system(cmd.c_str());
                    fs::remove(zipPath, ec);
                    // 定位 mod 根：若解压结果是「单个子目录」则它是 mod；否则 zip 内容即 mod，用文件名作名。
                    auto isModDir = [](const fs::path& d) {
                        std::error_code e; return fs::exists(d / "descriptor.json", e)
                            || fs::is_directory(d / "reply", e) || fs::is_directory(d / "script", e);
                    };
                    fs::path src; std::string modName;
                    if (isModDir(tmpDir)) {
                        modName = filename;
                        if (auto dot = modName.rfind('.'); dot != std::string::npos) modName = modName.substr(0, dot);
                        src = tmpDir;
                    } else {
                        int dirs = 0; fs::path only;
                        for (auto& e : fs::directory_iterator(tmpDir, ec)) if (e.is_directory()) { ++dirs; only = e.path(); }
                        if (dirs == 1 && isModDir(only)) { src = only; modName = dnx_u8str(only.filename()); }
                    }
                    if (src.empty() || modName.empty()) {
                        fs::remove_all(tmpDir, ec);
                        cb(jsonResp({{"code", 1}, {"message", "zip 内未找到 Lua mod（需含 descriptor.json 或 reply/ 或 script/）"}}));
                        return;
                    }
                    // basename 防穿越
                    if (auto p = modName.find_last_of("/\\"); p != std::string::npos) modName = modName.substr(p + 1);
                    fs::path dest = fs::path("data/mod") / modName;
                    fs::remove_all(dest, ec);
                    fs::rename(src, dest, ec);
                    if (ec) { fs::copy(src, dest, fs::copy_options::recursive, ec); }
                    fs::remove_all(tmpDir, ec);
                    luaMod.reload();
                    cb(jsonResp({{"code", 0}, {"message", "ok"}, {"data", luaList()}}));
                } catch (const std::exception& e) { cb(jsonResp({{"code", 1}, {"message", e.what()}})); }
            }, {drogon::Post});
    }

    // ── 插件分群启停（C#27 地基）：列出全部插件 + 在某群的启用状态；按群切换 ──
    {
        auto jResp = [](const nlohmann::json& out) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(out.dump());
            return resp;
        };
        app.registerHandler("/api/groups/plugins",
            [jResp, &cmdRouter, &jsMod, &luaMod](const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                std::string platform = req->getParameter("platform");
                std::string group = req->getParameter("group");
                nlohmann::json arr = nlohmann::json::array();
                for (auto& m : luaMod.mods()) {
                    std::string id = "lua:" + m.name;
                    arr.push_back({{"id", id}, {"name", m.title.empty() ? m.name : m.title},
                                   {"kind", "lua"}, {"enabledGlobal", m.enabled},
                                   {"enabledInGroup", cmdRouter.isPluginEnabledInGroup(platform, group, id)}});
                }
                for (auto& p : jsMod.listAll()) {
                    std::string file = p.file;
                    const std::string sfx = ".disabled";
                    if (file.size() > sfx.size() && file.substr(file.size() - sfx.size()) == sfx)
                        file = file.substr(0, file.size() - sfx.size());
                    std::string id = "js:" + file;
                    arr.push_back({{"id", id}, {"name", p.name.empty() ? file : p.name},
                                   {"kind", "js"}, {"enabledGlobal", p.enabled},
                                   {"enabledInGroup", cmdRouter.isPluginEnabledInGroup(platform, group, id)}});
                }
                cb(jResp({{"code", 0}, {"message", "ok"}, {"data", {{"plugins", arr}}}}));
            }, {drogon::Get});
        app.registerHandler("/api/groups/plugins/toggle",
            [jResp, &cmdRouter](const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                try {
                    auto b = nlohmann::json::parse(req->getBody());
                    std::string platform = b.value("platform", std::string());
                    std::string group = b.value("group", std::string());
                    std::string id = b.value("pluginId", std::string());
                    bool enabled = b.value("enabled", true);
                    if (group.empty() || id.empty()) { cb(jResp({{"code", 1}, {"message", "group/pluginId required"}})); return; }
                    cmdRouter.setPluginEnabledInGroup(platform, group, id, enabled);
                    cb(jResp({{"code", 0}, {"message", "ok"}}));
                } catch (const std::exception& e) { cb(jResp({{"code", 1}, {"message", e.what()}})); }
            }, {drogon::Post});
    }

    // ── C#27 规则包 bundle 管理（data/rulepacks/<包>/）：列表 / 上传zip / 启停 / 删除 ──
    {
        namespace fs = std::filesystem;
        auto jResp = [](const nlohmann::json& out) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(out.dump());
            return resp;
        };
        auto reloadPacks = [&luaMod, &jsMod]() {
            // 同一时间可能有上传、启停、删除三个入口。串行整个事务，防止 A 请求
            // 扫描目录时 B 请求已删除目录；插件层另有互斥锁，但规则/帮助注册表也需要同序。
            static std::mutex reloadMutex;
            std::lock_guard<std::mutex> guard(reloadMutex);
            try {
                dice::CommandRouter::reloadRulePacks({"rules", "data/rules"});   // 重载 rules + bundles(内含 loadRulePackBundles)
                dice::CommandRouter::loadHelpDocs();                             // 刷新帮助文档（含包内 helpdoc）
                // C#27：重算规则包附加插件目录并热重载 lua/js（包内插件按群激活 gating）。
                std::vector<std::string> luaDirs, jsDirs;
                dice::CommandRouter::packPluginDirs(luaDirs, jsDirs);
                luaMod.setExtraDirs(luaDirs); luaMod.reload();
                jsMod.setExtraDirs(jsDirs);
                jsMod.reload(jsMod.pluginDir().empty() ? "data/plugins/js" : jsMod.pluginDir());
                dice::CommandRouter::reloadJsGameSystems(jsMod.gameSystemTemplates());   // JS 规则插件属性模板同步
            } catch (const std::exception& e) {
                DICE_LOG_ERROR("Rule-pack hot reload failed without stopping service: {}", e.what());
            } catch (...) {
                DICE_LOG_ERROR("Rule-pack hot reload failed without stopping service: unknown error");
            }
        };
        auto bundleList = []() {
            nlohmann::json arr = nlohmann::json::array();
            std::shared_lock<std::shared_mutex> lk(dice::rulesLock());
            for (auto& b : dice::CommandRouter::rulePackBundles())
                arr.push_back({{"name", b.name}, {"folder", b.folder}, {"version", b.version},
                               {"author", b.author}, {"description", b.description}, {"enabled", b.enabled},
                               {"setKeys", b.setKeys}, {"ruleFiles", b.ruleFiles}, {"cmdCount", b.cmdCount},
                               {"helpdocEntries", b.helpdocEntries}, {"luaMods", b.luaMods}, {"jsPlugins", b.jsPlugins}});
            return arr;
        };
        app.registerHandler("/api/rulepacks",
            [jResp, bundleList](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                cb(jResp({{"code", 0}, {"message", "ok"}, {"data", {{"bundles", bundleList()}}}}));
            }, {drogon::Get});
        app.registerHandler("/api/rulepacks/upload",
            [jResp, bundleList, reloadPacks](const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                namespace fs = std::filesystem;
                try {
                    auto j = nlohmann::json::parse(req->getBody());
                    std::string filename = j.value("filename", std::string("pack.zip"));
                    std::string content = j.value("content", std::string());
                    if (auto comma = content.find(",base64,"); comma != std::string::npos) content = content.substr(comma + 8);
                    else if (auto c2 = content.find(','); c2 != std::string::npos && content.rfind("data:", 0) == 0) content = content.substr(c2 + 1);
                    std::string bytes = drogon::utils::base64Decode(content);
                    if (bytes.empty()) { cb(jResp({{"code", 1}, {"message", "empty/invalid file"}})); return; }
                    fs::create_directories("data/rulepacks");
                    std::string tag = std::to_string((long long)std::time(nullptr));
                    fs::path zipPath = fs::path("data/rulepacks") / ("_imp_" + tag + ".zip");
                    fs::path tmpDir  = fs::path("data/rulepacks") / ("_imp_" + tag);
                    { std::ofstream f(zipPath, std::ios::binary); f.write(bytes.data(), (std::streamsize)bytes.size()); }
                    fs::create_directories(tmpDir);
                    std::error_code ec;
#if defined(_WIN32)
                    std::system(("tar -xf \"" + zipPath.string() + "\" -C \"" + tmpDir.string() + "\"").c_str());
#else
                    std::system(("unzip -o -q \"" + zipPath.string() + "\" -d \"" + tmpDir.string() + "\"").c_str());
#endif
                    fs::remove(zipPath, ec);
                    // 定位包根：含 pack.json 的目录（zip 根，或唯一子目录）。
                    auto isPackDir = [](const fs::path& d) { std::error_code e; return fs::exists(d / "pack.json", e)
                        || fs::is_directory(d / "rules", e) || fs::is_directory(d / "helpdoc", e); };
                    fs::path src; std::string packName;
                    if (isPackDir(tmpDir)) { src = tmpDir; packName = filename;
                        if (auto dot = packName.rfind('.'); dot != std::string::npos) packName = packName.substr(0, dot);
                    } else {
                        int dirs = 0; fs::path only;
                        for (auto& e : fs::directory_iterator(tmpDir, ec)) if (e.is_directory()) { ++dirs; only = e.path(); }
                        if (dirs == 1 && isPackDir(only)) { src = only; packName = dnx_u8str(only.filename()); }
                    }
                    // 优先用 pack.json 里的 name
                    if (!src.empty()) { try { if (fs::exists(src / "pack.json", ec)) {
                        std::ifstream pf(src / "pack.json", std::ios::binary); auto pj = nlohmann::json::parse(pf, nullptr, false, true);
                        if (pj.is_object() && pj.contains("name") && pj["name"].is_string()) packName = pj["name"].get<std::string>();
                    } } catch (...) {} }
                    if (src.empty() || packName.empty()) { fs::remove_all(tmpDir, ec);
                        cb(jResp({{"code", 1}, {"message", "zip 内未找到规则包（需含 pack.json 或 rules/ 或 helpdoc/）"}})); return; }
                    if (auto p = packName.find_last_of("/\\"); p != std::string::npos) packName = packName.substr(p + 1);
                    // ⚠️ 必须用同一个 std::string 取 begin/end —— 之前对两个临时 string 分别取
                    // begin()/end()，迭代器属于不同对象，算出的长度是天文数字→std::length_error
                    // "string too long"，导致任何规则包上传都失败。
                    const std::string destPath = "data/rulepacks/" + packName;
                    fs::path dest = u8path(destPath);
                    fs::remove_all(dest, ec);
                    fs::rename(src, dest, ec);
                    if (ec) { fs::copy(src, dest, fs::copy_options::recursive, ec); }
                    fs::remove_all(tmpDir, ec);
                    reloadPacks();
                    cb(jResp({{"code", 0}, {"message", "ok"}, {"data", {{"bundles", bundleList()}}}}));
                } catch (const std::exception& e) { cb(jResp({{"code", 1}, {"message", e.what()}})); }
            }, {drogon::Post});
        app.registerHandler("/api/rulepacks/toggle",
            [jResp, bundleList, reloadPacks](const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                namespace fs = std::filesystem;
                try {
                    auto j = nlohmann::json::parse(req->getBody());
                    std::string folder = j.value("folder", std::string());
                    bool enable = j.value("enabled", true);
                    if (folder.empty() || folder.find("..") != std::string::npos
                        || folder.find('/') != std::string::npos || folder.find('\\') != std::string::npos) {
                        cb(jResp({{"code", 1}, {"message", "bad folder"}})); return; }
                    std::string base = folder; const std::string sfx = ".disabled";
                    if (base.size() > sfx.size() && base.substr(base.size() - sfx.size()) == sfx) base = base.substr(0, base.size() - sfx.size());
                    std::error_code ec;
                    auto u8p = [](const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); };  // 按 UTF-8 构造，避开 Windows narrow 误解中文
                    fs::path on = u8p("data/rulepacks/" + base), off = u8p("data/rulepacks/" + base + ".disabled");
                    fs::path from = enable ? off : on, to = enable ? on : off;
                    if (fs::exists(from, ec)) fs::rename(from, to, ec);
                    reloadPacks();
                    cb(jResp({{"code", 0}, {"message", "ok"}, {"data", {{"bundles", bundleList()}}}}));
                } catch (const std::exception& e) { cb(jResp({{"code", 1}, {"message", e.what()}})); }
            }, {drogon::Post});
        app.registerHandler("/api/rulepacks/delete",
            [jResp, bundleList, reloadPacks](const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                namespace fs = std::filesystem;
                try {
                    auto j = nlohmann::json::parse(req->getBody());
                    std::string folder = j.value("folder", std::string());
                    if (folder.empty() || folder.find("..") != std::string::npos
                        || folder.find('/') != std::string::npos || folder.find('\\') != std::string::npos) {
                        cb(jResp({{"code", 1}, {"message", "bad folder"}})); return; }
                    std::error_code ec;
                    auto u8p = [](const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); };
                    fs::remove_all(u8p("data/rulepacks/" + folder), ec);
                    reloadPacks();
                    cb(jResp({{"code", 0}, {"message", "ok"}, {"data", {{"bundles", bundleList()}}}}));
                } catch (const std::exception& e) { cb(jResp({{"code", 1}, {"message", e.what()}})); }
            }, {drogon::Post});
    }

    // ── Deck file upload + hot reload ─────────────────────────
    app.registerHandler("/api/decks/upload",
        [&cardDeck](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            nlohmann::json out;
            try {
                auto j = nlohmann::json::parse(req->getBody());
                std::string name = j.value("filename", "deck.json");
                std::string content = j.value("content", "");
                if (content.empty()) {
                    out["code"] = 1; out["message"] = "文件内容为空";
                } else if (name.size() < 5 || name.find(".json") == std::string::npos) {
                    out["code"] = 1; out["message"] = "仅支持 .json 格式的牌堆文件";
                } else {
                    auto pos = name.find_last_of("/\\");
                    if (pos != std::string::npos) name = name.substr(pos + 1);
                    // User uploads go to data/decks (upgrade-safe), not bundled decks/.
                    std::filesystem::create_directories("data/decks");
                    std::ofstream f(u8path("data/decks/" + name), std::ios::binary);
                    f << content;
                    f.close();
                    int loaded = cardDeck.loadDir("data/decks");
                    out["code"] = 0; out["message"] = "ok";
                    out["data"] = {{"filename", name}, {"total_decks", (int)cardDeck.deckCount()}, {"loaded", loaded}};
                    DICE_LOG_INFO("Deck uploaded: '{}', {} decks total", name, cardDeck.deckCount());
                }
            } catch (const std::exception& e) {
                out["code"] = 1; out["message"] = e.what();
            }
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(out.dump());
            cb(resp);
        }, {drogon::Post});

    // ── Image asset upload + serve (for 发图 in dice text / replies) ──
    // POST /api/assets/upload  {filename, data:"<base64 or dataURL>"} → {url, code, name}
    app.registerHandler("/api/assets/upload",
        [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            nlohmann::json out;
            try {
                auto j = nlohmann::json::parse(req->getBody());
                std::string fn = j.value("filename", std::string("image.png"));
                std::string data = j.value("data", std::string(""));
                if (data.rfind("data:", 0) == 0) { auto c = data.find(','); if (c != std::string::npos) data = data.substr(c + 1); }
                std::string bytes = drogon::utils::base64Decode(data);
                if (bytes.empty()) { out = {{"code",1},{"message","空文件或无效 base64"}}; }
                else {
                    std::string ext = "png";
                    if (auto d = fn.find_last_of('.'); d != std::string::npos) ext = fn.substr(d + 1);
                    // sanitize ext to a short alnum token
                    std::string safeExt; for (char c : ext) if (std::isalnum((unsigned char)c) && safeExt.size() < 5) safeExt += (char)std::tolower((unsigned char)c);
                    if (safeExt.empty()) safeExt = "png";
                    std::string name = std::to_string((long long)std::time(nullptr)) + "_" +
                                       drogon::utils::genRandomString(6) + "." + safeExt;
                    std::filesystem::create_directories("data/assets");
                    { std::ofstream f("data/assets/" + name, std::ios::binary); f.write(bytes.data(), (std::streamsize)bytes.size()); }
                    // C#56/57：不再把访问时的 host（往往是 localhost，跨设备失效）烧进
                    // 链接。回复里存平台中立码 [img,file=<本地路径>]，发送时按「图片发送
                    // 方式」配置转换；url 返回相对路径，仅供 WebUI 预览。
                    out = {{"code",0},{"message","ok"},{"data", {{"url","/api/assets/" + name},{"name",name},
                           {"code","[img,file=data/assets/" + name + "]"}}}};
                    DICE_LOG_INFO("asset uploaded: {} ({} bytes)", name, bytes.size());
                }
            } catch (const std::exception& e) { out = {{"code",1},{"message", e.what()}}; }
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(out.dump());
            cb(resp);
        }, {drogon::Post});

    // GET /api/assets/{name} — serve an uploaded image.
    app.registerHandler("/api/assets/{1}",
        [](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& cb,
           const std::string& name) {
            // Reject path traversal.
            if (name.find("..") != std::string::npos || name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
                auto r = drogon::HttpResponse::newHttpResponse(); r->setStatusCode(drogon::k400BadRequest); cb(r); return;
            }
            std::string path = "data/assets/" + name;
            std::ifstream f(path, std::ios::binary);
            if (!f) { auto r = drogon::HttpResponse::newHttpResponse(); r->setStatusCode(drogon::k404NotFound); cb(r); return; }
            std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::string ext; if (auto d = name.find_last_of('.'); d != std::string::npos) ext = name.substr(d + 1);
            std::string mime = ext == "jpg" || ext == "jpeg" ? "image/jpeg" : ext == "gif" ? "image/gif"
                              : ext == "webp" ? "image/webp" : ext == "bmp" ? "image/bmp" : "image/png";
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeString(mime);
            resp->setBody(std::move(body));
            cb(resp);
        }, {drogon::Get});

    // ── Deck file: read content + edit ────────────────────────
    // GET  /api/decks/file?name=xxx  → {content, meta: {version, author, date}, entries: [...]}
    // PUT  /api/decks/file           → {filename, content} → save + reload
    auto deckFileHandler = [&cardDeck](const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        nlohmann::json out;
        try {
            if (req->method() == drogon::Get) {
                auto n = req->getParameter("name");
                if (n.empty()) { out["code"] = 1; out["message"] = "缺少 name 参数"; }
                else {
                    // Prefer the user copy in data/decks, fall back to bundled decks/.
                    std::filesystem::path rp = u8path("data/decks/" + n);
                    if (!std::filesystem::exists(rp)) rp = u8path("decks/" + n);
                    std::ifstream f(rp);
                    if (!f) { out["code"] = 1; out["message"] = "文件不存在"; }
                    else {
                        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                        nlohmann::json meta;
                        nlohmann::json entries = nlohmann::json::array();
                        try {
                            auto j = nlohmann::json::parse(content);
                            if (j.is_object()) {
                                if (j.contains("_meta") && j["_meta"].is_object()) meta = j["_meta"];
                                for (auto& [k, v] : j.items())
                                    if (k != "_meta" && v.is_array()) entries.push_back(k);
                            }
                        } catch (...) {}
                        out["code"] = 0; out["message"] = "ok";
                        out["data"] = {{"content", content}, {"meta", meta}, {"entries", entries}};
                    }
                }
            } else if (req->method() == drogon::Put) {
                auto j = nlohmann::json::parse(req->getBody());
                std::string name = j.value("filename", "");
                std::string content = j.value("content", "");
                if (name.empty()) { out["code"] = 1; out["message"] = "缺少 filename"; }
                else {
                    auto pos = name.find_last_of("/\\");
                    if (pos != std::string::npos) name = name.substr(pos + 1);
                    // Edits are saved to data/decks (a user override of any bundled deck).
                    std::filesystem::create_directories("data/decks");
                    std::ofstream f(u8path("data/decks/" + name), std::ios::binary);
                    f << content;
                    f.close();
                    cardDeck.loadDir("data/decks");
                    out["code"] = 0; out["message"] = "ok";
                    out["data"] = {{"filename", name}, {"total_decks", (int)cardDeck.deckCount()}};
                }
            }
        } catch (const std::exception& e) { out["code"] = 1; out["message"] = e.what(); }
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        resp->setBody(out.dump());
        cb(resp);
    };
    app.registerHandler("/api/decks/file", deckFileHandler, {drogon::Get, drogon::Put});

    // ── Deck file: delete from disk ───────────────────────────
    app.registerHandler("/api/decks/file/{1}", [&cardDeck](const drogon::HttpRequestPtr&,
        std::function<void(const drogon::HttpResponsePtr&)>&& cb, const std::string& name) {
        nlohmann::json out;
        try {
            // User decks live in data/decks; fall back to bundled decks/ if absent.
            std::filesystem::path path = u8path("data/decks/" + name);
            if (!std::filesystem::exists(path)) path = u8path("decks/" + name);
            std::error_code rmEc;
            if (std::filesystem::remove(path, rmEc) && !rmEc) {
                cardDeck.reload({"decks", "data/decks"});   // 全量重扫，删除的牌堆即时消失
                out["code"] = 0; out["message"] = "ok";
            } else {
                out["code"] = 1; out["message"] = "删除失败";
            }
        } catch (const std::exception& e) { out["code"] = 1; out["message"] = e.what(); }
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        resp->setBody(out.dump());
        cb(resp);
    }, {drogon::Delete});

    // ── Deck reload: re-scan decks/ + data/decks/ from disk (WebUI「重载」) ──
    // 修复：手动把 json 放进文件夹后点重载无效——之前重载只刷新列表、不重扫文件。
    app.registerHandler("/api/decks/reload", [&cardDeck](const drogon::HttpRequestPtr&,
        std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        cardDeck.reload({"decks", "data/decks"});
        nlohmann::json out{{"code", 0}, {"message", "ok"},
                           {"data", {{"total_decks", (int)cardDeck.deckCount()}}}};
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        resp->setBody(out.dump());
        cb(resp);
    }, {drogon::Post});

    // ── 开机自启动（Windows，写 HKCU\...\Run）GET/PUT /api/system/autostart ──
    app.registerHandler("/api/system/autostart", [](const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        nlohmann::json out{{"code", 0}, {"message", "ok"}};
        try {
            if (req->method() == drogon::Put) {
                auto j = nlohmann::json::parse(req->body());
                dice::setAutostart(j.value("enabled", false));
            }
            out["data"] = {{"enabled", dice::isAutostartEnabled()}};
        } catch (const std::exception& e) { out["code"] = 1; out["message"] = e.what(); }
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        resp->setBody(out.dump());
        cb(resp);
    }, {drogon::Get, drogon::Put});

    DICE_LOG_INFO("Test API ready → web admin \344\276\247\350\276\271\346\240\217\343\200\214\346\265\213\350\257\225\345\217\260\343\200\215");

    // Start adapters after the event loop is running (WebSocket needs an active
    // loop). Adapters were already registered above; start each exactly ONCE here
    // (double-starting reverse-WS recreates its TcpServer and crashes trantor).
    app.getLoop()->runAfter(0.5, [&adapterMgr]() {
        adapterMgr.startAll();
    });

    // ── C#44：聊天记录保留期清理（启动后 1 分钟 + 之后每 6 小时）──
    // 删除 chat.db 里早于 chat/retention_days（默认 7 天，0=不清理）的消息。
    {
        auto chatCleanup = [&db, &configMgr]() {
            int days = configMgr.get<int>("chat/retention_days", 7);
            if (days <= 0) return;
            auto* cst = db.getChatStorage(); if (!cst) return;
            try {
                namespace orm = sqlite_orm;
                int64_t cutoff = static_cast<int64_t>(std::time(nullptr)) - (int64_t)days * 86400;
                cst->remove_all<dice::ChatMsgRow>(orm::where(orm::c(&dice::ChatMsgRow::time) < cutoff));
                dice::chatimg::pruneOld(cutoff);   // C#65：同步清理超期的缓存图片
            } catch (...) {}
        };
        app.getLoop()->runAfter(60.0, chatCleanup);
        app.getLoop()->runEvery(21600.0, chatCleanup);
    }

    // ── C#52：自动清理好友（N 天未在任何位置触发指令 → 删除好友）──
    // dice/friend_clean_days（0=关闭）。豁免：骰主/白名单(信任)用户/trustLevel>0。
    // 无玩家档案的好友跳过（无法判断，宁可不删）；lastCmdAt 为空回退 createdAt。
    {
        auto parseIsoUtc = [](const std::string& s) -> int64_t {
            std::tm tm{}; int y, mo, d, h, mi, se;
            if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
            tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
            tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = se;
#if defined(_WIN32)
            return static_cast<int64_t>(_mkgmtime(&tm));
#else
            return static_cast<int64_t>(timegm(&tm));
#endif
        };
        auto friendCleanup = [&db, &configMgr, &adapterMgr, &cmdRouter, parseIsoUtc]() {
            int days = configMgr.get<int>("dice/friend_clean_days", 0);
            if (days <= 0) return;
            auto* st = db.getStorage(); if (!st) return;
            int64_t cutoff = static_cast<int64_t>(std::time(nullptr)) - (int64_t)days * 86400;
            namespace orm = sqlite_orm;
            for (auto& a : adapterMgr.allAdapters()) {
                if (!a->isConnected()) continue;
                for (const auto& uid : a->getFriendList()) {
                    if (uid.empty() || uid == a->getLoginId()) continue;
                    if (cmdRouter.isMasterUser(a->platform(), uid)) continue;   // 骰主豁免
                    if (cmdRouter.isUserWhitelisted(uid)) continue;             // 白名单(信任)豁免
                    try {
                        auto rows = st->get_all<dice::PlayerProfileRow>(orm::where(
                            orm::c(&dice::PlayerProfileRow::platform) == a->platform() and
                            orm::c(&dice::PlayerProfileRow::userId) == uid));
                        if (rows.empty()) continue;                             // 无档案 → 不判断
                        const auto& p = rows.front();
                        if (p.trustLevel > 0) continue;                         // 有信任等级豁免
                        std::string lastIso = p.lastCmdAt.empty() ? p.createdAt : p.lastCmdAt;
                        int64_t last = parseIsoUtc(lastIso);
                        if (last <= 0 || last >= cutoff) continue;
                        a->deleteFriend(uid);
                        DICE_LOG_INFO("C#52 自动清理好友：{}（{} 天无指令，最后 {}）", uid, days, lastIso);
                    } catch (...) {}
                }
            }
        };
        app.getLoop()->runAfter(300.0, friendCleanup);     // 启动 5 分钟后（等好友列表同步）
        app.getLoop()->runEvery(43200.0, friendCleanup);   // 之后每 12 小时
    }

    // ── 调度循环 (#48 定时任务 / #47 不活跃自动退群)，每 30s 一跳 ──
    app.getLoop()->runEvery(30.0, [&db, &cmdRouter, &i18n, &localeResolver, &configMgr]() {
        auto* st = db.getStorage(); if (!st) return;
        std::time_t now = std::time(nullptr); std::tm lt{};
#if defined(_WIN32)
        localtime_s(&lt, &now);
#else
        lt = *std::localtime(&now);
#endif
        char hm[8], ymd[16];
        std::strftime(hm, sizeof(hm), "%H:%M", &lt);
        std::strftime(ymd, sizeof(ymd), "%Y-%m-%d", &lt);
        std::string curHM = hm, curYMD = ymd; int wday = lt.tm_wday;   // 0=周日

        // 定时任务：到点且当天未触发则发送。
        try {
            for (auto& tk : st->get_all<dice::ScheduledTaskRow>()) {
                if (!tk.enabled || tk.cronTime != curHM || tk.lastRun == curYMD) continue;
                if (!tk.days.empty()) {
                    bool ok = false; std::stringstream ss(tk.days); std::string d;
                    while (std::getline(ss, d, ',')) { try { if (!d.empty() && std::stoi(d) == wday) { ok = true; break; } } catch (...) {} }
                    if (!ok) continue;
                }
                tk.lastRun = curYMD; st->update(tk);   // 当天已评估（防 30s 重复触发），无论条件是否满足
                // 因果条件（如 inactive>=7：本群 ≥7 天无指令）不满足 → 本次跳过。
                if (!cmdRouter.evalScheduledCondition(tk.condition, tk.platform, tk.targetType, tk.targetId)) continue;
                if (tk.action == "leave") {
                    cmdRouter.leaveGroupWith(tk.platform, tk.targetId, tk.content);   // content=告别语（内部已通知骰主）
                    DICE_LOG_INFO("scheduled leave: {} -> group {}", tk.name, tk.targetId);
                } else {
                    cmdRouter.sendScheduled(tk.platform, tk.targetType, tk.targetId, tk.content);
                    DICE_LOG_INFO("scheduled task fired: {} -> {}:{}", tk.name, tk.platform, tk.targetId);
                    // B：定时任务触发通知骰主。
                    cmdRouter.notifyMasters(dice::notice::kImportant,
                        "\xe5\xae\x9a\xe6\x97\xb6\xe4\xbb\xbb\xe5\x8a\xa1\xe3\x80\x8c" + tk.name + "\xe3\x80\x8d\xe5\xb7\xb2\xe6\x89\xa7\xe8\xa1\x8c \xe2\x86\x92 " + tk.targetId, "schedule");
                }
            }
        } catch (...) {}

        // 不活跃自动退群：每天 04:00 检查一次（config dice/inactive_group_line 天）。
        if (curHM == "04:00") {
            int days = 0;
            try {
                nlohmann::json all = configMgr.getAll();
                if (all.contains("dice") && all["dice"].contains("inactive_group_line"))
                    days = all["dice"]["inactive_group_line"].get<int>();
            } catch (...) {}
            if (days > 0) {
                for (auto& [plat, gid] : cmdRouter.inactiveGroups(days)) {
                    dice::Message lm; lm.platform = plat; lm.targetId = gid; lm.type = dice::MessageType::kGroup;
                    cmdRouter.leaveGroupWith(plat, gid,
                        i18n.tr(localeResolver.resolve(lm), "event.leave_unused", {{"day", std::to_string(days)}}));
                    DICE_LOG_INFO("inactive auto-leave: group {} (> {} days)", gid, days);
                }
            }
        }
    });

    // System tray icon (Windows): 打开应用目录 / 显示·隐藏控制台 / 打开网页面板 / 退出。
    // C#13：默认启动即最小化到托盘（隐藏控制台+弹气泡+禁用其X）；config dice/console_start_hidden=false 可保留旧行为（启动就显示控制台）。
    bool startHidden = configMgr.get<bool>("dice/console_start_hidden", true);
    dice::startSystemTray(static_cast<uint16_t>(port), [] {
        g_running.store(false);
        drogon::app().quit();   // unblock app.run() so we shut down cleanly
    }, startHidden);

    DICE_LOG_INFO("Server starting — press Ctrl+C to stop");
    dice::crashdiag::setPhase("running");
    app.run();

    // ── 10. Graceful shutdown ────────────────────────────────
    DICE_LOG_INFO("Shutting down...");

    if (hotReload) {
        hotReload->stop();
        delete hotReload;
        hotReload = nullptr;
    }

    db.close();
    DICE_LOG_INFO("Dice!Next stopped. Goodbye!");

    return 0;
}

// ── 崩溃诊断：main 整体包裹——未捕获 C++ 异常在此直接拿到 what() 落盘
// （SEH 兜底只有异常码 0xE06D7363，这里可读性最好）。
int main(int argc, char* argv[]) {
    try {
        return realMain(argc, argv);
    } catch (const std::exception& e) {
        dice::crashdiag::reportFatal("uncaught std::exception in main", e.what());
        return 3;
    } catch (...) {
        dice::crashdiag::reportFatal("uncaught non-std exception in main", "?");
        return 3;
    }
}
