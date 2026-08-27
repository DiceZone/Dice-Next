#include "dice_expression.h"
#include "dice_result.h"
#include "../../common/errors.h"

#include <sstream>
#include <cctype>
#include <iomanip>
#include <stdexcept>

namespace dice {

namespace {

std::string unexpectedInputUnit(const std::string& input, size_t offset) {
    const unsigned char first = static_cast<unsigned char>(input[offset]);
    if (first < 0x80u) return std::string("character '") + input[offset] + "'";

    size_t length = 0;
    if (first >= 0xc2u && first <= 0xdfu) length = 2;
    else if (first >= 0xe0u && first <= 0xefu) length = 3;
    else if (first >= 0xf0u && first <= 0xf4u) length = 4;

    bool valid = length != 0 && offset + length <= input.size();
    for (size_t i = 1; valid && i < length; ++i)
        valid = (static_cast<unsigned char>(input[offset + i]) & 0xc0u) == 0x80u;
    if (valid && length == 3) {
        const unsigned char second = static_cast<unsigned char>(input[offset + 1]);
        valid = (first != 0xe0u || second >= 0xa0u) &&
                (first != 0xedu || second <= 0x9fu);
    } else if (valid && length == 4) {
        const unsigned char second = static_cast<unsigned char>(input[offset + 1]);
        valid = (first != 0xf0u || second >= 0x90u) &&
                (first != 0xf4u || second <= 0x8fu);
    }
    if (valid)
        return std::string("character '") + input.substr(offset, length) + "'";

    std::ostringstream escaped;
    escaped << "byte 0x" << std::uppercase << std::hex << std::setw(2)
            << std::setfill('0') << static_cast<unsigned int>(first);
    return escaped.str();
}

}  // namespace


// ═══════════════════════════════════════════════════════════════
// ASTNode factory methods
// ═══════════════════════════════════════════════════════════════

std::unique_ptr<ASTNode> ASTNode::makeNumber(int val) {
    auto n = std::make_unique<ASTNode>();
    n->kind = Kind::kNumber;
    n->value = val;
    return n;
}

std::unique_ptr<ASTNode> ASTNode::makeDiceRoll(
    std::unique_ptr<ASTNode> count,
    std::unique_ptr<ASTNode> sides) {
    auto n = std::make_unique<ASTNode>();
    n->kind = Kind::kDiceRoll;
    n->left = std::move(count);
    n->right = std::move(sides);
    return n;
}

std::unique_ptr<ASTNode> ASTNode::makeBinaryOp(
    char op,
    std::unique_ptr<ASTNode> l,
    std::unique_ptr<ASTNode> r) {
    auto n = std::make_unique<ASTNode>();
    n->kind = Kind::kBinaryOp;
    n->op = op;
    n->left = std::move(l);
    n->right = std::move(r);
    return n;
}

std::unique_ptr<ASTNode> ASTNode::makeUnaryMinus(
    std::unique_ptr<ASTNode> operand) {
    auto n = std::make_unique<ASTNode>();
    n->kind = Kind::kUnaryMinus;
    n->op = '~';
    n->left = std::move(operand);
    return n;
}

// ═══════════════════════════════════════════════════════════════
// DiceExpression implementation
// ═══════════════════════════════════════════════════════════════

DiceExpression::DiceExpression(RandomFunc randomFn)
    : randomFn_(std::move(randomFn)) {
}

// ─── Public API ──────────────────────────────────────────────

DiceResult DiceExpression::evaluate(const std::string& input) {
    lastExpression_ = input;

    // Display form keeps what the user typed (uppercased D); the parser sees a
    // version with implicit "1" inserted before a bare leading 'd'.
    const std::string display = normalizeDisplay(input);

    // Lex (must not throw — return a failure so callers can fall back, e.g. to OneDice)
    try {
        tokens_ = tokenize(preprocessForParse(input));
    } catch (const AppException& e) {
        return DiceResult::failure(input, e.userMessage(), e.errorCodeInt());
    } catch (const std::exception& e) {
        return DiceResult::failure(input, std::string("lex error: ") + e.what(), 2001);
    }
    pos_ = 0;

    if (tokens_.empty() || tokens_.back().type == TokenType::EOF_TOKEN && tokens_.size() == 1) {
        return DiceResult::failure(input, "empty expression", 2001);
    }

    // Parse
    std::unique_ptr<ASTNode> ast;
    try {
        ast = parseExpression();
        if (tokens_[pos_].type != TokenType::EOF_TOKEN) {
            // Extra tokens after valid expression
            std::ostringstream oss;
            oss << "unexpected token '" << tokens_[pos_].value << "' after expression";
            return DiceResult::failure(input, oss.str(), 2001);
        }
    } catch (const AppException& e) {
        return DiceResult::failure(input, e.userMessage(), e.errorCodeInt());
    } catch (const std::exception& e) {
        return DiceResult::failure(input, std::string("parse error: ") + e.what(), 2001);
    }

    // Evaluate & render in the original Dice! style:
    //   显示式 [=分项展开] [=各项合计] [=总和]   (collapsing identical stages)
    try {
        std::vector<int> results;

        // Split the top-level additive spine into signed terms.
        std::vector<std::pair<bool, const ASTNode*>> terms;
        flattenAdditive(ast.get(), false, terms);
        const bool multiTerm = terms.size() > 1;

        std::string sep;       // 分项: dice expanded, e.g. "(4+2+6)+2"
        std::string combined;  // 各项: per-term subtotals,  e.g. "12+2"
        std::string evaluatedDisplay;
        int total = 0;

        for (size_t i = 0; i < terms.size(); ++i) {
            const bool neg = terms[i].first;
            const ASTNode* node = terms[i].second;
            // A term's dice get parentheses when the expression has several terms
            // or the term itself is compound (×/÷).
            const bool parenDice = multiTerm || (node->kind == ASTNode::Kind::kBinaryOp);

            RenderResult r = renderEval(node, parenDice, results);
            if (!multiTerm && !neg && !r.displayOverride.empty())
                evaluatedDisplay = r.displayOverride;
            const int signedVal = neg ? -r.value : r.value;
            total += signedVal;

            const std::string sign = (i == 0) ? (neg ? "-" : "") : (neg ? "-" : "+");
            sep      += sign + r.sep;
            combined += sign + std::to_string(r.value);
        }

        const std::string totalStr = std::to_string(total);

        // FormCompleteString: chain the stages, dropping any that repeat the previous.
        std::string formatted = evaluatedDisplay.empty() ? display : evaluatedDisplay;
        if (formatted != sep)      formatted += "=" + sep;
        if (sep != combined)       formatted += "=" + combined;
        if (combined != totalStr)  formatted += "=" + totalStr;

        return DiceResult::success(input, results, total, false, false,
                                    formatted, formatted);
    } catch (const AppException& e) {
        return DiceResult::failure(input, e.userMessage(), e.errorCodeInt(),
                                   DiceFailureKind::kEvaluation);
    } catch (const std::exception& e) {
        return DiceResult::failure(input, std::string("evaluation error: ") + e.what(), 2001,
                                   DiceFailureKind::kEvaluation);
    }
}

// ─── Faithful formatting helpers ─────────────────────────────

void DiceExpression::flattenAdditive(const ASTNode* node, bool negative,
        std::vector<std::pair<bool, const ASTNode*>>& out) {
    if (node && node->kind == ASTNode::Kind::kBinaryOp &&
        (node->op == '+' || node->op == '-')) {
        flattenAdditive(node->left.get(), negative, out);
        // For '-', the right operand's sign flips.
        flattenAdditive(node->right.get(),
            node->op == '-' ? !negative : negative, out);
    } else {
        out.emplace_back(negative, node);
    }
}

DiceExpression::RenderResult DiceExpression::renderEval(
        const ASTNode* node, bool parenDice, std::vector<int>& results) {
    RenderResult out;
    if (!node) return out;

    switch (node->kind) {
        case ASTNode::Kind::kNumber:
            out.value = node->value;
            out.sep = std::to_string(node->value);
            return out;

        case ASTNode::Kind::kUnaryMinus: {
            RenderResult r = renderEval(node->left.get(), parenDice, results);
            out.value = -r.value;
            out.sep = "-" + r.sep;
            return out;
        }

        case ASTNode::Kind::kBinaryOp: {
            // Nested +/- (inside parens) and ×/÷ both force child dice to parenthesize.
            RenderResult l = renderEval(node->left.get(), true, results);
            RenderResult r = renderEval(node->right.get(), true, results);
            switch (node->op) {
                case '+': out.value = l.value + r.value; out.sep = l.sep + "+" + r.sep; break;
                case '-': out.value = l.value - r.value; out.sep = l.sep + "-" + r.sep; break;
                case '*': out.value = l.value * r.value; out.sep = l.sep + "×" + r.sep; break;
                case '/':
                    if (r.value == 0)
                        throw AppException(ApiErrorCode::kDiceInvalidExpression, "division by zero");
                    out.value = l.value / r.value; out.sep = l.sep + "/" + r.sep; break;
                default:
                    throw AppException(ApiErrorCode::kDiceInvalidExpression,
                        std::string("unknown operator: ") + node->op);
            }
            return out;
        }

        case ASTNode::Kind::kDiceRoll: {
            const RenderResult countResult = renderEval(node->left.get(), true, results);
            const RenderResult sidesResult = renderEval(node->right.get(), true, results);
            const int count = countResult.value;
            const int sides = sidesResult.value;
            if (count < 0)
                throw AppException(ApiErrorCode::kDiceInvalidExpression,
                    "dice count cannot be negative: " + std::to_string(count));
            if (sides < 1)
                throw AppException(ApiErrorCode::kDiceInvalidExpression,
                    "dice sides must be >= 1: " + std::to_string(sides));
            if (count > 1000)
                throw AppException(ApiErrorCode::kDiceInvalidExpression,
                    "cannot roll more than 1000 dice: " + std::to_string(count));

            int sum = 0;
            std::string inner;
            for (int i = 0; i < count; ++i) {
                const int roll = randomFn_(sides);
                results.push_back(roll);
                sum += roll;
                if (i > 0) inner += "+";
                inner += std::to_string(roll);
            }
            out.value = sum;
            const bool dynamicDice = node->left->kind != ASTNode::Kind::kNumber
                                  || node->right->kind != ASTNode::Kind::kNumber;
            if (dynamicDice) {
                out.displayOverride = std::to_string(count) + "D" + std::to_string(sides);
                out.sep = "{" + inner + "}(" + std::to_string(sum) + ")";
            } else {
                out.sep = (count > 1 && parenDice) ? ("(" + inner + ")") : inner;
            }
            return out;
        }
    }
    return out;
}

std::string DiceExpression::normalizeDisplay(const std::string& input) {
    std::string s = input;
    for (auto& c : s) {
        if (c == 'd') c = 'D';
    }
    return s;
}

std::string DiceExpression::preprocessForParse(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 2);
    for (size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if ((c == 'd' || c == 'D')) {
            // Insert implicit "1" if 'd' is at the start or follows an operator/paren.
            const bool bareLeading = out.empty() ||
                out.back() == '+' || out.back() == '-' || out.back() == '*' ||
                out.back() == '/' || out.back() == '(';
            if (bareLeading) out += '1';
        }
        out += c;
    }
    return out;
}

