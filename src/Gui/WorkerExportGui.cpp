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

#include <map>
#include <memory>
#include <sstream>
#include <string>

#include <QMessageBox>
#include <QTimer>

#include <nlohmann/json.hpp>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/WorkerPool.h>
#include <Base/Console.h>
#include <Base/FileInfo.h>
#include <Base/Interpreter.h>
#include <Base/Parameter.h>
#include <Base/Tools.h>

#include "Application.h"
#include "Macro.h"
#include "MainWindow.h"
#include "WorkerExportGui.h"

FC_LOG_LEVEL_INIT("Workers", true, true)

using namespace Gui;

namespace
{

/// An export with the workers: what it was, and whether the document
/// changed since -- then it can no more be made here as it was asked for
struct Pending
{
    std::string document;
    std::string file;
    std::string code;
    std::string settings;
    bool changed = false;
    std::vector<fastsignals::scoped_connection> watch;
};

ParameterGrp::handle params()
{
    return App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Workers");
}

void say(const QString& text, int ms = 0)
{
    if (auto mw = getMainWindow()) {
        mw->showMessage(text, ms);
    }
}

void fail(const QString& text)
{
    // Not inside the pool's own call
    QTimer::singleShot(0, [text]() {
        QMessageBox::critical(getMainWindow(), QObject::tr("Export failed"), text);
    });
}

void addRecent(const std::string& file)
{
    // As an export here adds it (Gui::Application::exportTo)
    if (!App::GetApplication()
             .GetParameterGroupByPath("User parameter:BaseApp/Preferences/General")
             ->GetBool("RecentIncludesExported", false)) {
        return;
    }
    Base::FileInfo info(file);
    if (!App::GetApplication().getImportFilters(info.extension().c_str()).empty()) {
        if (auto mw = getMainWindow()) {
            mw->appendRecentFile(QString::fromUtf8(info.filePath().c_str()));
        }
    }
}

/// The module a worker exports with for `module`, which may be a GUI one:
/// ImportGui's STEP and IGES are Import's, the looks asked of the objects
/// (docs/ShapeAppearanceDesign.md sec 14.6.7); its glTF reads render settings
/// only view providers have. Empty: not a worker's
std::string workerModule(const std::string& module, const std::string& file)
{
    std::stringstream list(params()->GetASCII("ExportModules", "Import;ImportGui;Part;Mesh"));
    std::string item;
    bool listed = false;
    while (std::getline(list, item, ';')) {
        listed = listed || item == module;
    }
    if (!listed) {
        return {};
    }
    if (module != "ImportGui") {
        return module;
    }
    Base::FileInfo info(file);
    return info.hasExtension({"step", "stp", "iges", "igs"}) ? std::string("Import")
                                                             : std::string();
}

/// The export here, with the options already asked for
std::string codeWith(const std::string& module, const std::vector<App::DocumentObject*>& objs,
                     const std::string& file, const std::string& options)
{
    std::stringstream str;
    str << "import " << module << "\n__objs__=[]\n";
    for (auto obj : objs) {
        str << "__objs__.append(" << obj->getFullName(true) << ")\n";
    }
    if (options.empty()) {
        str << module << ".export(__objs__, " << Base::Tools::pythonLiteral(file) << ")\n";
    }
    else {
        str << "import json\n"
            << module << ".export(__objs__, " << Base::Tools::pythonLiteral(file)
            << ", json.loads(" << Base::Tools::pythonLiteral(options) << "))\n";
    }
    return str.str();
}

void exportHere(const std::shared_ptr<Pending>& p)
{
    QTimer::singleShot(0, [p]() {
        if (!App::GetApplication().getDocument(p->document.c_str())) {
            return;
        }
        try {
            Base::PyGILStateLocker lock;
            Base::Interpreter().runString(p->code.c_str());
            Base::Interpreter().runString("del __objs__");
            addRecent(p->file);
            say(QObject::tr("Exported %1").arg(QString::fromUtf8(p->file.c_str())), 8000);
        }
        catch (const Base::PyException& e) {
            e.ReportException();
            QMessageBox::critical(getMainWindow(), QObject::tr("Export failed"),
                                  QString::fromUtf8(e.what()));
        }
        catch (Base::Exception& e) {
            e.ReportException();
            QMessageBox::critical(getMainWindow(), QObject::tr("Export failed"),
                                  QString::fromUtf8(e.what()));
        }
    });
}

void finished(const std::shared_ptr<Pending>& p, App::WorkerPool::Result& r)
{
    p->watch.clear();
    if (!p->settings.empty()) {
        Base::FileInfo(p->settings).deleteFile();
    }
    const QString file = QString::fromUtf8(p->file.c_str());
    using Status = App::WorkerPool::Result::Status;
    switch (r.status) {
        case Status::Done:
            addRecent(p->file);
            say(QObject::tr("Exported %1 by a worker in %2 s").arg(file).arg(r.seconds, 0, 'f', 1),
                8000);
            return;
        case Status::Failed:
            if (r.died) {
                // Not tried here: what ended a worker would end this process
                fail(QObject::tr("The worker process exporting %1 ended before it was written; "
                                 "it was not tried here, where it could end this session too.")
                         .arg(file));
            }
            else {
                // The exporter's own failure: here it would fail the same
                fail(QString::fromUtf8(r.error.c_str()));
            }
            return;
        case Status::Unfit:
        case Status::Cancelled:
            break;
    }
    if (!p->changed) {
        FC_LOG(p->document << ": exported here: " << r.error);
        say(QObject::tr("Exporting %1 here: %2").arg(file, QString::fromUtf8(r.error.c_str())));
        exportHere(p);
        return;
    }
    fail(QObject::tr("%1 was not written: %2. The document changed since the export was asked "
                     "for; export it again.")
             .arg(file, QString::fromUtf8(r.error.c_str())));
}

}  // namespace

