// SPDX-License-Identifier: LGPL-2.1-or-later

// The neutral formula evaluator (Solver/Formula.h, docs/AssemblySolverInterface.md
// sec 4.6.1) against OndselSolver's SymbolicParser, which it replaces as the
// workbench's reading of a motion formula: the same value at every time, for
// every formula the workbench and its tests use, the help text's examples and
// the edge cases of the grammar; derivatives against finite differences; and
// the errors, with their positions.
//
// This is the only place the evaluator meets OndselSolver: the comparison.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include <Base/Tools.h>  // M_PI, which OndselSolver's headers use
#include <Mod/Assembly/App/Solver/Formula.h>

#include <OndselSolver/SymbolicParser.h>
#include <OndselSolver/SymTime.h>

using namespace Assembly::Solver;

namespace
{

// A formula as OndselSolver's motions read it (ASMTRotationalMotion::createMbD):
// SymbolicParser with the variable "time", before units and simplification.
class OndselFormula
{
public:
    explicit OndselFormula(const std::string& text)
    {
        MbD::SymbolicParser parser;
        parser.variables->insert(std::make_pair(std::string("time"), time));
        parser.parseString(text);
        expr = parser.stack->top();
    }
    double value(double t)
    {
        time->setValue(t);
        return expr->getValue();
    }
    // The first two time derivatives as runKINEMATIC forms them
    // (ASMTRotationalMotion::createMbD simplifies, then
    // EndFrameqct::initpPhiThePsiptBlks differentiates and simplifies, twice).
    // Throws where a function has no derivative in OndselSolver.
    void differentiate()
    {
        auto simple = expr->simplified(expr);
        auto d1 = simple->differentiateWRT(time);
        rateExpr = d1->simplified(d1);
        auto d2 = rateExpr->differentiateWRT(time);
        accelerationExpr = d2->simplified(d2);
    }
    double rate(double t)
    {
        time->setValue(t);
        return rateExpr->getValue();
    }
    double acceleration(double t)
    {
        time->setValue(t);
        return accelerationExpr->getValue();
    }

private:
    // An independent variable, as the system's time a motion's geoTime
    // stands for (differentiating a plain Variable throws).
    std::shared_ptr<MbD::Time> time = std::make_shared<MbD::Time>();
    MbD::Symsptr expr;
    MbD::Symsptr rateExpr;
    MbD::Symsptr accelerationExpr;
};

bool ondselAccepts(const std::string& text)
{
    try {
        OndselFormula formula(text);
        return true;
    }
    catch (const std::exception&) {
        return false;
    }
}

struct Case
{
    std::string text;
    double tMin = 0.01;  // the domain sampled
    double tMax = 2.5;
    std::vector<double> kinks = {};  // no finite difference across these
    double fdStep = 1e-4;  // for the derivative checks
};

// Every formula the workbench's tests use (initialValue substituted the way
// AssemblyObject does, precision 10), the help text's examples
// (CommandCreateSimulation.py) with numbers for the capitals, the property's
// and the dialog's defaults, and the corners of the grammar.
std::vector<Case> corpus()
{
    return {
        // TestSolverGate.py
        {"36*time"},
        {"5*time + 12.5"},
        {"5*time + -3.25"},
        {"5*time + 1e-05"},
        {"90*time"},
        {"2*sin(time)"},
        {"45*time"},
        // defaults: the Formula property's tooltip and the dialog
        {"1.0*time"},
        {"0.7853981634 + 5*time"},
        {"-0.7853981634 + 5*time"},
        // help text
        {"1.5 + 2*time"},
        {"1.5 + 2*time + 3*time^2"},
        {"1 + 2*sin(3*time - 0.5)"},
        {"10 + (20 - 10)*((1/2) + (1/pi)*arctan(50*(time - 0.5)))"},
        {"(4/pi)*(arctan(20*(time - 0.4)) - arctan(20*(time - 1.2)))"},
        {"((1/pi)*(arctan(1000*(time - 0.4)) - arctan(1000*(time - 1.2))))*(((7 - 3)/(1.2 - 0.4))*(time - 0.4) + 3)",
         0.01,
         2.5,
         {0.4, 1.2},
         1e-6},
        // signs
        {"-time"},
        {"+time"},
        {"-time^2"},
        {"-2^2*time"},
        {"3*-2^2*time"},
        {"time*-2"},
        {"time/-2"},
        {"2^-1*time"},
        {"time^-2"},
        {"1 - -2*time"},
        {"1--2*time"},
        {"- -2*time"},
        {"++2*time"},
        {"time - +3"},
        {"-(time - 1)"},
        {"2*(-time)"},
        // precedence and associativity
        {"1 - 2 - 3*time"},
        {"time/2/3"},
        {"2*time/3*4"},
        {"2^3^0.5*time"},
        {"2^time^2"},
        {"(2^time)^2"},
        {"time^2*3"},
        {"3*time^2/2"},
        {"1 + 2*3 - 4/5*time"},
        {"((((time))))"},
        {"  time  "},
        {"time*time*time - 2*time"},
        // numbers
        {"1.5e2*time"},
        {"1.5E2*time"},
        {"2e-3*time"},
        {"1.5d-1*time"},
        {"2d2*time"},
        {"1.*time"},
        {"007*time"},
        {"-1.234567891e-05 + time"},
        {"pi*time"},
        // functions
        {"sin(time)"},
        {"cos(time)"},
        {"sin(time)^2 + cos(time)^2"},
        {"cos(time)*sin(2*time)"},
        {"sin(cos(sin(time)))"},
        {"abs(sin(time) - 0.3)", 0.01, 2.5, {std::asin(0.3)}},
        {"abs(time - 1)*time", 0.01, 2.5, {1.0}},
        {"asin(time/4)"},
        {"arcsin(time/4)"},
        {"arctan(time)"},
        {"arctan(3*time - 2)^2"},
        {"time^0.5"},
        {"time^1.5 + time^3"},
        {"(time + 1)^time"},
        {"2^time"},
        {"time^(sin(time) + 2)"},
        {"1/(1 + time)"},
        {"1/(1 + time^2) - time/(2 + cos(time))"},
        // tan, exp, ln, sqrt; the help text's Exponential with C = 2, TIMEC = 0.8
        {"2*exp(time/0.8)"},
        {"exp(time)"},
        {"exp(-time)*sin(3*time)"},
        {"time*exp(-time^2)"},
        {"exp(time)^2"},
        {"ln(time)", 0.1},
        {"ln(2 + sin(time))"},
        {"ln(1 + time^2)/2"},
        {"exp(ln(1 + time))"},
        {"sqrt(time)", 0.1},
        {"sqrt(1 + time^2)"},
        {"1/sqrt(1 + time)"},
        {"sqrt(exp(time))*ln(1 + time^2)"},
        {"tan(time/4)"},
        {"tan(time)", 0.01, 1.4},
        {"tan(time + 2)", 0.01, 1.0},  // cos < 0
        {"tan(sqrt(time))/ln(3 + time)"},
        {"-2*tan(time/4)^2 + sqrt(2)*exp(1)"},
        {"rampstep(time, 0.2, 1, 0.8, 3)", 0.01, 2.5, {0.2, 0.8}},
        {"rampstep(2*time, 0.4, -1, 1.6, 2.5)*time", 0.01, 2.5, {0.2, 0.8}},
        {"piecewise(time, functions(0, time^2, 1), transitions(0.3, 0.7))", 0.01, 2.5, {0.3, 0.7}},
        {"piecewise(time - 1, functions(sin(time), 2*time), transitions(0))", 0.01, 2.5, {1.0}},
        // blank: the constant 0, as for the solver
        {""},
        {"   "},
    };
}

std::vector<double> samples(const Case& c)
{
    std::vector<double> times;
    const int n = 97;
    for (int i = 0; i < n; ++i) {
        times.push_back(c.tMin + (c.tMax - c.tMin) * i / (n - 1));
    }
    return times;
}

bool nearKink(const Case& c, double t, double reach)
{
    return std::ranges::any_of(c.kinks, [&](double k) {
        return std::abs(t - k) < reach;
    });
}

// Five-point central difference.
double derivative(const std::function<double(double)>& f, double t, double h)
{
    return (-f(t + 2 * h) + 8 * f(t + h) - 8 * f(t - h) + f(t - 2 * h)) / (12 * h);
}

}  // namespace

