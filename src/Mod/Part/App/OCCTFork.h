/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com>*
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

#ifndef PART_OCCTFORK_H
#define PART_OCCTFORK_H

/** What FreeCAD asks of the OCCT fork (realthunder/OCCT), in one place.
 *
 * FreeCAD must load with upstream OCCT as well as with the fork, so nothing
 * of the fork's is called by its C++ name: such a symbol is missing from
 * upstream's libraries and the loader then refuses the whole module. A fork
 * function FreeCAD needs is a C function the fork exports, looked up at run
 * time by name (lookUpOCCT), null where the libraries are upstream's or a
 * fork's from before it. Every such lookup lives here, behind a typed
 * wrapper that says what it does without the fork, so that this header is
 * the list of what FreeCAD needs of it. Python sees the same lookup as
 * Part.hasOCCTFunction(toolkit, name).
 */

#include <TopoDS_Shape.hxx>
#include <TopoDS_TShape.hxx>

#include <Mod/Part/PartGlobal.h>

class Standard_Transient;
class GeomTools_Curve2dSet;
class BinTools_ShapeSet;
class BRepTools_ShapeSet;
class BRepFilletAPI_MakeFillet;
class TopoDS_Vertex;
class TopoDS_Edge;
class TopoDS_Face;

namespace Part {

/// A function of one of the OCCT fork's toolkits ("TKBRep", "TKFillet",
/// "TKDESTEP", ...) by name, looked up at run time among the libraries
/// loaded, then in the toolkit's library by its file name; null where the
/// libraries are upstream's, or a fork's from before the function.
PartExport void *lookUpOCCT(const char *toolkit, const char *name);
/// A function of the OCCT fork's TKFillet by name, null without it
PartExport void *lookUpTKFillet(const char *name);
/// A function of the OCCT fork's TKBRep by name, null without it
PartExport void *lookUpTKBRep(const char *name);

/// The fork's extension version (SetFuncShowTopoShape, which also installs
/// FreeCAD's hook for the fork's intermediate shapes), -1 without the fork.
/// ImmutableShapeValues defaults to version 2 or later.
PartExport int initOCCTExtension();

// --- TKBRep, TKGeomBase: frozen shapes and stable writes ---------------

/// Whether a BRep curve or point representation is a cache of a frozen
/// (Immutable) shape (the fork's BRep_Builder_IsCache); false without the
/// fork, which alone freezes
PartExport bool isOCCTCacheRepresentation(const Standard_Transient *rep);
/// Whether the set merges equal 2D curves (the fork's
/// GeomTools_Curve2dSet_SetMerging); false, and nothing set, without it
PartExport bool setOCCTCurve2dSetMerging(GeomTools_Curve2dSet &set, bool merging);
/// Whether the binary set leaves out pcurves on a plane (the fork's
/// BinTools_ShapeSet_SetOmitPCurvesOnPlane); false without it
PartExport bool setOCCTOmitPCurvesOnPlane(BinTools_ShapeSet &set, bool omit);
/// Whether the binary set writes stable bytes (the fork's
/// BinTools_ShapeSet_SetStableBytes); false, and nothing set, without it
PartExport bool setOCCTStableBytes(BinTools_ShapeSet &set, bool stable);
/// Names the surfaces a stable write of the text set keeps representations
/// on (the fork's BRepTools_ShapeSet_AddOwnSurfaces); false without it
PartExport bool addOCCTOwnSurfaces(BRepTools_ShapeSet &set, const TopoDS_Shape &shape);
/// The TShape a thawed copy stands for (the fork's
/// TopoDS_TShape_ThawedFrom); null without the fork, which alone thaws
PartExport Handle(TopoDS_TShape) thawedFromOCCT(const TopoDS_TShape *copy);
/// How many times an algorithm refused to change a frozen shape on this
/// thread (the fork's TopoDS_LockedShape_Raised); 0 without the fork
PartExport unsigned int raisedOCCTLockedShapes();
/// The fork's lock on a frozen TShape's representations
/// (BRep_RepresentationLock_Acquire): what releaseOCCTRepresentationLock
/// is to be given, null where nothing was locked or without the fork
PartExport void *acquireOCCTRepresentationLock(const TopoDS_TShape *tshape);
PartExport void releaseOCCTRepresentationLock(void *lock);

/// Holds the OCCT fork's lock on a frozen (Immutable) vertex's, edge's or
/// face's representations while in scope (BRep_RepresentationLock); for
/// any other TShape, and where the OCCT libraries are upstream's -- which
/// freeze nothing -- it does nothing.
class OCCTRepresentationLock
{
public:
    explicit OCCTRepresentationLock(const TopoDS_TShape *tshape)
        : lock(acquireOCCTRepresentationLock(tshape))
    {}
    ~OCCTRepresentationLock()
    {
        releaseOCCTRepresentationLock(lock);
    }
    OCCTRepresentationLock(const OCCTRepresentationLock &) = delete;
    OCCTRepresentationLock &operator=(const OCCTRepresentationLock &) = delete;

private:
    void *lock;
};

// --- TKFillet: the fork's fillet settings and corners ---------------------

/// Hands PartParams FilletPlateG0FallbackRatio to the fork's fillet: a corner
/// plate that misses its boundary by more than this fraction of the smallest
/// radius at the corner while held tangent to the stripes is built again on
/// positions alone (ChFi3d_SetPlateG0FallbackRatio); 0 or less turns the
/// fallback off. False without the fork.
PartExport bool setOCCTPlateG0FallbackRatio(double ratio);
/// Hands PartParams FilletCornerSetbackFallback to the fork's fillet: where a
/// fillet fails at a corner, the corner is set back, up to this many times
/// the radius, and built again (ChFi3d_SetCornerSetbackFallback,
/// docs/CornerBlending.md section 9). False without the fork.
PartExport bool setOCCTCornerSetbackFallback(double multiple);
/// Sets, for the calling thread, whether a fillet whose end at a vertex
/// would cut into the body past the vertex ends there in a flat face instead
/// (ChFi3d_SetFlatEndAtVertex); returns the previous setting, 0 or 1, or -1
/// without the fork, where nothing is set. Part::FilletFlatEnd scopes it.
PartExport int setOCCTFlatEndAtVertex(int flat);
/// Sets a fillet corner back at a vertex -- for every fillet ending there, or
/// for the one on the edge given (BRepFilletAPI_SetSetback, the fork's corner
/// blending, docs/CornerBlending.md). False without the fork, where nothing
/// is set: a caller that asked for a setback must not build without it.
PartExport bool setOCCTFilletSetback(BRepFilletAPI_MakeFillet &mkFillet,
                                     const TopoDS_Vertex &vertex,
                                     const TopoDS_Edge *edge,
                                     double setback);
/// Sets how deep a set back corner's curve reaches into a face at the vertex
/// (BRepFilletAPI_SetFaceDepth). False without the fork, as above.
PartExport bool setOCCTFilletFaceDepth(BRepFilletAPI_MakeFillet &mkFillet,
                                       const TopoDS_Vertex &vertex,
                                       const TopoDS_Face &face,
                                       double depth);

} // namespace Part

#endif // PART_OCCTFORK_H
