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

#pragma once

#include <App/Property.h>
#include <Mod/PartDesign/PartDesignGlobal.h>

#include "SheetMetal/Model.h"

namespace PartDesign
{

/** A sheet metal feature's model of the sheet (docs/SheetMetal.md sec 4.2)
 *
 * In the feature's own frame, as its shape is before its placement. Read
 * only from Python: a dict of the thickness and the elements.
 */
class PartDesignExport PropertySheetModel : public App::Property
{
    TYPESYSTEM_HEADER_WITH_OVERRIDE();

public:
    const SheetMetal::Model& getValue() const
    {
        return model;
    }
    void setValue(const SheetMetal::Model& value = {});

    PyObject* getPyObject() override;
    void setPyObject(PyObject* value) override;

    void Save(Base::Writer& writer) const override;
    void Restore(Base::XMLReader& reader) override;

    Property* Copy() const override;
    void Paste(const Property& from) override;
    Property* copyBeforeChange() const override
    {
        return Copy();
    }
    unsigned int getMemSize() const override;
    bool isSame(const Property& other) const override;

private:
    SheetMetal::Model model;
};

}  // namespace PartDesign
