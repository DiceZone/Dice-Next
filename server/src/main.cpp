#include "common/logger.h"
#include "common/types.h"
#include "common/errors.h"
#include "common/utils.h"
#include "common/hot_reload.h"
#include "config/config_manager.h"
#include "config/scoped_settings.h"
#include "storage/database.h"
#include "storage/migration.h"
#include "storage/legacy_importer.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include "service/api_service.h"
#include "service/web_auth.h"          // WebUI 登录鉴权
#include "service/chat_image.h"        // 模拟聊天图片本地化
#include "service/image_host.h"        // 图床（QQ 官方富媒体需要公网 URL）
#include "service/ai_polish.h"         // AI 回复润色
#include "service/ai_translate.h"      // AI 回复翻译（.lang 自定义语言）
#include "service/ai_chat.h"           // 智能化阶段A：AI 对话回复
#include "service/ai_memory.h"         // 智能化阶段B/C：群聊滚动摘要 + 长期事实记忆
#include "service/ai_tools.h"          // 智能化阶段D：AI 工具调用（掷骰/抽牌/查卡）
#include "service/ai_npc.h"            // 智能化阶段E：NPC 扮演
#include "service/ai_vision.h"         // 多模态图像识别
#include "service/ai_worker.h"         // AI 后台线程（AI 调用不再阻塞消息管线）
#include "service/heart_service.h"     // 心跳上报（heart.dice.zone）
#include "service/cloudban_service.h"  // 云黑名单同步（cloudban.dice.zone）
#include "service/backup_service.h"
#include "platform/instance_guard.h"   // 必须在 tray_win.h(<windows.h>) 之前：先引 winsock2.h
#include "platform/autostart_win.h"    // Windows 注册表开机自启；其他平台提供 no-op 接口。
#include "platform/tray_win.h"
#include "platform/crash_diag_win.h"
#include "adapter/adapter_interface.h"
#include "adapter/adapter_manager.h"
#include "adapter/onebot_v11_adapter.h"
#include "adapter/qq_official_adapter.h"
#include "core/command_router.h"
#include "core/friend_approval_policy.h"
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
#include <unordered_map>
#include <unordered_set>

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
/// (Windows 内部转 wide)。用于一切「按用户给的名字开文件」的场景（文件名编码）。
inline std::filesystem::path u8path(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

// 路径 → UTF-8 窄串：Windows 上 path::string() 走 ANSI 代码页，文件名含 GBK 无映射
// 字符（emoji 等）会抛 system_error（Server 2012/2016 启动崩溃根因）。u8string 永不抛。
static inline std::string dnx_u8str(const std::filesystem::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// Exit code understood by dice-next.exe.  The manager restarts the core only
// after its process and port have fully exited.
constexpr int kManagedRestartExitCode = 42;
std::atomic<int> g_requestedExitCode{0};

/// Request a restart.  A package managed by dice-next.exe lets that manager
/// perform the restart; direct legacy launches retain the old self-relaunch.
inline void relaunchSelf() {
#if defined(_WIN32) || defined(_WIN64)
    wchar_t managed[8]{};
    if (GetEnvironmentVariableW(L"DICENEXT_MANAGED", managed, static_cast<DWORD>(std::size(managed))) > 0 &&
        std::wstring_view(managed) == L"1") {
        g_requestedExitCode.store(kManagedRestartExitCode);
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            drogon::app().quit();
        }).detach();
        return;
    }
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
                      const dice::Database& /*db*/,
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

    const std::string apiKeyMask = apiKey.empty() ? std::string("not set") : apiKey.substr(0, 4) + "****";
    std::cout << "  │  API Key     : " << apiKeyMask;
    for (int i = 0; i < 22 - static_cast<int>(apiKeyMask.size()); ++i)
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
    // A restore uploaded from the WebUI is only applied during startup, before
    // config/database/plugin files are opened.  This is the safe point to
    // replace an entire data directory.
    {
        std::string restoreNotice;
        if (!dice::backup::applyPendingRestore(restoreNotice)) {
            DICE_LOG_ERROR("{}", restoreNotice);
            return 1;
        }
        if (!restoreNotice.empty()) DICE_LOG_WARN("{}", restoreNotice);
    }

    // ── 2. Register signal handlers ──────────────────────────
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // ── 3. Load configuration ────────────────────────────────
    dice::crashdiag::setPhase("config");
    std::string configPath = "config";
    if (argc > 1) {
        configPath = argv[1];
    }

    dice::ConfigManager configMgr(configPath);
    const bool configLoaded = configMgr.load();
    const bool obsoleteDefaultConfigDiscarded = configMgr.discardedObsoleteDefaultConfig();
    // 统一时区：server/timezone_minutes（相对 UTC 的分钟偏移，东为正；
    // 缺省/INT_MIN = 跟随系统本地时区）。所有面向用户的展示/上传时间都走它。
    dice::utils::setTimezoneOffset(configMgr.get<int>(
        "server/timezone_minutes", (std::numeric_limits<int>::min)()));
    bool adaptersNeedExport = !configLoaded || configMgr.createdOnLoad();
    const std::string databasePathForRecovery = configLoaded
        ? configMgr.get<std::string>("server/db_path", "./data/dice.db")
        : configMgr.recoveryDatabasePath("./data/dice.db");
    if (!configLoaded)
        DICE_LOG_ERROR("Configuration files are invalid; database snapshot recovery will be attempted after opening the database");
    else
        DICE_LOG_INFO("Configuration loaded from '{}'", configPath);

    // ── 3.4. 单实例守卫（同一套数据只允许一个进程）────────────
    {
        namespace fs = std::filesystem;
        const std::string& dbp = databasePathForRecovery;
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
    if (configLoaded) {
        int wantPort = configMgr.get<int>("server/port", 18088);
        uint16_t got = dice::findAvailablePort(static_cast<uint16_t>(wantPort));
        if (got != wantPort) {
            DICE_LOG_WARN("WebUI 端口 {} 已被占用，自动改用 {} 并写入配置文件。", wantPort, got);
            configMgr.set<int>("server/port", static_cast<int>(got));
            configMgr.save();
        }
    }

    // ── 4. Open database ─────────────────────────────────────
    dice::crashdiag::setPhase("database");
    const std::string dbPath = databasePathForRecovery;
    dice::Database db;
    if (!db.open(dbPath)) {
        DICE_LOG_ERROR("Failed to open database at '{}'", dbPath);
        return 1;
    }
    if (auto* configStorage = db.getStorage()) {
        configMgr.setSnapshotWriter([configStorage](const dice::json& snapshot) {
            const std::string key = "system_config_snapshot";
            auto rows = configStorage->get_all<dice::DiceConfigRow>(
                sqlite_orm::where(sqlite_orm::c(&dice::DiceConfigRow::key) == key));
            if (rows.empty()) configStorage->insert(dice::DiceConfigRow{0, key, snapshot.dump()});
            else { rows.front().value = snapshot.dump(); configStorage->update(rows.front()); }
        });
        if (!configLoaded) {
            try {
                const auto rows = configStorage->get_all<dice::DiceConfigRow>(
                    sqlite_orm::where(sqlite_orm::c(&dice::DiceConfigRow::key) == "system_config_snapshot"));
                if (!rows.empty() && configMgr.restoreSnapshot(dice::json::parse(rows.front().value)) && configMgr.save()) {
                    adaptersNeedExport = false;
                    DICE_LOG_WARN("Invalid configuration files restored from the last database snapshot");
                } else if (obsoleteDefaultConfigDiscarded) {
                    // Never let the historical default_config.json migration
                    // overwrite a populated database with factory defaults.
                    // The file was discarded because it is not authoritative;
                    // without a durable snapshot there is no safe recovery.
                    DICE_LOG_ERROR("Obsolete default_config.json was discarded, but no valid database configuration snapshot exists; refusing to overwrite current database settings");
                    return 1;
                } else {
                    // A pre-snapshot installation still has adapter rows in the
                    // database. Start from safe defaults once, then export those
                    // rows into adapters.json below.
                    if (!configMgr.save()) return 1;
                    DICE_LOG_WARN("No database configuration snapshot found; created safe split defaults");
                }
            } catch (const std::exception& e) {
                DICE_LOG_ERROR("Database configuration snapshot recovery failed: {}", e.what());
                return 1;
            }
        } else {
            configMgr.save();
        }
    }

    // ── 4.5. Initialize i18n from the validated/recovered config ──
    dice::IAdapter::setCardMessageMode(
        configMgr.get<std::string>("dice/message_format", "traditional") == "card");
    std::string i18nDir = configMgr.get<std::string>("i18n/resource_dir", "i18n");
    std::string defaultLocaleCode = configMgr.get<std::string>("i18n/default_locale", "zh-Hans");
    dice::I18n i18n(i18nDir, dice::localeFromString(defaultLocaleCode));
    if (!i18n.load()) {
        DICE_LOG_WARN("i18n: no translation bundles loaded — replies may show raw keys");
    } else {
        DICE_LOG_INFO("i18n: ready (default locale '{}', {} bundle(s) loaded)",
            defaultLocaleCode, i18n.availableLocales().size());
    }
    // Persona system initialized later (after DB open + migration)

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
        hotReload->start("config", hotReloadDebounce, [&configMgr](const std::string&) {
            configMgr.reload();  // writing_ guard inside will skip self-saves
            dice::utils::setTimezoneOffset(configMgr.get<int>(
                "server/timezone_minutes", (std::numeric_limits<int>::min)()));
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
        int bundleN = dice::CommandRouter::loadRulePackBundles();   // data/rulepacks/<包>/
        int aliasN = dice::CommandRouter::loadModelTemplates();     // 规则 mod 的 model/*.xml 属性别名 → .st/.ra
        DICE_LOG_INFO("规则包：加载 {} 个规则 + {} 个规则包(bundle) + {} 个属性模板别名", packN, bundleN, aliasN);
    }
    dice::CommandRouter::loadHelpFiles();   // 加载 data/help/*.md 帮助文档
    dice::CommandRouter::loadHelpDocs();    // 加载 data/helpdoc/**/*.json 结构化帮助文档（海豹兼容+随包速查）
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

    // Causal rule engine — managers instantiated here, wired into the
    // message pipeline below (checked before regular ReplyManager).
    dice::CooldownManager cooldownMgr;
    dice::CounterStore counterStore(db);
    dice::CausalRuleManager causalMgr(db, configMgr, cooldownMgr, counterStore);
    causalMgr.loadRules();

    // Persona switching system
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
        return cmdRouter.jsHttpFetch(method, url, headers, body, status, true);   // T8: JS fetch 默认放行（可配 js_fetch_strict 恢复拦截）
    });
    // 插件更新检测/下载（面板管理操作，免外置API开关，仅 SSRF 防护）。
    jsMod.setUpdateFetch([&cmdRouter](const std::string& url, int& status) {
        return cmdRouter.fetchPluginUrl(url, status);
    });
    // 真计时器：把 JS setTimeout/setInterval 排到 drogon 事件循环。
    jsMod.setScheduler([](double sec, std::function<void()> cb) {
        drogon::app().getLoop()->runAfter(sec, std::move(cb));
    });
    // 插件分群启停（地基）：JS 指令派发前问「该群是否启用此插件（按源文件）」。
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
    // seal.favor 与内置 .favor 共用玩家档案存储及成长规则。
    jsMod.setFavorBridge(
        [&cmdRouter](const std::string& p, const std::string& u) { return cmdRouter.pluginFavorGet(p, u); },
        [&cmdRouter](const std::string& p, const std::string& u, int v) { return cmdRouter.pluginFavorSet(p, u, v); },
        [&cmdRouter](const std::string& p, const std::string& u, int d) { return cmdRouter.pluginFavorAdd(p, u, d); },
        [&cmdRouter](const std::string& p, const std::string& u) { return cmdRouter.pluginFavorGrow(p, u); });
    // 群名片解析器 —— JS 规则包读 msg.sender.card / ctx.player.name（显示名）。
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
    // 规则包 data/rulepacks/<包>/js 附加加载（按群激活 gating）。
    { std::vector<std::string> luaDirs, jsDirs; dice::CommandRouter::packPluginDirs(luaDirs, jsDirs); jsMod.setExtraDirs(jsDirs); }
    // 插件目录优先 data/plugins/js（数据迁移），旧部署回退到根 plugins/js。
    dice::crashdiag::setPhase("js-plugins");
    jsMod.loadDir(std::filesystem::exists("data/plugins/js") ? "data/plugins/js" : "plugins/js");
    // JS 规则插件(seal.gameSystem)的属性模板：把模板原文交给 CommandRouter 解析（别名/衍生 → .st/.ra）。
    dice::CommandRouter::jsGameSystemTemplates() = jsMod.gameSystemTemplates();
    dice::CommandRouter::loadJsGameSystems();

    // 初始化 Lua 插件子系统：引擎、模块发现和核心 API。
    dice::LuaPluginManager luaMod;
    dice::LuaPluginManager::setCpathStrict(configMgr.get<bool>("dice/lua_cpath_strict", false));   // 兼容优先默认关
    luaMod.init();
    luaMod.setDeckDraw([&cardDeck](const std::string& name) -> std::string {
        return cardDeck.has(name) ? cardDeck.drawFromDeck(name).value_or("") : std::string();
    });
    luaMod.setSelfName(configMgr.get<std::string>("dice/self_name", std::string("\xe9\xaa\xb0\xe5\xa8\x98")));
    luaMod.setBotId(configMgr.get<std::string>("dice/self_qq", std::string()));   // getDiceQQ()
    jsMod.setSelfInfo(configMgr.get<std::string>("dice/self_qq", std::string()),
                      configMgr.get<std::string>("dice/self_name", std::string()));   // ctx.endPoint.userId/nickname
    // Lua getFavor/setFavor/addFavor/growFavor 直达内置好感度系统。
    luaMod.setFavorBridge(
        [&cmdRouter](const std::string& p, const std::string& u) { return cmdRouter.pluginFavorGet(p, u); },
        [&cmdRouter](const std::string& p, const std::string& u, int v) { return cmdRouter.pluginFavorSet(p, u, v); },
        [&cmdRouter](const std::string& p, const std::string& u, int d) { return cmdRouter.pluginFavorAdd(p, u, d); },
        [&cmdRouter](const std::string& p, const std::string& u) { return cmdRouter.pluginFavorGrow(p, u); });
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
    // .game 团务与 Lua msg.game/GameTable 同源存储（lua_mod.db conf "game:<gid>"）。
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
    // 多平台会话定位：按群/用户档案反查其所属平台（OneBot/QQ官方/Discord/KOOK
    // 同时在线时，Lua sendMsg/eventMsg 不能盲取「第一个已连适配器」，会串平台）。
    auto resolveChatPlatform = [&db](const std::string& gid, const std::string& uid) -> std::string {
        namespace orm = sqlite_orm;
        auto* st = db.getStorage(); if (!st) return {};
        try {
            if (!gid.empty()) {
                auto rows = st->get_all<dice::GroupSettingRow>(orm::where(
                    orm::c(&dice::GroupSettingRow::groupId) == gid), orm::limit(1));
                if (!rows.empty()) return rows.front().platform;
            } else if (!uid.empty()) {
                auto rows = st->get_all<dice::PlayerProfileRow>(orm::where(
                    orm::c(&dice::PlayerProfileRow::userId) == uid), orm::limit(1));
                if (!rows.empty()) return rows.front().platform;
            }
        } catch (...) {}
        return {};
    };
    // askExtra(json) 透传 {action, params} 到平台扩展查询。
    // OneBot invokeAction（如 get_group_member_list）；不支持该动作的适配器
    // （QQ官方/Discord/KOOK 返回 null）跳过，继续问下一个，别让首位非 OneBot 断链。
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
            }
        } catch (...) {}
        return {};
    });
    // Lua 插件 sendMsg(text,gid,uid) → 优先发到该会话所属平台的适配器，找不到再回退首个已连。
    luaMod.setSender([&adapterMgr, resolveChatPlatform](const std::string& text, const std::string& gid, const std::string& uid) {
        const std::string plat = resolveChatPlatform(gid, uid);
        auto send = [&](const dice::AdapterPtr& a) {
            if (!gid.empty()) a->sendGroupMessage(gid, text);
            else if (!uid.empty()) a->sendPrivateMessage(uid, text);
        };
        if (!plat.empty())
            for (auto& a : adapterMgr.allAdapters())
                if (a->isConnected() && a->platform() == plat) { send(a); return; }
        for (auto& a : adapterMgr.allAdapters())
            if (a->isConnected()) { send(a); return; }
    });
    // ── 统一回复兜底链 ───────────────────────────────────────
    // JS插件指令 → Lua因果 → C++因果规则 → 自定义回复 → JS非指令钩子。
    // live 消息管线 / 戳一戳映射 / Lua eventMsg / 网页测试台四处共用。
    // 此前四处各手抄一份且互有出入（eventMsg 与测试台漏掉了因果规则引擎，
    // eventMsg 还漏了 JS 非指令钩子），统一后不再漂移。
    auto replyFallback = [&cmdRouter, &replyManager, &jsMod, &luaMod, &causalMgr](
            const dice::Message& m, std::string& replySrc) -> std::string {
        std::string reply;
        const bool pv = m.type == dice::MessageType::kPrivate;
        const std::string nick = m.senderName.empty() ? m.senderId : m.senderName;
        // extra 可能是默认构造的 null json（poke/eventMsg/测试台自造的消息），
        // 对 null 调 .value() 会抛 type_error.306。
        const std::string card = m.extra.is_object() ? m.extra.value("card", std::string()) : std::string();
        // JS 插件指令（海豹兼容）优先于自定义回复。
        if (jsMod.ready()) {
            if (auto body = cmdRouter.commandBody(m.content); body && !body->empty()) {
                auto jr = jsMod.handle(m.platform, m.senderId, nick, m.targetId, card, pv, *body,
                                       cmdRouter.jsPrivilegeLevel(m), m.atList);
                if (jr.matched && !jr.reply.empty()) { reply = jr.reply; replySrc = "plugin_command"; }
            }
        }
        // Lua 模块的因果回复。
        if (reply.empty() && luaMod.ready()) {
            int trust = cmdRouter.jsPrivilegeLevel(m) >= 70 ? 4 : 0;   // master/信任≥4 → trust4
            auto lr = luaMod.dispatch(m.content, m.senderId, pv ? "" : m.targetId, nick, card, pv, trust, m.platform);
            // Legacy Lua modules often hard-code `.command` in msg_order or
            // reply prefixes. Retry only an otherwise-unmatched command using
            // that legacy spelling after CommandRouter has validated an
            // owner-configured prefix such as `>`.
            if (!lr.matched) if (auto body = cmdRouter.commandBody(m.content); body && !body->empty()) {
                const std::string legacy = "." + *body;
                if (legacy != m.content)
                    lr = luaMod.dispatch(legacy, m.senderId, pv ? "" : m.targetId, nick, card, pv, trust, m.platform);
            }
            if (lr.matched && !lr.reply.empty()) { reply = lr.reply; replySrc = "plugin_command"; }
        }
        // C++ 因果规则（优先于普通自定义回复）。
        if (reply.empty()) {
            auto cr = causalMgr.matchAndExecute(m.content, m.senderId, pv ? "" : m.targetId, nick);
            if (cr.matched && !cr.reply.empty()) {
                // Build counter context for {counter:name} resolution in renderReply
                std::map<std::string, std::string> counterCtx;
                for (auto& cc : cr.counterChanges) counterCtx[cc.name] = std::to_string(cc.newValue);
                cmdRouter.setCounterContext(counterCtx);
                reply = cmdRouter.renderReply(m, cr.reply, "", dice::MatchType::kKeyword);
                cmdRouter.clearCounterContext();
                replySrc = "reply";
            }
        }
        // 普通自定义回复（完整触发管线：匹配→范围→冷却→日限→概率）。
        if (reply.empty()) {
            dice::ReplyCtx rctx{m.platform, pv ? "" : m.targetId, m.senderId};
            auto pk = replyManager.pickReply(m.content, rctx);
            if (pk.rule) {
                reply = cmdRouter.renderReply(m, replyManager.pickResult(*pk.rule),
                                              pk.rule->matchContent, pk.rule->matchType);
                if (!reply.empty()) replySrc = "reply";
            } else if (!pk.notice.empty()) {
                // 冷却/日限提示语（原版 cd@echo / 限额回复）。
                reply = cmdRouter.renderReply(m, pk.notice, "", dice::MatchType::kKeyword);
                if (!reply.empty()) replySrc = "reply";
            }
        }
        // 仍无回复 → JS 插件的非指令消息钩子（自动回复 / 随机抓话等）。
        if (reply.empty() && jsMod.ready()) {
            auto nc = jsMod.handleNonCommand(m.platform, m.senderId, nick, m.targetId, card, pv, m.content,
                                             cmdRouter.jsPrivilegeLevel(m), m.atList);
            if (nc.matched && !nc.reply.empty()) { reply = nc.reply; replySrc = "plugin"; }
        }
        return reply;
    };

    // Lua 插件 eventMsg(text,gid,uid) 将 text 作为消息走完整回复管线。
    // 复用统一兜底链（内置指令→兜底链→applySelf→发送）。
    // 用线程局部深度计数防 eventMsg 递归风暴（eventMsg 的回复又触发 eventMsg）。
    luaMod.setEventMsg([&adapterMgr, &cmdRouter, replyFallback, resolveChatPlatform](
            const std::string& text, const std::string& gid, const std::string& uid) {
        static thread_local int depth = 0;
        if (depth >= 4) { DICE_LOG_INFO("eventMsg recursion capped at depth 4: '{}'", text); return; }
        struct Guard { int& d; Guard(int& x):d(x){ ++d; } ~Guard(){ --d; } } guard(depth);
        // 会话所属平台优先（多平台在线时别拿「第一个适配器」的平台冒充）。
        std::string platform = resolveChatPlatform(gid, uid);
        if (platform.empty())
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
            std::string src;
            reply = replyFallback(pm, src);
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
    // 插件分群启停（地基）：Lua mod 派发前问「该群是否启用此 mod」。
    luaMod.setGroupGate([&cmdRouter](const std::string& platform, const std::string& group, const std::string& pluginId) {
        return cmdRouter.isPluginEnabledInGroup(platform, group, pluginId);
    });
    { std::vector<std::string> luaDirs, jsDirs; dice::CommandRouter::packPluginDirs(luaDirs, jsDirs); luaMod.setExtraDirs(luaDirs); }   // 规则包 lua 附加加载
    dice::crashdiag::setPhase("lua-mods");
    luaMod.loadDir("data/mod");   // 与 JS 规则插件共用 data/mod（Lua mod=目录，JS=文件）

    // 把 JS 插件 cmd.help + Lua mod descriptor.helpdoc 喂给 .help 帮助系统
    //（解耦：router 不直接依赖各引擎）。
    cmdRouter.setHelpProvider([&jsMod, &luaMod]() {
        std::vector<std::pair<std::string, std::string>> v;
        for (auto& ch : jsMod.commandHelps()) if (!ch.help.empty()) v.push_back({ch.name, ch.help});
        for (auto& h : luaMod.helpEntries()) if (!h.text.empty()) v.push_back({h.topic, h.text});
        return v;
    });

    // .plugin 指令的插件清单（lua mods + js 插件），与 /api/groups/plugins 同源。
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
            // 执行任意指令 / 帮助搜索 —— 以发送者身份走 command_router，权限=该用户。
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
    adapterMgr.onMessage([&adapterMgr, &cmdRouter, &jsMod, &db, &configMgr, &engine, &cardDeck, &makeAiTool, replyFallback](const dice::Message& msg) {
        // Multi-bot群: a known dice bot always wins over @-as-argument and JS
        // plugin exceptions.  Otherwise retain the plugin @-argument exception.
        if (cmdRouter.mentionsOtherKnownDiceBot(msg) ||
            (cmdRouter.isForAnotherBot(msg) && !jsCommandMatches(jsMod, cmdRouter, msg))) return;
        // Black/white-list: ignore blacklisted users/groups (and non-whitelisted in whitelist mode).
        if (cmdRouter.isBlocked(msg)) return;
        // 群自动化：消息命中「自动踢出/禁言」关键字则执行并跳过后续处理。
        {
            std::string act = cmdRouter.applyGroupAutoModeration(msg);
            if (!act.empty()) {
                DICE_LOG_INFO("event: 群 {} 自动{} 用户 {}（命中关键字）", msg.targetId,
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
        bool didCommand = !reply.empty();
        // 阶段3：回复来源分类（builtin/plugin/reply），供 AI 翻译按范围过滤。
        std::string replySrc = "builtin";
        // No command matched → custom replies, unless the group is disabled or has
        // custom replies turned off (.group +禁用回复).
        bool replyOff = msg.type == dice::MessageType::kGroup && !msg.targetId.empty()
                        && cmdRouter.isReplyDisabledFor(msg.platform, msg.targetId);
        // 自控：操作者用骰娘账号手打的消息走**完整管线**（内置/插件/自定义回复）；
        // 骰娘自己的回复回声已在适配器层被自回声去重丢弃，不会到这里，故无需在此限制。
        if (reply.empty() && (!disabled || forcedByAt) && !replyOff)
            reply = replyFallback(msg, replySrc);
        if (!reply.empty() && replySrc == "plugin_command") didCommand = true;
        // ── 先在消息线程消费本条消息的一次性路由状态 ─────────────
        // 回复投递可能转入 AI 后台线程，这些状态晚取会被下一条消息拿走/污染。
        std::string aiCat = (replySrc == "plugin" || replySrc == "plugin_command") ? "plugin"
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

        // ── 统一的回复投递（润色→翻译→link转发→日志→发送）────────
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
            // 阶段2：AI 润色 —— 类别在覆盖范围内 + 总开关+润色开关开启。失败/超时/
            // 破坏数字一律回退原文，绝不影响掷骰结果。
            if (!reply.empty() && !msg.fromSelf
                && dice::aipolish::enabled(configMgr) && dice::aipolish::covers(configMgr, aiCat)) {
                reply = dice::aipolish::polish(configMgr, msg.content, reply);
            }
            // 阶段3：AI 翻译 —— 本群/本用户 .lang 切到骰主自定义语言时，回复先按正常
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
                // 骰娘自己的消息也显示名字（QQ 昵称，缺失回退「骰娘」）。
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
                auto segs = cmdRouter.segmentReply(reply, msg.platform);
                bool priv = msg.type == dice::MessageType::kPrivate;
                // #6 合并转发(聊天记录)：仅群消息、开关开启、且**回复字符数超过阈值**(默认1200，
                // 应用于所有回复内容)时强制转发。节点也遵守分段设置：有显式节点(.coc/.dnd 每条
                // 结果)就逐个再按分段切，否则整段按分段切。适配器不支持则回退普通分段发送。
                bool wantForward = !priv && !msg.targetId.empty() && cmdRouter.forwardEnabled()
                    && dice::CommandRouter::textCharCount(reply) > cmdRouter.forwardThreshold();
                if (wantForward) {
                    if (!fwdNodes.empty()) {
                        std::vector<std::string> nodes;
                        for (auto& n : fwdNodes)
                            for (auto& s : cmdRouter.segmentReply(n, msg.platform)) nodes.push_back(s);
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
                        // 分段之间留几毫秒，避免同一适配器并发发消息导致客户端乱序。
                        if (k > 0) std::this_thread::sleep_for(std::chrono::milliseconds(30));
                        if (k == 0 && quoteFirst) a->sendReply(replyMsg, segs[0]);
                        // 私聊回复发到 targetId（普通私聊=对方=senderId；自身消息自控时
                        // =对话对方，避免回复发给骰娘自己）。sendReply 亦用 targetId，一致。
                        else if (priv) a->sendPrivateMessage(msg.targetId, segs[k]);
                        else a->sendGroupMessage(msg.targetId, segs[k]);
                    }
                };
                // 只通过消息来源的那个适配器回复，避免多账号/多适配器时串台。
                if (auto a = adapterMgr.getAdapter(msg.adapterId)) {
                    if (a->isConnected()) sendSegs(a);
                } else {
                    for (auto& ad : adapterMgr.allAdapters())   // 回退：来源不明时发首个已连接
                        if (ad->isConnected()) { sendSegs(ad); break; }
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
        // 触发判定留在消息线程（廉价），上下文/记忆检索/图像识别/生成/工具调用整段
        // 投给 AI 后台线程 —— 大模型再慢也不影响其他指令；完成后经 finishReply 发送。
        if (reply.empty() && !disabled && !replyOff && !msg.fromSelf
            && msg.type == dice::MessageType::kGroup && !msg.targetId.empty()
            && (dice::aichat::enabled(configMgr) || dice::ainpc::enabled(configMgr))
            && cmdRouter.aiEnabledForGroup(msg.platform, msg.targetId, msg.adapterId)      // 本账号在本群的 AI 开关
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
            // 被@/命中关键词（Strong）或 NPC 命中 → 必回，无视冷却；待机搭话（Standby）受冷却。
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
                    // 多模态 —— 消息带图且开启图像识别时，识别图片内容并注入当前消息。
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
                    DICE_LOG_INFO("AI \xe9\x98\x9f\xe5\x88\x97\xe5\xb7\xb2\xe6\xbb\xa1\xef\xbc\x8c\xe4\xb8\xa2\xe5\xbc\x83\xe6\x9c\xac\xe6\xac\xa1 AI \xe5\xaf\xb9\xe8\xaf\x9d\xe8\xa7\xa6\xe5\x8f\x91 group {}", msg.targetId);  // 队列已满，丢弃本次 AI 对话触发
            }
        }

        // Triggered broadcast: a pending announcement, delivered ONCE per group as
        // a SEPARATE plain group message (not a quoted reply), only when the group
        // actually triggered the bot. Not mass-sent → avoids spam/ban.
        std::string broadcast;
        if (!disabled && !reply.empty() && !msg.fromSelf && msg.type == dice::MessageType::kGroup && !msg.targetId.empty())
            broadcast = dice::BroadcastManager::instance().takeFor(msg.platform + ":" + msg.targetId);

        // Auto-build the player's profile. Only built-in/rule-pack commands and
        // JS/Lua command matches count; custom replies, non-command hooks and AI
        // conversation are ordinary chat and must not grant usage-based approval.
        if (!msg.fromSelf) cmdRouter.recordPlayerActivity(msg, didCommand);
        // “活跃” = 本群最近用过指令（与 recordPlayerActivity 同口径）。
        // 这样定时任务的 inactive>=N 条件表示“N 天无指令”，纯聊天不计入，符合“无指令退群”语义。
        if (msg.type == dice::MessageType::kGroup && !msg.targetId.empty() && didCommand)
            cmdRouter.markGroupActive(msg.platform, msg.targetId);   // #47 群活跃度（按指令）
        // .log transcript recording (skipped for disabled groups). 操作者手打的
        // 自控消息（fromSelf 且已过自回声去重）视同正常消息记录；骰娘自己的回复回声不会到这里。
        // 只记入站；骰娘回复待润色/翻译定稿后在 finishReply 里记（recordBotReply）。
        if (!disabled) cmdRouter.recordIncoming(msg);
        // Feed the web "模拟聊天" live window (incoming line + bot reply + broadcast).
        if ((msg.type == dice::MessageType::kGroup && !msg.targetId.empty())
            || (msg.type == dice::MessageType::kPrivate && !msg.senderId.empty())) {
            std::string chatScope = msg.type == dice::MessageType::kPrivate
                ? "private:" + msg.senderId : msg.targetId;
            std::string key = msg.platform + ":" + chatScope;
            // 喂 CQ 原文(msg.rawContent，含 [CQ:image,file=URL])给模拟聊天，前端据此渲染真图片；
            // content 是去掉 CQ 的纯指令文本(无图)，rawContent 才保留图片链接。带 userId 作头像/标识。
            std::string chatContent = !msg.rawContent.empty() ? msg.rawContent
                : (msg.displayContent.empty() ? msg.content : msg.displayContent);
            dice::GroupChatLog::instance().add(key,
                msg.senderName.empty() ? msg.senderId : msg.senderName, msg.senderId,
                chatContent, false);
            // 骰娘回复/广播的模拟聊天与 chat.db 写入移到 finishReply（定稿后）。
            // 同步持久化到 chat.db（带 msgId，供撤回标注与保留期管理）。
            if (auto* cst = db.getChatStorage()) {
                try {
                    int64_t now = static_cast<int64_t>(std::time(nullptr));
                    dice::ChatMsgRow rin;
                    rin.platform = msg.platform; rin.groupId = chatScope; rin.msgId = msg.id;
                    rin.userId = msg.senderId;
                    // 群员显示群名片(群昵称)优先，其次 QQ 昵称，最后 QQ 号。
                    std::string sndCard = (msg.extra.contains("card") && msg.extra["card"].is_string())
                                        ? msg.extra["card"].get<std::string>() : std::string();
                    rin.sender = !sndCard.empty() ? sndCard
                               : (msg.senderName.empty() ? msg.senderId : msg.senderName);
                    rin.content = chatContent; rin.self = 0; rin.time = now;
                    int64_t inId = cst->insert(rin);
                    // 入站消息若含远端(NTQQ)图片，趁 rkey 新鲜后台下载到本地并
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
                && cmdRouter.aiEnabledForGroup(msg.platform, msg.targetId, msg.adapterId)      // 本账号在本群关 AI 则不建记忆
                && cmdRouter.aiWhitelistOk(msg.platform, msg.targetId, true)) { // AI 白名单模式
                std::string plat = msg.platform, gid = msg.targetId;
                std::thread([&configMgr, &db, plat, gid]() {
                    dice::aimemory::maybeFold(configMgr, db.getChatStorage(), plat, gid);
                }).detach();
            }
        }
        // ── 投递回复 ── 需要 AI 后处理（润色/翻译命中覆盖范围）时走后台线程，
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
                    DICE_LOG_INFO("AI \xe9\x98\x9f\xe5\x88\x97\xe5\xb7\xb2\xe6\xbb\xa1\xef\xbc\x8c\xe8\xb7\xb3\xe8\xbf\x87\xe6\xb6\xa6\xe8\x89\xb2/\xe7\xbf\xbb\xe8\xaf\x91\xe7\x9b\xb4\xe6\x8e\xa5\xe5\x8f\x91\xe9\x80\x81");  // 队列已满，跳过润色/翻译直接发送
                    finishReply(msg, reply, broadcast, "", quoteId, fwdNodes, !disabled, linkReplyOk);
                }
            } else {
                finishReply(msg, reply, broadcast, aiCat, quoteId, fwdNodes, !disabled, linkReplyOk);
            }
        }
    });

    // Non-message events: 入群欢迎词、被加好友欢迎、加好友/加群条件自动同意。
    adapterMgr.onEvent([&adapterMgr, &configMgr, &i18n, &localeResolver, &cmdRouter, &jsMod, &db, replyFallback](const dice::BotEvent& e) {
        using ET = dice::EventType;
        nlohmann::json cfgAll = configMgr.getAll();
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
        nlohmann::json ev = dice::scoped_settings::resolveSection(
            cfgAll, "events", a->platform(), e.adapterId);

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
        // 通知里群/人显示「名字(号码)」；名字未知（未入群/无档案）回退纯号码。
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

        // ── 聊天记录持久化 —— 撤回标注 / 历史消息回流入库 ──
        if (e.type == ET::kGroupUpload) {
            // 群文件上传 → 记入模拟聊天 + chat.db（[CQ:file,...] 供前端渲染
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
            DICE_LOG_INFO("event: 群 {} 文件上传 {} by {}", e.groupId, f.value("name", std::string()), e.userId);
            return;
        }
        if (e.type == ET::kGroupRecall) {
            std::string mid;
            try {
                if (e.extra.contains("message_id") && e.extra["message_id"].is_string()) mid = e.extra["message_id"].get<std::string>();
                else if (e.extra.contains("message_id") && e.extra["message_id"].is_number_integer()) mid = std::to_string(e.extra["message_id"].get<int64_t>());
            } catch (...) {}
            if (mid.empty() || e.groupId.empty()) return;
            int logRemoved = 0;
            if (auto* cst = db.getChatStorage()) try {
                namespace orm = sqlite_orm;
                auto rows = cst->get_all<dice::ChatMsgRow>(orm::where(
                    orm::c(&dice::ChatMsgRow::platform) == e.platform and
                    orm::c(&dice::ChatMsgRow::groupId) == e.groupId and
                    orm::c(&dice::ChatMsgRow::msgId) == mid));
                for (auto r : rows) { r.recalled = 1; cst->update(r); }
            } catch (...) {}
            if (auto* lst = db.getLogStorage()) try {
                namespace orm = sqlite_orm;
                const auto logs = lst->get_all<dice::GameLogRow>(orm::where(
                    orm::c(&dice::GameLogRow::groupId) == e.groupId));
                for (const auto& log : logs) {
                    const auto condition = orm::where(
                        orm::c(&dice::GameLogMessageRow::logId) == log.id and
                        orm::c(&dice::GameLogMessageRow::messageId) == mid);
                    const int removed = static_cast<int>(lst->count<dice::GameLogMessageRow>(condition));
                    if (removed > 0) lst->remove_all<dice::GameLogMessageRow>(condition);
                    logRemoved += removed;
                }
            } catch (...) {}
            DICE_LOG_INFO("event: 群 {} 消息 {} 已撤回：聊天记录标注，游戏日志移除 {} 条", e.groupId, mid, logRemoved);
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
                    // 历史消息里的远端图片也本地化（拉历史时 rkey 通常仍新鲜）。
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
                        "\xe8\xa2\xab\xe6\x8b\x89\xe5\x85\xa5\xe9\xbb\x91\xe5\x90\x8d\xe5\x8d\x95\xe7\xbe\xa4 " + groupLabel(e.groupId) + "\xef\xbc\x8c\xe5\xb7\xb2\xe8\x87\xaa\xe5\x8a\xa8\xe9\x80\x80\xe5\x87\xba", "blacklist_leave", e.adapterId);
                    return;
                }
                // 非好友强拉兜底：人少的群可不经审批直接把 bot 拉进来（无 request 事件），
                // 开启「拒绝非好友邀请」时按拉人者(operator，缺失则回退已记录的邀请人)判定，
                // 非好友 → 提示后立即退群。Master/白名单豁免；好友列表未同步时放行。
                if (ev.value("group_invite_reject_nonfriend", false)) {
                    std::string puller = e.operatorId;
                    if (puller.empty())
                        puller = cmdRouter.groupSettingValue(e.platform, e.groupId, "inviter", e.adapterId);
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
                                "\xe8\xa2\xab\xe9\x9d\x9e\xe5\xa5\xbd\xe5\x8f\x8b " + userLabel(puller) + " \xe6\x8b\x89\xe5\x85\xa5\xe7\xbe\xa4 " + groupLabel(e.groupId) + "\xef\xbc\x8c\xe5\xb7\xb2\xe8\x87\xaa\xe5\x8a\xa8\xe9\x80\x80\xe5\x87\xba", "nonfriend_leave", e.adapterId);
                            return;
                        }
                    }
                }
                // 曾「删除记录」过的群重新加回来 → 清墓碑标记，否则群组管理
                // 的自动发现会一直跳过它，刷不出这个群。
                if (cmdRouter.groupSettingValue(e.platform, e.groupId, "__removed", e.adapterId) == "1") {
                    cmdRouter.setGroupSettingFor(e.platform, e.groupId, "__removed", "0", e.adapterId);
                    cmdRouter.setGroupSettingFor(e.platform, e.groupId, "enabled", "1", e.adapterId);
                    DICE_LOG_INFO("event: 群 {} 重新加入，已清除移除标记（记录可重建）", e.groupId);
                }
                // 曾指令退群的群重新加回来 → 清「已退群/退群中」状态。
                if (cmdRouter.groupSettingValue(e.platform, e.groupId, "left", e.adapterId) == "1" ||
                    cmdRouter.groupSettingValue(e.platform, e.groupId, "leaving", e.adapterId) == "1") {
                    cmdRouter.setGroupSettingFor(e.platform, e.groupId, "left", "0", e.adapterId);
                    cmdRouter.setGroupSettingFor(e.platform, e.groupId, "leaving", "0", e.adapterId);
                }
                // 直接被拉进群（无邀请事件）时，operator 即邀请人
                // 已有邀请人记录时不覆盖。
                if (!e.operatorId.empty() &&
                    cmdRouter.groupSettingValue(e.platform, e.groupId, "inviter", e.adapterId).empty())
                    cmdRouter.setGroupSettingFor(e.platform, e.groupId, "inviter", e.operatorId, e.adapterId);
                // 群名关键词自动退群：邀请/入群事件不含群名，入群后靠反查暖缓存，
                // 延迟数秒按群名判关键词（空白分隔多词，任一命中）→ 提示 + 退群 + 通知骰主。
                {
                    std::string kws = ev.value("group_name_keyword_leave", std::string());
                    if (!kws.empty()) {
                        auto aw = std::weak_ptr<dice::IAdapter>(a);
                        std::string gid = e.groupId, plat = e.platform, evAid = e.adapterId;
                        drogon::app().getLoop()->runAfter(5.0,
                            [aw, gid, plat, evAid, kws, loc, &cmdRouter, &i18n]() {
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
                                    + "\xe3\x80\x8d\xef\xbc\x8c\xe5\xb7\xb2\xe8\x87\xaa\xe5\x8a\xa8\xe9\x80\x80\xe5\x87\xba", "keyword_leave", evAid);
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
                cmdRouter.blacklistQuitLevel(e.platform, e.groupId, e.adapterId) == "member") {
                a->sendGroupMessage(e.groupId, i18n.tr(loc, "event.blacklist_quit"));
                a->leaveGroup(e.groupId);
                DICE_LOG_INFO("event: leaving group {} — blacklisted user {} joined", e.groupId, e.userId);
                cmdRouter.notifyMasters(dice::notice::kImportant,
                    "\xe9\xbb\x91\xe5\x90\x8d\xe5\x8d\x95\xe7\x94\xa8\xe6\x88\xb7 " + userLabel(e.userId) + " \xe5\x8a\xa0\xe5\x85\xa5\xe7\xbe\xa4 " + groupLabel(e.groupId) + "\xef\xbc\x8c\xe5\xb7\xb2\xe8\x87\xaa\xe5\x8a\xa8\xe9\x80\x80\xe5\x87\xba", "blacklist_leave", e.adapterId);
                return;
            }
            // ── 必须加入用户群 —— 挂靠黑名单检查点（入群事件）。
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
                        DICE_LOG_INFO("event: 群 {} 无成员在用户群 {} → 自动退群", gid2, ug2);
                    });
                }
            }
            // 入群反馈开关。
            if (!diceFlag("listen_group_add", true)) return;
            // A member joined → send the group's .welcome text if configured.
            // welcome with delay + cooldown
            std::string welcome = cmdRouter.groupSettingValue(e.platform, e.groupId, "welcome", e.adapterId);
            if (welcome.empty()) return;
            int welcomeDelay = 0;
            try { auto d = cmdRouter.groupSettingValue(e.platform, e.groupId, "welcome_delay", e.adapterId); if (!d.empty()) welcomeDelay = std::stoi(d); } catch (...) {}
            int welcomeCooldown = 0;
            try { auto c = cmdRouter.groupSettingValue(e.platform, e.groupId, "welcome_cooldown", e.adapterId); if (!c.empty()) welcomeCooldown = std::stoi(c); } catch (...) {}
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
                // v2: welcome with debounce + cooldown + proper timer cancellation
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
                            double elapsed = static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count());
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
                            double elapsed = static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count());
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
                cmdRouter.setGroupSettingFor(e.platform, e.groupId, "left", "1", e.adapterId);
                std::string who = (!e.operatorId.empty() && e.operatorId != e.selfId)
                    ? ("\xef\xbc\x88\xe6\x93\x8d\xe4\xbd\x9c\xe8\x80\x85 " + userLabel(e.operatorId) + "\xef\xbc\x89") : std::string();
                cmdRouter.notifyMasters(dice::notice::kImportant,
                    "\xe9\xaa\xb0\xe5\xa8\x98\xe5\xb7\xb2\xe7\xa6\xbb\xe5\xbc\x80\xe7\xbe\xa4 " + groupLabel(e.groupId) + who, "group_left", e.adapterId);
            }
        } else if (e.type == ET::kPoke) {
            if (!ev.value("poke_enabled", true)) return;   // 戳一戳回复总开关（默认开）
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
                    std::string src;
                    reply = replyFallback(pm, src);
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
                "\xe6\x96\xb0\xe5\xa5\xbd\xe5\x8f\x8b\xe5\xb7\xb2\xe6\xb7\xbb\xe5\x8a\xa0\xef\xbc\x9a" + userLabel(e.userId), "friend_add", e.adapterId);
            std::string fw = ev.value("friend_welcome", std::string());
            if (fw.empty()) fw = i18n.tr(loc, "event.friend_welcome");
            if (!fw.empty()) a->sendPrivateMessage(e.userId, fw);
            // ── 新好友不在用户群 → 私聊发送用户群邀请（群号+引导文本）。
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
                        DICE_LOG_INFO("event: 新好友 {} 不在用户群 {} → 已私聊邀请", uid2, ug2);
                    });
                }
            }
        } else if (e.type == ET::kFriendRequest) {
            if (!diceFlag("listen_friend_request", true)) return;    // 好友请求事件开关
            // B：通知骰主收到好友申请（含附言），便于人工处理。
            cmdRouter.notifyMasters(dice::notice::kImportant,
                "\xe6\x94\xb6\xe5\x88\xb0\xe5\xa5\xbd\xe5\x8f\x8b\xe7\x94\xb3\xe8\xaf\xb7\xef\xbc\x9a" + userLabel(e.userId)
                    + (e.comment.empty() ? "" : "\xef\xbc\x88\xe9\x99\x84\xe8\xa8\x80\xef\xbc\x9a" + e.comment + "\xef\xbc\x89"), "friend_req", e.adapterId);
            // 策略：all=任意通过 / keyword=含关键词才通过(其余留人工) /
            //       group_used=曾在任意群触发指令才通过 / reject=禁止任何人添加 /
            //       manual=不自动处理。未设 friend_policy 时由旧 auto_approve_friend 派生。
            std::string pol = ev.value("friend_policy", std::string());
            if (pol.empty()) pol = ev.value("auto_approve_friend", false)
                ? (ev.value("friend_keyword", std::string()).empty() ? "all" : "keyword") : "manual";
            const bool usedInGroup = pol == "group_used"
                && cmdRouter.hasGroupCommandHistory(e.platform, e.userId);
            const auto decision = dice::evaluateFriendRequest(
                pol, e.comment, ev.value("friend_keyword", std::string()), usedInGroup);
            if (decision == dice::FriendRequestDecision::kApprove) {
                a->setFriendRequest(e.flag, true);
                if (pol == "group_used")
                    DICE_LOG_INFO("event: 好友申请因存在群聊指令记录自动通过 from {}", e.userId);
                else if (pol == "keyword")
                    DICE_LOG_INFO("event: 好友申请含关键词自动通过 from {}", e.userId);
                else
                    DICE_LOG_INFO("event: 好友申请自动通过(任意) from {}", e.userId);
            } else if (decision == dice::FriendRequestDecision::kReject) {
                a->setFriendRequest(e.flag, false);
                DICE_LOG_INFO("event: 好友申请自动拒绝(禁止添加) from {}", e.userId);
            }   // manual / 条件不满足 → 留人工，不处理
        } else if (e.type == ET::kGroupRequest) {
            if (!diceFlag("listen_group_request", true)) return;     // 群请求事件开关
            if (e.subType == "invite") {
                // 记录群邀请人。
                // 无论走哪条审批路径（自动/人工）都先记下，邀请人视同群管理（canRoomHost）。
                if (!e.userId.empty() && !e.groupId.empty())
                    cmdRouter.setGroupSettingFor(e.platform, e.groupId, "inviter", e.userId, e.adapterId);
                // B：通知骰主收到加群邀请（含邀请人），便于人工处理。
                cmdRouter.notifyMasters(dice::notice::kImportant,
                    "\xe6\x94\xb6\xe5\x88\xb0\xe5\x8a\xa0\xe7\xbe\xa4\xe9\x82\x80\xe8\xaf\xb7\xef\xbc\x9a\xe7\xbe\xa4 " + groupLabel(e.groupId)
                        + " \xef\xbc\x88\xe9\x82\x80\xe8\xaf\xb7\xe4\xba\xba " + userLabel(e.userId) + "\xef\xbc\x89", "group_invite", e.adapterId);
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
                            "\xe9\x9d\x9e\xe5\xa5\xbd\xe5\x8f\x8b " + userLabel(e.userId) + " \xe9\x82\x80\xe8\xaf\xb7\xe5\x8a\xa0\xe7\xbe\xa4 " + groupLabel(e.groupId) + "\xef\xbc\x8c\xe5\xb7\xb2\xe8\x87\xaa\xe5\x8a\xa8\xe6\x8b\x92\xe7\xbb\x9d", "nonfriend_leave", e.adapterId);
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
                // 优先看本群 .group auto pass 设置（all=全过 / 关键字=验证消息含关键字才过）。
                std::string ap = cmdRouter.groupSettingValue(e.platform, e.groupId, "autoPass", e.adapterId);
                if (!ap.empty()) {
                    if (ap == "all" || (!e.comment.empty() && e.comment.find(ap) != std::string::npos)) {
                        a->setGroupRequest(e.flag, e.subType, true);
                        DICE_LOG_INFO("event: 群 {} 加群申请自动通过(auto pass) from {}", e.groupId, e.userId);
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
            if (!ev.value("poke_enabled", true)) return;   // 戳一戳回复总开关（默认开）
            // Only react when the BOT itself is poked, in a group.
            if (e.userId != e.selfId || e.groupId.empty()) return;
            if (cmdRouter.isGroupDisabledFor(e.platform, e.groupId, e.adapterId)) return;
            a->sendGroupMessage(e.groupId, i18n.tr(loc, "event.poke"));
        }
    });

    // Register adapters from DB (don't start yet — wait for event loop)
    auto* st = db.getStorage();
    if (st) {
        auto rowFromConfig = [](const nlohmann::json& a) {
            dice::AdapterRow row;
            row.name = a.value("name", std::string("Bot"));
            row.type = static_cast<int>(dice::adapterTypeFromString(a.value("type", std::string("onebot_v11"))));
            const std::string mode = a.value("connection_mode", a.value("connectionMode", std::string("forward_ws")));
            row.connectionMode = static_cast<int>(dice::connectionModeFromString(mode));
            row.endpoint = a.value("endpoint", std::string());
            row.accessToken = a.value("access_token", a.value("accessToken", std::string()));
            row.enabled = a.value("enabled", false);
            nlohmann::json extra{
                {"heartApiKey", a.value("heart_api_key", a.value("heartApiKey", std::string()))},
            };
            if (row.type == static_cast<int>(dice::AdapterType::kQQOfficial)) {
                extra["appId"] = a.value("app_id", a.value("appId", std::string()));
                extra["appSecret"] = a.value("app_secret", a.value("appSecret", std::string()));
                extra["qqNumber"] = a.value("qq_number", a.value("qqNumber", std::string()));
                extra["forceVerifyImageResource"] = a.value("force_verify_image_resource", a.value("forceVerifyImageResource", false));
            }
            row.config = extra.dump();
            return row;
        };
        auto configFromRow = [](const dice::AdapterRow& row) {
            nlohmann::json extra = nlohmann::json::parse(row.config, nullptr, false);
            if (!extra.is_object()) extra = nlohmann::json::object();
            nlohmann::json a{{"id", row.id}, {"name", row.name}, {"type", dice::adapterTypeToString(static_cast<dice::AdapterType>(row.type))},
                             {"connection_mode", row.connectionMode == 1 ? "reverse_ws" : row.connectionMode == 2 ? "http" : "forward_ws"},
                             {"endpoint", row.endpoint}, {"access_token", row.accessToken}, {"enabled", row.enabled},
                             {"heart_api_key", extra.value("heartApiKey", std::string())}};
            if (row.type == static_cast<int>(dice::AdapterType::kQQOfficial)) {
                a["app_id"] = extra.value("appId", std::string());
                a["app_secret"] = extra.value("appSecret", std::string());
                a["qq_number"] = extra.value("qqNumber", std::string());
                a["force_verify_image_resource"] = extra.value("forceVerifyImageResource", false);
            }
            return a;
        };
        if (adaptersNeedExport) {
            nlohmann::json exported = nlohmann::json::array();
            for (const auto& row : st->get_all<dice::AdapterRow>()) exported.push_back(configFromRow(row));
            configMgr.set<nlohmann::json>("adapters", exported);
            configMgr.save();
            DICE_LOG_INFO("Exported {} database adapter(s) into adapters.json", exported.size());
        } else {
            const nlohmann::json cfgAll = configMgr.getAll();
            std::unordered_set<int> configuredIds;
            std::unordered_map<int, dice::AdapterRow> existing;
            for (const auto& row : st->get_all<dice::AdapterRow>()) existing.emplace(row.id, row);
            nlohmann::json normalized = nlohmann::json::array();
            if (cfgAll.contains("adapters") && cfgAll["adapters"].is_array()) {
                for (const auto& a : cfgAll["adapters"]) {
                    if (!a.is_object()) continue;
                    dice::AdapterRow row = rowFromConfig(a);
                    int configuredId = 0;
                    try {
                        if (a.contains("id") && a["id"].is_number_integer()) configuredId = a["id"].get<int>();
                        else if (a.contains("id") && a["id"].is_string()) configuredId = std::stoi(a["id"].get<std::string>());
                    } catch (...) { configuredId = 0; }
                    if (configuredId > 0 && existing.count(configuredId)) {
                        row.id = configuredId;
                        st->update(row);
                    } else {
                        row.id = st->insert(row);
                    }
                    configuredIds.insert(row.id);
                    normalized.push_back(configFromRow(row));
                }
            }
            for (const auto& [id, row] : existing) if (!configuredIds.count(id)) st->remove<dice::AdapterRow>(id);
            // New entries and old UUID-style IDs become concrete, stable IDs.
            configMgr.set<nlohmann::json>("adapters", normalized);
            configMgr.save();
            DICE_LOG_INFO("Loaded {} adapter(s) from adapters.json", normalized.size());
        }

        // Older builds stored one Heart key globally. It can be migrated without
        // ambiguity only when exactly one enabled adapter exists.
        const std::string legacyHeartKey = configMgr.get<std::string>("dice/heart_token", std::string());
        if (!legacyHeartKey.empty()) {
            auto rows = st->get_all<dice::AdapterRow>();
            std::vector<dice::AdapterRow*> enabledRows;
            for (auto& row : rows) if (row.enabled) enabledRows.push_back(&row);
            if (enabledRows.size() == 1) {
                auto& row = *enabledRows.front();
                nlohmann::json extra = nlohmann::json::parse(row.config, nullptr, false);
                if (!extra.is_object()) extra = nlohmann::json::object();
                if (extra.value("heartApiKey", std::string()).empty()) {
                    extra["heartApiKey"] = legacyHeartKey;
                    row.config = extra.dump();
                    st->update(row);
                    configMgr.set<std::string>("dice/heart_token", std::string());
                    nlohmann::json exported = nlohmann::json::array();
                    for (const auto& item : st->get_all<dice::AdapterRow>()) exported.push_back(configFromRow(item));
                    configMgr.set<nlohmann::json>("adapters", exported);
                    configMgr.save();
                    DICE_LOG_INFO("Migrated legacy global Heart API Key to adapter '{}'", row.name);
                }
            } else {
                DICE_LOG_WARN("Global Heart API Key cannot be migrated automatically: configure a Key on each adapter");
            }
        }

        auto adapters = st->get_all<dice::AdapterRow>();
        for (auto& row : adapters) {
            if (row.enabled) {
                // 与 WebUI 增改共用同一工厂（api_service::makeRuntimeAdapter），
                // 避免此处漏掉新平台——曾把 Discord/KOOK 行当 OneBot 建导致空端点重连死循环。
                adapterMgr.registerAdapter(dice::api::makeRuntimeAdapter(row));
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
    app.addListener(host, static_cast<uint16_t>(port));
    app.setThreadNum(4);
    // 上传体积上限：规则包(含 lua/js mod)、牌堆、图片都走 base64 塞进 JSON body，
    // drogon 默认 client_max_body_size 仅 1MB，稍大的规则包就会被拒（报 "string too long"）。
    // 放宽到 128MB，并让大 body 直接驻内存（getBody() 可直接读，不落临时文件）。
    app.setClientMaxBodySize(128 * 1024 * 1024);
    app.setClientMaxMemoryBodySize(128 * 1024 * 1024);

    // ── WebUI 登录鉴权 ───────────────────────────────────
    dice::WebAuth::instance().configure(configMgr.get<std::string>("webui/password", ""),
        std::filesystem::path(configPath) / "webui_sessions.json",
        configMgr.get<std::string>("server/api_key", ""));
    // H2: X-API-Key 凭据（服务间/脚本调用）。仅配置了 key 时生效。
    const std::string apiKey = configMgr.get<std::string>("server/api_key", "");

    // 前置拦截：设了口令时，/api/* 需有效会话 Cookie（放行登录/状态查询与静态文件）。
    app.registerPreHandlingAdvice(
        [apiKey](const drogon::HttpRequestPtr& req,
           drogon::AdviceCallback&& stop, drogon::AdviceChainCallback&& next) {
            const std::string& path = req->path();
            if (path.rfind("/api/", 0) != 0 ||                       // 静态文件
                path == "/api/auth/login" || path == "/api/auth/status" ||
                path == "/api/auth/logout" || path == "/api/auth/setup") { next(); return; }
            auto& auth = dice::WebAuth::instance();
            // 未设置口令：强制先设置（默认 0.0.0.0 监听，空口令直通 = 未授权接管/RCE）。
            if (!auth.hasPassword()) {
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k401Unauthorized);
                resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                resp->setBody("{\"code\":401,\"message\":\"need_setup\",\"need_setup\":true}");
                stop(resp); return;
            }
            if (auth.validToken(req->getCookie("dice_session"))) { next(); return; }
            // 服务间/脚本调用：X-API-Key 与 server/api_key 匹配（仅配置了 key 时生效）。
            if (!apiKey.empty() && auth.checkApiKey(req->getHeader("X-API-Key"))) { next(); return; }
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
                bool needSetup = !required;
                bool authed = required && dice::WebAuth::instance().validToken(req->getCookie("dice_session"));
                cb(jResp({{"code", 0}, {"message", "ok"}, {"data", {{"required", required}, {"authed", authed}, {"need_setup", needSetup}}}}));
            }, {drogon::Get});
        // 登录：校验口令 → 颁发 token，写 Cookie。
        app.registerHandler("/api/auth/login",
            [jResp](const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                try {
                    // M1: 登录限速（同一 IP 60 秒内最多 5 次失败）。
                    static std::mutex sLoginMu;
                    static std::unordered_map<std::string, std::pair<int, long long>> sLoginFails;
                    const std::string ip = req->peerAddr().toIp();
                    {
                        std::lock_guard<std::mutex> lk(sLoginMu);
                        const long long now = static_cast<long long>(std::time(nullptr));
                        auto& f = sLoginFails[ip];
                        if (now - f.second >= 60) f = {0, now};
                        if (f.first >= 5) {
                            cb(jResp({{"code", 429}, {"message", "尝试过于频繁，请稍后再试"}}, drogon::k429TooManyRequests));
                            return;
                        }
                    }
                    auto j = nlohmann::json::parse(req->getBody());
                        std::string pw = j.value("password", "");
                        if (!dice::WebAuth::instance().hasPassword()) {
                            cb(jResp({{"code", 401}, {"message", "请先设置管理口令"}}, drogon::k401Unauthorized));
                            return;
                        }
                        if (dice::WebAuth::instance().checkPassword(pw)) {
                            { std::lock_guard<std::mutex> lk(sLoginMu); sLoginFails.erase(ip); }
                            const bool trustDevice = j.value("trust_device", false);
                            bool trustedPersisted = true;
                            std::string token = dice::WebAuth::instance().issueToken(trustDevice, &trustedPersisted);
                            if (!trustedPersisted) {
                                cb(jResp({{"code", 1}, {"message", "无法保存可信设备会话，请检查 config 目录的写入权限"}}, drogon::k500InternalServerError));
                                return;
                            }
                            auto resp = jResp({{"code", 0}, {"message", "ok"}, {"data", {{"authed", true}}}});
                        drogon::Cookie ck("dice_session", token);
                        ck.setPath("/"); ck.setHttpOnly(true);
                        // Explicitly persist the cookie across browser and
                        // program restarts. Lax is appropriate for a local
                        // management panel and remains compatible with HTTP.
                        ck.setSameSite(drogon::Cookie::SameSite::kLax);
                        if (trustDevice) ck.setMaxAge(30 * 24 * 3600);
                        resp->addCookie(ck);
                        cb(resp);
                    } else {
                        { std::lock_guard<std::mutex> lk(sLoginMu); const long long now = static_cast<long long>(std::time(nullptr)); auto& f = sLoginFails[ip]; if (now - f.second >= 60) f = {0, now}; ++f.first; }
                        cb(jResp({{"code", 401}, {"message", "密码错误"}}, drogon::k401Unauthorized));
                    }
                } catch (const std::exception& e) { cb(jResp({{"code", 1}, {"message", e.what()}})); }
            }, {drogon::Post});
        // 首次设置管理口令（仅空口令时可用；成功后自动登录）。
        app.registerHandler("/api/auth/setup",
            [jResp, &configMgr](const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                try {
                    if (dice::WebAuth::instance().hasPassword()) {
                        cb(jResp({{"code", 403}, {"message", "口令已设置"}}, drogon::k403Forbidden));
                        return;
                    }
                    auto j = nlohmann::json::parse(req->getBody());
                    std::string pw = j.value("password", "");
                    if (pw.size() < 4) { cb(jResp({{"code", 400}, {"message", "口令至少 4 位"}}, drogon::k400BadRequest)); return; }
                    configMgr.set<std::string>("webui/password", dice::WebAuth::instance().hashPassword(pw));
                    configMgr.save();
                    dice::WebAuth::instance().setPassword(pw);
                    auto resp = jResp({{"code", 0}, {"message", "ok"}, {"data", {{"authed", true}}}});
                    drogon::Cookie ck("dice_session", dice::WebAuth::instance().issueToken(false));
                    ck.setPath("/"); ck.setHttpOnly(true);
                    ck.setSameSite(drogon::Cookie::SameSite::kLax);
                    resp->addCookie(ck);
                    cb(resp);
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
                        configMgr.set<std::string>("webui/password", dice::WebAuth::instance().hashPassword(pw));
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
    // 自响应（用骰娘账号自身发指令自控）：默认关。
    dice::OneBotV11Adapter::s_respondSelf = configMgr.get<bool>("dice/respond_self", false);

    // 图片发送方式（[img,file=..] 发送期解析要读 dice/image_send 配置）。
    dice::imgsend::init(configMgr);

    // QQ 官方富媒体：本地文件现在走官方 upload_prepare 分片直传，不再依赖图床；
    // 发布器仅作为兼容性注册保留（generic=图床上传；local=public_base 直链）。
    dice::QQOfficialAdapter::setImagePublisher([&configMgr](const std::string& local) -> std::string {
        namespace ih = dice::imghost;
        const std::string m = ih::mode(configMgr);
        if (m == "generic") { auto u = ih::uploadGeneric(configMgr, local); return u.value_or(std::string()); }
        if (m == "local") {
            std::string norm = local; for (auto& c : norm) if (c == '\\') c = '/';
            if (norm.rfind("data/assets/", 0) == 0) return ih::resolveRef(configMgr, norm);
            if (norm.rfind("data/chat/images/", 0) == 0) {
                std::string base = ih::publicBase(configMgr);
                if (base.empty()) return {};
                if (base.back() == '/') base.pop_back();
                return base + "/api/chat/images/" + norm.substr(std::string("data/chat/images/").size());
            }
        }
        return {};
    });

    // ── 心跳上报 + 云黑名单服务（heart.dice.zone / cloudban.dice.zone）──
    // 单例 init 注入依赖；本地黑名单 CRUD 经回调转发到 cmdRouter（自带去重）。
    dice::heart::HeartService::instance().init(&configMgr, &adapterMgr, [st]() {
        long long total = 0;
        try {
            for (const auto& player : st->get_all<dice::PlayerProfileRow>())
                total += player.cmdCount;
        } catch (...) {}
        return total;
    }, [st](const std::string& qq) {
        namespace orm = sqlite_orm;
        try {
            auto rows = st->get_all<dice::PlayerProfileRow>(orm::where(
                orm::c(&dice::PlayerProfileRow::platform) == std::string("onebot_v11") and
                orm::c(&dice::PlayerProfileRow::userId) == qq), orm::limit(1));
            if (!rows.empty()) return rows.front().nickname;
        } catch (...) {}
        return std::string();
    });
    dice::cloudban::CloudbanService::instance().init(&configMgr,
        [&cmdRouter](int t, int l, const std::string& id) { return cmdRouter.cloudBanHas(t, l, id); },
        [&cmdRouter](int t, int l, const std::string& id, const std::string& r) { cmdRouter.cloudBanAdd(t, l, id, r); },
        [&cmdRouter](int t, int l, const std::string& id) { return cmdRouter.cloudBanRemove(t, l, id); },
        [&cmdRouter]() { return cmdRouter.banlistAll(); });
    // 主人 .blackqq/.blackgroup 主动拉黑 → 云黑上报（分享开启时；服务内部做门控与去重）。
    cmdRouter.onMasterBan = [](const std::string& tt, const std::string& id, const std::string& reason) {
        dice::cloudban::CloudbanService::instance().reportToCloud(tt, id, "other", reason);
    };

    // ── Register real REST API endpoints ─────────────────────
    dice::utils::setStartupEpoch();
    dice::api::registerApiRoutes(db, configMgr, adapterMgr, engine, cardDeck, replyManager, i18n, jsMod, luaMod,
                                 causalMgr, cooldownMgr, counterStore, personaMgr);
    DICE_LOG_INFO("REST API routes registered");

    // ── Playground test harness ──────────────────────────────
    // The chat UI lives in the React app (sidebar → 测试台). This is the
    // backend it calls: run a fake message through the command router.
    // ── 定时任务「立即执行」（#48 网页测试按钮）────────────────
    // 无视触发时刻/当日去重/因果条件直接执行动作（前端对 leave 任务有确认弹窗），
    // 不写 lastRun（手动执行不消耗当日的定时触发）。返回 conditionMet 供前端提示
    // 「当前条件不满足，定时触发时会被跳过」。
    app.registerHandler("/api/schedules/{1}/run",
        [&db, &cmdRouter](const drogon::HttpRequestPtr&,
                          std::function<void(const drogon::HttpResponsePtr&)>&& cb, const std::string& sid) {
            nlohmann::json out;
            try {
                auto* st = db.getStorage();
                int id = 0; try { id = std::stoi(sid); } catch (...) {}
                auto tk = st ? st->get_pointer<dice::ScheduledTaskRow>(id) : nullptr;
                if (!tk) { out = {{"code", 1}, {"message", "task not found"}}; }
                else {
                    // 单目标：无视条件强制执行，并回报 conditionMet 供前端提示；
                    // "*" 全群任务：任何时候都尊重条件（强制对所有群退群=自毁），
                    // conditionMet 恒 true，count 表示实际命中的群数。
                    bool condMet = tk->targetId == "*"
                        || cmdRouter.evalScheduledCondition(tk->condition, tk->platform, tk->targetType, tk->targetId);
                    int count = cmdRouter.execScheduledAction(*tk, /*force=*/true);
                    DICE_LOG_INFO("scheduled task '{}' 手动执行：count={} conditionMet={}", tk->name, count, condMet);
                    out = {{"code", 0}, {"data", {{"executed", count > 0}, {"count", count}, {"conditionMet", condMet}}}};
                }
            } catch (const std::exception& e) { out = {{"code", 1}, {"message", e.what()}}; }
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(out.dump());
            cb(resp);
        }, {drogon::Post});

    // ── 自定义回复「真引擎测试」───────────────────────────────
    // 网页回复页的实时预览以前是前端自己模拟匹配：四种模式的语义全部和后端
    // 对不上（keyword 当成包含、前缀区分大小写、优先级还排反了）。这里直接
    // 问真引擎：candidates=按真实顺序的全部命中规则；rule=完整触发管线（范围/
    // 冷却）选中的那条（dry-run：不消耗冷却、不掷概率）；reply=渲染后的回复。
    app.registerHandler("/api/replies/test",
        [&cmdRouter, &replyManager](const drogon::HttpRequestPtr& req,
                                    std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            nlohmann::json out;
            try {
                auto body = nlohmann::json::parse(req->getBody());
                const std::string text = body.value("text", std::string());
                dice::Message msg;
                msg.id = "reply-test";
                msg.platform = body.value("platform", std::string("onebot_v11"));
                msg.content = text; msg.rawContent = text; msg.displayContent = text;
                msg.senderId = body.value("userId", std::string("10001"));
                msg.senderName = body.value("nickname", std::string("\xe6\xb5\x8b\xe8\xaf\x95\xe5\x91\x98"));
                std::string gid = body.value("groupId", std::string());
                msg.type = gid.empty() ? dice::MessageType::kPrivate : dice::MessageType::kGroup;
                msg.targetId = gid.empty() ? msg.senderId : gid;

                auto candidates = replyManager.matchMessage(text);
                nlohmann::json cand = nlohmann::json::array();
                for (auto& r : candidates)
                    cand.push_back({{"id", r.id}, {"priority", r.priority},
                                    {"matchType", dice::matchTypeToString(r.matchType)},
                                    {"matchContent", r.matchContent},
                                    {"prob", r.prob}, {"cooldownSec", r.cooldownSec}});
                dice::ReplyCtx rctx{msg.platform, gid, msg.senderId};
                auto pk = replyManager.pickReply(text, rctx, /*commit=*/false);
                nlohmann::json skipped = nlohmann::json::array();
                for (auto& s : pk.skipped) skipped.push_back({{"id", s.id}, {"reason", s.reason}});
                std::string rendered;
                if (pk.rule)
                    rendered = cmdRouter.renderReply(msg, replyManager.pickResult(*pk.rule),
                                                     pk.rule->matchContent, pk.rule->matchType);
                else if (!pk.notice.empty())
                    rendered = cmdRouter.renderReply(msg, pk.notice, "", dice::MatchType::kKeyword);
                out = {{"code", 0}, {"data", {
                    {"matched", pk.rule.has_value()},
                    {"ruleId", pk.rule ? nlohmann::json(pk.rule->id) : nlohmann::json()},
                    {"prob", pk.rule ? pk.rule->prob : 100},
                    {"reply", rendered},
                    {"notice", !pk.notice.empty()},
                    {"noticeRuleId", pk.noticeRuleId},
                    {"candidates", cand},
                    {"skipped", skipped}
                }}};
            } catch (const std::exception& e) { out = {{"code", 1}, {"message", e.what()}}; }
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(out.dump());
            cb(resp);
        }, {drogon::Post});

    app.registerHandler("/api/test/message",
        [&cmdRouter, &jsMod, &configMgr, &db, &makeAiTool, replyFallback](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            nlohmann::json out;
            try {
                auto body = nlohmann::json::parse(req->getBody());
                dice::Message msg;
                msg.id         = "playground";
                msg.platform   = body.value("platform", std::string("onebot_v11"));
                msg.content    = body.value("text", std::string(""));
                msg.rawContent = body.value("rawContent", msg.content);   // 可带 CQ 图码测识图
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
                if (!cmdRouter.mentionsOtherKnownDiceBot(msg)
                    && (!cmdRouter.isForAnotherBot(msg) || jsCommandMatches(jsMod, cmdRouter, msg))
                    && !cmdRouter.isBlocked(msg)) {
                    bool disabled = cmdRouter.isGroupDisabled(msg);
                    bool forcedByAt = dice::CommandRouter::isAtSelf(msg) && !cmdRouter.isGroupLocked(msg);
                    bool replyOff = msg.type == dice::MessageType::kGroup && !msg.targetId.empty()
                                    && cmdRouter.isReplyDisabledFor(msg.platform, msg.targetId);
                    reply = cmdRouter.handleMessage(msg, forced);
                    bool didCommand = !reply.empty();
                    std::string replySrc = "builtin";   // 来源分类（同 live 管线）
                    if (reply.empty() && (!disabled || forcedByAt) && !replyOff)
                        reply = replyFallback(msg, replySrc);   // 与 live 完全同链（含因果规则）
                    if (!reply.empty() && replySrc == "plugin_command") didCommand = true;
                    // 智能化阶段A：测试台也走 AI 对话（无其它回复+触发时），方便骰主预览。
                    // 测试台不读 chat.db 上下文（用空上下文），仅验证触发+生成+发送。
                    if (reply.empty() && !disabled && (dice::aichat::enabled(configMgr) || dice::ainpc::enabled(configMgr))
                        && (msg.targetId.empty() || (cmdRouter.aiEnabledForGroup(msg.platform, msg.targetId)
                            && cmdRouter.aiWhitelistOk(msg.platform, msg.targetId, true)))) {  // 开关 + AI 白名单
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
                            // 测试台也走图像识别注入（便于骰主预览）。
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
                    // 测试台同样走 AI 润色 + 翻译（与 live 一致），方便骰主在
                    // 「指令测试」页直接预览效果；失败/破坏数字回退原文。
                    std::string aiCat = (replySrc == "plugin" || replySrc == "plugin_command") ? "plugin"
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
                    // 测试台沿用统计行为，但绝不能伪造真实的群聊使用资格。
                    cmdRouter.recordPlayerActivity(msg, didCommand, false);
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

    // ── 可视化生成器：对未保存的规则包 JSON 实时测试一条指令 ──────────
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

    // ── Lua mod 管理（列表 / 启停 / 删除 / 重载）──────────────────
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
                nlohmann::json item = {{"name", m.name}, {"title", m.title}, {"author", m.author},
                                       {"version", m.version}, {"brief", m.brief}, {"enabled", m.enabled},
                                       {"replies", m.replies}, {"scripts", m.scripts}, {"helpTopics", help},
                                       {"commands", cmds},
                                       {"singleFile", m.singleFile}, {"ruleCompat", m.ruleCompat}};
                if (auto owner = dice::CommandRouter::pluginOwnerBundle("lua:" + m.name)) {
                    item["ownerBundle"] = owner->first;
                    item["ownerBundleFolder"] = owner->second;
                }
                arr.push_back(std::move(item));
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
        // 导入 Lua mod：上传 .lua/.json/.zip（base64）。归位语义见 LuaPluginManager::importUpload
        //（对齐原版：json+目录成对、一包多 mod、纯 json 查询类、仅 model/rulebook 的规则类都认）。
        app.registerHandler("/api/mod/lua/upload",
            [jsonResp, luaList, &luaMod, &configMgr](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                try {
                    auto j = nlohmann::json::parse(req->getBody());
                    std::string filename = j.value("filename", std::string("mod.zip"));
                    std::string content = j.value("content", std::string());
                    // dataURL 前缀（data:...;base64,）剥离
                    if (auto comma = content.find(",base64,"); comma != std::string::npos) content = content.substr(comma + 8);
                    else if (auto c2 = content.find(','); c2 != std::string::npos && content.rfind("data:", 0) == 0) content = content.substr(c2 + 1);
                    std::string bytes = drogon::utils::base64Decode(content);
                    const bool dryRun = j.value("dry_run", false);
                    std::string err; std::vector<std::string> names, perms, risks;
                    if (!dryRun) {
                        // 真正安装时才校验签名；预检只展示权限/风险。
                        std::string verr;
                        if (!dice::pluginverify::verify(configMgr.get<std::string>("dice/plugin_verify_key", ""),
                                                        bytes, j.value("signature", std::string()), verr)) {
                            cb(jsonResp({{"code", 1}, {"message", verr}}));
                            return;
                        }
                    }
                    if (!luaMod.importUpload(filename, bytes, &err, &names, dryRun, &perms, &risks)) {
                        cb(jsonResp({{"code", 1}, {"message", err}}));
                        return;
                    }
                    if (dryRun) {
                        cb(jsonResp({{"code", 0}, {"message", "ok"}, {"data", {{"dry_run", true}, {"permissions", perms}, {"risks", risks}}}}));
                        return;
                    }
                    luaMod.reload();
                    cb(jsonResp({{"code", 0}, {"message", "ok"}, {"data", {{"mods", luaList()}, {"permissions", perms}, {"risks", risks}}}}));
                } catch (const std::exception& e) { cb(jsonResp({{"code", 1}, {"message", e.what()}})); }
            }, {drogon::Post});
    }

    // ── 插件分群启停（地基）：列出全部插件 + 在某群的启用状态；按群切换 ──
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
                std::string adapterId = req->getParameter("adapterId");
                nlohmann::json arr = nlohmann::json::array();
                for (auto& m : luaMod.mods()) {
                    std::string id = "lua:" + m.name;
                    arr.push_back({{"id", id}, {"name", m.title.empty() ? m.name : m.title},
                                   {"kind", "lua"}, {"enabledGlobal", m.enabled},
                                   {"enabledInGroup", cmdRouter.isPluginEnabledInGroup(platform, group, id, adapterId)}});
                }
                for (auto& p : jsMod.listAll()) {
                    std::string file = p.file;
                    const std::string sfx = ".disabled";
                    if (file.size() > sfx.size() && file.substr(file.size() - sfx.size()) == sfx)
                        file = file.substr(0, file.size() - sfx.size());
                    std::string id = "js:" + file;
                    arr.push_back({{"id", id}, {"name", p.name.empty() ? file : p.name},
                                   {"kind", "js"}, {"enabledGlobal", p.enabled},
                                   {"enabledInGroup", cmdRouter.isPluginEnabledInGroup(platform, group, id, adapterId)}});
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
                    std::string adapterId = b.value("adapterId", std::string());
                    if (group.empty() || id.empty()) { cb(jResp({{"code", 1}, {"message", "group/pluginId required"}})); return; }
                    cmdRouter.setPluginEnabledInGroup(platform, group, id, enabled, adapterId);
                    cb(jResp({{"code", 0}, {"message", "ok"}}));
                } catch (const std::exception& e) { cb(jResp({{"code", 1}, {"message", e.what()}})); }
            }, {drogon::Post});
    }

    // ── 规则包 bundle 管理（data/rulepacks/<包>/）：列表 / 上传zip / 启停 / 删除 ──
    {
        namespace fs = std::filesystem;
        static std::mutex rulepackTxnMutex;
        static std::atomic<unsigned long long> rulepackTxnSeq{0};
        auto jResp = [](const nlohmann::json& out) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(out.dump());
            return resp;
        };
        auto reloadPacks = [&luaMod, &jsMod]() -> std::string {
            try {
                dice::CommandRouter::reloadRulePacks({"rules", "data/rules"});
                dice::CommandRouter::loadHelpDocs();
                std::vector<std::string> luaDirs, jsDirs;
                dice::CommandRouter::packPluginDirs(luaDirs, jsDirs);
                luaMod.setExtraDirs(luaDirs); luaMod.reload();
                jsMod.setExtraDirs(jsDirs);
                jsMod.reload(jsMod.pluginDir().empty() ? "data/plugins/js" : jsMod.pluginDir());
                dice::CommandRouter::reloadJsGameSystems(jsMod.gameSystemTemplates());
                return {};
            } catch (const std::exception& e) {
                DICE_LOG_ERROR("Rule-pack hot reload failed without stopping service: {}", e.what());
                return e.what();
            } catch (...) {
                DICE_LOG_ERROR("Rule-pack hot reload failed without stopping service: unknown error");
                return "unknown reload error";
            }
        };
        auto bundleList = []() {
            nlohmann::json arr = nlohmann::json::array();
            std::shared_lock<std::shared_mutex> lk(dice::rulesLock());
            for (auto& b : dice::CommandRouter::rulePackBundles())
                arr.push_back({{"name", b.name}, {"folder", b.folder}, {"version", b.version},
                               {"author", b.author}, {"description", b.description}, {"enabled", b.enabled},
                               {"setKeys", b.setKeys}, {"ruleFiles", b.ruleFiles}, {"cmdCount", b.cmdCount},
                               {"helpdocEntries", b.helpdocEntries}, {"luaMods", b.luaMods}, {"jsPlugins", b.jsPlugins},
                               {"ruleNames", b.ruleNames}, {"helpdocFiles", b.helpdocFiles},
                               {"luaNames", b.luaNames}, {"jsNames", b.jsNames}});
            return arr;
        };
        auto safeFolder = [](const std::string& value) {
            if (value.empty() || value.size() > 128 || value == "." || value == "..") return false;
            if (value.back() == '.' || value.back() == ' ') return false;
            for (unsigned char c : value) {
                if (c < 0x20 || c == 0x7f || c == '<' || c == '>' || c == ':' || c == '"' ||
                    c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') return false;
            }
            std::string upper = value;
            std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return (char)std::toupper(c); });
            const auto dot = upper.find('.'); if (dot != std::string::npos) upper.resize(dot);
            static const std::unordered_set<std::string> reserved = {
                "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
                "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
            };
            return reserved.find(upper) == reserved.end();
        };
        auto safeArchiveEntry = [](std::string entry) {
            if (!entry.empty() && entry.back() == '\r') entry.pop_back();
            if (entry.empty() || entry.front() == '/' || entry.front() == '\\' || entry.find('\\') != std::string::npos ||
                entry.find(':') != std::string::npos || entry.find('\0') != std::string::npos) return false;
            std::istringstream parts(entry); std::string part;
            while (std::getline(parts, part, '/')) if (part.empty() || part == "." || part == "..") return false;
            return true;
        };
        auto nextTag = []() {
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            return std::to_string(now) + "_" + std::to_string(rulepackTxnSeq.fetch_add(1));
        };        // 读取 ZIP central directory，在真正解压前限制总展开大小，并校验 central/local 文件名一致。
        // 这样恶意压缩包无法先把磁盘撑满再被事后检查。
        auto validateZipBytes = [safeArchiveEntry](const std::string& bytes, std::string& error) {
            auto u16 = [&](size_t p) -> uint16_t { return (uint16_t)((unsigned char)bytes[p] | ((unsigned char)bytes[p + 1] << 8)); };
            auto u32 = [&](size_t p) -> uint32_t { return (uint32_t)u16(p) | ((uint32_t)u16(p + 2) << 16); };
            auto sig = [&](size_t p, uint32_t value) { return p + 4 <= bytes.size() && u32(p) == value; };
            if (bytes.size() < 22) { error = "不是有效的 ZIP 文件"; return false; }
            const size_t minEocd = bytes.size() > 65557 ? bytes.size() - 65557 : 0;
            size_t eocd = bytes.size() - 22; bool found = false;
            for (;;) { if (sig(eocd, 0x06054b50u)) { found = true; break; } if (eocd == minEocd) break; --eocd; }
            if (!found || eocd + 22u + u16(eocd + 20) > bytes.size()) { error = "ZIP 目录损坏"; return false; }
            const uint16_t disk = u16(eocd + 4), centralDisk = u16(eocd + 6);
            const uint16_t entriesOnDisk = u16(eocd + 8), entryCount = u16(eocd + 10);
            const uint32_t centralSize = u32(eocd + 12), centralOffset = u32(eocd + 16);
            if (disk != 0 || centralDisk != 0 || entriesOnDisk != entryCount || entryCount == 0 || entryCount == 0xffffu ||
                centralSize == 0xffffffffu || centralOffset == 0xffffffffu) { error = "不支持分卷、空包或 ZIP64 规则包"; return false; }
            if (entryCount > 2048 || (uint64_t)centralOffset + centralSize > bytes.size()) { error = "ZIP 文件数量或目录范围异常"; return false; }
            size_t pos = centralOffset; uint64_t total = 0; std::unordered_set<std::string> names;
            for (uint16_t i = 0; i < entryCount; ++i) {
                if (!sig(pos, 0x02014b50u) || pos + 46 > bytes.size()) { error = "ZIP central directory 损坏"; return false; }
                const uint16_t flags = u16(pos + 8), method = u16(pos + 10);
                const uint32_t expanded = u32(pos + 24), localOffset = u32(pos + 42), external = u32(pos + 38);
                const uint16_t nameLen = u16(pos + 28), extraLen = u16(pos + 30), commentLen = u16(pos + 32);
                const size_t next = pos + 46u + nameLen + extraLen + commentLen;
                if (nameLen == 0 || next > bytes.size() || next > (uint64_t)centralOffset + centralSize) { error = "ZIP 文件名或目录项损坏"; return false; }
                std::string name = bytes.substr(pos + 46, nameLen);
                if (!safeArchiveEntry(name)) { error = "压缩包包含不安全的文件路径"; return false; }
                std::string folded = name; std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                if (!names.insert(folded).second) { error = "压缩包包含重复或大小写冲突的文件路径"; return false; }
                if ((flags & 1u) != 0 || (method != 0 && method != 8)) { error = "规则包不支持加密或特殊压缩算法"; return false; }
                const unsigned unixMode = (external >> 16) & 0170000u;
                if (unixMode == 0120000u) { error = "规则包不允许包含符号链接"; return false; }
                total += expanded;
                if (total > 128u * 1024u * 1024u) { error = "规则包解压后不能超过 128 MiB"; return false; }
                if (!sig(localOffset, 0x04034b50u) || localOffset + 30 > bytes.size()) { error = "ZIP local header 损坏"; return false; }
                const uint16_t localNameLen = u16(localOffset + 26), localExtraLen = u16(localOffset + 28);
                if ((uint64_t)localOffset + 30u + localNameLen + localExtraLen > bytes.size() ||
                    bytes.compare(localOffset + 30, localNameLen, name) != 0) { error = "ZIP 文件名索引不一致"; return false; }
                pos = next;
            }
            return true;
        };
        app.registerHandler("/api/rulepacks",
            [jResp, bundleList](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                cb(jResp({{"code", 0}, {"message", "ok"}, {"data", {{"bundles", bundleList()}}}}));
            }, {drogon::Get});
        app.registerHandler("/api/rulepacks/upload",
            [jResp, bundleList, reloadPacks, safeFolder, validateZipBytes, nextTag, &jsMod, &luaMod](const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                std::lock_guard<std::mutex> transaction(rulepackTxnMutex);
                namespace fs = std::filesystem;
                fs::path zipPath, tmpDir, installed;
                std::error_code ec;
                auto cleanup = [&]() { if (!zipPath.empty()) fs::remove(zipPath, ec); if (!tmpDir.empty()) fs::remove_all(tmpDir, ec); };
                auto fail = [&](const std::string& message) { cleanup(); cb(jResp({{"code", 1}, {"message", message}})); };
                try {
                    auto j = nlohmann::json::parse(req->getBody());
                    std::string filename = j.value("filename", std::string("pack.zip"));
                    std::string content = j.value("content", std::string());
                    if (auto comma = content.find(",base64,"); comma != std::string::npos) content = content.substr(comma + 8);
                    else if (auto c2 = content.find(','); c2 != std::string::npos && content.rfind("data:", 0) == 0) content = content.substr(c2 + 1);
                    std::string bytes = drogon::utils::base64Decode(content);
                    constexpr size_t kMaxZipBytes = 32u * 1024u * 1024u;
                    if (bytes.empty()) { fail("规则包为空或 Base64 无效"); return; }
                    if (bytes.size() > kMaxZipBytes) { fail("规则包压缩文件不能超过 32 MiB"); return; }
                    fs::create_directories("data/rulepacks", ec);
                    if (ec) { fail("无法创建规则包目录：" + ec.message()); return; }
                    const std::string tag = nextTag();
                    zipPath = fs::path("data/rulepacks") / ("_imp_" + tag + ".zip");
                    tmpDir  = fs::path("data/rulepacks") / ("_imp_" + tag);
                    { std::ofstream f(zipPath, std::ios::binary); if (!f) { fail("无法保存上传文件"); return; }
                      f.write(bytes.data(), (std::streamsize)bytes.size()); if (!f) { fail("上传文件写入失败"); return; } }

                    std::string zipError;
                    if (!validateZipBytes(bytes, zipError)) { fail(zipError); return; }


                    fs::create_directories(tmpDir, ec);
                    if (ec || std::system(dice::backup::archiveExtractCommand(zipPath, tmpDir).c_str()) != 0) {
                        fail("规则包解压失败"); return;
                    }
                    fs::remove(zipPath, ec); zipPath.clear();

                    size_t fileCount = 0; uintmax_t totalBytes = 0;
                    for (fs::recursive_directory_iterator it(tmpDir, fs::directory_options::skip_permission_denied, ec), end;
                         it != end; it.increment(ec)) {
                        if (ec) { fail("无法检查解压后的规则包"); return; }
                        const auto status = it->symlink_status(ec);
                        if (ec || fs::is_symlink(status)) { fail("规则包不允许包含符号链接"); return; }
                        if (fs::is_regular_file(status)) {
                            if (++fileCount > 2048) { fail("规则包文件数量超过 2048 个"); return; }
                            totalBytes += it->file_size(ec);
                            if (ec || totalBytes > 128u * 1024u * 1024u) { fail("规则包解压后不能超过 128 MiB"); return; }
                        }
                    }

                    auto hasManifest = [](const fs::path& d) { std::error_code e; return fs::is_regular_file(d / "pack.json", e); };
                    fs::path src;
                    if (hasManifest(tmpDir)) src = tmpDir;
                    else {
                        int dirs = 0; fs::path only;
                        for (auto& e : fs::directory_iterator(tmpDir, ec)) if (e.is_directory()) { ++dirs; only = e.path(); }
                        if (dirs == 1 && hasManifest(only)) src = only;
                    }
                    if (src.empty()) { fail("规则包必须在根目录或唯一子目录中包含 pack.json"); return; }

                    nlohmann::json manifest;
                    try { std::ifstream pf(src / "pack.json", std::ios::binary); manifest = nlohmann::json::parse(pf); }
                    catch (...) { fail("pack.json 不是有效的 JSON"); return; }
                    if (!manifest.is_object()) { fail("pack.json 必须是 JSON 对象"); return; }
                    const std::string displayName = manifest.value("name", std::string());
                    if (displayName.empty() || displayName.size() > 128) { fail("pack.json 的 name 必须为 1-128 字节"); return; }

                    std::vector<std::string> setKeys, newJsNames, newLuaNames;
                    auto addSetKeys = [&](const nlohmann::json& values, const std::string& field) {
                        if (!values.is_array()) throw std::runtime_error(field + " 必须是字符串数组");
                        for (const auto& value : values) {
                            if (!value.is_string() || value.get<std::string>().empty()) throw std::runtime_error(field + " 包含空值或非字符串");
                            const std::string key = value.get<std::string>();
                            if (key.size() > 64 || key.find_first_of(" \t\r\n") != std::string::npos)
                                throw std::runtime_error(field + " 的激活键不能超过 64 字节或包含空白");
                            if (std::none_of(setKeys.begin(), setKeys.end(), [&](const std::string& old) { return dice::utils::toLower(old) == dice::utils::toLower(key); }))
                                setKeys.push_back(key);
                        }
                    };
                    if (manifest.contains("setKeys")) addSetKeys(manifest["setKeys"], "pack.json.setKeys");

                    size_t supportedFiles = 0;
                    const fs::path rulesDir = src / "rules";
                    if (fs::is_directory(rulesDir, ec)) for (const auto& entry : fs::directory_iterator(rulesDir, ec)) {
                        if (!entry.is_regular_file()) continue;
                        const std::string file = dnx_u8str(entry.path().filename());
                        if (!(entry.path().extension() == ".json" || (file.size() > 14 && file.substr(file.size() - 14) == ".json.disabled"))) continue;
                        ++supportedFiles;
                        try {
                            std::ifstream f(entry.path(), std::ios::binary); auto rule = nlohmann::json::parse(f);
                            if (!rule.is_object()) throw std::runtime_error("not object");
                            if (rule.contains("set") && rule["set"].is_object() && rule["set"].contains("keys"))
                                addSetKeys(rule["set"]["keys"], file + ".set.keys");
                        } catch (const std::exception& e) { fail("规则文件 " + file + " 无效：" + e.what()); return; }
                    }
                    const fs::path helpDir = src / "helpdoc";
                    if (fs::is_directory(helpDir, ec)) for (fs::recursive_directory_iterator it(helpDir, ec), end; it != end; it.increment(ec)) {
                        if (ec) { fail("无法读取 helpdoc 目录"); return; }
                        if (!it->is_regular_file() || it->path().extension() != ".json") continue;
                        ++supportedFiles; const std::string file = dnx_u8str(it->path().filename());
                        try {
                            std::ifstream f(it->path(), std::ios::binary); auto doc = nlohmann::json::parse(f);
                            if (!doc.is_object() || !doc.contains("helpdoc") || !doc["helpdoc"].is_object()) throw std::runtime_error("缺少 helpdoc 对象");
                        } catch (const std::exception& e) { fail("帮助文件 " + file + " 无效：" + e.what()); return; }
                    }
                    const fs::path jsDir = src / "js";
                    if (fs::is_directory(jsDir, ec)) for (const auto& entry : fs::directory_iterator(jsDir, ec))
                        if (entry.is_regular_file() && entry.path().extension() == ".js") { ++supportedFiles; newJsNames.push_back(dnx_u8str(entry.path().filename())); }
                    const fs::path luaDir = src / "lua";
                    if (fs::is_directory(luaDir, ec)) for (const auto& entry : fs::directory_iterator(luaDir, ec)) {
                        if (entry.is_directory()) { ++supportedFiles; newLuaNames.push_back(dnx_u8str(entry.path().filename())); }
                        else if (entry.is_regular_file() && entry.path().extension() == ".lua") { ++supportedFiles; newLuaNames.push_back(dnx_u8str(entry.path().stem())); }
                    }
                    if (supportedFiles == 0) { fail("规则包没有可加载的 rules、helpdoc、lua 或 js 内容"); return; }
                    if (setKeys.empty()) { fail("规则包必须通过 pack.json.setKeys 或 rules/*.json 的 set.keys 声明至少一个激活键"); return; }
                    for (const auto& key : setKeys) if (auto owner = dice::CommandRouter::ruleSetKeyOwner(key)) {
                        fail("激活键 .set " + key + " 已被「" + *owner + "」使用"); return;
                    }
                    auto sameName = [](const std::string& a, const std::string& b) { return dice::utils::toLower(a) == dice::utils::toLower(b); };
                    for (const auto& file : newJsNames) {
                        if (auto owner = dice::CommandRouter::pluginOwnerBundle("js:" + file)) { fail("JS 插件 " + file + " 已属于规则包「" + owner->first + "」"); return; }
                        for (const auto& plugin : jsMod.listAll()) {
                            std::string existing = plugin.file; const std::string suffix = ".disabled";
                            if (existing.size() > suffix.size() && existing.substr(existing.size() - suffix.size()) == suffix) existing.resize(existing.size() - suffix.size());
                            if (sameName(existing, file)) { fail("JS 插件文件名冲突：" + file); return; }
                        }
                    }
                    for (const auto& name : newLuaNames) {
                        if (auto owner = dice::CommandRouter::pluginOwnerBundle("lua:" + name)) { fail("Lua 模组 " + name + " 已属于规则包「" + owner->first + "」"); return; }
                        for (const auto& mod : luaMod.mods()) if (sameName(mod.name, name)) { fail("Lua 模组名称冲突：" + name); return; }
                    }

                    std::string folder;
                    if (manifest.contains("id") && manifest["id"].is_string()) folder = manifest["id"].get<std::string>();
                    else if (src != tmpDir) folder = dnx_u8str(src.filename());
                    else { folder = dnx_u8str(fs::path(std::u8string(filename.begin(), filename.end())).filename()); const auto dot = folder.rfind('.'); if (dot != std::string::npos) folder.resize(dot); }
                    if (!safeFolder(folder)) { fail("规则包目录标识无效；可在 pack.json 中设置安全的 id"); return; }

                    fs::path dest = u8path("data/rulepacks/" + folder);
                    fs::path disabled = u8path("data/rulepacks/" + folder + ".disabled");
                    if (fs::exists(dest, ec) || fs::exists(disabled, ec)) {
                        fail("同标识规则包已存在，请先删除旧包再导入（不会自动覆盖）"); return;
                    }
                    fs::rename(src, dest, ec);
                    if (ec) {
                        ec.clear(); fs::path staging = u8path("data/rulepacks/_install_" + tag);
                        fs::copy(src, staging, fs::copy_options::recursive, ec);
                        if (ec) { fs::remove_all(staging, ec); fail("安装规则包失败：" + ec.message()); return; }
                        fs::rename(staging, dest, ec);
                        if (ec) { fs::remove_all(staging, ec); fail("安装规则包失败：" + ec.message()); return; }
                    }
                    installed = dest; cleanup();
                    if (const std::string reloadError = reloadPacks(); !reloadError.empty()) {
                        fs::remove_all(installed, ec); (void)reloadPacks();
                        cb(jResp({{"code", 1}, {"message", "规则包加载失败，安装已回滚：" + reloadError}})); return;
                    }
                    cb(jResp({{"code", 0}, {"message", "ok"}, {"data", {{"bundles", bundleList()}}}}));
                } catch (const std::exception& e) { fail(e.what()); }
            }, {drogon::Post});
        app.registerHandler("/api/rulepacks/toggle",
            [jResp, bundleList, reloadPacks, safeFolder](const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                std::lock_guard<std::mutex> transaction(rulepackTxnMutex);
                namespace fs = std::filesystem;
                try {
                    auto j = nlohmann::json::parse(req->getBody());
                    std::string folder = j.value("folder", std::string()); bool enable = j.value("enabled", true);
                    const std::string sfx = ".disabled"; std::string base = folder;
                    if (base.size() > sfx.size() && base.substr(base.size() - sfx.size()) == sfx) base.resize(base.size() - sfx.size());
                    if (!safeFolder(base)) { cb(jResp({{"code", 1}, {"message", "规则包目录标识无效"}})); return; }
                    fs::path on = u8path("data/rulepacks/" + base), off = u8path("data/rulepacks/" + base + sfx);
                    fs::path from = enable ? off : on, to = enable ? on : off; std::error_code ec;
                    if (!fs::is_directory(from, ec)) { cb(jResp({{"code", 1}, {"message", "规则包不存在或状态已改变"}})); return; }
                    if (fs::exists(to, ec)) { cb(jResp({{"code", 1}, {"message", "目标状态目录已存在，未执行覆盖"}})); return; }
                    fs::rename(from, to, ec);
                    if (ec) { cb(jResp({{"code", 1}, {"message", "切换失败：" + ec.message()}})); return; }
                    if (const std::string reloadError = reloadPacks(); !reloadError.empty()) {
                        ec.clear(); fs::rename(to, from, ec); (void)reloadPacks();
                        cb(jResp({{"code", 1}, {"message", "热重载失败，状态已回滚：" + reloadError}})); return;
                    }
                    cb(jResp({{"code", 0}, {"message", "ok"}, {"data", {{"bundles", bundleList()}}}}));
                } catch (const std::exception& e) { cb(jResp({{"code", 1}, {"message", e.what()}})); }
            }, {drogon::Post});
        app.registerHandler("/api/rulepacks/delete",
            [jResp, bundleList, reloadPacks, safeFolder, nextTag](const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                std::lock_guard<std::mutex> transaction(rulepackTxnMutex);
                namespace fs = std::filesystem;
                try {
                    auto j = nlohmann::json::parse(req->getBody());
                    std::string folder = j.value("folder", std::string());
                    std::string base = folder; const std::string sfx = ".disabled";
                    if (base.size() > sfx.size() && base.substr(base.size() - sfx.size()) == sfx) base.resize(base.size() - sfx.size());
                    if (!safeFolder(base) || (folder != base && folder != base + sfx)) {
                        cb(jResp({{"code", 1}, {"message", "规则包目录标识无效"}})); return;
                    }
                    fs::path target = u8path("data/rulepacks/" + folder); std::error_code ec;
                    if (!fs::is_directory(target, ec)) { cb(jResp({{"code", 1}, {"message", "规则包不存在"}})); return; }
                    fs::path trash = u8path("data/.rulepack-trash-" + nextTag());
                    fs::rename(target, trash, ec);
                    if (ec) { cb(jResp({{"code", 1}, {"message", "无法暂存待删除规则包：" + ec.message()}})); return; }
                    if (const std::string reloadError = reloadPacks(); !reloadError.empty()) {
                        ec.clear(); fs::rename(trash, target, ec); (void)reloadPacks();
                        cb(jResp({{"code", 1}, {"message", "热重载失败，删除已回滚：" + reloadError}})); return;
                    }
                    fs::remove_all(trash, ec);
                    if (ec) DICE_LOG_WARN("Rule-pack trash cleanup failed: {}", ec.message());
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
                    // 不再把访问时的 host（往往是 localhost，跨设备失效）烧进
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

    // ── 聊天记录保留期清理（启动后 1 分钟 + 之后每 6 小时）──
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
                dice::chatimg::pruneOld(cutoff);   // 同步清理超期的缓存图片
            } catch (...) {}
        };
        app.getLoop()->runAfter(60.0, chatCleanup);
        app.getLoop()->runEvery(21600.0, chatCleanup);
    }

    // ── 自动清理好友（N 天未在任何位置触发指令 → 删除好友）──
    // dice/friend_clean_days（0=关闭）。豁免：骰主/白名单(信任)用户/trustLevel>0。
    // 无玩家档案的好友跳过（无法判断，宁可不删）；lastCmdAt 为空回退 createdAt。
    {
        auto parseIsoUtc = [](const std::string& s) -> int64_t {
            std::tm tm{}; int y, mo, d, h, mi, se;
#if defined(_WIN32)
            if (sscanf_s(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
#else
            if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
#endif
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
                        DICE_LOG_INFO("自动清理好友：{}（{} 天无指令，最后 {}）", uid, days, lastIso);
                    } catch (...) {}
                }
            }
        };
        app.getLoop()->runAfter(300.0, friendCleanup);     // 启动 5 分钟后（等好友列表同步）
        app.getLoop()->runEvery(43200.0, friendCleanup);   // 之后每 12 小时
    }

    // ── 旧「不活跃自动退群」配置 → 定时任务迁移 ─────────────────
    // 以前是写死每天 04:00 的独立代码路径（config dice/inactive_group_line 天），
    // 和「定时任务 condition=inactive>=N action=leave」重复且不可见。现在迁成
    // 一条 targetId="*"（全部群）的普通任务，网页定时任务页可见可改可停；
    // 迁移后把旧配置清零防止双跑（设置页该字段已移除）。
    try {
        int days = 0;
        {
            nlohmann::json all = configMgr.getAll();
            if (all.contains("dice") && all["dice"].contains("inactive_group_line"))
                days = all["dice"]["inactive_group_line"].get<int>();
        }
        if (days > 0) {
            if (auto* storage = db.getStorage()) {
                const std::string sysName = "\xe4\xb8\x8d\xe6\xb4\xbb\xe8\xb7\x83\xe8\x87\xaa\xe5\x8a\xa8\xe9\x80\x80\xe7\xbe\xa4";   // 不活跃自动退群
                bool exists = false;
                for (auto& tk : storage->get_all<dice::ScheduledTaskRow>())
                    if (tk.name == sysName) {
                        auto up = tk; up.condition = "inactive>=" + std::to_string(days); up.enabled = 1;
                        storage->update(up); exists = true; break;
                    }
                if (!exists) {
                    dice::ScheduledTaskRow tk;
                    tk.name = sysName; tk.platform = ""; tk.targetType = "group"; tk.targetId = "*";
                    tk.cronTime = "04:00"; tk.action = "leave";
                    tk.condition = "inactive>=" + std::to_string(days);
                    tk.content = ""; tk.enabled = 1;
                    storage->insert(tk);
                }
                configMgr.set<int>("dice/inactive_group_line", 0);
                configMgr.save();
                DICE_LOG_INFO("migrated inactive_group_line={} to scheduled task '*' leave", days);
            }
        }
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("inactive_group_line migration failed: {}", e.what());
    }

    // ── 调度循环 (#48 定时任务，含迁移进来的不活跃自动退群)，每 30s 一跳 ──
    app.getLoop()->runEvery(30.0, [&db, &cmdRouter]() {
        auto* st = db.getStorage(); if (!st) return;
        const std::time_t now = std::time(nullptr);
        const std::tm lt = dice::utils::timezoneTm(now);
        const std::string curHM = dice::utils::formatTimeInTimezone(now, "%H:%M");
        const std::string curYMD = dice::utils::formatTimeInTimezone(now, "%Y-%m-%d");
        const int wday = lt.tm_wday;   // 0=周日

        // 定时任务：时刻已过、当天未触发、且在 60 分钟补发窗口内 → 发送。
        // 以前是 cronTime==curHM 的精确分钟匹配：事件循环卡顿/系统睡眠唤醒/DST
        // 跳变（如 02:30 在夏令时被整段跳过）越过那一分钟就整天漏发。补发窗口
        // 覆盖这些场景；停机超过窗口（如半天后才启动）不补发——早安消息深夜才
        // 补发出来更糟——标记当日已处理并留日志。
        try {
            auto hmMin = [](const std::string& s) -> int {   // "HH:MM" → 分钟数；畸形 → -1
                if (s.size() != 5 || s[2] != ':') return -1;
                for (int i : {0, 1, 3, 4}) if (s[i] < '0' || s[i] > '9') return -1;
                return ((s[0]-'0')*10 + (s[1]-'0')) * 60 + (s[3]-'0')*10 + (s[4]-'0');
            };
            const int nowMin = hmMin(curHM);
            // "YYYY-MM-DD HH:MM" → epoch 秒；畸形 → -1（interval 型 lastRun 用）。
            auto parseYmdHm = [](const std::string& s) -> int64_t {
                if (s.size() != 16 || s[4] != '-' || s[7] != '-' || s[10] != ' ' || s[13] != ':') return -1;
                std::tm tm{};
                try { tm.tm_year = std::stoi(s.substr(0, 4)) - 1900;
                      tm.tm_mon  = std::stoi(s.substr(5, 2)) - 1;
                      tm.tm_mday = std::stoi(s.substr(8, 2));
                      tm.tm_hour = std::stoi(s.substr(11, 2));
                      tm.tm_min  = std::stoi(s.substr(14, 2)); } catch (...) { return -1; }
                tm.tm_isdst = -1;
                std::time_t t = std::mktime(&tm);
                return t <= 0 ? -1 : static_cast<int64_t>(t);
            };
            auto fire = [&](dice::ScheduledTaskRow& tk) {
                // 执行（含 targetId="*" 全群展开，条件逐群评估；leave 内部已通知骰主）。
                int done = cmdRouter.execScheduledAction(tk, /*force=*/false);
                if (done <= 0) return;
                DICE_LOG_INFO("scheduled task fired: {} -> {}:{} ({} target(s))",
                              tk.name, tk.platform.empty() ? "any" : tk.platform, tk.targetId, done);
                if (tk.action != "leave") {
                    // B：定时任务触发通知骰主。
                    cmdRouter.notifyMasters(dice::notice::kImportant,
                        "\xe5\xae\x9a\xe6\x97\xb6\xe4\xbb\xbb\xe5\x8a\xa1\xe3\x80\x8c" + tk.name + "\xe3\x80\x8d\xe5\xb7\xb2\xe6\x89\xa7\xe8\xa1\x8c \xe2\x86\x92 " + tk.targetId, "schedule");
                }
            };
            for (auto& tk : st->get_all<dice::ScheduledTaskRow>()) {
                if (!tk.enabled) continue;

                // ── interval：每 N 分钟（lastRun 存 "YYYY-MM-DD HH:MM"）──
                if (tk.triggerType == "interval") {
                    if (tk.intervalMin <= 0) continue;
                    int64_t last = parseYmdHm(tk.lastRun);
                    if (last < 0) {   // 无/坏时间戳 → 从现在起算一个间隔（防导入老数据立刻触发）
                        tk.lastRun = curYMD + " " + curHM; st->update(tk); continue;
                    }
                    if (static_cast<int64_t>(now) - last < static_cast<int64_t>(tk.intervalMin) * 60) continue;
                    tk.lastRun = curYMD + " " + curHM; st->update(tk);
                    fire(tk);
                    continue;
                }

                // ── once：onceDate 当天 cronTime 执行一次后自动停用 ──
                if (tk.triggerType == "once") {
                    if (!tk.lastRun.empty()) continue;   // 已执行/已标记
                    if (tk.onceDate != curYMD) {
                        if (!tk.onceDate.empty() && tk.onceDate < curYMD) {   // 停机跨过了整天
                            tk.lastRun = curYMD + " " + curHM; tk.enabled = 0; st->update(tk);
                            DICE_LOG_INFO("scheduled once task '{}' 已过期（{}），标记完成并停用", tk.name, tk.onceDate);
                        }
                        continue;
                    }
                    const int dueMin = hmMin(tk.cronTime);
                    if (dueMin < 0 || nowMin < dueMin) continue;
                    tk.lastRun = curYMD + " " + curHM; tk.enabled = 0; st->update(tk);   // 单次：跑完即停用
                    if (nowMin - dueMin > 60) {
                        DICE_LOG_INFO("scheduled once task '{}' 超过补发窗口（迟 {} 分钟），跳过并停用", tk.name, nowMin - dueMin);
                        continue;
                    }
                    fire(tk);
                    continue;
                }

                // ── daily（默认）：每日 cronTime + 星期过滤 ──
                if (tk.lastRun == curYMD) continue;
                const int dueMin = hmMin(tk.cronTime);
                if (dueMin < 0 || nowMin < dueMin) continue;   // 畸形时刻 / 今天还没到点
                if (!tk.days.empty()) {
                    bool ok = false; std::stringstream ss(tk.days); std::string d;
                    while (std::getline(ss, d, ',')) { try { if (!d.empty() && std::stoi(d) == wday) { ok = true; break; } } catch (...) {} }
                    if (!ok) continue;
                }
                tk.lastRun = curYMD; st->update(tk);   // 当天已评估（防 30s 重复触发），无论条件是否满足
                if (nowMin - dueMin > 60) {
                    DICE_LOG_INFO("scheduled task '{}' 超过补发窗口（迟 {} 分钟），今日跳过", tk.name, nowMin - dueMin);
                    continue;
                }
                fire(tk);
            }
        } catch (...) {}
        // 注：原「每天 04:00 硬编码不活跃自动退群」已并入定时任务——启动时把
        // config dice/inactive_group_line 迁移为一条 targetId="*" 的可管理任务。
    });

    // ── 自动备份：按间隔或每日时刻执行，按保留天数清理旧档案 ──
    auto autoBackupTick = [&db, &configMgr]() {
        try {
            if (!configMgr.get<bool>("backup/auto_enabled", true)) return;
            const std::time_t now = std::time(nullptr);
            const long long last = configMgr.get<long long>("backup/auto_last_at", 0);
            const std::string schedule = configMgr.get<std::string>("backup/auto_schedule", "interval");
            bool due = false;
            if (schedule == "daily") {
                const std::string at = configMgr.get<std::string>("backup/auto_daily_time", "04:00");
                if (at.size() != 5 || at[2] != ':') return;
                const int dueMin = std::stoi(at.substr(0, 2)) * 60 + std::stoi(at.substr(3, 2));
                const std::tm current = dice::utils::timezoneTm(now);
                std::tm previous{};
                if (last > 0) previous = dice::utils::timezoneTm(static_cast<std::time_t>(last));
                const int currentMin = current.tm_hour * 60 + current.tm_min;
                due = currentMin >= dueMin && (last <= 0 || current.tm_year != previous.tm_year || current.tm_yday != previous.tm_yday);
            } else {
                const int hours = configMgr.get<int>("backup/auto_interval_hours", 24);
                due = last <= 0 || static_cast<long long>(now) - last >= static_cast<long long>(hours) * 3600;
            }
            if (!due) return;
            const auto selection = dice::backup::Selection::fromJson(
                configMgr.get<dice::json>("backup/auto_selection", dice::json::object()));
            std::filesystem::path archive; std::string error;
            if (!dice::backup::createArchive(db, configMgr.configPath(), archive, error, selection, true)) {
                DICE_LOG_ERROR("automatic backup failed: {}", error); return;
            }
            configMgr.set<long long>("backup/auto_last_at", static_cast<long long>(now));
            configMgr.save();
            dice::backup::cleanupArchives(configMgr.get<int>("backup/auto_keep_days", 7));
            DICE_LOG_INFO("automatic backup created: {}", archive.string());
        } catch (const std::exception& e) { DICE_LOG_ERROR("automatic backup failed: {}", e.what()); }
    };
    app.getLoop()->runAfter(20.0, autoBackupTick);
    app.getLoop()->runEvery(60.0, autoBackupTick);

    // Persist a lightweight adapter-availability sample every five minutes.
    // Only aggregate counts are stored; no message or account content is kept.
    auto onlineSampleTick = [st, &adapterMgr]() {
        try {
            dice::OnlineSampleRow sample;
            sample.sampledAt = dice::heart::nowUtcIso();
            for (const auto& adapter : adapterMgr.allAdapters()) {
                ++sample.totalCount;
                if (adapter->isConnected()) ++sample.onlineCount;
            }
            st->insert(sample);
            const std::time_t cutoff = std::time(nullptr) - 90LL * 24 * 60 * 60;
            std::tm utc{};
#if defined(_WIN32)
            gmtime_s(&utc, &cutoff);
#else
            gmtime_r(&cutoff, &utc);
#endif
            char cutoffIso[24];
            std::strftime(cutoffIso, sizeof(cutoffIso), "%Y-%m-%dT%H:%M:%SZ", &utc);
            st->remove_all<dice::OnlineSampleRow>(sqlite_orm::where(
                sqlite_orm::c(&dice::OnlineSampleRow::sampledAt) < std::string(cutoffIso)));
        } catch (const std::exception& e) {
            DICE_LOG_DEBUG("online statistics sample failed: {}", e.what());
        }
    };
    app.getLoop()->runAfter(10.0, onlineSampleTick);
    app.getLoop()->runEvery(300.0, onlineSampleTick);

    // ── 心跳上报 + 云黑同步：启动后首跳 + 每 60s 驱动（服务内部按各自 interval 自行节流）──
    app.getLoop()->runAfter(15.0, [] { dice::heart::HeartService::instance().tick(); });      // 上线立即报（等 adapter 连上）
    app.getLoop()->runAfter(60.0, [] { dice::cloudban::CloudbanService::instance().tick(); }); // 首次云黑同步
    app.getLoop()->runEvery(60.0, [] {
        dice::heart::HeartService::instance().tick();
        dice::cloudban::CloudbanService::instance().tick();
    });

    // System tray icon (Windows): 打开应用目录 / 显示·隐藏控制台 / 打开网页面板 / 退出。
    // 默认启动即最小化到托盘（隐藏控制台+弹气泡+禁用其X）；config dice/console_start_hidden=false 可保留旧行为（启动就显示控制台）。
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

    // 正常退出时同步报一次 offline（max-time 5s，服务端据此结算在线时长）。
    dice::heart::HeartService::instance().shutdownReport();

    if (hotReload) {
        hotReload->stop();
        delete hotReload;
        hotReload = nullptr;
    }

    db.close();
    DICE_LOG_INFO("Dice!Next stopped. Goodbye!");

    return g_requestedExitCode.load();
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
