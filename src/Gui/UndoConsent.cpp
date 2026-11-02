/***************************************************************************
 *   Copyright (c) 2026 Zheng, Lei <realthunder.dev@gmail.com>             *
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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPushButton>
#endif

#include <map>

#include <App/Actor.h>
#include <App/Application.h>
#include <App/Document.h>

#include "UndoConsent.h"
#include "MainWindow.h"


using namespace Gui;

namespace
{

struct Pending
{
    UndoConsent::Request request;
    /// Each owner's yes, in the order of request.owners
    std::vector<bool> agreed;
};

std::map<int, Pending>& requests()
{
    static std::map<int, Pending> map;
    return map;
}

int nextId = 0;
UndoConsent::Deliver deliver;
UndoConsent::Notify notify;

QString nameOf(const App::Actor* actor)
{
    return actor && actor->kind != App::Actor::Local ? QString::fromUtf8(actor->name.c_str())
                                                     : QObject::tr("the desktop user");
}

QJsonArray names(const std::vector<std::string>& list)
{
    QJsonArray out;
    for (const auto& n : list)
        out.append(QString::fromUtf8(n.c_str()));
    return out;
}

std::string json(const QJsonObject& o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();
}

/// The request \a id ended: the undo made when \a allowed, and the one who
/// asked told
void finish(int id, bool allowed, const QString& by)
{
    auto it = requests().find(id);
    if (it == requests().end())
        return;
    const UndoConsent::Request request = std::move(it->second.request);
    requests().erase(it);

    bool done = false;
    std::string why;
    if (allowed) {
        App::Document* doc = App::GetApplication().getDocument(request.doc.c_str());
        if (!doc) {
            why = "the document is gone";
        }
        else {
            // as the one who asked: the inverse rows are theirs
            App::ActorScope actor(request.requester);
            done = doc->undoSteps(request.steps);
            if (!done)
                why = doc->undoRefusal();
            doc->recomputeAfterUndo();
        }
    }

    QJsonObject result;
    result[QLatin1String("op")] = QLatin1String("consent.result");
    result[QLatin1String("request")] = id;
    result[QLatin1String("doc")] = QString::fromUtf8(request.doc.c_str());
    result[QLatin1String("what")] = QString::fromUtf8(request.what.c_str());
    result[QLatin1String("allowed")] = allowed;
    result[QLatin1String("done")] = done;
    result[QLatin1String("by")] = by;
    if (!why.empty())
        result[QLatin1String("why")] = QString::fromUtf8(why.c_str());
    if (request.requesterClient) {
        if (notify)
            notify(request.requesterClient, json(result));
        return;
    }
    // the desktop asked: said where it is, without stopping it
    auto box = new QMessageBox(getMainWindow());
    box->setObjectName(QStringLiteral("undoConsentResult"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::NonModal);
    box->setWindowTitle(QObject::tr("Undo"));
    const QString what = QString::fromUtf8(request.what.c_str());
    if (!allowed)
        box->setText(QObject::tr("%1 did not agree: '%2' is not undone.").arg(by, what));
    else if (done)
        box->setText(QObject::tr("%1 agreed: '%2' is undone.").arg(by, what));
    else
        box->setText(QObject::tr("%1 agreed, but '%2' did not go: %3")
                         .arg(by, what, QString::fromUtf8(why.c_str())));
    box->setIcon(done ? QMessageBox::Information : QMessageBox::Warning);
    box->show();
}

}  // namespace

void UndoConsent::setTransport(Deliver d, Notify n)
{
    deliver = std::move(d);
    notify = std::move(n);
}

std::size_t UndoConsent::pending()
{
    return requests().size();
}

int UndoConsent::ask(Request request, std::string& why)
{
    if (request.owners.empty()) {
        why = "nobody to ask";
        return 0;
    }
    const int id = ++nextId;
    QJsonObject question;
    question[QLatin1String("op")] = QLatin1String("consent.ask");
    question[QLatin1String("request")] = id;
    question[QLatin1String("doc")] = QString::fromUtf8(request.doc.c_str());
    question[QLatin1String("from")] = nameOf(request.requester.get());
    question[QLatin1String("what")] = QString::fromUtf8(request.what.c_str());
    question[QLatin1String("steps")] = names(request.theirs);

    // Every served owner first: one who cannot be asked ends it before
    // anyone is
    for (const auto& owner : request.owners) {
        if (!owner || owner->kind == App::Actor::Local)
            continue;
        if (!deliver || !deliver(owner.get(), json(question))) {
            why = owner->name + " is not connected";
            return 0;
        }
    }
    Pending pending {request, std::vector<bool>(request.owners.size(), false)};
    requests()[id] = std::move(pending);

    // The desktop user, in a dialog that does not stop the desktop
    for (const auto& owner : request.owners) {
        if (owner && owner->kind != App::Actor::Local)
            continue;
        auto box = new QMessageBox(getMainWindow());
        box->setObjectName(QStringLiteral("undoConsentAsk"));
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->setWindowModality(Qt::NonModal);
        box->setIcon(QMessageBox::Question);
        box->setWindowTitle(QObject::tr("Undo"));
        box->setText(QObject::tr("%1 asks to undo '%2', which needs these steps of yours "
                                 "undone with it:")
                         .arg(nameOf(request.requester.get()),
                              QString::fromUtf8(request.what.c_str())));
        QStringList theirs;
        for (const auto& n : request.theirs)
            theirs << QStringLiteral("  ") + QString::fromUtf8(n.c_str());
        box->setInformativeText(theirs.join(QLatin1Char('\n')));
        QPushButton* allow = box->addButton(QObject::tr("Allow"), QMessageBox::AcceptRole);
        box->addButton(QObject::tr("Deny"), QMessageBox::RejectRole);
        QObject::connect(box, &QMessageBox::buttonClicked, box, [id, allow](QAbstractButton* b) {
            std::string why;
            UndoConsent::answer(id, b == allow, nullptr, why);
        });
        box->show();
    }
    return id;
}

bool UndoConsent::answer(int id, bool allow, const App::Actor* by, std::string& why)
{
    auto it = requests().find(id);
    if (it == requests().end()) {
        why = "no such request";
        return false;
    }
    Pending& pending = it->second;
    std::size_t i = 0;
    for (; i < pending.request.owners.size(); ++i) {
        if (App::Actor::sameUser(pending.request.owners[i].get(), by))
            break;
    }
    if (i == pending.request.owners.size()) {
        why = "not yours to answer";
        return false;
    }
    const QString who = nameOf(by);
    if (!allow) {
        finish(id, false, who);
        return true;
    }
    pending.agreed[i] = true;
    if (std::all_of(pending.agreed.begin(), pending.agreed.end(), [](bool b) { return b; }))
        finish(id, true, who);
    return true;
}
