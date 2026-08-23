cmake_minimum_required(VERSION 3.20)
project(dice-next-server VERSION 3.0.0 LANGUAGES C CXX)

# ─── C++ Standard ─────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── vcpkg Toolchain ─────────────────────────────────────────
# Set CMAKE_TOOLCHAIN_FILE to vcpkg.cmake before configuring,
# or use: cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake

# ─── Find Dependencies ───────────────────────────────────────

# drogon — HTTP framework with WebSocket
find_package(Drogon REQUIRED)
find_package(OpenSSL REQUIRED)

# nlohmann/json — JSON parsing
find_package(nlohmann_json REQUIRED)

# sqlite_orm — ORM for SQLite (header-only)
find_package(SqliteOrm CONFIG REQUIRED)

# spdlog — async logging
find_package(spdlog REQUIRED)

# yaml-cpp — YAML parsing (legacy format support)
find_package(yaml-cpp REQUIRED)

# zlib — SealDice 兼容日志上传（items JSON 需 zlib 压缩），drogon 依赖里已带
find_package(ZLIB REQUIRED)

# zstd — SealDice V105 日志上传（Parquet 页压缩用 zstd）
find_package(zstd CONFIG REQUIRED)
if(TARGET zstd::libzstd_shared)
    set(DICE_ZSTD_TARGET zstd::libzstd_shared)
elseif(TARGET zstd::libzstd_static)
    set(DICE_ZSTD_TARGET zstd::libzstd_static)
else()
    set(DICE_ZSTD_TARGET zstd::libzstd)
endif()

# quickjs-ng — embeddable JS engine for the JS (SealDice-compatible) mod subsystem
find_package(qjs CONFIG REQUIRED)

# Lua — embeddable engine for the original Dice! Lua mod subsystem (C#6)
find_package(Lua REQUIRED)
# MD4C — pinned CommonMark/GFM parser used for structural Markdown-to-text
# downgrade on adapters which only support plain text.
add_library(dice-md4c STATIC third_party/md4c/md4c.c)
target_include_directories(dice-md4c PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/md4c
)
set_target_properties(dice-md4c PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)

# onedice-cpp-lib — sibling dice-string engine project
set(ONEDICE_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../onedice-cpp-lib
                 ${CMAKE_BINARY_DIR}/onedice-cpp-lib)

# dicescript-c-lib — sibling C99 DiceScript compatibility engine
set(DICESCRIPT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DICESCRIPT_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../dicescript-c-lib
                 ${CMAKE_BINARY_DIR}/dicescript-c-lib)

# ─── Source Files ────────────────────────────────────────────

set(SOURCES
    src/main.cpp
    src/common/version.cpp
    src/common/logger.cpp
    src/common/markdown.cpp
    src/common/markdown_md4c.cpp
    src/common/hot_reload.cpp
    src/config/config_manager.cpp
    src/storage/database.cpp
    src/storage/migration.cpp
    src/storage/legacy_importer.cpp
    src/core/dice/dice_expression.cpp
    src/core/dice/dice_engine.cpp
    src/core/reply/reply_matcher.cpp
    src/core/reply/reply_manager.cpp
    src/core/character/card_store.cpp
    src/core/deck/card_deck.cpp
    src/core/mod/js_plugin_manager.cpp
    src/core/mod/lua_plugin_manager.cpp
    src/core/causal/causal_rule_manager.cpp
    src/core/causal/cooldown_manager.cpp
    src/core/causal/counter_store.cpp
    src/core/persona/persona_manager.cpp
    src/message/cq_code.cpp
    src/message/message_formatter.cpp
    src/platform/system_info.cpp
    src/i18n/i18n.cpp
    src/i18n/locale_resolver.cpp
)

# ─── Version / build-number generation ───────────────────────
# Generates build/generated/version_build.cpp with the next build number.
# The source counter is committed only after the executable links successfully.

set(VERSION_BUILD_SOURCE ${CMAKE_BINARY_DIR}/generated/version_build.cpp)
set(VERSION_BUILD_PENDING ${CMAKE_BINARY_DIR}/generated/version_build.pending)
set(VERSION_COUNTER_FILE ${CMAKE_CURRENT_SOURCE_DIR}/build_counter.txt)
set(BUMP_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/cmake/bump_build.cmake)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated)
set_source_files_properties(${VERSION_BUILD_SOURCE} PROPERTIES GENERATED TRUE)

