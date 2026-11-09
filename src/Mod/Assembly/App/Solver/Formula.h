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

#pragma once

// The workbench's formula of time (docs/AssemblySolverInterface.md sec 4.6.1):
// what a motion's Formula may say, parsed once into a tree and evaluated at any
// time with its first two time derivatives. Owned by the neutral layer; no
// solver is involved, so every backend reads a formula the same way.
//
//   expression := [ '+' | '-' ] term { ( '+' | '-' ) term }
//   term       := factor { ( '*' | '/' ) factor }
//   factor     := primary [ '^' factor ]                 (right associative)
//   primary    := number | sign number | 'pi' | 'time'
//               | '(' expression ')' | function '(' arguments ')'
//
// The full grammar, with the units and the cases where it is stricter than
// OndselSolver's SymbolicParser it was derived from, is in the doc.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <Mod/Assembly/AssemblyGlobal.h>

namespace Assembly::Solver
{

// A formula's value and its first two derivatives with respect to time.
struct FormulaValue
{
    double value = 0.0;
    double rate = 0.0;  // d/dt
    double acceleration = 0.0;  // d2/dt2
};

struct AssemblyExport FormulaError
{
    std::string message;  // empty: no error
    std::size_t position = 0;  // 0-based offset into the text

    // "message at column N" (1-based), for reports.
    std::string describe() const;
};

class AssemblyExport FormulaTree
{
public:
    // nullptr and the error filled in when the text is not a formula. A blank
    // text is the constant 0, as it was for the solver.
    static std::shared_ptr<const FormulaTree> parse(const std::string& text, FormulaError* error = nullptr);

    const std::string& text() const
    {
        return source;
    }
    bool dependsOnTime() const;

    // Plain IEEE arithmetic: outside a function's domain (ln of a number
    // <= 0, sqrt of a negative, asin beyond +-1, a division by 0) the value
    // is NaN or infinite, as are the derivatives there. Not an error here;
    // a caller that holds a driver at the value checks it is finite.
    double value(double time) const;
    // Value and derivatives, analytic per node (a piecewise formula's
    // derivative is the derivative of the piece in effect: jumps are not seen).
    FormulaValue evaluate(double time) const;

    enum class Op : std::uint8_t
    {
        Constant,
        Time,
        Negate,
        Add,
        Subtract,
        Multiply,
        Divide,
        Power,
        Abs,
        Sin,
        Cos,
        ArcSin,
        ArcTan,
        Tan,
        Exp,
        Ln,
        Sqrt,
        // args: x, then n + 1 pieces, then n transitions
        Piecewise,
    };
    struct Node
    {
        Op op = Op::Constant;
        bool timeDependent = false;
        double constant = 0.0;
        std::vector<std::uint32_t> args;
    };

private:
    friend class FormulaParser;
    FormulaValue eval(std::uint32_t node, double time) const;

    std::string source;
    std::vector<Node> nodes;
    std::uint32_t root = 0;
};

}  // namespace Assembly::Solver
