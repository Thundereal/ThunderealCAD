# SPDX-License-Identifier: LGPL-2.1-or-later
"""CalculiX input of a ConstraintBodyMotion. Not upstream.

The load per unit of mass, g - a - alpha x r - omega x (omega x r) at r from
the reference point, as CalculiX's body loads (*DLOAD), which add up:
- g - a, one GRAV on all elements;
- - omega x (omega x r), the centrifugal load: one CENTRIF, omega^2 about
  the axis of omega through the reference point;
- - alpha x r, which CalculiX has no load for: a GRAV on each element, at
  its centroid's r. Its resultant is exact; its moment about the element's
  centroid is left out, the field varying linearly across it."""

__title__ = "FreeCAD FEM calculix constraint body motion"
__url__ = "https://www.freecad.org"

import FreeCAD


def get_analysis_types():
    return ["buckling", "static", "thermomech"]


def get_constraint_title():
    return "Body motion Constraint"


def get_before_write_constraint():
    return ""


def get_after_write_constraint():
    return ""


def _grav(f, target, v):
    """A GRAV line of acceleration v on target, if any"""
    size = v.Length
    if size <= 0.0:
        return
    d = v * (1.0 / size)
    f.write(f"{target},GRAV,{size:.13G},{d.x:.13G},{d.y:.13G},{d.z:.13G}\n")


def _elements(femmesh):
    """The ids of the mesh's elements of the highest dimension it has"""
    for ids in (femmesh.Volumes, femmesh.Faces, femmesh.Edges):
        if ids:
            return ids
    return ()


def write_constraint(f, femobj, motion_obj, ccxwriter):

    # floats read from ccx should use {:.13G}, see comment in writer module
    point = FreeCAD.Vector(motion_obj.ReferencePoint)
    gravity = FreeCAD.Vector(motion_obj.Gravity)
    accel = FreeCAD.Vector(motion_obj.LinearAcceleration)
    omega = FreeCAD.Vector(motion_obj.AngularVelocity)
    alpha = FreeCAD.Vector(motion_obj.AngularAcceleration)

    f.write("*DLOAD\n")
    _grav(f, ccxwriter.ccx_eall, gravity - accel)
    if omega.Length > 0.0:
        axis = omega * (1.0 / omega.Length)
        f.write(
            "{},CENTRIF,{:.13G},{:.13G},{:.13G},{:.13G},{:.13G},{:.13G},{:.13G}\n".format(
                ccxwriter.ccx_eall,
                omega.Length**2,
                point.x,
                point.y,
                point.z,
                axis.x,
                axis.y,
                axis.z,
            )
        )
    if alpha.Length > 0.0:
        femmesh = ccxwriter.mesh_object.FemMesh
        nodes = femmesh.Nodes
        for element in _elements(femmesh):
            ids = femmesh.getElementNodes(element)
            centre = FreeCAD.Vector()
            for n in ids:
                centre += nodes[n]
            centre *= 1.0 / len(ids)
            _grav(f, element, alpha.cross(centre - point) * -1.0)
    f.write("\n")
