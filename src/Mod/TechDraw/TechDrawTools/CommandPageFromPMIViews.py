# SPDX-License-Identifier: LGPL-2.1-or-later
# ***************************************************************************
# *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
# *                                                                         *
# *   This file is part of the FreeCAD CAx development system.              *
# *                                                                         *
# *   This library is free software; you can redistribute it and/or         *
# *   modify it under the terms of the GNU Library General Public           *
# *   License as published by the Free Software Foundation; either          *
# *   version 2 of the License, or (at your option) any later version.      *
# *                                                                         *
# ***************************************************************************
"""Provides the TechDraw PageFromPMIViews GuiCommand: a page drawing the
selected PMI views, each with its annotations (TechDrawPMI); and
SyncPMIViews: such a page's views made to look again as their PMI views'
cameras do."""

from PySide import QtCore
from PySide.QtCore import QT_TRANSLATE_NOOP

import FreeCAD as App
import FreeCADGui as Gui

# How long the views' projections are waited for before importing, in ms
WAIT_MS = 60000
STEP_MS = 100


def _pmi_views():
    return [o for o in Gui.Selection.getSelection() if o.TypeId == "PMI::View"]


def _annotate(views, left, replace=False):
    """Import each view's annotations once it is projected; those still
    projecting are looked at again a moment later. With \a replace, those
    placed before are placed again (TechDrawPMI.importAnnotations)"""
    import TechDrawPMI

    waiting = []
    for view in views:
        try:
            if view.Document is None:
                continue
        except Exception:
            continue  # deleted meanwhile
        # A view projecting again still names the edges it had before
        if view.waitingForResult() or not view.getEdgeNames():
            waiting.append(view)
            continue
        doc = view.Document
        doc.openTransaction("Import Model Annotations")
        try:
            TechDrawPMI.importAnnotations(view, replace=replace)
            doc.recompute()
        finally:
            doc.commitTransaction()
    if waiting and left > 0:
        QtCore.QTimer.singleShot(STEP_MS,
                                 lambda: _annotate(waiting, left - STEP_MS, replace))
    elif waiting:
        App.Console.PrintWarning(
            "Page from PMI views: %s not projected yet -- run Import Model Annotations "
            "on it later\n" % ", ".join(v.Label for v in waiting))


class CommandPageFromPMIViews:
    """A page drawing the selected PMI views"""

    def GetResources(self):
        return {"Pixmap": "actions/TechDraw_PageDefault",
                "MenuText": QT_TRANSLATE_NOOP("TechDraw_PageFromPMIViews",
                                              "Page From PMI Views"),
                "ToolTip": QT_TRANSLATE_NOOP(
                    "TechDraw_PageFromPMIViews",
                    "A new page drawing the selected PMI views: for each, a view looking "
                    "as its camera does at what it shows, named after it, with its "
                    "annotations imported")}

    def IsActive(self):
        return bool(_pmi_views())

    def Activated(self):
        import TechDrawPMI

        doc = App.ActiveDocument
        doc.openTransaction("Page From PMI Views")
        try:
            page, views = TechDrawPMI.pageFromViews(_pmi_views(), annotate=False)
        finally:
            doc.commitTransaction()
        if page is None:
            return
        page.ViewObject.show()
        _annotate(views, WAIT_MS)


Gui.addCommand("TechDraw_PageFromPMIViews", CommandPageFromPMIViews())


def _synced_views():
    import TechDrawPMI

    return TechDrawPMI.pmiViews(Gui.Selection.getSelection())


class CommandSyncPMIViews:
    """The selected views drawn from PMI views, or those on the selected
    pages, made to look again as their PMI views' cameras do"""

    def GetResources(self):
        return {"Pixmap": "actions/TechDraw_RedrawPage",
                "MenuText": QT_TRANSLATE_NOOP("TechDraw_SyncPMIViews", "Sync PMI Views"),
                "ToolTip": QT_TRANSLATE_NOOP(
                    "TechDraw_SyncPMIViews",
                    "The selected views drawn from PMI views, or those on the selected "
                    "pages, look again as their PMI views' cameras do now, at what they "
                    "show, and their annotations are imported again. The page's scale and "
                    "the views' places stay; what you moved is kept")}

    def IsActive(self):
        return bool(_synced_views())

    def Activated(self):
        import TechDrawPMI

        views = _synced_views()
        doc = views[0].Document
        doc.openTransaction("Sync PMI Views")
        try:
            synced, left = TechDrawPMI.syncViews(views, annotate=False)
        finally:
            doc.commitTransaction()
        if not synced and not left:
            App.Console.PrintMessage("Sync PMI views: %s already as their PMI views are\n"
                                     % ", ".join(v.Label for v in views))
            return
        if synced:
            App.Console.PrintMessage("Sync PMI views: %s synced\n"
                                     % ", ".join(v.Label for v in synced))
            _annotate(synced, WAIT_MS, replace=True)


Gui.addCommand("TechDraw_SyncPMIViews", CommandSyncPMIViews())
