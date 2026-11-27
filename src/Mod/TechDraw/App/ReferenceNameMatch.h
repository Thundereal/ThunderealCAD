// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   This file is part of the FreeCAD CAx development system.               *
 *                                                                          *
 *   This library is free software; you can redistribute it and/or          *
 *   modify it under the terms of the GNU Library General Public            *
 *   License as published by the Free Software Foundation; either           *
 *   version 2 of the License, or (at your option) any later version.       *
 *                                                                          *
 ***************************************************************************/

#ifndef TECHDRAW_REFERENCENAMEMATCH_H
#define TECHDRAW_REFERENCENAMEMATCH_H

#include <string>

namespace TechDraw
{

//! A reference into a view whose saved name another element carries now.
//! A recompute takes such an element only where it also has the geometry
//! saved for the reference (sameGeometry); one found by its name alone is
//! the user's to take, on their edit of the object, and said to them (the
//! reporter, 2026-11-26).
struct ReferenceNameMatch
{
    int index;
    //! the element the reference names
    std::string was;
    //! the element that carries its saved name
    std::string now;
    //! whether `now` has the geometry saved for the reference
    bool sameGeometry;
    bool operator==(const ReferenceNameMatch& other) const
    {
        return index == other.index && was == other.was && now == other.now
            && sameGeometry == other.sameGeometry;
    }
};

}  // namespace TechDraw

#endif  // TECHDRAW_REFERENCENAMEMATCH_H
