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

// The 3D sketch (docs/Sketch3D.md section 5): constrained geometry in 3D, in an
// attachable frame of its own, solved by the PlaneGCS 3D layer through Sketch3DSolver.
// Separate from the 2D SketchObject, which Pad, Pocket and the rest rely on being
// planar.

#include <fastsignals/signal.h>

#include <App/PropertyLinks.h>
#include <Mod/Part/App/AttachExtension.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Part/App/PropertyGeometryList.h>

#include "PropertyConstraintList.h"
#include "Sketch3DSolver.h"

namespace Sketcher
{

class SketcherExport Sketch3DObject: public Part::Feature, public Part::AttachExtension
{
    PROPERTY_HEADER_WITH_EXTENSIONS(Sketcher::Sketch3DObject);

public:
    Sketch3DObject();
    ~Sketch3DObject() override;

    /// The frame's own references, before the external ones: geometry id -1 is the X
    /// axis (its start the origin), then the Y and Z axes and the XY, YZ, ZX planes.
    enum FrameGeoId
    {
        XAxis = -1,
        YAxis = -2,
        ZAxis = -3,
        XYPlane = -4,
        YZPlane = -5,
        ZXPlane = -6,
        RefExt = -7,  // the first external reference
    };

    Part::PropertyGeometryList Geometry;
    Sketcher::PropertyConstraintList Constraints;
    App::PropertyLinkSubList ExternalGeometry;
    /// The external references as geometry in sketch coordinates, kept with the file so
    /// a document opens with its constraints valid before any recompute.
    Part::PropertyGeometryList ExternalGeo;

    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;
    const char* getViewProviderName() const override
    {
        return "SketcherGui::ViewProviderSketch3D";
    }
    PyObject* getPyObject() override;

    /// Geometry: the sketch's own, then the frame's and the external references in
    /// reverse, as the 2D sketch orders it for its constraint list.
    std::vector<const Part::Geometry*> getCompleteGeometry() const;
    /// The frame references and the external ones; index 0 is geometry id -1.
    std::vector<const Part::Geometry*> getExternalGeometry() const;
    const Part::Geometry* getGeometry(int geoId) const;

    int addGeometry(const Part::Geometry* geo, bool construction = false);
    void delGeometry(int geoId);
    int addConstraint(const Constraint* constraint);
    void delConstraint(int index);
    /// The constraint's index by name, or -1.
    int getConstraintIndex(const std::string& name) const;
    void setDatum(int index, double value);
    double getDatum(int index) const;
    void setDriving(int index, bool driving);
    /// Names a constraint, so an expression can use it; empty unnames it.
    void renameConstraint(int index, const std::string& name);
    void setConstruction(int geoId, bool construction);

    /// Adds a reference to a model vertex, edge, planar face or datum; returns its
    /// geometry id.
    int addExternal(App::DocumentObject* obj, const char* subname);
    void delExternal(int geoId);

    /** Solves and, when updateGeometry, writes the solution back.
     * @return 0 solved, -1 solver failure, -3 conflicting, -4 redundant,
     *         -5 malformed constraints (codes as the 2D sketch's).
     */
    int solve(bool updateGeometry = true);
    /// After every solve, for what shows the status (DoF, conflicts): it
    /// can change with no property changing
    fastsignals::signal<void()> signalSolverUpdate;
    /// Drags a point (or a curve with PointPos::none); 0 when solved.
    int movePoint(int geoId, PointPos pos, const Base::Vector3d& to, bool relative = false);
    /** A corner fillet (S3c): the arc of radius tangent to two lines at the corner where
     * the given ends meet, in their plane, the lines trimmed back to it. The corner's
     * coincidence is replaced by the arc's tangency at both ends. Returns the arc's id.
     */
    int fillet(int geo1, PointPos pos1, int geo2, PointPos pos2, double radius);

    int getDoF() const
    {
        return lastDoF;
    }
    const std::vector<int>& getConflicting() const
    {
        return lastConflicting;
    }
    const std::vector<int>& getRedundant() const
    {
        return lastRedundant;
    }
    const std::vector<int>& getPartiallyRedundant() const
    {
        return lastPartiallyRedundant;
    }
    const std::vector<int>& getMalformed() const
    {
        return lastMalformed;
    }

    /// Resolves ExternalGeometry into ExternalGeo.
    /// The references that could not be built: their geometry is kept as it was.
    std::vector<std::size_t> rebuildExternalGeometry();

protected:
    void onChanged(const App::Property* prop) override;
    void onDocumentRestored() override;
    void setupObject() override;
    void onUndoRedoFinished() override;
    /// The geometry is in the sketch's frame, so the shape always takes Placement --
    /// also when a Python call solves outside a recompute.
    bool shouldApplyPlacement() override
    {
        return true;
    }

private:
    int solveNow(bool updateGeometry);
    void acceptGeometry();
    // Writes the solver's result into Geometry; false (nothing written) when the
    // solved geometry cannot be built.
    bool applySolution();
    void buildShape();
    void updateDrivenValues();
    // Geometry ids are counted per sketch, as the 2D sketch counts them, so an
    // output edge keeps its name through a solve and a reorder.
    long nextGeometryId();

    Sketch3DSolver solver;
    std::vector<std::unique_ptr<Part::Geometry>> frameGeo;
    int lastDoF {-1};
    std::vector<int> lastConflicting, lastRedundant, lastPartiallyRedundant, lastMalformed;
    bool solving {false};
};

}  // namespace Sketcher
