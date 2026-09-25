#pragma once
// ─── Dice!Next — Card deck (牌堆) ────────────────────────────
// Compatible with the original Dice! deck format:
//   name → [cards];  card may contain
//     {牌堆}   draw from a sub-deck WITHOUT replacement (depletes)
//     {%牌堆}  draw from a sub-deck WITH replacement
//     [骰子表达式]  rolled and substituted
//     ::N::card   weighted entry (weight N)
//     \{ \[       escaped literals
// Decks are loaded from JSON files ({"name": ["card", ...], ...}) plus a
// small built-in set.

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <functional>
#include <random>
#include <mutex>

namespace dice {

class CardDeck {
public:
    CardDeck();

    /// Inject a dice evaluator for [expr] substitution (returns the rolled total).
    void setDiceEval(std::function<long long(const std::string&)> fn) { diceEval_ = std::move(fn); }

    /// Inject a help-doc lookup: {词条} 引用若不是牌堆名，则查帮助词条并展开其内容
    /// （溯洄/OneDice 引用语义）。返回 nullopt 表示无此词条。
    void setHelpLookup(std::function<std::optional<std::string>(const std::string&)> fn) { helpLookup_ = std::move(fn); }

    /// 把任意文本里的 {引用}/[骰子] 展开（供 .help 词条内容复用同一套展开语义）。
    std::string expandText(const std::string& text) { TempMap t; return expand(text, t, 0); }

    /// Load all *.json deck files from a directory. Returns number of decks loaded.
    int loadDir(const std::string& dir);

    /// Re-scan the given directories from scratch: drop all file decks, re-seed the
    /// built-ins, then reload every dir. Reflects manually added/edited/removed files
    /// (the WebUI「重载」button). Thread-safe.
    void reload(const std::vector<std::string>& dirs);

    bool has(const std::string& name) const;
    std::vector<std::string> deckNames() const;
    size_t deckCount() const;

    struct SourceInfo {
        std::string filename;
        bool bundled = false;
    };

    /// Get the source file and whether it belongs to the read-only release.
    /// nullopt means the emergency in-process fallback rather than a file.
    std::optional<SourceInfo> getSourceInfo(const std::string& name) const;

    /// Backward-compatible filename-only accessor.
    std::string getSourceFile(const std::string& name) const;

    /// Draw one (fully expanded) card from the named deck. nullopt if no such deck.
    std::optional<std::string> drawFromDeck(const std::string& name);

    // One authored reply shares depletion state across its legacy references.
    // Keeping the context local prevents state leaking between users/messages.
    struct ReferenceContext {
        std::unordered_map<std::string, std::vector<std::string>> decks;
    };
    std::string expandReference(const std::string& token, ReferenceContext& context);

private:
    using Deck = std::vector<std::string>;
    using TempMap = std::unordered_map<std::string, Deck>;

    static std::string lower(const std::string& s);
    const Deck* find(const std::string& name) const;
    void seedBuiltins();                       // caller holds mutex_ / construction
    int  loadDirLocked(const std::string& dir, bool bundled);  // caller holds mutex_

    /// Expand {…} references and [dice] in @p expr (recursive, depleting tempMap).
    std::string expand(std::string expr, TempMap& temp, int depth);
    /// Weighted random pick from @p deck; if !back, deplete the chosen entry.
    std::string drawCard(Deck& deck, bool back, TempMap& temp, int depth);

    std::unordered_map<std::string, Deck> decks_;   // keyed by lower(name)
    std::unordered_map<std::string, SourceInfo> sourceFiles_;  // deck name → source
    std::function<long long(const std::string&)> diceEval_;
    std::function<std::optional<std::string>(const std::string&)> helpLookup_;
    std::mt19937_64 rng_;
    mutable std::mutex mutex_;
};

}  // namespace dice
