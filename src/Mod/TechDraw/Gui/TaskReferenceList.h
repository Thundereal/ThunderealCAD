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

#ifndef TECHDRAWGUI_TASKREFERENCELIST_H
#define TECHDRAWGUI_TASKREFERENCELIST_H

#include <functional>
#include <string>
#include <vector>

#include <App/DocumentObserver.h>
#include <QStringList>
#include <QWidget>

#include <Gui/Selection.h>
#include <Mod/TechDraw/App/ReferenceNameMatch.h>
#include <Mod/TechDraw/TechDrawGlobal.h>

class QLabel;
class QListWidget;
class QPushButton;

namespace TechDraw
{
class DrawViewPart;
}

namespace TechDrawGui
{

/** The references of a hatch or a centre line into a view, for their panels:
 * listed, those taken by name when the panel opened marked and said in amber
 * (as PartDesign's dress-up panels do), the object's recompute error in red,
 * and Reattach -- select a row, press it, pick the element in the view -- to
 * put one on another element (the reporter, 2026-11-26: "expose that also for
 * user to reattach").
 */
class TaskReferenceList: public QWidget, public Gui::SelectionObserver
{
    Q_OBJECT

public:
    /// The references as they are now
    using Lister = std::function<std::vector<std::string>()>;
    /// Reference `index` put on `subName`; false if it could not be
    using Replacer = std::function<bool(int index, const std::string& subName)>;
    /// The object's error, empty for none
    using Error = std::function<std::string()>;

    TaskReferenceList(TechDraw::DrawViewPart* view, const QStringList& kinds, Lister lister,
                      Replacer replacer, Error error, QWidget* parent = nullptr);

    /// What was taken by name when the panel opened
    void setRefound(const std::vector<TechDraw::ReferenceNameMatch>& refound);
    const std::vector<TechDraw::ReferenceNameMatch>& refound() const
    {
        return m_refound;
    }
    /// The list and the message as the object is now
    void refresh();

    /// The amber note for what was taken by name, empty for nothing
    static QString refoundNote(const std::vector<TechDraw::ReferenceNameMatch>& refound);

private:
    void onReattachToggled(bool checked);
    void onSelectionChanged(const Gui::SelectionChanges& msg) override;

    App::DocumentObjectT m_view;
    TechDraw::DrawViewPart* view() const;
    QStringList m_kinds;
    Lister m_lister;
    Replacer m_replacer;
    Error m_error;
    std::vector<TechDraw::ReferenceNameMatch> m_refound;
    QLabel* m_message;
    QListWidget* m_list;
    QPushButton* m_reattach;
};

}  // namespace TechDrawGui

#endif  // TECHDRAWGUI_TASKREFERENCELIST_H
