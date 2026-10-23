// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 *   FreeCAD is distributed in the hope that it will be useful, but         *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of             *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
 *   Lesser General Public License for more details.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ***************************************************************************/

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <numbers>

#include "Formula.h"

using namespace Assembly::Solver;

std::string FormulaError::describe() const
{
    return message + " at column " + std::to_string(position + 1);
}

namespace Assembly::Solver
{

// Recursive descent, one function per rule of the grammar in Formula.h, in the
// shape of OndselSolver's SymbolicParser (expression / term / symfunction /
// raisedTo) so that precedence and associativity are the same.
class FormulaParser
{
public:
    FormulaParser(const std::string& text, FormulaTree& tree)
        : text(text)
        , tree(tree)
    {}

    struct Failure
    {
        std::string message;
        std::size_t position;
    };

    std::uint32_t parseAll()
    {
        next();
        if (tok.kind == Kind::End) {
            return constant(0.0);  // a blank formula, as SymbolicParser::expression()
        }
        std::uint32_t root = expression();
        if (tok.kind != Kind::End) {
            if (isChar(')')) {
                fail("unbalanced ')'");
            }
            fail("expected an operator or the end of the formula, found " + describeToken());
        }
        return root;
    }

private:
    using Op = FormulaTree::Op;
    enum class Kind
    {
        End,
        Number,
        Word,
        Char
    };
    struct Token
    {
        Kind kind = Kind::End;
        std::size_t start = 0;
        std::size_t end = 0;
        double number = 0.0;
        std::string word;
        char ch = 0;
    };

    [[noreturn]] void fail(const std::string& message, std::size_t position) const
    {
        throw Failure {message, position};
    }
    [[noreturn]] void fail(const std::string& message) const
    {
        fail(message, tok.start);
    }

    static bool isDigit(char c)
    {
        return c >= '0' && c <= '9';
    }
    static bool isAlpha(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    }
    char at(std::size_t i) const
    {
        return i < text.size() ? text[i] : '\0';
    }

    std::string describeToken() const
    {
        switch (tok.kind) {
            case Kind::End:
                return "the end of the formula";
            case Kind::Number:
                return "the number '" + text.substr(tok.start, tok.end - tok.start) + "'";
            case Kind::Word:
                return "'" + tok.word + "'";
            case Kind::Char:
                break;
        }
        return std::string("'") + tok.ch + "'";
    }

    // Digits, an optional fraction, an optional exponent: what an istream
    // reads as a double. Then, as SymbolicParser::xDigit(), an optional
    // 'd' exponent: 1.5d3 is 1.5 * 10^3.
    void lexNumber()
    {
        std::size_t i = pos;
        while (isDigit(at(i))) {
            ++i;
        }
        if (at(i) == '.') {
            ++i;
            while (isDigit(at(i))) {
                ++i;
            }
        }
        if (at(i) == 'e' || at(i) == 'E') {
            std::size_t j = i + 1;
            if (at(j) == '+' || at(j) == '-') {
                ++j;
            }
            if (!isDigit(at(j))) {
                fail("incomplete exponent", i);
            }
            while (isDigit(at(j))) {
                ++j;
            }
            i = j;
        }
        double mantissa = 0.0;
        auto res = std::from_chars(text.data() + pos, text.data() + i, mantissa);
        if (res.ec == std::errc::invalid_argument) {
            fail("bad number", pos);
        }
        // Out of range: from_chars leaves the value alone; an istream gives
        // the largest value, std::pow below an infinity. Keep that.
        if (res.ec == std::errc::result_out_of_range) {
            mantissa = std::strtod(text.substr(pos, i - pos).c_str(), nullptr);
        }
        double value = mantissa;
        if (at(i) == 'd') {
            std::size_t j = i + 1;
            bool negative = false;
            if (at(j) == '+' || at(j) == '-') {
                negative = at(j) == '-';
                ++j;
            }
            if (!isDigit(at(j))) {
                fail("incomplete exponent", i);
            }
            int exponent = 0;
            while (isDigit(at(j))) {
                exponent = std::min(exponent * 10 + (at(j) - '0'), 100000);
                ++j;
            }
            value = mantissa * std::pow(10.0, negative ? -exponent : exponent);
            i = j;
        }
        tok.kind = Kind::Number;
        tok.number = value;
        tok.end = i;
        pos = i;
    }

