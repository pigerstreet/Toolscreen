
#include "expression_parser.h"
#include "gui.h"
#include "logic_thread.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>


// Using Expr prefix to avoid Windows header macro conflicts
enum class ExprTokenKind {
    Number,
    Identifier,
    Plus,
    Minus,
    Star,
    Slash,
    LParen,
    RParen,
    Comma,
    End,
    Invalid
};

struct ExprToken {
    ExprTokenKind kind;
    std::string text;
    double numValue;

    ExprToken() : kind(ExprTokenKind::End), text(""), numValue(0) {}
    ExprToken(ExprTokenKind k, const std::string& s, double n) : kind(k), text(s), numValue(n) {}
};

class Tokenizer {
public:
    Tokenizer(const std::string& input) : m_input(input), m_pos(0) {}

    ExprToken next() {
        skipWhitespace();
        if (m_pos >= m_input.size()) { return ExprToken(ExprTokenKind::End, "", 0); }

        char c = m_input[m_pos];

        if (c == '+') {
            m_pos++;
            return ExprToken(ExprTokenKind::Plus, "+", 0);
        }
        if (c == '-') {
            m_pos++;
            return ExprToken(ExprTokenKind::Minus, "-", 0);
        }
        if (c == '*') {
            m_pos++;
            return ExprToken(ExprTokenKind::Star, "*", 0);
        }
        if (c == '/') {
            m_pos++;
            return ExprToken(ExprTokenKind::Slash, "/", 0);
        }
        if (c == '(') {
            m_pos++;
            return ExprToken(ExprTokenKind::LParen, "(", 0);
        }
        if (c == ')') {
            m_pos++;
            return ExprToken(ExprTokenKind::RParen, ")", 0);
        }
        if (c == ',') {
            m_pos++;
            return ExprToken(ExprTokenKind::Comma, ",", 0);
        }

        if (std::isdigit(c) || c == '.') {
            size_t start = m_pos;
            bool hasDecimal = false;
            while (m_pos < m_input.size() && (std::isdigit(m_input[m_pos]) || m_input[m_pos] == '.')) {
                if (m_input[m_pos] == '.') {
                    if (hasDecimal) break;
                    hasDecimal = true;
                }
                m_pos++;
            }
            std::string numStr = m_input.substr(start, m_pos - start);
            double num = std::stod(numStr);
            return ExprToken(ExprTokenKind::Number, numStr, num);
        }

        if (std::isalpha(c) || c == '_') {
            size_t start = m_pos;
            while (m_pos < m_input.size() && (std::isalnum(m_input[m_pos]) || m_input[m_pos] == '_')) { m_pos++; }
            std::string id = m_input.substr(start, m_pos - start);
            return ExprToken(ExprTokenKind::Identifier, id, 0);
        }

        m_pos++;
        return ExprToken(ExprTokenKind::Invalid, std::string(1, c), 0);
    }

    size_t position() const { return m_pos; }

private:
    void skipWhitespace() {
        while (m_pos < m_input.size() && std::isspace(m_input[m_pos])) { m_pos++; }
    }

    std::string m_input;
    size_t m_pos;
};


class ExpressionParser {
public:
    ExpressionParser(const std::string& expr, int screenWidth, int screenHeight)
        : m_tokenizer(expr), m_screenWidth(screenWidth), m_screenHeight(screenHeight) {
        m_currentToken = m_tokenizer.next();
    }

    double parse() {
        double result = parseExpression();
        if (m_currentToken.kind != ExprTokenKind::End) { throw std::runtime_error("Unexpected token at end: " + m_currentToken.text); }
        return result;
    }

    std::string validate() {
        try {
            parseExpression();
            if (m_currentToken.kind != ExprTokenKind::End) { return "Unexpected token at end: " + m_currentToken.text; }
            return "";
        } catch (const std::exception& e) { return e.what(); }
    }

private:
    double parseExpression() {
        double left = parseTerm();
        while (m_currentToken.kind == ExprTokenKind::Plus || m_currentToken.kind == ExprTokenKind::Minus) {
            ExprTokenKind op = m_currentToken.kind;
            advance();
            double right = parseTerm();
            if (op == ExprTokenKind::Plus) {
                left = left + right;
            } else {
                left = left - right;
            }
        }
        return left;
    }

    double parseTerm() {
        double left = parseUnary();
        while (m_currentToken.kind == ExprTokenKind::Star || m_currentToken.kind == ExprTokenKind::Slash) {
            ExprTokenKind op = m_currentToken.kind;
            advance();
            double right = parseUnary();
            if (op == ExprTokenKind::Star) {
                left = left * right;
            } else {
                if (right == 0) { throw std::runtime_error("Division by zero"); }
                left = left / right;
            }
        }
        return left;
    }

