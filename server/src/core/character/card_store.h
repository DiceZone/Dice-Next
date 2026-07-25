#pragma once
// ─── Dice!Next — Character Card store ────────────────────────
// Cards are named and global to a user (key = userId + cardName); the empty
// name "" is the default unnamed card. Which card is active in a given
// group/scope is a per-(user, scope) binding (.pc tag). Attributes are
// name→int, persisted as a JSON object in the character_cards table.
// Resolves common attribute synonyms (san↔理智, 侦察↔侦查, ...).
//
// The (user, scope) attribute API below operates on whichever card the user
// has bound in that scope, so .st/.ra/.sc/.en need no changes.

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <mutex>
#include <nlohmann/json.hpp>

namespace dice {

class Database;

class CharacterCardStore {
public:
    explicit CharacterCardStore(Database& db);

    // ─── Active-card attribute API (operates on the bound card) ──

    /// All attributes of the user's bound card in @p scope. Empty if none.
    std::map<std::string, int> getAttrs(const std::string& user,
                                        const std::string& scope) const;

    /// One attribute value (synonym-resolved), or nullopt if absent.
    std::optional<int> getAttr(const std::string& user, const std::string& scope,
                               const std::string& name) const;

    /// Set/overwrite one attribute (synonym-resolved), creating the bound
    /// card if it doesn't exist yet.
    void setAttr(const std::string& user, const std::string& scope,
                 const std::string& name, int value);

    /// Empty the bound card's attributes. Returns true if it had any.
    bool clear(const std::string& user, const std::string& scope);

    /// Remove one attribute from the bound card. Returns true if it existed.
    bool eraseAttr(const std::string& user, const std::string& scope,
                   const std::string& name);

    // ─── 卡片元数据（原版 CardTemp 移植：锁定 / 文本属性）────────────
    // 存在 attrs JSON 的保留键 "__meta" 里（saveAttrsOf 保留非整数键不摧毁）。

    /// 卡片锁（原版 CharaCard::lock）：key = "w"(锁写：.st set/del/clr/.pc build 拒绝)
    /// 或 "r"(锁读：.st show 拒绝)。按「绑定卡」操作。
    bool cardLocked(const std::string& user, const std::string& scope, const std::string& key) const;
    bool lockCard(const std::string& user, const std::string& scope, const std::string& key);
    bool unlockCard(const std::string& user, const std::string& scope, const std::string& key);

    /// 文本属性（.pc build bg 的 性别/职业/背景等；数字属性之外的补充）。
    std::map<std::string, std::string> getTexts(const std::string& user, const std::string& scope) const;
    void setText(const std::string& user, const std::string& scope,
                 const std::string& name, const std::string& value);   // 空值=删除

    // ─── Multi-card management (.pc) ─────────────────────────────

    /// All of the user's card names ("" = default unnamed card), de-duplicated.
    std::vector<std::string> listCards(const std::string& user) const;

    /// Whether a named card exists for the user.
    bool cardExists(const std::string& user, const std::string& name) const;

    /// Create a blank named card. Returns false if one already exists.
    bool createCard(const std::string& user, const std::string& name);

    /// Delete a named card. Returns false if it didn't exist.
    bool deleteCard(const std::string& user, const std::string& name);

    /// Number of attributes on a named card.
    int attrCount(const std::string& user, const std::string& name) const;

    /// Name of the card the user has bound in @p scope ("" if none/default).
    std::string boundCard(const std::string& user, const std::string& scope) const;

    /// Bind a card (by name) as active for the user in @p scope.
    void bindCard(const std::string& user, const std::string& scope,
                  const std::string& name);

    /// Rename a card (oldName → newName) for the user, updating any scope bindings
    /// that pointed at it. Returns false if oldName doesn't exist or newName is
    /// already taken (and differs from oldName).
    bool renameCard(const std::string& user, const std::string& oldName,
                    const std::string& newName);

    /// Canonicalize an attribute name (synonyms → canonical Chinese name).
    static std::string canonical(const std::string& name);

    /// Register one alias→canonical mapping into the global synonym table.
    /// ⚠️ Startup-only (no locking; canonical() reads concurrently while serving).
    static void registerAlias(const std::string& alias, const std::string& canonName);
    /// Reset the synonym table back to the built-in defaults (drops rule-pack
    /// merges). Used when reloading rule packs. ⚠️ Not for use while serving.
    static void resetAliases();

    /// Load every rule pack in @p dir (rules/*.json) and merge each pack's
    /// "alias" block into the global synonym table. Returns the count registered.
    /// Call once at startup before serving (rule-pack system P1, global merge).
    static int loadRulePackAliases(const std::string& dir);

private:
    // Internal helpers keyed by (user, cardName).
    std::map<std::string, int> attrsOf(const std::string& user,
                                       const std::string& name) const;
    void saveAttrsOf(const std::string& user, const std::string& name,
                     const std::map<std::string, int>& attrs);
    nlohmann::json rawCardJson(const std::string& user, const std::string& name) const;
    void saveRawCardJson(const std::string& user, const std::string& name,
                         const nlohmann::json& j);

    Database& db_;
    mutable std::mutex mutex_;
};

}  // namespace dice