# Generate the tentative next number before every build without changing the
# persistent counter. A failed compile therefore does not consume a number.
add_custom_target(prepare_version ALL
    COMMAND ${CMAKE_COMMAND}
    -DMODE=PREPARE -DVERSION=${PROJECT_VERSION} -DOUT=${VERSION_BUILD_SOURCE}
    -DPENDING=${VERSION_BUILD_PENDING}
    -DCOUNTER=${VERSION_COUNTER_FILE} -P ${BUMP_SCRIPT}
    BYPRODUCTS ${VERSION_BUILD_SOURCE} ${VERSION_BUILD_PENDING}
    COMMENT "Preparing Dice!Next build number")

# ─── Executable Target ───────────────────────────────────────

add_executable(dice-next-server ${SOURCES} ${VERSION_BUILD_SOURCE})
add_dependencies(dice-next-server prepare_version)
# Keep the build value in a generated .cpp rather than a generated header.
# Visual Studio reliably recompiles a source file produced by a dependency
# target, whereas its include scanner can miss generated headers.  This keeps
# the exe's .bot banner in sync with the build number committed after linking.
add_custom_command(TARGET dice-next-server POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DMODE=COMMIT -DVERSION=${PROJECT_VERSION}
        -DCOUNTER=${VERSION_COUNTER_FILE} -DPENDING=${VERSION_BUILD_PENDING}
        -P ${BUMP_SCRIPT}
    COMMENT "Recording successful Dice!Next build number")

target_include_directories(dice-next-server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)

target_compile_definitions(dice-next-server PRIVATE
    DICE_VERSION_STRING="${PROJECT_VERSION}"
)

target_link_libraries(dice-next-server PRIVATE
    Drogon::Drogon
    OpenSSL::Crypto
    OpenSSL::SSL
    nlohmann_json::nlohmann_json
    sqlite_orm::sqlite_orm
    spdlog::spdlog
    yaml-cpp
    ZLIB::ZLIB
    ${DICE_ZSTD_TARGET}
    onedice::onedice
    dicescript::dicescript
    qjs
    dice-md4c
    ${LUA_LIBRARIES}
)

target_include_directories(dice-next-server PRIVATE ${LUA_INCLUDE_DIR})

# ─── Platform-specific settings ──────────────────────────────

if(WIN32)
    # The package manager intentionally has no third-party DLL dependency. It
    # starts dice-next-core.exe with the package-local lib/ directory on PATH.
    add_executable(dice-next-launcher src/platform/launcher_win.cpp)
    set_target_properties(dice-next-launcher PROPERTIES OUTPUT_NAME "dice-next")
    # The manager must run before lib/ is added to PATH, so do not make it
    # depend on the package's MSVC runtime DLLs.
    set_property(TARGET dice-next-launcher PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_libraries(dice-next-launcher PRIVATE shell32)
    target_compile_options(dice-next-launcher PRIVATE /W4 /utf-8)
    target_sources(dice-next-launcher PRIVATE src/resources/app.rc)

    # Windows: link additional system libraries for ReadDirectoryChangesW
    target_link_libraries(dice-next-server PRIVATE
        ws2_32
        crypt32
    )
    # Application icon resource. Edition selects the logo: stable | dev.
    # Override with: cmake -DDICE_EDITION=dev ...  (default stable)
    set(DICE_EDITION "stable" CACHE STRING "Build edition (stable|dev) — selects the app/tray icon")
    set(_dice_ico "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/app-${DICE_EDITION}.ico")
    if(NOT EXISTS "${_dice_ico}")
        set(_dice_ico "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/app-stable.ico")
    endif()
    # Copy the chosen edition icon to app.ico (what app.rc embeds). Reconfigure
    # re-copies when DICE_EDITION changes.
    configure_file("${_dice_ico}" "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/app.ico" COPYONLY)
    message(STATUS "Dice!Next edition: ${DICE_EDITION} (icon: ${_dice_ico})")
    target_sources(dice-next-server PRIVATE src/resources/app.rc)
endif()

if(UNIX AND NOT APPLE)
    # Linux: no extra libs needed for inotify (kernel API)
endif()

# ─── Compiler Warnings ───────────────────────────────────────

if(MSVC)
    # /bigobj: main.cpp pulls in many header-only units (drogon + quickjs + all api
    # handlers) and exceeds the default object section limit (C1128).
    target_compile_options(dice-next-server PRIVATE /W4 /utf-8 /bigobj)
else()
    target_compile_options(dice-next-server PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ─── Install ─────────────────────────────────────────────────
install(TARGETS dice-next-server DESTINATION bin)

# ─── Tests ───────────────────────────────────────────────────
enable_testing()
add_subdirectory(tests)
