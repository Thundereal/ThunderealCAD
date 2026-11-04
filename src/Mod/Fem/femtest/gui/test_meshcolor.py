# SPDX-License-Identifier: LGPL-2.1-or-later
"""Not upstream: a FEM mesh's colours by node and by element, read off the
material its view draws with (the SoMaterial and SoMaterialBinding of the
faces), on two tetrahedra sharing a face.

- NodeColor and ElementColor, set as a dict, are drawn: setting one picks
  its ColorMode, which before kept them in the property but drew the
  mesh's own colour while the mode was Overall, its default.
- resetNodeColor after setNodeColorByScalars draws the mode's colours
  again, in any mode (before, only in ByNode; FEM's mesh preview panel
  set ColorMode to itself to get there).
- setNodeColorByScalars: an optional fixed scale; nodes not listed, with
  ids past the largest listed, read no colour past its list; lists of
  different lengths are an error rather than a crash.
"""

import unittest

import FreeCAD
import FreeCADGui

import Fem

RED = (1.0, 0.0, 0.0)
BLUE = (0.0, 0.0, 1.0)
GREEN = (0.0, 1.0, 0.0)
YELLOW = (1.0, 1.0, 0.0)


def two_tets():
    mesh = Fem.FemMesh()
    for i, p in enumerate(((0, 0, 0), (10, 0, 0), (0, 10, 0), (0, 0, 10), (10, 10, 10)), 1):
        mesh.addNode(*p, i)
    mesh.addVolume([1, 2, 3, 4], 1)
    mesh.addVolume([2, 3, 4, 5], 2)
    return mesh


def faces_material(vobj):
    """The material and binding the faces draw with: the SoMaterial just
    before the view's own SoMaterialBinding."""
    from pivy import coin

    search = coin.SoSearchAction()
    search.setType(coin.SoMaterialBinding.getClassTypeId())
    search.setInterest(coin.SoSearchAction.ALL)
    search.setSearchingAll(True)
    search.apply(vobj.RootNode)
    for path in search.getPaths():
        binding = path.getTail()
        group = path.getNodeFromTail(1)
        index = group.findChild(binding)
        if index > 0:
            material = group.getChild(index - 1)
            if material.isOfType(coin.SoMaterial.getClassTypeId()):
                return material, binding
    raise AssertionError("no material before a material binding")


def diffuse(material):
    field = material.diffuseColor
    return [tuple(round(c, 3) for c in field[i].getValue()) for i in range(field.getNum())]


