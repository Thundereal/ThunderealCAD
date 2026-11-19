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
/** A face picked on a part, as an annotation refers to it: the solid body
 * of a PartDesign Body that holds the face, and the face's name in its
 * shape -- a pick lands on the Body's tip feature, (Body, "Hole.Face6"),
 * and must become (Solid, "Face7"). A face of a shape that is no Body stays
 * as it is. False, with \a why, if it is no face of a solid.
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
}  // namespace Api

}  // namespace PMI

#endif  // PMI_API_H