// ─── Lexer ───────────────────────────────────────────────────

std::vector<DiceToken> DiceExpression::tokenize(const std::string& input) {
    std::vector<DiceToken> tokens;
    size_t i = 0;

    while (i < input.size()) {
        char ch = input[i];

        // Skip whitespace
        if (std::isspace(static_cast<unsigned char>(ch))) {
            ++i;
            continue;
        }

        // Number
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            std::string numStr;
            while (i < input.size() && std::isdigit(static_cast<unsigned char>(input[i]))) {
                numStr += input[i];
                ++i;
            }
            int val = std::stoi(numStr);
            tokens.emplace_back(TokenType::NUMBER, numStr, val);
            continue;
        }

        // Dice operator (case-insensitive 'd')
        if (ch == 'd' || ch == 'D') {
            tokens.emplace_back(TokenType::DICE_OP, std::string(1, 'd'));
            ++i;
            continue;
        }

        // Bonus operator
        if (ch == 'b' || ch == 'B') {
            tokens.emplace_back(TokenType::BONUS, std::string(1, 'b'));
            ++i;
            continue;
        }

        // Single-char operators
        switch (ch) {
            case '+': tokens.emplace_back(TokenType::PLUS, "+");   ++i; break;
            case '-': tokens.emplace_back(TokenType::MINUS, "-");  ++i; break;
            case '*': tokens.emplace_back(TokenType::STAR, "*");   ++i; break;
            case '/': tokens.emplace_back(TokenType::SLASH, "/");  ++i; break;
            case '(': tokens.emplace_back(TokenType::LPAREN, "("); ++i; break;
            case ')': tokens.emplace_back(TokenType::RPAREN, ")"); ++i; break;
            default: {
                // Unknown character
                std::ostringstream oss;
                oss << "unexpected " << unexpectedInputUnit(input, i);
                throw AppException(ApiErrorCode::kDiceInvalidExpression, oss.str());
            }
        }
    }

    tokens.emplace_back(TokenType::EOF_TOKEN, "<EOF>");
    return tokens;
}