TEST(FormulaEvaluator, valuesMatchOndselSolver)
{
    for (const auto& c : corpus()) {
        SCOPED_TRACE(c.text);
        FormulaError error;
        auto tree = FormulaTree::parse(c.text, &error);
        ASSERT_TRUE(tree) << error.describe();
        OndselFormula ondsel(c.text);
        for (double t : samples(c)) {
            const double expected = ondsel.value(t);
            const double actual = tree->value(t);
            ASSERT_TRUE(std::isfinite(expected)) << "t=" << t;
            EXPECT_NEAR(actual, expected, 1e-12 * std::max(1.0, std::abs(expected))) << "t=" << t;
            EXPECT_EQ(tree->evaluate(t).value, actual) << "t=" << t;
        }
    }
}

TEST(FormulaEvaluator, derivativesMatchFiniteDifferences)
{
    for (const auto& c : corpus()) {
        SCOPED_TRACE(c.text);
        auto tree = FormulaTree::parse(c.text);
        ASSERT_TRUE(tree);
        auto value = [&](double t) {
            return tree->evaluate(t).value;
        };
        auto rate = [&](double t) {
            return tree->evaluate(t).rate;
        };
        for (double t : samples(c)) {
            if (nearKink(c, t, 10 * c.fdStep) || t - 2 * c.fdStep <= 0.0) {
                continue;
            }
            const FormulaValue v = tree->evaluate(t);
            const double d1 = derivative(value, t, c.fdStep);
            const double d2 = derivative(rate, t, c.fdStep);
            EXPECT_NEAR(v.rate, d1, 1e-6 * std::max(1.0, std::abs(d1))) << "t=" << t;
            EXPECT_NEAR(v.acceleration, d2, 1e-6 * std::max(1.0, std::abs(d2))) << "t=" << t;
        }
    }
}

