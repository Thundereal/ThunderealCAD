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

#include <string>
#include <vector>

#include <Mod/Assembly/AssemblyGlobal.h>

namespace App
{
class DocumentObject;
}

namespace Assembly
{

/** The coefficients of friction of pairs of materials (docs/AssemblyDynamics.md,
 * P6), for a contact set whose FrictionFrom is Materials: the module's
 * ContactFriction.json, and the user's of the same name in <user data>/Assembly
 * over it -- its families tried first, its pairs in place of the module's.
 * A material card is in the first family one of whose words its name, its
 * KindOfMaterial or one of its tags holds, ignoring case.
 */
class AssemblyExport FrictionTable
{
public:
    struct Family
    {
        std::string name;
        std::vector<std::string> words;
    };
    struct Pair
    {
        std::string a;
        std::string b;
        double muStatic = 0.0;
        double muDynamic = 0.0;
        std::string source;
    };

    /// The table, read once
    static const FrictionTable& instance();

    /// A table read from these files, the later over the earlier; a file
    /// that is missing or not of the form adds nothing, with a warning
    static FrictionTable read(const std::vector<std::string>& paths);

    /// A component's family: its first shape's material card's that is in
    /// one; empty for none -- no card, the Default one, or in no family
    std::string familyOf(App::DocumentObject* component) const;

    /// The family a card of this name, KindOfMaterial and tags is in;
    /// empty for none
    std::string familyOf(const std::string& name,
                         const std::string& kind,
                         const std::vector<std::string>& tags) const;

    /// The pair's coefficients, either way round; null when not listed
    const Pair* pairOf(const std::string& a, const std::string& b) const;

    const std::vector<Family>& families() const
    {
        return families_;
    }
    const std::vector<Pair>& pairs() const
    {
        return pairs_;
    }

private:
    std::vector<Family> families_;
    std::vector<Pair> pairs_;
};

}  // namespace Assembly