    void next()
    {
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
            ++pos;
        }
        tok = Token {};
        tok.start = pos;
        if (pos >= text.size()) {
            tok.kind = Kind::End;
            tok.end = pos;
            return;
        }
        char c = text[pos];
        if (isAlpha(c)) {
            std::size_t i = pos + 1;
            while (isAlpha(at(i)) || isDigit(at(i)) || at(i) == '_') {
                ++i;
            }
            tok.kind = Kind::Word;
            tok.word = text.substr(pos, i - pos);
            tok.end = pos = i;
            return;
        }
        if (isDigit(c)) {
            lexNumber();
            return;
        }
        static const std::string singles = "+-*/^(),";
        if (singles.find(c) == std::string::npos) {
            if (c == '.') {
                fail("a number must start with a digit (write 0.5, not .5)");
            }
            if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x7f) {
                fail("unexpected character");
            }
            fail(std::string("unexpected character '") + c + "'");
        }
        tok.kind = Kind::Char;
        tok.ch = c;
        tok.end = ++pos;
    }

    bool isChar(char c) const
    {
        return tok.kind == Kind::Char && tok.ch == c;
    }
    void expect(char c)
    {
        if (!isChar(c)) {
            fail(std::string("expected '") + c + "', found " + describeToken());
        }
        next();
    }

    std::uint32_t add(Op op, std::vector<std::uint32_t> args = {}, double value = 0.0)
    {
        FormulaTree::Node node;
        node.op = op;
        node.constant = value;
        node.timeDependent = op == Op::Time;
        for (auto arg : args) {
            node.timeDependent = node.timeDependent || tree.nodes[arg].timeDependent;
        }
        node.args = std::move(args);
        tree.nodes.push_back(std::move(node));
        return static_cast<std::uint32_t>(tree.nodes.size() - 1);
    }
    std::uint32_t constant(double value)
    {
        return add(Op::Constant, {}, value);
    }

    // expression := [ '+' | '-' ] term { ( '+' | '-' ) term }
    std::uint32_t expression()
    {
        std::uint32_t acc = 0;
        if (isChar('+')) {
            next();
            acc = term();
        }
        else if (isChar('-')) {
            next();
            acc = add(Op::Negate, {term()});
        }
        else {
            acc = term();
        }
        while (isChar('+') || isChar('-')) {
            Op op = isChar('+') ? Op::Add : Op::Subtract;
            next();
            acc = add(op, {acc, term()});
        }
        return acc;
    }

    // term := factor { ( '*' | '/' ) factor }
    std::uint32_t term()
    {
        std::uint32_t acc = factor();
        while (isChar('*') || isChar('/')) {
            Op op = isChar('*') ? Op::Multiply : Op::Divide;
            next();
            acc = add(op, {acc, factor()});
        }
        return acc;
    }

    // factor := primary [ '^' factor ]
    std::uint32_t factor()
    {
        std::uint32_t base = primary();
        if (isChar('^')) {
            next();
            return add(Op::Power, {base, factor()});
        }
        return base;
    }

    std::uint32_t primary()
    {
        if (isChar('(')) {
            next();
            std::uint32_t inner = expression();
            expect(')');
            return inner;
        }
        if (tok.kind == Kind::Number) {
            std::uint32_t num = constant(tok.number);
            next();
            return num;
        }
        // A sign glued to a number, as SymbolicParser::signedNumber(): 2*-3 and
        // 2^-1, but not 2*- 3 or 2*-time.
        if ((isChar('+') || isChar('-')) && isDigit(at(tok.end))) {
            const bool negative = isChar('-');
            next();
            std::uint32_t num = constant(negative ? -tok.number : tok.number);
            next();
            return num;
        }
        if (isChar('+') || isChar('-')) {
            if (at(tok.end) == '.') {
                fail("a number must start with a digit (write 0.5, not .5)", tok.end);
            }
            fail(
                std::string("a sign here must be followed directly by a number; write ") + tok.ch
                + "1*(...) or put the term in parentheses"
            );
        }
        if (tok.kind == Kind::Word) {
            return word();
        }
        if (tok.kind == Kind::End) {
            fail("expected a number, 'time', 'pi', a function or '(', found the end of the formula");
        }
        fail("expected a number, 'time', 'pi', a function or '(', found " + describeToken());
    }

    std::uint32_t word()
    {
        const std::string name = tok.word;
        const std::size_t start = tok.start;
        if (name == "pi") {
            next();
            return constant(std::numbers::pi);
        }
        if (name == "time") {
            next();
            return add(Op::Time);
        }
        struct Unary
        {
            const char* name;
            Op op;
        };
        static const Unary unary[] = {
            {"abs", Op::Abs},
            {"sin", Op::Sin},
            {"cos", Op::Cos},
            {"asin", Op::ArcSin},
            {"arcsin", Op::ArcSin},
            {"arctan", Op::ArcTan},
            {"tan", Op::Tan},
            {"exp", Op::Exp},
            {"ln", Op::Ln},
            {"sqrt", Op::Sqrt},
        };
        for (const auto& fn : unary) {
            if (name == fn.name) {
                next();
                auto args = arguments(name);
                if (args.size() != 1) {
                    fail("'" + name + "' takes one argument", start);
                }
                return add(fn.op, {args[0]});
            }
        }
        if (name == "rampstep") {
            next();
            return rampstep(start);
        }
        if (name == "piecewise") {
            next();
            return piecewise();
        }
        if (name == "functions" || name == "transitions") {
            fail("'" + name + "' is only allowed as an argument of piecewise");
        }
        if (name == "spline" || name == "integral") {
            fail("'" + name + "' is not supported in a motion formula");
        }
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        });
        static const char* known[] = {
            "pi",
            "time",
            "abs",
            "sin",
            "cos",
            "asin",
            "arcsin",
            "arctan",
            "tan",
            "exp",
            "ln",
            "sqrt",
            "rampstep",
            "piecewise",
        };
        for (const char* k : known) {
            if (lower == k) {
                fail("unknown name '" + name + "' (names are case-sensitive: write '" + k + "')");
            }
        }
        next();
        if (isChar('(')) {
            fail("unknown function '" + name + "'", start);
        }
        fail("unknown name '" + name + "' (the variable is 'time')", start);
    }

    // '(' expression { ',' expression } ')', after the function's name.
    std::vector<std::uint32_t> arguments(const std::string& name)
    {
        if (!isChar('(')) {
            fail("expected '(' after '" + name + "'");
        }
        next();
        std::vector<std::uint32_t> args;
        args.push_back(expression());
        while (isChar(',')) {
            next();
            args.push_back(expression());
        }
        expect(')');
        return args;
    }

    // rampstep(x, x0, y0, x1, y1): y0 below x0, y1 from x1, the line between
    // them in between, as OndselSolver's RampStepFunction.
    std::uint32_t rampstep(std::size_t start)
    {
        std::vector<std::size_t> positions;
        if (!isChar('(')) {
            fail("expected '(' after 'rampstep'");
        }
        next();
        std::vector<std::uint32_t> args;
        positions.push_back(tok.start);
        args.push_back(expression());
        while (isChar(',')) {
            next();
            positions.push_back(tok.start);
            args.push_back(expression());
        }
        expect(')');
        if (args.size() != 5) {
            fail("'rampstep' takes five arguments: rampstep(x, x0, y0, x1, y1)", start);
        }
        double c[4] {};
        for (int i = 0; i < 4; ++i) {
            if (tree.nodes[args[i + 1]].timeDependent) {
                fail("rampstep's x0, y0, x1 and y1 must not depend on time", positions[i + 1]);
            }
            c[i] = tree.eval(args[i + 1], 0.0).value;
        }
        const double x0 = c[0], y0 = c[1], x1 = c[2], y1 = c[3];
        if (x0 == x1) {
            fail("rampstep's x0 and x1 must differ", positions[1]);
        }
        const double slope = (y1 - y0) / (x1 - x0);
        const double intercept = y0 - (y1 - y0) * x0 / (x1 - x0);
        std::uint32_t line = add(Op::Add, {add(Op::Multiply, {constant(slope), args[0]}), constant(intercept)});
        return add(
            Op::Piecewise,
            {args[0], constant(y0), line, constant(y1), constant(x0), constant(x1)}
        );
    }

    // piecewise(x, functions(f0, ..., fn), transitions(x1, ..., xn)): the first
    // fi with x < x(i+1), else fn, as OndselSolver's PiecewiseFunction.
    std::uint32_t piecewise()
    {
        if (!isChar('(')) {
            fail("expected '(' after 'piecewise'");
        }
        next();
        std::uint32_t x = expression();
        expect(',');
        auto list = [this](const char* name) {
            if (tok.kind != Kind::Word || tok.word != name) {
                fail(std::string("expected ") + name + "(...), found " + describeToken());
            }
            next();
            return arguments(name);
        };
        const std::size_t fnStart = tok.start;
        auto pieces = list("functions");
        expect(',');
        auto transitions = list("transitions");
        expect(')');
        if (pieces.size() != transitions.size() + 1) {
            fail("piecewise needs one function more than transitions", fnStart);
        }
        std::vector<std::uint32_t> args {x};
        args.insert(args.end(), pieces.begin(), pieces.end());
        args.insert(args.end(), transitions.begin(), transitions.end());
        return add(Op::Piecewise, std::move(args));
    }

    const std::string& text;
    FormulaTree& tree;
    std::size_t pos = 0;
    Token tok;
};

}  // namespace Assembly::Solver

