// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

#ifndef PMI_DATUM_H
#define PMI_DATUM_H

#include <string>
#include <vector>

#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <Mod/Part/App/PropertyTopoShape.h>

#include "Annotation.h"

namespace PMI
{

class TolerancedFeature;

/** A datum feature and the datum simulated from it (docs/GDandT.md
 * sec 4.3): the faces are the feature's, the exact plane, axis or point in
 * Shape is the datum. Upstream's PMI_Datum.
 */
class PMIExport Datum: public Annotation
{
    PROPERTY_HEADER_WITH_OVERRIDE(PMI::Datum);

public:
    Datum();

    App::PropertyString Letter;
    App::PropertyLink Feature;
    /// The simulated datum: a plane, an axis or a point
    Part::PropertyPartShape Shape;

    App::DocumentObjectExecReturn* execute() override;

    TolerancedFeature* getFeature() const;

    /// Whether \a letter is a legal datum letter under the set's standard
    /// (ASME refuses I, O and Q); \a why says what is wrong
    static bool isLegalLetter(const std::string& letter, bool asme, std::string& why);
};

/** A datum reference frame (docs/GDandT.md sec 4.3): up to three ordered
 * compartments, each a datum or a common datum with its modifiers, in the
 * frame's own notation: "A", "B(M)", "C-D", "A-B(L,F)". Upstream's
 * PMI_DatumReferenceFrame.
 */
class PMIExport DatumReferenceFrame: public Annotation
{
    PROPERTY_HEADER_WITH_OVERRIDE(PMI::DatumReferenceFrame);

public:
    DatumReferenceFrame();

    App::PropertyStringList Compartments;

    App::DocumentObjectExecReturn* execute() override;

    struct Compartment
    {
        /// One letter, or several for a common datum (A-B)
        std::vector<std::string> datums;
        std::vector<std::string> modifiers;
    };
    /// Parse the compartments; false with \a why on the first error
    bool parse(std::vector<Compartment>& result, std::string& why) const;

    /// The datum modifiers allowed in a compartment
    static const std::vector<std::string>& modifierNames();
};

}  // namespace PMI

#endif  // PMI_DATUM_H