// OndselSolver's symbolic derivatives, as runKINEMATIC uses them for the
// velocities and accelerations of a motion, against the evaluator's. abs,
// asin and arctan have no derivative in OndselSolver (FunctionX throws "To be
// implemented"), so a kinematic run of a formula using them fails there.
// Four formulas get wrong derivatives from OndselSolver, none through tan,
// exp, ln or sqrt: a power with a time-dependent base and exponent
// (Symbolic::times appends to the terms vector of the Product it is given,
// which the Power's derivative still shares), and a piecewise whose x is not
// time itself (PiecewiseFunction::differentiateWRTx differentiates the pieces
// with respect to x, and the pieces are functions of time). The test keeps
// them, asserting that they still disagree, so a fix shows up here.
TEST(FormulaEvaluator, derivativesMatchOndselSolver)
{
    const std::vector<std::string> underivable = {"abs(", "asin(", "arcsin(", "arctan("};
    const std::vector<std::string> wrongInOndsel = {
        "(time + 1)^time",
        "time^(sin(time) + 2)",
        "rampstep(2*time, 0.4, -1, 1.6, 2.5)*time",
        "piecewise(time - 1, functions(sin(time), 2*time), transitions(0))",
    };
    for (const auto& c : corpus()) {
        SCOPED_TRACE(c.text);
        auto tree = FormulaTree::parse(c.text);
        ASSERT_TRUE(tree);
        OndselFormula ondsel(c.text);
        const bool expectFailure = std::ranges::any_of(underivable, [&](const std::string& f) {
            return c.text.find(f) != std::string::npos;
        });
        try {
            ondsel.differentiate();
        }
        catch (const std::exception&) {
            EXPECT_TRUE(expectFailure);
            continue;
        }
        EXPECT_FALSE(expectFailure);
        const bool wrong = std::ranges::find(wrongInOndsel, c.text) != wrongInOndsel.end();
        bool differs = false;
        for (double t : samples(c)) {
            if (nearKink(c, t, 1e-9)) {
                continue;
            }
            const FormulaValue v = tree->evaluate(t);
            const double rate = ondsel.rate(t);
            const double acceleration = ondsel.acceleration(t);
            if (wrong) {
                differs = differs || std::abs(v.rate - rate) > 1e-6 * std::max(1.0, std::abs(rate));
                continue;
            }
            EXPECT_NEAR(v.rate, rate, 1e-10 * std::max(1.0, std::abs(rate))) << "t=" << t;
            EXPECT_NEAR(v.acceleration, acceleration, 1e-10 * std::max(1.0, std::abs(acceleration)))
                << "t=" << t;
        }
        EXPECT_EQ(differs, wrong);
    }
}