std::shared_ptr<const FormulaTree> FormulaTree::parse(const std::string& text, FormulaError* error)
{
    auto tree = std::make_shared<FormulaTree>();
    tree->source = text;
    try {
        FormulaParser parser(tree->source, *tree);
        tree->root = parser.parseAll();
    }
    catch (const FormulaParser::Failure& failure) {
        if (error) {
            error->message = failure.message;
            error->position = failure.position;
        }
        return nullptr;
    }
    if (error) {
        *error = FormulaError {};
    }
    return tree;
}

bool FormulaTree::dependsOnTime() const
{
    return !nodes.empty() && nodes[root].timeDependent;
}

double FormulaTree::value(double time) const
{
    return eval(root, time).value;
}

FormulaValue FormulaTree::evaluate(double time) const
{
    return eval(root, time);
}

FormulaValue FormulaTree::eval(std::uint32_t index, double time) const
{
    const Node& node = nodes[index];
    auto arg = [&](std::size_t i) {
        return eval(node.args[i], time);
    };
    switch (node.op) {
        case Op::Constant:
            return {node.constant, 0.0, 0.0};
        case Op::Time:
            return {time, 1.0, 0.0};
        case Op::Negate: {
            auto a = arg(0);
            return {-a.value, -a.rate, -a.acceleration};
        }
        case Op::Add: {
            auto a = arg(0);
            auto b = arg(1);
            return {a.value + b.value, a.rate + b.rate, a.acceleration + b.acceleration};
        }
        case Op::Subtract: {
            auto a = arg(0);
            auto b = arg(1);
            return {a.value - b.value, a.rate - b.rate, a.acceleration - b.acceleration};
        }
        case Op::Multiply: {
            auto a = arg(0);
            auto b = arg(1);
            return {
                a.value * b.value,
                a.rate * b.value + a.value * b.rate,
                a.acceleration * b.value + 2.0 * a.rate * b.rate + a.value * b.acceleration
            };
        }
        case Op::Divide: {
            auto a = arg(0);
            auto b = arg(1);
            FormulaValue q;
            q.value = a.value / b.value;
            if (a.rate == 0.0 && a.acceleration == 0.0 && b.rate == 0.0 && b.acceleration == 0.0) {
                return q;
            }
            q.rate = (a.rate - q.value * b.rate) / b.value;
            q.acceleration = (a.acceleration - 2.0 * q.rate * b.rate - q.value * b.acceleration)
                / b.value;
            return q;
        }
        case Op::Power: {
            auto u = arg(0);
            auto w = arg(1);
            FormulaValue f;
            f.value = std::pow(u.value, w.value);
            const bool baseConstant = u.rate == 0.0 && u.acceleration == 0.0;
            const bool exponentConstant = w.rate == 0.0 && w.acceleration == 0.0;
            if (baseConstant && exponentConstant) {
                return f;
            }
            if (exponentConstant) {
                // d(u^c) = c u^(c-1) u'
                const double c = w.value;
                const double d1 = c * std::pow(u.value, c - 1.0);
                const double d2 = c * (c - 1.0) * std::pow(u.value, c - 2.0);
                f.rate = d1 * u.rate;
                f.acceleration = d2 * u.rate * u.rate + d1 * u.acceleration;
                return f;
            }
            if (baseConstant) {
                // d(b^w) = b^w ln(b) w'
                const double lnb = std::log(u.value);
                f.rate = f.value * lnb * w.rate;
                f.acceleration = f.value * (lnb * lnb * w.rate * w.rate + lnb * w.acceleration);
                return f;
            }
            // u^w = exp(g), g = w ln(u)
            const double lnu = std::log(u.value);
            const double g1 = w.rate * lnu + w.value * u.rate / u.value;
            const double g2 = w.acceleration * lnu + 2.0 * w.rate * u.rate / u.value
                + w.value * (u.acceleration * u.value - u.rate * u.rate) / (u.value * u.value);
            f.rate = f.value * g1;
            f.acceleration = f.value * (g1 * g1 + g2);
            return f;
        }
        case Op::Abs: {
            auto a = arg(0);
            const double s = (a.value > 0.0) - (a.value < 0.0);
            return {std::abs(a.value), s * a.rate, s * a.acceleration};
        }
        case Op::Sin: {
            auto a = arg(0);
            const double s = std::sin(a.value);
            const double c = std::cos(a.value);
            return {s, c * a.rate, -s * a.rate * a.rate + c * a.acceleration};
        }
        case Op::Cos: {
            auto a = arg(0);
            const double s = std::sin(a.value);
            const double c = std::cos(a.value);
            return {c, -s * a.rate, -c * a.rate * a.rate - s * a.acceleration};
        }
        case Op::ArcSin: {
            auto a = arg(0);
            FormulaValue f {std::asin(a.value), 0.0, 0.0};
            if (a.rate == 0.0 && a.acceleration == 0.0) {
                return f;
            }
            const double s = 1.0 / std::sqrt(1.0 - a.value * a.value);
            f.rate = s * a.rate;
            f.acceleration = s * a.acceleration + a.value * s * s * s * a.rate * a.rate;
            return f;
        }
        case Op::ArcTan: {
            auto a = arg(0);
            const double s = 1.0 / (1.0 + a.value * a.value);
            return {
                std::atan(a.value),
                s * a.rate,
                s * a.acceleration - 2.0 * a.value * s * s * a.rate * a.rate
            };
        }
        case Op::Tan: {
            auto a = arg(0);
            const double t = std::tan(a.value);
            const double sec2 = 1.0 + t * t;  // the derivative, 1 / cos^2
            return {t, sec2 * a.rate, sec2 * (a.acceleration + 2.0 * t * a.rate * a.rate)};
        }
        case Op::Exp: {
            auto a = arg(0);
            const double e = std::exp(a.value);
            return {e, e * a.rate, e * (a.acceleration + a.rate * a.rate)};
        }
        case Op::Ln: {
            auto a = arg(0);
            FormulaValue f {std::log(a.value), 0.0, 0.0};
            if (a.rate == 0.0 && a.acceleration == 0.0) {
                return f;
            }
            const double r = a.rate / a.value;
            f.rate = r;
            f.acceleration = a.acceleration / a.value - r * r;
            return f;
        }
        case Op::Sqrt: {
            auto a = arg(0);
            FormulaValue f {std::sqrt(a.value), 0.0, 0.0};
            if (a.rate == 0.0 && a.acceleration == 0.0) {
                return f;
            }
            f.rate = 0.5 * a.rate / f.value;
            f.acceleration = (0.5 * a.acceleration - f.rate * f.rate) / f.value;
            return f;
        }
        case Op::Piecewise: {
            const double x = arg(0).value;
            const std::size_t n = (node.args.size() - 2) / 2;  // transitions
            for (std::size_t i = 0; i < n; ++i) {
                if (x < arg(2 + n + i).value) {
                    return arg(1 + i);
                }
            }
            return arg(1 + n);
        }
    }
    return {};
}
