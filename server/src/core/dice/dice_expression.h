#pragma once

#include "dice_token.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace dice {

// ─── Forward declarations ────────────────────────────────────

struct DiceResult;

/**
 * @brief AST (Abstract Syntax Tree) node for dice expressions.
 *
 * Supports:
 *   - XdY (dice roll):          left = count, right (or diceSides) = sides
 *   - Arithmetic:                operator = '+', '-', '*', '/'
 *   - Unary minus:               operator = '~' (negate)
 *   - Literal number:            value field
 *   - Compound:                  left = sub-expression for dice count (e.g., (2d6+1)d8)
 */
struct ASTNode {
    enum class Kind {
        kNumber,
        kDiceRoll,
        kBinaryOp,
        kUnaryMinus,
    };

    Kind kind = Kind::kNumber;
    char op = '\0';           // Binary operator or '~' for unary minus

    std::unique_ptr<ASTNode> left;
    std::unique_ptr<ASTNode> right;

    int value = 0;             // For kNumber
    int diceSides = 0;         // For kDiceRoll (alternative to right child)

    ASTNode() = default;

    static std::unique_ptr<ASTNode> makeNumber(int val);
    static std::unique_ptr<ASTNode> makeDiceRoll(std::unique_ptr<ASTNode> count, std::unique_ptr<ASTNode> sides);
    static std::unique_ptr<ASTNode> makeBinaryOp(char op, std::unique_ptr<ASTNode> l, std::unique_ptr<ASTNode> r);
    static std::unique_ptr<ASTNode> makeUnaryMinus(std::unique_ptr<ASTNode> operand);
};

/**
 * @brief Recursive-descent parser for dice expressions.
 *
 * Grammar (simplified):
 *   Expression  → Term (('+' | '-') Term)*
 *   Term        → Factor (('*' | '/') Factor)*
 *   Factor      → NUMBER | '(' Expression ')' | DiceExpr
 *   DiceExpr    → Factor DICE_OP Factor
 *
 * All dice rolls use an externally supplied random-number generator.
 * The parser supports:
 *   - Basic:         2d6, 1d100
 *   - With modifier: 2d6+3, 4d8-2
 *   - Multiplication: 2d6*2
 *   - Nested dice:   (1d10)d10, (2d6+1)d8
 *   - Parentheses:   2d(4+2), (2d10+5)*2
 *   - Multiple dice: 2d6+3d8
 *   - Simple constant: 42 (treated as literal)
 */
class DiceExpression {
public:
    using RandomFunc = std::function<int(int sides)>;

    /**
     * @brief Construct with a random-number generator.
     * @param randomFn  Takes sides (N), returns uniform [1, N].
     */
    explicit DiceExpression(RandomFunc randomFn);

    /**
     * @brief Parse and evaluate a dice expression.
     * @param input  Raw input string (e.g., "2d6+3", "(1d10)d8").
     * @return DiceResult with the evaluated result.
     */
    DiceResult evaluate(const std::string& input);

private:
    RandomFunc randomFn_;
    std::vector<DiceToken> tokens_;
    size_t pos_ = 0;
    std::string lastExpression_;

    // ─── Lexer ────────────────────────────────────────────────

    /**
     * @brief Tokenize the input string into a vector of DiceToken.
     * @param input  Raw input string.
     * @return Vector of tokens.
     */
    std::vector<DiceToken> tokenize(const std::string& input);

    // ─── Recursive-descent parser ─────────────────────────────

    DiceToken peek() const;
    DiceToken advance();
    bool match(TokenType type);
    DiceToken expect(TokenType type);

    std::unique_ptr<ASTNode> parseExpression();
    std::unique_ptr<ASTNode> parseTerm();
    std::unique_ptr<ASTNode> parseFactor();

    // ─── Evaluator ───────────────────────────────────────────

    /**
     * @brief Evaluate an AST node, collecting individual die results.
     * @param node    The AST node to evaluate.
     * @param results Output: vector of individual die face values (only from DiceRoll nodes).
     * @return The computed value of the subtree.
     */
    int evalNode(const ASTNode* node, std::vector<int>& results);

    // ─── Faithful formatting (mirrors original Dice! RD::FormCompleteString) ──

    /// Value + "separated" rendering of a subtree (dice expanded as r1+r2+...).
    struct RenderResult {
        int value = 0;
        std::string sep;
    };

    /// Roll + render a subtree. @p parenDice wraps multi-die groups in ().
    RenderResult renderEval(const ASTNode* node, bool parenDice, std::vector<int>& results);

    /// Flatten the top-level additive spine into signed terms (for the "combined" stage).
    void flattenAdditive(const ASTNode* node, bool negative,
                         std::vector<std::pair<bool, const ASTNode*>>& out);

    /// Uppercase the dice operator for display ("3d6" → "3D6").
    static std::string normalizeDisplay(const std::string& input);

    /// Insert an implicit "1" before a bare leading/standalone 'd' so the parser
    /// accepts "d100" the way the original does ("d100" → "1d100").
    static std::string preprocessForParse(const std::string& input);

    // ─── Helpers ──────────────────────────────────────────────

    std::string formatDetail(const std::string& expr,
                             const std::vector<int>& results,
                             int total, int modified);
};

}  // namespace dice