    double parseUnary() {
        if (m_currentToken.kind == ExprTokenKind::Minus) {
            advance();
            return -parseUnary();
        }
        if (m_currentToken.kind == ExprTokenKind::Plus) {
            advance();
            return parseUnary();
        }
        return parsePrimary();
    }

    double parsePrimary() {
        if (m_currentToken.kind == ExprTokenKind::Number) {
            double val = m_currentToken.numValue;
            advance();
            return val;
        }

        if (m_currentToken.kind == ExprTokenKind::Identifier) {
            std::string id = m_currentToken.text;
            advance();

            if (m_currentToken.kind == ExprTokenKind::LParen) { return parseFunctionCall(id); }

            return lookupVariable(id);
        }

        if (m_currentToken.kind == ExprTokenKind::LParen) {
            advance();
            double val = parseExpression();
            expect(ExprTokenKind::RParen, "Expected ')'");
            return val;
        }

        throw std::runtime_error("Unexpected token: " + m_currentToken.text);
    }

    double parseFunctionCall(const std::string& funcName) {
        expect(ExprTokenKind::LParen, "Expected '(' after function name");

        std::vector<double> args;
        if (m_currentToken.kind != ExprTokenKind::RParen) {
            args.push_back(parseExpression());
            while (m_currentToken.kind == ExprTokenKind::Comma) {
                advance();
                args.push_back(parseExpression());
            }
        }
        expect(ExprTokenKind::RParen, "Expected ')' after function arguments");

        return callFunction(funcName, args);
    }

    double lookupVariable(const std::string& name) {
        if (name == "screenWidth") { return static_cast<double>(m_screenWidth); }
        if (name == "screenHeight") { return static_cast<double>(m_screenHeight); }

        throw std::runtime_error("Unknown variable: " + name);
    }

    double callFunction(const std::string& name, const std::vector<double>& args) {
        if (name == "min") {
            if (args.size() != 2) { throw std::runtime_error("min() requires 2 arguments"); }
            return (std::min)(args[0], args[1]);
        }
        if (name == "max") {
            if (args.size() != 2) { throw std::runtime_error("max() requires 2 arguments"); }
            return (std::max)(args[0], args[1]);
        }
        if (name == "floor") {
            if (args.size() != 1) { throw std::runtime_error("floor() requires 1 argument"); }
            return std::floor(args[0]);
        }
        if (name == "ceil") {
            if (args.size() != 1) { throw std::runtime_error("ceil() requires 1 argument"); }
            return std::ceil(args[0]);
        }
        if (name == "round") {
            if (args.size() != 1) { throw std::runtime_error("round() requires 1 argument"); }
            return std::round(args[0]);
        }
        if (name == "abs") {
            if (args.size() != 1) { throw std::runtime_error("abs() requires 1 argument"); }
            return std::abs(args[0]);
        }
        if (name == "roundEven") {
            if (args.size() != 1) { throw std::runtime_error("roundEven() requires 1 argument"); }
            return std::ceil(args[0] / 2.0) * 2.0;
        }

        throw std::runtime_error("Unknown function: " + name);
    }

    void advance() { m_currentToken = m_tokenizer.next(); }

    void expect(ExprTokenKind k, const std::string& error) {
        if (m_currentToken.kind != k) { throw std::runtime_error(error); }
        advance();
    }

    Tokenizer m_tokenizer;
    ExprToken m_currentToken;
    int m_screenWidth;
    int m_screenHeight;
};


int EvaluateExpression(const std::string& expr, int screenWidth, int screenHeight, int defaultValue) {
    if (expr.empty()) { return defaultValue; }

    std::string trimmed = expr;
    size_t start = trimmed.find_first_not_of(" \t\r\n");
    size_t end = trimmed.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) { return defaultValue; }
    trimmed = trimmed.substr(start, end - start + 1);

    if (trimmed.empty()) { return defaultValue; }

    try {
        ExpressionParser parser(trimmed, screenWidth, screenHeight);
        double result = parser.parse();
        return static_cast<int>(std::floor(result));
    } catch (const std::exception&) { return defaultValue; }
}

bool IsExpression(const std::string& str) {
    if (str.empty()) { return false; }

    std::string trimmed = str;
    size_t start = trimmed.find_first_not_of(" \t\r\n");
    size_t end = trimmed.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) { return false; }
    trimmed = trimmed.substr(start, end - start + 1);

    size_t checkStart = 0;
    if (!trimmed.empty() && trimmed[0] == '-') { checkStart = 1; }

    if (checkStart >= trimmed.size()) { return true; }

    for (size_t i = checkStart; i < trimmed.size(); i++) {
        if (!std::isdigit(trimmed[i])) { return true; }
    }
    return false;
}

