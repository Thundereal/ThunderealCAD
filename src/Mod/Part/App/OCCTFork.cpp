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

#include "PreCompiled.h"
#ifndef _PreComp_
# ifdef FC_OS_WIN32
#  include <libloaderapi.h>
# else
#  include <dlfcn.h>
# endif
# include <string>
# include <BinTools_ShapeSet.hxx>
# include <BRepFilletAPI_MakeFillet.hxx>
# include <BRepTools_ShapeSet.hxx>
# include <GeomTools_Curve2dSet.hxx>
# include <TopoDS_Edge.hxx>
# include <TopoDS_Face.hxx>
# include <TopoDS_Vertex.hxx>
#endif

#include "OCCTFork.h"

// The lookups: every function here is `static const auto func = (Type)
// lookUp...("Name")`, found once per process, and says what it does when
// func is null -- the OCCT libraries are upstream's, or a fork's from before
// the function. See the header for why nothing is called by its C++ name.

namespace Part {

void *lookUpOCCT(const char *toolkit, const char *name)
{
#ifdef FC_OS_WIN32
    // GetModuleHandleA/LoadLibraryA, not the unsuffixed macros: the build
    // defines UNICODE, so those resolve to the wide versions and reject
    // these narrow strings. GetProcAddress is narrow-only in either case.
    const std::string dll = std::string(toolkit) + ".dll";
    HMODULE hModule = GetModuleHandleA(dll.c_str());
    if (!hModule)
        hModule = LoadLibraryA(dll.c_str());
    return hModule ? (void *)GetProcAddress(hModule, name) : nullptr;
#else
    // Looked up among the libraries already loaded first: Part links the
    // toolkits, so each is there under whatever file name it has, where the
    // name below is the development symlink -- absent from a runtime-only
    // install, and never the name on macOS.
    void *f = dlsym(RTLD_DEFAULT, name);
    if (!f) {
#ifdef FC_OS_MACOSX
        const std::string lib = std::string("lib") + toolkit + ".dylib";
#else
        const std::string lib = std::string("lib") + toolkit + ".so";
#endif
        void *hModule = dlopen(lib.c_str(), RTLD_LAZY);
        if (hModule)
            f = dlsym(hModule, name);
    }
    return f;
#endif
}

void *lookUpTKFillet(const char *name)
{
    return lookUpOCCT("TKFillet", name);
}

void *lookUpTKBRep(const char *name)
{
    return lookUpOCCT("TKBRep", name);
}

// --- TKBRep, TKGeomBase ---------------------------------------------------

typedef void (*FuncSetCurve2dSetMerging)(GeomTools_Curve2dSet &, bool);

/// Says whether the set merges a 2D curve into an entry that would be written
/// the same (the OCCT fork's GeomTools_Curve2dSet_SetMerging). False without
/// the fork's function: upstream's set merges nothing, and there is nothing
/// to set.
bool setOCCTCurve2dSetMerging(GeomTools_Curve2dSet &set, bool merging)
{
    static const auto func = (FuncSetCurve2dSetMerging)lookUpOCCT(
        "TKGeomBase", "GeomTools_Curve2dSet_SetMerging");
    if (!func)
        return false;
    func(set, merging);
    return true;
}

typedef void (*FuncSetOmitPCurvesOnPlane)(BinTools_ShapeSet &, bool);

/// Says whether the binary shape set leaves out the pcurves a plane can
/// rebuild (the OCCT fork's BinTools_ShapeSet_SetOmitPCurvesOnPlane). False
/// without the fork's function: upstream's set writes them all.
bool setOCCTOmitPCurvesOnPlane(BinTools_ShapeSet &set, bool omit)
{
    static const auto func = (FuncSetOmitPCurvesOnPlane)lookUpTKBRep(
        "BinTools_ShapeSet_SetOmitPCurvesOnPlane");
    if (!func)
        return false;
    func(set, omit);
    return true;
}

typedef bool (*FuncIsCacheRepresentation)(const Standard_Transient *);

/// Whether a BRep curve or point representation is a cache: one a frozen
/// (Immutable) edge or vertex of the OCCT fork was given after it was frozen,
/// such as a pcurve for a face built on the edge elsewhere. The fork's
/// BRep_Builder_IsCache, looked up at run time; false without it -- upstream
/// OCCT freezes nothing and has no caches.
bool isOCCTCacheRepresentation(const Standard_Transient *rep)
{
    static const auto func = (FuncIsCacheRepresentation)lookUpTKBRep("BRep_Builder_IsCache");
    return func && rep && func(rep);
}

typedef void (*FuncSetStableBytes)(BinTools_ShapeSet &, bool);

/// Says whether the binary shape set writes the same bytes for the same
/// shape whatever was written before (the OCCT fork's
/// BinTools_ShapeSet_SetStableBytes). False without the fork's function:
/// upstream's set has no such option.
bool setOCCTStableBytes(BinTools_ShapeSet &set, bool stable)
{
    static const auto func = (FuncSetStableBytes)lookUpTKBRep("BinTools_ShapeSet_SetStableBytes");
    if (!func)
        return false;
    func(set, stable);
    return true;
}

typedef void (*FuncAddOwnSurfaces)(BRepTools_ShapeSet &, const TopoDS_Shape &);

/// Names the surfaces of a shape to a text shape set before its geometry is
/// added, for a stable write (the OCCT fork's
/// BRepTools_ShapeSet_AddOwnSurfaces). False without the fork's function:
/// upstream's set writes every representation and has nothing to name.
bool addOCCTOwnSurfaces(BRepTools_ShapeSet &set, const TopoDS_Shape &shape)
{
    static const auto func = (FuncAddOwnSurfaces)lookUpTKBRep("BRepTools_ShapeSet_AddOwnSurfaces");
    if (!func)
        return false;
    func(set, shape);
    return true;
}

typedef void (*FuncThawedFrom)(const TopoDS_TShape *, Handle(TopoDS_TShape) &);

/// The TShape a thawed copy was made of: an algorithm of the OCCT fork copied
/// a frozen (Immutable) sub-shape where it would have changed it in place
/// (the fork's TopoDS_TShape_ThawedFrom). Null for any other TShape, and
/// without the fork's function -- upstream OCCT freezes nothing and thaws
/// nothing.
Handle(TopoDS_TShape) thawedFromOCCT(const TopoDS_TShape *copy)
{
    static const auto func = (FuncThawedFrom)lookUpTKBRep("TopoDS_TShape_ThawedFrom");
    Handle(TopoDS_TShape) original;
    if (func && copy)
        func(copy, original);
    return original;
}

typedef unsigned int (*FuncRaisedLockedShapes)();

/// How many TopoDS_LockedShape the OCCT fork has raised on this thread: an
/// algorithm that was refused a change of a frozen shape (the fork's
/// TopoDS_LockedShape_Raised). 0 without the fork's function: upstream's
/// exception counts nothing, and nothing there is frozen to be refused.
unsigned int raisedOCCTLockedShapes()
{
    static const auto func = (FuncRaisedLockedShapes)lookUpTKBRep("TopoDS_LockedShape_Raised");
    return func ? func() : 0;
}

typedef void *(*FuncAcquireRepresentationLock)(const TopoDS_TShape *);
typedef void (*FuncReleaseRepresentationLock)(void *);

/// Locks a frozen TShape's representations against the edits a frozen shape
/// still takes -- a mesher's polygons, a pcurve for a new face -- while a
/// writer walks them (the OCCT fork's BRep_RepresentationLock_Acquire). Null
/// where nothing was locked, and without the fork's functions: upstream OCCT
/// freezes nothing, so there is nothing to guard.
void *acquireOCCTRepresentationLock(const TopoDS_TShape *tshape)
{
    static const auto func =
        (FuncAcquireRepresentationLock)lookUpTKBRep("BRep_RepresentationLock_Acquire");
    return func && tshape ? func(tshape) : nullptr;
}

/// Lets go of what acquireOCCTRepresentationLock returned, on the thread that
/// acquired it; null is nothing.
void releaseOCCTRepresentationLock(void *lock)
{
    static const auto func =
        (FuncReleaseRepresentationLock)lookUpTKBRep("BRep_RepresentationLock_Release");
    if (func && lock)
        func(lock);
}

// --- TKFillet -------------------------------------------------------------

typedef double (*FuncSetFilletSetting)(double);

/// Hands PartParams FilletPlateG0FallbackRatio to the OCCT fork's fillet: a
/// corner plate that misses its boundary by more than this fraction of the
/// smallest radius at the corner while held tangent to the stripes is built
/// again on positions alone (ChFi3d_Builder::SetPlateG0FallbackRatio). False
/// without the fork's function, or one from before the ratio.
bool setOCCTPlateG0FallbackRatio(double ratio)
{
    static const auto func =
        (FuncSetFilletSetting)lookUpTKFillet("ChFi3d_SetPlateG0FallbackRatio");
    if (!func)
        return false;
    // 0 or less turns the fallback off: every tangent plate is kept (the
    // fork's distance setting, which takes over then, defaults to off)
    func(ratio > 0.0 ? ratio : 0.0);
    return true;
}

/// Hands PartParams FilletCornerSetbackFallback to the OCCT fork's fillet:
/// where a fillet fails at a corner, the corner is set back, up to this many
/// times the radius, and built again (ChFi3d_Builder::SetCornerSetbackFallback,
/// docs/CornerBlending.md section 9). False without the fork's function.
bool setOCCTCornerSetbackFallback(double multiple)
{
    static const auto func =
        (FuncSetFilletSetting)lookUpTKFillet("ChFi3d_SetCornerSetbackFallback");
    if (!func)
        return false;
    func(multiple > 0.0 ? multiple : 0.0);
    return true;
}

typedef int (*FuncSetFlatEndAtVertex)(int);

/// The calling thread's flat end at a vertex (ChFi3d_Builder::
/// SetFlatEndAtVertex, through the fork's ChFi3d_SetFlatEndAtVertex): a
/// fillet whose end at a vertex would cut into the body past the vertex --
/// the far side of a pocket's acute tip -- ends there in a flat face instead.
/// Returns what was set before, 0 or 1; -1 without the fork's function, where
/// nothing is set. Part::FilletFlatEnd holds it for a scope.
int setOCCTFlatEndAtVertex(int flat)
{
    static const auto func = (FuncSetFlatEndAtVertex)lookUpTKFillet("ChFi3d_SetFlatEndAtVertex");
    return func ? func(flat) : -1;
}

typedef void (*FuncSetFilletSetback)(BRepFilletAPI_MakeFillet &,
                                     const TopoDS_Vertex &,
                                     const TopoDS_Edge *,
                                     double);

/// Sets a fillet corner back at a vertex, for every fillet ending there or
/// for the one on the edge given (the fork's BRepFilletAPI_SetSetback, the
/// corner blending of docs/CornerBlending.md). False without the fork's
/// function: a caller that asked for a setback throws then rather than build
/// a different shape (TopoShape::makeElementFillet).
bool setOCCTFilletSetback(BRepFilletAPI_MakeFillet &mkFillet,
                          const TopoDS_Vertex &vertex,
                          const TopoDS_Edge *edge,
                          double setback)
{
    static const auto func = (FuncSetFilletSetback)lookUpTKFillet("BRepFilletAPI_SetSetback");
    if (!func)
        return false;
    func(mkFillet, vertex, edge, setback);
    return true;
}

typedef void (*FuncSetFilletFaceDepth)(BRepFilletAPI_MakeFillet &,
                                       const TopoDS_Vertex &,
                                       const TopoDS_Face &,
                                       double);

/// Sets how deep a set back corner's curve reaches into a face at the vertex
/// (the fork's BRepFilletAPI_SetFaceDepth). False without the fork's
/// function, or one from before face depths.
bool setOCCTFilletFaceDepth(BRepFilletAPI_MakeFillet &mkFillet,
                            const TopoDS_Vertex &vertex,
                            const TopoDS_Face &face,
                            double depth)
{
    static const auto func = (FuncSetFilletFaceDepth)lookUpTKFillet("BRepFilletAPI_SetFaceDepth");
    if (!func)
        return false;
    func(mkFillet, vertex, face, depth);
    return true;
}

typedef int (*FuncSetFilletFixLevel)(int);

/// The calling thread's fillet fix level (the OCCT fork's
/// ChFi3d_SetFixLevel): which of the fork's corrections to fillets and
/// chamfers that give a different shape than before are made. Returns what
/// was set before; -1 without the fork's function, where nothing is set.
int setOCCTFilletFixLevel(int level)
{
    static const auto func = (FuncSetFilletFixLevel)lookUpTKFillet("ChFi3d_SetFixLevel");
    return func ? func(level) : -1;
}

// --- TKOffset -------------------------------------------------------------

typedef int (*FuncSetOffsetFixLevel)(int);

/// The calling thread's offset fix level (the OCCT fork's
/// BRepOffset_SetFixLevel): which of the fork's corrections to the offset
/// that give a different shape than before are made. Returns what was set
/// before; -1 without the fork's function, where nothing is set.
int setOCCTOffsetFixLevel(int level)
{
    static const auto func =
        (FuncSetOffsetFixLevel)lookUpOCCT("TKOffset", "BRepOffset_SetFixLevel");
    return func ? func(level) : -1;
}

} // namespace Part