bool Gui::exportByWorkers(App::Document& doc, const std::vector<App::DocumentObject*>& objs,
                          const std::string& module, const std::string& file,
                          const std::string& code)
{
    if (!params()->GetBool("ExportCommand", true) || objs.empty()) {
        return false;
    }
    const std::string runner = workerModule(module, file);
    if (runner.empty() || !App::WorkerPool::supported()) {
        return false;
    }
    nlohmann::json names = nlohmann::json::array();
    for (auto obj : objs) {
        if (!obj || !obj->isAttachedToDocument() || obj->getDocument() != &doc) {
            return false;
        }
        names.push_back(obj->getNameInDocument());
    }
    // The options are asked for here, where the module's dialog is: once
    // they are, a failure to hand it over exports here with them
    nlohmann::json options;
    std::string asked;
    {
        Base::PyGILStateLocker lock;
        try {
            Py::Module mod(PyImport_ImportModule(module.c_str()), true);
            if (mod.isNull() || mod.hasAttr("exportSelection")) {
                return false;  // it chooses its objects itself
            }
            if (mod.hasAttr("exportOptions")) {
                Py::Callable ask(mod.getAttr("exportOptions"));
                Py::Tuple args(1);
                args.setItem(0, Py::String(file));
                Py::Object answer = ask.apply(args);
                Py::Module json(PyImport_ImportModule("json"), true);
                asked = Py::String(Py::Callable(json.getAttr("dumps")).apply(Py::TupleN(answer)))
                            .as_std_string("utf-8");
                options = nlohmann::json::parse(asked, nullptr, false);
            }
        }
        catch (Py::Exception&) {
            Base::PyException e;
            e.ReportException();
            return false;
        }
    }
    auto here = [&]() {
        // Asked for already: not asked again
        if (asked.empty()) {
            return false;
        }
        auto p = std::make_shared<Pending>();
        p->document = doc.getName();
        p->file = file;
        p->code = codeWith(module, objs, file, asked);
        Application::Instance->macroManager()->addLine(MacroManager::App, code.c_str());
        exportHere(p);
        return true;
    };
    if (!asked.empty() && (options.is_discarded() || !options.is_object()
                           || options.value("legacy", false))) {
        // Not JSON, or the legacy exporter, which reads the view providers
        return here();
    }
    // ImportGui's options are Import's keyword arguments
    nlohmann::json keywords = nlohmann::json::object();
    if (runner != module && options.is_object()) {
        for (const auto& [key, value] : options.items()) {
            if ((key != "exportHidden" && key != "keepPlacement" && key != "legacy")
                || !value.is_boolean()) {
                return here();  // what Import takes no word for
            }
            keywords[key] = value;
        }
        options = nlohmann::json();
    }
    App::WorkerPool* pool = App::WorkerPool::of(doc);
    if (!pool || !pool->start()) {
        FC_LOG(doc.getName() << ": not exported by a worker: "
                             << (pool ? pool->reason() : std::string("no pool")));
        return here();
    }
    auto p = std::make_shared<Pending>();
    p->document = doc.getName();
    p->file = file;
    p->code = codeWith(module, objs, file, asked);
    // The preferences as they are now: an exporter reads its own there, and
    // the worker's copy is from when it was started
    static int serial = 0;
    p->settings = pool->info().directory + "/out/export-" + std::to_string(++serial) + ".cfg";
    try {
        App::GetApplication()
            .GetUserParameter()
            .GetGroup("BaseApp")
            ->GetGroup("Preferences")
            ->exportTo(p->settings.c_str());
    }
    catch (Base::Exception& e) {
        FC_LOG(doc.getName() << ": not exported by a worker: " << e.what());
        return here();
    }
    App::WorkerPool::Job job;
    job.kind = "export";
    job.tag = "export";
    job.pinned = true;
    job.inputs = nlohmann::json {{"module", runner},
                                 {"objects", names},
                                 {"file", file},
                                 {"options", asked.empty() ? nlohmann::json() : options},
                                 {"keywords", keywords},
                                 {"settings", p->settings}}
                     .dump();
    job.done = [p](App::WorkerPool::Result& r) {
        finished(p, r);
    };
    // Held by the connections the pending export holds: not by itself
    std::weak_ptr<Pending> weak = p;
    auto changed = [weak](const App::Document& d) {
        auto pending = weak.lock();
        if (pending && d.getName() == pending->document) {
            pending->changed = true;
        }
    };
    p->watch.emplace_back(doc.signalChangedObject.connect(
        [changed](const App::DocumentObject& obj, const App::Property&) {
            if (auto d = obj.getDocument()) {
                changed(*d);
            }
        }));
    p->watch.emplace_back(doc.signalNewObject.connect([changed](const App::DocumentObject& obj) {
        if (auto d = obj.getDocument()) {
            changed(*d);
        }
    }));
    p->watch.emplace_back(
        doc.signalDeletedObject.connect([changed](const App::DocumentObject& obj) {
            if (auto d = obj.getDocument()) {
                changed(*d);
            }
        }));
    if (!pool->submit(std::move(job))) {
        p->watch.clear();
        Base::FileInfo(p->settings).deleteFile();
        return here();
    }
    // Recorded as if it had been run here: a macro replayed exports there
    Application::Instance->macroManager()->addLine(MacroManager::App, code.c_str());
    say(QObject::tr("%1: exporting %2 in a worker process")
            .arg(QString::fromUtf8(doc.Label.getValue()), QString::fromUtf8(file.c_str())));
    return true;
}