// ─── Parser helpers ─────────────────────────────────────────

DiceToken DiceExpression::peek() const {
    if (pos_ < tokens_.size()) return tokens_[pos_];
    return DiceToken(TokenType::EOF_TOKEN, "<EOF>");
}

DiceToken DiceExpression::advance() {
    if (pos_ < tokens_.size()) return tokens_[pos_++];
    return DiceToken(TokenType::EOF_TOKEN, "<EOF>");
}

bool DiceExpression::match(TokenType type) {
    if (peek().type == type) {
        advance();
        return true;
    }
    return false;
}

DiceToken DiceExpression::expect(TokenType type) {
    auto tok = advance();
    if (tok.type != type) {
        std::ostringstream oss;
        oss << "expected " << tokenTypeName(type)
            << " but got " << tokenTypeName(tok.type)
            << " ('" << tok.value << "')";
        throw AppException(ApiErrorCode::kDiceInvalidExpression, oss.str());
    }
    return tok;
}

// ─── Recursive-descent parser ────────────────────────────────
// Expression → Term (('+' | '-') Term)*

std::unique_ptr<ASTNode> DiceExpression::parseExpression() {
    auto left = parseTerm();

    while (true) {
        TokenType t = peek().type;
        if (t == TokenType::PLUS) {
            advance();
            auto right = parseTerm();
            left = ASTNode::makeBinaryOp('+', std::move(left), std::move(right));
        } else if (t == TokenType::MINUS) {
            advance();
            auto right = parseTerm();
            left = ASTNode::makeBinaryOp('-', std::move(left), std::move(right));
        } else {
            break;
        }
    }

    return left;
}

