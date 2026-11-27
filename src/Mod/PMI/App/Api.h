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

#ifndef PMI_API_H
#define PMI_API_H

#include <string>
#include <vector>

#include <Mod/PMI/PMIGlobal.h>

namespace App
{
class DocumentObject;
}

namespace PMI
{

class AnnotationSet;
class Callout;
class TolerancedFeature;

/** What the commands do, App side: headless, scriptable, tested
 * (docs/GDandT.md sec 3, 4). The Gui commands are one call each to these.
 */
namespace Api
{
/** A face, an edge or a vertex picked on a part, as an annotation refers
 * to it: the solid body of a PartDesign Body that holds it, and its name in
 * the solid body's shape -- a pick lands on the Body's tip feature, (Body,
 * "Hole.Face6"), and must become (Solid, "Face7"). An element of a shape
 * that is no Body stays as it is. False, with \a why, if it is none of a
 * solid's.
 */
PMIExport bool solidReference(App::DocumentObject* obj, const std::string& subname,
                              App::DocumentObject*& solid, std::string& face, std::string& why);

/** The set annotating \a part (a Body, one of its solid bodies, or another
 * shape), made if \a create: in the Body, or beside the shape.
 */
PMIExport AnnotationSet* getSet(App::DocumentObject* part, bool create = true);

/** The set's toleranced feature on exactly these faces of \a solid, made if
 * there is none (sec 4.2: one per face set, shared).
 */
PMIExport TolerancedFeature* feature(AnnotationSet* set, App::DocumentObject* solid,
                                     const std::vector<std::string>& faces,
                                     const char* kind = nullptr);

/// The set's datum reference frame with exactly these compartments, made if
/// there is none: one per distinct frame (CAx-IF sec 6.9.8)
PMIExport App::DocumentObject* frame(AnnotationSet* set, const std::vector<std::string>& compartments);

/// The first datum letter the solid has not used (ASME skips I, O, Q)
PMIExport std::string nextDatumLetter(AnnotationSet* set, App::DocumentObject* solid);
/// The callout \a callout is stacked under by default (docs/GDandT.md sec
/// 6.4): the set's last one on the same feature, or null. A linear or an
/// angular dimension, drawn with its own lines, is never in a stack.
PMIExport Callout* stackUnder(const Callout* callout);

/// The features a callout is about: a dimension's, or the one feature of a
/// tolerance, a datum or a note
PMIExport std::vector<TolerancedFeature*> featuresOf(const Callout* callout);

/// A callout that applies to a face, and how: "on" it, or citing a datum on
/// it ("datum A")
struct Applies
{
    Callout* callout;
    std::string how;
};

/** What applies to one face of a solid (docs/GDandT.md sec 6.5; upstream's
 * query of a feature's specifications): the callouts on a feature holding
 * the face ("on" it, or "all over" for one on the whole part), then the
 * tolerances whose datum frame cites a datum on it -- the datum letters are
 * the solid's own. In the set's order, each once.
 */
PMIExport std::vector<Applies> query(const App::DocumentObject* solid, const std::string& face);

/// What tolerances a face (docs/GDandT.md sec 8, coverage)
enum class Coverage
{
    /// Nothing: no callout is on it and the set has no general tolerance
    None,
    /// Only the set's general tolerance
    General,
    /// A callout on it, or one all over its part
    Annotated,
};

struct FaceCoverage
{
    const App::DocumentObject* solid;
    std::string face;
    Coverage coverage;
};

/** Every face of the set's part -- of each solid body of a Body -- and
 * what tolerances it: the "fully toleranced" question DimXpert's colours
 * answer. A face is annotated when a dimension, a tolerance, a datum, a
 * datum target or a note is on a feature holding it; a datum cited is not
 * the face toleranced. Faces in order, solid by solid.
 */
PMIExport std::vector<FaceCoverage> coverage(const AnnotationSet* set);
}  // namespace Api

}  // namespace PMI

#endif  // PMI_API_H
