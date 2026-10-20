#pragma once
namespace dice {
class Database;
class ConfigManager;
class AdapterManager;
void registerNoticeRuleRoutes(Database &, ConfigManager &, AdapterManager &);
} // namespace dice