// Term → Factor (('*' | '/') Factor)*

std::unique_ptr<ASTNode> DiceExpression::parseTerm() {
    auto left = parseFactor();

    while (true) {
        TokenType t = peek().type;
        if (t == TokenType::STAR) {
            advance();
            auto right = parseFactor();
            left = ASTNode::makeBinaryOp('*', std::move(left), std::move(right));
        } else if (t == TokenType::SLASH) {
            advance();
            auto right = parseFactor();
            left = ASTNode::makeBinaryOp('/', std::move(left), std::move(right));
        } else {
            break;
        }
    }

    return left;
}

// Factor → NUMBER | '(' Expression ')' | ('+' | '-') Factor | DiceExpr
// DiceExpr → Factor 'd' Factor

std::unique_ptr<ASTNode> DiceExpression::parseFactor() {
    // Handle unary plus/minus
    if (peek().type == TokenType::PLUS) {
        advance();  // Discard unary plus
        return parseFactor();
    }
    if (peek().type == TokenType::MINUS) {
        advance();
        auto operand = parseFactor();
        return ASTNode::makeUnaryMinus(std::move(operand));
    }

    // Dice operators share one precedence level and therefore fold from left
    // to right: 10d10d1 is (10d10)d1, never 10d(10d1).
    auto left = parsePrimary();
    while (peek().type == TokenType::DICE_OP) {
        advance();

        // Consume a sign for this operand without recursively consuming a
        // following dice operator (which would restore right associativity).
        bool negative = false;
        while (peek().type == TokenType::PLUS || peek().type == TokenType::MINUS) {
            if (advance().type == TokenType::MINUS) negative = !negative;
        }
        auto right = parsePrimary();
        if (negative) right = ASTNode::makeUnaryMinus(std::move(right));
        left = ASTNode::makeDiceRoll(std::move(left), std::move(right));
    }
    return left;
}

