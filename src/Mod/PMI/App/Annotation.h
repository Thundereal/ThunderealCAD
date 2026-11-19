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

#ifndef PMI_ANNOTATION_H
#define PMI_ANNOTATION_H

#include <string>

#include <App/DocumentObject.h>
#include <App/PropertyStandard.h>

#include <Mod/PMI/PMIGlobal.h>

namespace PMI
{

class AnnotationSet;

/** What every PMI object shares: the set it is in, and its state.
 *
 * A PMI object never fails a recompute: a tolerance on a face that went is
 * an annotation to look at, not a broken document (docs/GDandT.md sec 7).
 * What is wrong with it is said by State and StateMessage instead, which
 * the check (sec 8) and the view read.
 */
class PMIExport Annotation: public App::DocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(PMI::Annotation);

public:
    Annotation();

    enum StateValue
    {
        Ok,
        /// Found again, but not by name (by history or geometry): look at it
        Remapped,
        /// What it refers to is gone; kept where it was until reattached
        Broken,
        /// Its own values do not make sense (sec 8)
        Invalid,
    };

    App::PropertyEnumeration State;
    App::PropertyString StateMessage;

    /// The set this object is in, or null
    AnnotationSet* getSet() const;

    StateValue getState() const
    {
        return static_cast<StateValue>(State.getValue());
    }
    void setState(StateValue state, const std::string& message = std::string());

    static const char* StateEnums[];

protected:
    /// A new annotation is recomputed once even with nothing of its own
    /// touched: the recompute skips such an object (OptimizeRecompute)
    void setupObject() override;
};

}  // namespace PMI

#endif  // PMI_ANNOTATION_H