// Outside a function's domain the evaluator gives what IEEE arithmetic gives
// (NaN, or an infinity), as SymbolicParser's functions do alone; a caller
// holding a driver checks the value is finite (simulateByPositionSolves).
TEST(FormulaEvaluator, domainErrors)
{
    auto at = [](const std::string& text, double t) {
        auto tree = FormulaTree::parse(text);
        EXPECT_TRUE(tree) << text;
        return tree ? tree->evaluate(t) : FormulaValue {};
    };
    for (const char* text : {"ln(time - 1)", "sqrt(time - 1)", "ln(-1)", "sqrt(-2)", "asin(time + 1)"}) {
        SCOPED_TRACE(text);
        FormulaValue v = at(text, 0.5);
        EXPECT_TRUE(std::isnan(v.value));
        EXPECT_TRUE(std::isnan(OndselFormula(text).value(0.5)));
    }
    EXPECT_EQ(at("ln(time - 0.5)", 0.5).value, -std::numeric_limits<double>::infinity());
    EXPECT_EQ(at("ln(0)", 0.0).value, -std::numeric_limits<double>::infinity());
    EXPECT_EQ(at("sqrt(time - 0.5)", 0.5).value, 0.0);
    EXPECT_EQ(at("sqrt(0)", 0.0).rate, 0.0);
    EXPECT_TRUE(std::isinf(at("sqrt(time - 0.5)", 0.5).rate));
    // OndselSolver's products drop a non-finite factor: 2*ln(time - 1) is 2
    // there at 0.5, NaN here.
    EXPECT_TRUE(std::isnan(at("2*ln(time - 1)", 0.5).value));
    EXPECT_EQ(OndselFormula("2*ln(time - 1)").value(0.5), 2.0);
}

TEST(FormulaEvaluator, exactDerivatives)
{
    auto at = [](const std::string& text, double t) {
        auto tree = FormulaTree::parse(text);
        EXPECT_TRUE(tree) << text;
        return tree ? tree->evaluate(t) : FormulaValue {};
    };
    FormulaValue v = at("3 + 2*time + 5*time^2", 2.0);
    EXPECT_DOUBLE_EQ(v.value, 27.0);
    EXPECT_DOUBLE_EQ(v.rate, 22.0);
    EXPECT_DOUBLE_EQ(v.acceleration, 10.0);

    v = at("4*sin(3*time)", 0.0);
    EXPECT_DOUBLE_EQ(v.value, 0.0);
    EXPECT_DOUBLE_EQ(v.rate, 12.0);
    EXPECT_DOUBLE_EQ(v.acceleration, 0.0);

    v = at("90*time", 0.25);
    EXPECT_DOUBLE_EQ(v.rate, 90.0);
    EXPECT_EQ(v.acceleration, 0.0);

    v = at("7", 1.0);
    EXPECT_EQ(v.rate, 0.0);
    EXPECT_EQ(v.acceleration, 0.0);
    EXPECT_FALSE(FormulaTree::parse("7 + pi^2")->dependsOnTime());
    EXPECT_TRUE(FormulaTree::parse("0*time")->dependsOnTime());

    // The piece in effect, not the jump.
    v = at("rampstep(time, 1, 0, 3, 4)", 2.0);
    EXPECT_DOUBLE_EQ(v.value, 2.0);
    EXPECT_DOUBLE_EQ(v.rate, 2.0);
    v = at("rampstep(time, 1, 0, 3, 4)", 0.5);
    EXPECT_EQ(v.value, 0.0);
    EXPECT_EQ(v.rate, 0.0);
    v = at("rampstep(time, 1, 0, 3, 4)", 3.0);
    EXPECT_EQ(v.value, 4.0);

    v = at("3*exp(2*time)", 0.0);
    EXPECT_DOUBLE_EQ(v.value, 3.0);
    EXPECT_DOUBLE_EQ(v.rate, 6.0);
    EXPECT_DOUBLE_EQ(v.acceleration, 12.0);

    v = at("ln(time)", 2.0);
    EXPECT_DOUBLE_EQ(v.value, std::log(2.0));
    EXPECT_DOUBLE_EQ(v.rate, 0.5);
    EXPECT_DOUBLE_EQ(v.acceleration, -0.25);

    v = at("sqrt(time)", 4.0);
    EXPECT_DOUBLE_EQ(v.value, 2.0);
    EXPECT_DOUBLE_EQ(v.rate, 0.25);
    EXPECT_DOUBLE_EQ(v.acceleration, -1.0 / 32.0);

    v = at("tan(time)", 0.0);
    EXPECT_EQ(v.value, 0.0);
    EXPECT_DOUBLE_EQ(v.rate, 1.0);
    EXPECT_EQ(v.acceleration, 0.0);
    v = at("tan(time)", std::numbers::pi / 4);
    EXPECT_DOUBLE_EQ(v.rate, 2.0);
    EXPECT_DOUBLE_EQ(v.acceleration, 4.0);
}

namespace
{

struct Bad
{
    std::string text;
    std::size_t position;
    std::string message;  // a part of it
};

}  // namespace