std::unique_ptr<ASTNode> DiceExpression::parsePrimary() {
    // Parenthesized expression
    if (peek().type == TokenType::LPAREN) {
        advance();  // '('
        auto expr = parseExpression();
        expect(TokenType::RPAREN);  // ')'
        return expr;
    }

    // Number literal
    if (peek().type == TokenType::NUMBER) {
        auto tok = advance();
        return ASTNode::makeNumber(tok.number);
    }

    // Unexpected token
    auto tok = peek();
    std::ostringstream oss;
    oss << "unexpected token '" << tok.value << "'";
    throw AppException(ApiErrorCode::kDiceInvalidExpression, oss.str());
}

// ─── Evaluator ───────────────────────────────────────────────

int DiceExpression::evalNode(const ASTNode* node, std::vector<int>& results) {
    if (!node) return 0;

    switch (node->kind) {
        case ASTNode::Kind::kNumber:
            return node->value;

        case ASTNode::Kind::kUnaryMinus:
            return -evalNode(node->left.get(), results);

        case ASTNode::Kind::kBinaryOp: {
            int leftVal = evalNode(node->left.get(), results);
            int rightVal = evalNode(node->right.get(), results);

            switch (node->op) {
                case '+': return leftVal + rightVal;
                case '-': return leftVal - rightVal;
                case '*': return leftVal * rightVal;
                case '/': {
                    if (rightVal == 0) {
                        throw AppException(ApiErrorCode::kDiceInvalidExpression,
                            "division by zero");
                    }
                    // Integer division (floor toward zero in C++)
                    return leftVal / rightVal;
                }
                default:
                    throw AppException(ApiErrorCode::kDiceInvalidExpression,
                        std::string("unknown operator: ") + node->op);
            }
        }

        case ASTNode::Kind::kDiceRoll: {
            int count = evalNode(node->left.get(), results);
            int sides = evalNode(node->right.get(), results);

            if (count < 0) {
                throw AppException(ApiErrorCode::kDiceInvalidExpression,
                    "dice count cannot be negative: " + std::to_string(count));
            }
            if (sides < 1) {
                throw AppException(ApiErrorCode::kDiceInvalidExpression,
                    "dice sides must be >= 1: " + std::to_string(sides));
            }
            if (count > 1000) {
                throw AppException(ApiErrorCode::kDiceInvalidExpression,
                    "cannot roll more than 1000 dice: " + std::to_string(count));
            }

            int sum = 0;
            for (int i = 0; i < count; ++i) {
                int roll = randomFn_(sides);
                results.push_back(roll);
                sum += roll;
            }
            return sum;
        }
    }

    return 0;
}

}  // namespace dice