bool ValidateExpression(const std::string& expr, std::string& errorOut) {
    if (expr.empty()) {
        errorOut = "Expression cannot be empty";
        return false;
    }

    std::string trimmed = expr;
    size_t start = trimmed.find_first_not_of(" \t\r\n");
    size_t end = trimmed.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) {
        errorOut = "Expression cannot be empty";
        return false;
    }
    trimmed = trimmed.substr(start, end - start + 1);

    try {
        ExpressionParser parser(trimmed, 1920, 1080);
        std::string error = parser.validate();
        if (!error.empty()) {
            errorOut = error;
            return false;
        }
        errorOut.clear();
        return true;
    } catch (const std::exception& e) {
        errorOut = e.what();
        return false;
    }
}

void RecalculateExpressionDimensions() {
    int screenW = GetCachedWindowWidth();
    int screenH = GetCachedWindowHeight();
    if (screenW < 1) screenW = 1;
    if (screenH < 1) screenH = 1;

    for (auto& mode : g_config.modes) {
        if (mode.id == "Fullscreen") {
            mode.width = screenW;
            mode.height = screenH;
            mode.useRelativeSize = true;
            mode.relativeWidth = 1.0f;
            mode.relativeHeight = 1.0f;

            mode.stretch.enabled = true;
            mode.stretch.x = 0;
            mode.stretch.y = 0;
            mode.stretch.width = screenW;
            mode.stretch.height = screenH;

            mode.stretch.widthExpr.clear();
            mode.stretch.heightExpr.clear();
            mode.stretch.xExpr.clear();
            mode.stretch.yExpr.clear();
        }

        // It must not be expression-driven.
        if (mode.id == "Preemptive") {
            mode.widthExpr.clear();
            mode.heightExpr.clear();
        }

        const bool widthIsRelative = mode.id != "Preemptive" && mode.widthExpr.empty() && mode.relativeWidth >= 0.0f && mode.relativeWidth <= 1.0f;
        const bool heightIsRelative = mode.id != "Preemptive" && mode.heightExpr.empty() && mode.relativeHeight >= 0.0f && mode.relativeHeight <= 1.0f;

        if (widthIsRelative) {
            int newWidth = static_cast<int>(std::lround(mode.relativeWidth * static_cast<float>(screenW)));
            if (newWidth < 1) newWidth = 1;
            mode.width = newWidth;
        }
        if (heightIsRelative) {
            int newHeight = static_cast<int>(std::lround(mode.relativeHeight * static_cast<float>(screenH)));
            if (newHeight < 1) newHeight = 1;
            mode.height = newHeight;
        }

        if (mode.id != "Preemptive" && !mode.widthExpr.empty()) {
            int newWidth = EvaluateExpression(mode.widthExpr, screenW, screenH, mode.width);
            if (newWidth > 0) { mode.width = newWidth; }
        }
        if (mode.id != "Preemptive" && !mode.heightExpr.empty()) {
            int newHeight = EvaluateExpression(mode.heightExpr, screenW, screenH, mode.height);
            if (newHeight > 0) { mode.height = newHeight; }
        }

        if (!mode.stretch.widthExpr.empty()) {
            int val = EvaluateExpression(mode.stretch.widthExpr, screenW, screenH, mode.stretch.width);
            if (val >= 0) { mode.stretch.width = val; }
        }
        if (!mode.stretch.heightExpr.empty()) {
            int val = EvaluateExpression(mode.stretch.heightExpr, screenW, screenH, mode.stretch.height);
            if (val >= 0) { mode.stretch.height = val; }
        }
        if (!mode.stretch.xExpr.empty()) { mode.stretch.x = EvaluateExpression(mode.stretch.xExpr, screenW, screenH, mode.stretch.x); }
        if (!mode.stretch.yExpr.empty()) { mode.stretch.y = EvaluateExpression(mode.stretch.yExpr, screenW, screenH, mode.stretch.y); }

        if (mode.id == "Thin" && mode.width < 330) { mode.width = 330; }
    }

    ModeConfig* eyezoomMode = nullptr;
    ModeConfig* preemptiveMode = nullptr;
    for (auto& mode : g_config.modes) {
        if (!eyezoomMode && mode.id == "EyeZoom") { eyezoomMode = &mode; }
        if (!preemptiveMode && mode.id == "Preemptive") { preemptiveMode = &mode; }
    }
    if (eyezoomMode && preemptiveMode) {
        preemptiveMode->width = eyezoomMode->width;
        preemptiveMode->height = eyezoomMode->height;
        preemptiveMode->manualWidth = (eyezoomMode->manualWidth > 0) ? eyezoomMode->manualWidth : eyezoomMode->width;
        preemptiveMode->manualHeight = (eyezoomMode->manualHeight > 0) ? eyezoomMode->manualHeight : eyezoomMode->height;
        preemptiveMode->useRelativeSize = false;
        preemptiveMode->relativeWidth = -1.0f;
        preemptiveMode->relativeHeight = -1.0f;
        preemptiveMode->widthExpr.clear();
        preemptiveMode->heightExpr.clear();
    }
}


