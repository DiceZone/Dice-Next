cmake_minimum_required(VERSION 3.20)

# ─── Test Executable ──────────────────────────────────────────
# Build: cmake --build build --target dice-next-tests --config Release
# Run:   ./build/tests/Release/dice-next-tests.exe

set(TEST_SOURCES
    # Test framework + test cases
    test_main.cpp
    test_cooldown_manager.cpp
    test_counter_store.cpp
    test_causal_rule_manager.cpp
    test_config_manager.cpp
    test_persona_manager.cpp
    test_i18n_persona.cpp
    test_import_v2.cpp
    test_group_account_scoping.cpp
    test_master_delivery.cpp
    test_backup_service.cpp
    test_timezone.cpp
    test_js_plugin_reload.cpp
    test_dice_expression.cpp
    test_expression_fallback.cpp
    test_roll_command_parser.cpp
    test_update_service.cpp
    test_check_roll_override.cpp
    test_markdown.cpp
    test_heart_debounce.cpp
    test_command_prefix_policy.cpp
    test_friend_approval_policy.cpp
    test_sensitive_word_filter.cpp

    # Project source files needed by tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/causal/cooldown_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/causal/counter_store.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/causal/causal_rule_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/persona/persona_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/i18n/i18n.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/storage/database.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config/config_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/common/logger.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/common/version.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/common/subprocess.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/service/update_service.cpp
    ${VERSION_BUILD_SOURCE}
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/common/markdown.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/common/markdown_md4c.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/i18n/locale_resolver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/reply/reply_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/reply/reply_matcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/dice/dice_expression.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/mod/js_plugin_manager.cpp
)

add_executable(dice-next-tests ${TEST_SOURCES})
add_dependencies(dice-next-tests prepare_version)

# Explicitly set the linker language (needed when sources are outside the dir)
set_target_properties(dice-next-tests PROPERTIES LINKER_LANGUAGE CXX)

target_include_directories(dice-next-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/generated
)

# Link against the same dependencies as the main server
target_link_libraries(dice-next-tests PRIVATE
    OpenSSL::Crypto
    Drogon::Drogon
    nlohmann_json::nlohmann_json
    sqlite_orm::sqlite_orm
    spdlog::spdlog
    qjs
    onedice::onedice
    dicescript::dicescript
    dice-md4c
)

# Keep the real Lua runtime tests in a separate executable: test_import_v2.cpp
# deliberately defines a lightweight LuaPluginManager stub for importer-only tests.
add_executable(dice-next-lua-tests
    test_main.cpp
    test_lua_plugin_compat.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/mod/lua_plugin_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/common/logger.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/common/subprocess.cpp
)
target_include_directories(dice-next-lua-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LUA_INCLUDE_DIR}
)
target_link_libraries(dice-next-lua-tests PRIVATE
    Drogon::Drogon
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    yaml-cpp::yaml-cpp
    unofficial::sqlite3::sqlite3
    ${LUA_LIBRARIES}
)
add_dependencies(dice-next-tests dice-next-lua-tests)
# MSVC needs /bigobj for large test files with many includes
if(MSVC)
    target_compile_options(dice-next-tests PRIVATE /utf-8 /bigobj)
    target_compile_options(dice-next-lua-tests PRIVATE /utf-8 /bigobj)
else()
    target_compile_options(dice-next-tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(dice-next-lua-tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ─── CTest Integration ───────────────────────────────────────
enable_testing()
add_test(NAME dice_next_tests COMMAND dice-next-tests)
add_test(NAME dice_next_lua_tests COMMAND dice-next-lua-tests)
