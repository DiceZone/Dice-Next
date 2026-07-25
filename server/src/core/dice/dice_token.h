#pragma once

#include <string>

namespace dice {

/**
 * @brief Lexer token type for the dice expression parser.
 */
enum class TokenType {
    NUMBER,     // Integer literal
    DICE_OP,    // 'd' operator
    PLUS,       // '+'
    MINUS,      // '-'
    STAR,       // '*'
    SLASH,      // '/'
    LPAREN,     // '('
    RPAREN,     // ')'
    BONUS,      // 'b' (bonus dice for some systems)
    EOF_TOKEN,  // End of input
};

inline const char* tokenTypeName(TokenType t) {
    switch (t) {
        case TokenType::NUMBER:   return "NUMBER";
        case TokenType::DICE_OP:  return "DICE_OP";
        case TokenType::PLUS:     return "PLUS";
        case TokenType::MINUS:    return "MINUS";
        case TokenType::STAR:     return "STAR";
        case TokenType::SLASH:    return "SLASH";
        case TokenType::LPAREN:   return "LPAREN";
        case TokenType::RPAREN:   return "RPAREN";
        case TokenType::BONUS:    return "BONUS";
        case TokenType::EOF_TOKEN: return "EOF";
        default:                  return "UNKNOWN";
    }
}

/**
 * @brief A single token produced by the lexer.
 */
struct DiceToken {
    TokenType type = TokenType::EOF_TOKEN;
    std::string value;  // Raw string representation
    int number = 0;     // Numeric value for NUMBER tokens

    DiceToken() = default;

    DiceToken(TokenType t, const std::string& val)
        : type(t), value(val) {}

    DiceToken(TokenType t, const std::string& val, int num)
        : type(t), value(val), number(num) {}
};

}  // namespace dice
