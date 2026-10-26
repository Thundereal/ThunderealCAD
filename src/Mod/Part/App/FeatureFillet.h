/***************************************************************************
 *   Copyright (c) 2008 Werner Mayer <wmayer[at]users.sourceforge.net>     *
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

#ifndef PART_FEATUREFILLET_H
#define PART_FEATUREFILLET_H

#include "PartFeature.h"


namespace Part
{

/** The OCCT fork's flat end at a vertex (ChFi3d_Builder::SetFlatEndAtVertex)
 * on the calling thread for as long as this lives, and as it was after: a
 * fillet whose end at a vertex would cut into the body past the vertex -- the
 * far side of a pocket's acute tip -- ends there in a flat face instead. Looked
 * up at run time; nothing where the OCCT libraries lack it.
 */
class PartExport FilletFlatEnd
{
public:
    explicit FilletFlatEnd(bool enable);
    ~FilletFlatEnd();
    FilletFlatEnd(const FilletFlatEnd &) = delete;
    FilletFlatEnd &operator=(const FilletFlatEnd &) = delete;

    /// Whether the OCCT libraries have it
    static bool isAvailable();

private:
    int previous = -1;
};

class Fillet : public Part::FilletBase
{
    PROPERTY_HEADER_WITH_OVERRIDE(Part::Fillet);

public:
    Fillet();

    /// 1 from the first: a fillet's end that would cut into the body past
    /// its vertex is flat (FilletFlatEnd); 0, a file's from before
    App::PropertyInteger _Version;

    /** @name methods override feature */
    //@{
    /// recalculate the feature
    App::DocumentObjectExecReturn *execute() override;
    void setupObject() override;
    /// returns the type name of the view provider
    const char* getViewProviderName() const override {
        return "PartGui::ViewProviderFillet";
    }
    //@}
};

} //namespace Part


#endif // PART_FEATUREFILLET_H
