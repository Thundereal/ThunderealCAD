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

#include "PreCompiled.h"
#ifndef _PreComp_
#include <algorithm>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#endif

#include <App/Document.h>
#include <Gui/MainWindow.h>
#include <Mod/TechDraw/App/DrawUtil.h>
#include <Mod/TechDraw/App/DrawViewPart.h>

#include "TaskReferenceList.h"

using namespace TechDrawGui;

TaskReferenceList::TaskReferenceList(TechDraw::DrawViewPart* view, const QStringList& kinds,
                                     Lister lister, Replacer replacer, Error error,
                                     QWidget* parent)
    : QWidget(parent),
      m_view(view),
      m_kinds(kinds),
      m_lister(std::move(lister)),
      m_replacer(std::move(replacer)),
      m_error(std::move(error))
{
    setObjectName(QStringLiteral("TaskReferenceList"));
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_message = new QLabel(this);
    m_message->setObjectName(QStringLiteral("lMessage"));
    m_message->setWordWrap(true);
    m_message->setTextFormat(Qt::RichText);
    layout->addWidget(m_message);
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("lwReferences"));
    m_list->setMaximumHeight(90);
    m_list->setToolTip(tr("The elements this is made on. Select one and press Reattach to "
                          "put it on another element."));
    layout->addWidget(m_list);
    m_reattach = new QPushButton(tr("Reattach"), this);
    m_reattach->setObjectName(QStringLiteral("pbReattach"));
    m_reattach->setCheckable(true);
    m_reattach->setToolTip(tr("Select a reference above, press this, then pick the element in "
                              "the view it should be on."));
    layout->addWidget(m_reattach);
    connect(m_reattach, &QPushButton::toggled, this, &TaskReferenceList::onReattachToggled);
    refresh();
}

TechDraw::DrawViewPart* TaskReferenceList::view() const
{
    return m_view.getObjectAs<TechDraw::DrawViewPart>();
}

void TaskReferenceList::setRefound(const std::vector<TechDraw::ReferenceNameMatch>& refound)
{
    m_refound = refound;
    refresh();
}

QString TaskReferenceList::refoundNote(const std::vector<TechDraw::ReferenceNameMatch>& refound)
{
    if (refound.empty()) {
        return {};
    }
    QStringList said;
    for (auto& found : refound) {
        said << tr("%1 is now %2").arg(QString::fromStdString(found.was),
                                       QString::fromStdString(found.now));
    }
    return tr("Found again by name: %1. Its geometry is not what the reference was made on: "
              "check it, then OK keeps it, Cancel leaves the reference as it was, or Reattach "
              "puts it on another element.")
        .arg(said.join(QStringLiteral("; ")));
}

void TaskReferenceList::refresh()
{
    const int current = m_list->currentRow();
    m_list->clear();
    const std::vector<std::string> refs = m_lister ? m_lister() : std::vector<std::string>();
    for (size_t i = 0; i < refs.size(); i++) {
        auto item = new QListWidgetItem(QString::fromStdString(refs[i]), m_list);
        item->setData(Qt::UserRole, int(i));
        for (auto& found : m_refound) {
            if (found.index == int(i) && found.now == refs[i]) {
                item->setForeground(QColor(0xb0, 0x70, 0x00));
                item->setToolTip(tr("Was %1: found again by its name when this panel opened. "
                                    "OK keeps it, Cancel leaves the reference as it was.")
                                     .arg(QString::fromStdString(found.was)));
            }
        }
    }
    if (current >= 0 && current < m_list->count()) {
        m_list->setCurrentRow(current);
    }
    else if (!m_refound.empty() && m_refound.front().index < m_list->count()) {
        // the one taken by name, ready for Reattach
        m_list->setCurrentRow(m_refound.front().index);
    }

    QString text;
    const std::string error = m_error ? m_error() : std::string();
    if (!error.empty()) {
        text = QStringLiteral("<font color='red'>%1<br/>%2</font>")
                   .arg(tr("Recompute failed"), QString::fromStdString(error).toHtmlEscaped());
    }
    const QString note = refoundNote(m_refound);
    if (!note.isEmpty()) {
        if (!text.isEmpty()) {
            text += QStringLiteral("<br/>");
        }
        text += QStringLiteral("<font color='#b07000'>%1</font>").arg(note.toHtmlEscaped());
    }
    m_message->setText(text);
    m_message->setVisible(!text.isEmpty());
}

void TaskReferenceList::onReattachToggled(bool checked)
{
    if (!checked) {
        return;
    }
    if (!m_list->currentItem()) {
        QSignalBlocker block(m_reattach);
        m_reattach->setChecked(false);
        QMessageBox::warning(Gui::getMainWindow(), tr("No reference selected"),
                             tr("Select the reference to reattach first."));
        return;
    }
    // the next element picked is the one taken
    Gui::Selection().clearSelection();
}

void TaskReferenceList::onSelectionChanged(const Gui::SelectionChanges& msg)
{
    if (msg.Type != Gui::SelectionChanges::AddSelection || !m_reattach->isChecked()) {
        return;
    }
    TechDraw::DrawViewPart* dvp = view();
    auto item = m_list->currentItem();
    if (!dvp || !item || !msg.pObjectName || !msg.pSubName || !msg.pDocName) {
        return;
    }
    std::string sub(msg.pSubName);
    std::string::size_type dot = sub.rfind('.');
    if (dot != std::string::npos) {
        sub = sub.substr(dot + 1);
    }
    std::string kind;
    try {
        kind = TechDraw::DrawUtil::getGeomTypeFromName(sub);
    }
    catch (const Base::Exception&) {
        return;
    }
    if (std::string(msg.pDocName) != dvp->getDocument()->getName()
        || std::string(msg.pObjectName) != dvp->getNameInDocument()
        || !m_kinds.contains(QString::fromStdString(kind))) {
        // not an element of the view this is on, of a kind it takes: wait
        return;
    }
    const int index = item->data(Qt::UserRole).toInt();
    {
        QSignalBlocker block(m_reattach);
        m_reattach->setChecked(false);
    }
    if (m_replacer && m_replacer(index, sub)) {
        // what was taken by name is settled for this one
        m_refound.erase(std::remove_if(m_refound.begin(), m_refound.end(),
                                       [index](const TechDraw::ReferenceNameMatch& m) {
                                           return m.index == index;
                                       }),
                        m_refound.end());
    }
    refresh();
}

#include "moc_TaskReferenceList.cpp"