class TestMeshColor(unittest.TestCase):
    def setUp(self):
        self.doc = FreeCAD.newDocument(self.__class__.__name__)
        self.obj = self.doc.addObject("Fem::FemMeshObject", "Mesh")
        self.obj.FemMesh = two_tets()
        self.doc.recompute()
        self.vobj = self.obj.ViewObject
        self.vobj.DisplayMode = "Faces"
        FreeCADGui.updateGui()

    def tearDown(self):
        FreeCAD.closeDocument(self.doc.Name)

    def binding(self):
        from pivy import coin

        value = faces_material(self.vobj)[1].value.getValue()
        names = {
            coin.SoMaterialBinding.OVERALL: "OVERALL",
            coin.SoMaterialBinding.PER_FACE: "PER_FACE",
            coin.SoMaterialBinding.PER_VERTEX_INDEXED: "PER_VERTEX_INDEXED",
        }
        return names.get(value, value)

    def colours(self):
        return diffuse(faces_material(self.vobj)[0])

    def test_node_color_dict_is_drawn(self):
        self.assertEqual(self.vobj.ColorMode, "Overall")
        self.vobj.NodeColor = {1: RED, 5: BLUE}
        self.assertEqual(self.vobj.ColorMode, "ByNode")
        self.assertEqual(self.binding(), "PER_VERTEX_INDEXED")
        colours = self.colours()
        self.assertGreater(len(colours), 1)
        self.assertIn(RED, colours)
        self.assertIn(BLUE, colours)
        # and kept: read back as set
        self.assertEqual(tuple(self.vobj.NodeColor[1]), RED)

    def test_element_color_dict_is_drawn(self):
        self.vobj.ElementColor = {2: RED}
        self.assertEqual(self.vobj.ColorMode, "ByElement")
        self.assertEqual(self.binding(), "PER_FACE")
        colours = self.colours()
        self.assertIn(RED, colours)
        self.assertGreater(len(set(colours)), 1)

    def test_dict_again_over_scalars(self):
        self.vobj.NodeColor = {1: BLUE}
        self.vobj.setNodeColorByScalars([1, 2], [0.0, 1.0])
        self.assertIn(RED, self.colours())
        self.vobj.NodeColor = {1: BLUE}
        self.assertNotIn(RED, self.colours())
        self.assertIn(BLUE, self.colours())

    def test_empty_dict_resets(self):
        self.vobj.NodeColor = {1: RED}
        self.vobj.NodeColor = {}
        self.assertNotIn(RED, self.colours())

    def test_reset_after_scalars_in_every_mode(self):
        for mode in ("Overall", "ByElement", "ByNode"):
            self.vobj.ColorMode = mode
            self.vobj.setNodeColorByScalars([1, 2, 3, 4, 5], [0.0, 1.0, 2.0, 3.0, 4.0])
            self.assertEqual(self.binding(), "PER_VERTEX_INDEXED", mode)
            self.assertIn(RED, self.colours(), mode)
            self.vobj.resetNodeColor()
            self.assertNotIn(RED, self.colours(), mode)
            if mode == "Overall":
                self.assertEqual(self.binding(), "OVERALL")

    def test_scalars_on_a_fixed_scale(self):
        self.vobj.setNodeColorByScalars([1, 2, 3, 4, 5], [0.0, 0.5, 1.0, 1.5, 2.0])
        self.assertIn(RED, self.colours())
        # 2 on a scale to 4 is half way: yellow, and nothing reaches red
        self.vobj.setNodeColorByScalars([1, 2, 3, 4, 5], [0.0, 0.5, 1.0, 1.5, 2.0], 0.0, 4.0)
        self.assertIn(YELLOW, self.colours())
        self.assertNotIn(RED, self.colours())

    def test_scalars_for_some_nodes(self):
        # node 5 is past the largest id given: it takes zero's colour
        self.vobj.setNodeColorByScalars([1], [1.0])
        colours = self.colours()
        self.assertEqual(len(colours), 5)
        self.assertEqual(colours.count(RED), 1)
        self.assertEqual(colours.count(GREEN), 4)

    def test_scalars_while_never_shown(self):
        obj = self.doc.addObject("Fem::FemMeshObject", "Hidden")
        obj.ViewObject.Visibility = False
        obj.FemMesh = two_tets()
        self.doc.recompute()
        FreeCADGui.updateGui()
        vobj = obj.ViewObject
        vobj.DisplayMode = "Faces"
        vobj.setNodeColorByScalars([1, 2, 3, 4, 5], [0.0, 1.0, 2.0, 3.0, 4.0])
        vobj.Visibility = True
        FreeCADGui.updateGui()
        colours = diffuse(faces_material(vobj)[0])
        self.assertEqual(len(colours), 5)
        self.assertIn(RED, colours)

    def test_scalars_bad_arguments(self):
        with self.assertRaises(ValueError):
            self.vobj.setNodeColorByScalars([1, 2], [1.0])
        with self.assertRaises(ValueError):
            self.vobj.setNodeColorByScalars([-1], [1.0])
        with self.assertRaises(TypeError):
            self.vobj.setNodeColorByScalars([1], ["a"])
        self.vobj.setNodeColorByScalars([], [])
        self.assertEqual(self.binding(), "OVERALL")