// Rejected by both: OndselSolver throws, the evaluator reports where.
TEST(FormulaEvaluator, errorsWithPositions)
{
    const std::vector<Bad> cases = {
        {"log(time)", 0, "unknown function 'log'"},
        {"2*acos (time)", 2, "unknown function 'acos'"},
        {"Exp(time)", 0, "case-sensitive: write 'exp'"},
        {"1 + x", 4, "unknown name 'x'"},
        {"TIME", 0, "case-sensitive: write 'time'"},
        {"Sin(time)", 0, "case-sensitive: write 'sin'"},
        {"2*-time", 2, "followed directly by a number"},
        {"--time", 1, "followed directly by a number"},
        {"1 - - 2", 4, "followed directly by a number"},
        {".5*time", 0, "must start with a digit"},
        {"2+", 2, "found the end of the formula"},
        {"2*", 2, "found the end of the formula"},
        {"(time", 5, "expected ')'"},
        {"sin(time", 8, "expected ')'"},
        {"sin()", 4, "found ')'"},
        {"sin", 3, "expected '(' after 'sin'"},
        {"time ^", 6, "found the end of the formula"},
    };
    for (const auto& bad : cases) {
        SCOPED_TRACE(bad.text);
        FormulaError error;
        EXPECT_FALSE(FormulaTree::parse(bad.text, &error));
        EXPECT_EQ(error.position, bad.position) << error.message;
        EXPECT_NE(error.message.find(bad.message), std::string::npos) << error.message;
        EXPECT_FALSE(ondselAccepts(bad.text));
    }
}

// Rejected by the evaluator only. SymbolicParser stops at the first token it
// cannot use and keeps what it has read (its expected() reports nothing), so
// these ran in the solver as a different formula; refusing them is the
// deliberate difference (sec 4.6.1).
TEST(FormulaEvaluator, trailingTextIsAnError)
{
    const std::vector<Bad> cases = {
        {"2t", 1, "expected an operator or the end of the formula, found 't'"},
        {"2 3", 2, "found the number '3'"},
        {"time)", 4, "unbalanced ')'"},
        {"time(2)", 4, "found '('"},
        {"1.2.3*time", 3, "must start with a digit"},
        {"3pi", 1, "found 'pi'"},
        {"2*time 5", 7, "found the number '5'"},
        {"2 $ time", 2, "unexpected character '$'"},
    };
    for (const auto& bad : cases) {
        SCOPED_TRACE(bad.text);
        FormulaError error;
        EXPECT_FALSE(FormulaTree::parse(bad.text, &error));
        EXPECT_EQ(error.position, bad.position) << error.message;
        EXPECT_NE(error.message.find(bad.message), std::string::npos) << error.message;
        EXPECT_TRUE(ondselAccepts(bad.text));
    }
}

// Rejected by the evaluator where SymbolicParser would assert, crash or read
// garbage, so they are not handed to it here.
TEST(FormulaEvaluator, malformedFunctionCalls)
{
    const std::vector<Bad> cases = {
        {"sin(time, 2)", 0, "'sin' takes one argument"},
        {"rampstep(time, 1, 2)", 0, "five arguments"},
        {"rampstep(time, time, 0, 1, 1)", 15, "must not depend on time"},
        {"rampstep(time, 1, 0, 1, 2)", 15, "must differ"},
        {"piecewise(time, functions(1, 2), transitions(1, 2))", 16, "one function more"},
        {"piecewise(time, 1, transitions(1))", 16, "expected functions(...)"},
        {"functions(1, 2)", 0, "only allowed as an argument of piecewise"},
        {"spline(time, 3, 0, 0, 1, 1)", 0, "not supported"},
        {"integral(time, time)", 0, "not supported"},
        {"-.5*time", 1, "must start with a digit"},
        {"1e*time", 1, "incomplete exponent"},
        {"2d*time", 1, "incomplete exponent"},
    };
    for (const auto& bad : cases) {
        SCOPED_TRACE(bad.text);
        FormulaError error;
        EXPECT_FALSE(FormulaTree::parse(bad.text, &error));
        EXPECT_EQ(error.position, bad.position) << error.message;
        EXPECT_NE(error.message.find(bad.message), std::string::npos) << error.message;
    }
}

TEST(FormulaEvaluator, describe)
{
    FormulaError error;
    EXPECT_FALSE(FormulaTree::parse("1 + x", &error));
    EXPECT_EQ(error.describe(), "unknown name 'x' (the variable is 'time') at column 5");
    EXPECT_TRUE(FormulaTree::parse("time", &error));
    EXPECT_TRUE(error.message.empty());
}
