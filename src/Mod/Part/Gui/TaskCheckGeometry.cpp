/***************************************************************************
 *   Copyright (c) 2012 Thomas Anderson <blobfish[at]gmx.com>              *
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

#include <memory>

#include <QPointer>

#include <nlohmann/json.hpp>
#ifndef _PreComp_
# include <QCheckBox>
# include <QCoreApplication>
# include <QHeaderView>
# include <QPushButton>
# include <QScrollBar>
# include <QTextEdit>
# include <QTextStream>
# include <QThread>
# include <QTimer>
# include <QTreeView>
# include <Standard_Version.hxx>
# include <Bnd_Box.hxx>
# include <BOPAlgo_ArgumentAnalyzer.hxx>
# include <BOPAlgo_ListOfCheckResult.hxx>
# include <BRepBndLib.hxx>
# include <BRepBuilderAPI_Copy.hxx>
# include <BRepCheck_Analyzer.hxx>
# include <BRepCheck_ListOfStatus.hxx>
# include <BRepCheck_Result.hxx>
# include <BRepTools_ShapeSet.hxx>
# include <ShapeAnalysis_FreeBounds.hxx>
# include <TopoDS.hxx>
# include <TopoDS_Compound.hxx>
# include <TopTools_IndexedMapOfShape.hxx>
# include <TopExp.hxx>
# include <TopExp_Explorer.hxx>
# include <Inventor/nodes/SoCube.h>
# include <Inventor/nodes/SoDrawStyle.h>
# include <Inventor/nodes/SoMaterial.h>
# include <Inventor/nodes/SoResetTransform.h>
# include <Inventor/nodes/SoSeparator.h>
# include <Inventor/nodes/SoSwitch.h>
# include <Inventor/nodes/SoTransform.h>
#endif //_PreComp_

#include <App/Application.h>
#include <App/Document.h>
#include <App/UnitsParams.h>
#include <App/WorkerPool.h>
#include <Base/Interpreter.h>
#include <Base/Tools.h>
#include <Gui/Application.h>
#include <Gui/BitmapFactory.h>
#include <Gui/MainWindow.h>
#include <Gui/Selection.h>
#include <Gui/ViewProvider.h>
#include <Gui/WaitCursor.h>
#include <Mod/Part/App/GeometryCheck.h>
#include <Mod/Part/App/PartFeature.h>

#include "TaskCheckGeometry.h"
#include "PartParams.h"


using namespace PartGui;

FC_LOG_LEVEL_INIT("Part", true, true)

QVector<QString> buildShapeEnumVector()
{
   QVector<QString>names;
   names.push_back(QObject::tr("Compound"));             //TopAbs_COMPOUND
   names.push_back(QObject::tr("Compound Solid"));       //TopAbs_COMPSOLID
   names.push_back(QObject::tr("Solid"));                //TopAbs_SOLID
   names.push_back(QObject::tr("Shell"));                //TopAbs_SHELL
   names.push_back(QObject::tr("Face"));                 //TopAbs_FACE
   names.push_back(QObject::tr("Wire"));                 //TopAbs_WIRE
   names.push_back(QObject::tr("Edge"));                 //TopAbs_EDGE
   names.push_back(QObject::tr("Vertex"));               //TopAbs_VERTEX
   names.push_back(QObject::tr("Shape"));                //TopAbs_SHAPE
   return names;
}

QString shapeEnumToString(const int &index)
{
    static QVector<QString> names = buildShapeEnumVector();
    if (index < 0 || index > TopAbs_SHAPE)
        return names.at(8);
    return names.at(index);
}

QVector<QString> buildCheckStatusStringVector()
{
    QVector<QString>names;
    names.push_back(QObject::tr("No Error"));                           //    BRepCheck_NoError
    names.push_back(QObject::tr("Invalid Point On Curve"));             //    BRepCheck_InvalidPointOnCurve
    names.push_back(QObject::tr("Invalid Point On Curve On Surface"));  //    BRepCheck_InvalidPointOnCurveOnSurface
    names.push_back(QObject::tr("Invalid Point On Surface"));           //    BRepCheck_InvalidPointOnSurface
    names.push_back(QObject::tr("No 3D Curve"));                        //    BRepCheck_No3DCurve
    names.push_back(QObject::tr("Multiple 3D Curve"));                  //    BRepCheck_Multiple3DCurve
    names.push_back(QObject::tr("Invalid 3D Curve"));                   //    BRepCheck_Invalid3DCurve
    names.push_back(QObject::tr("No Curve On Surface"));                //    BRepCheck_NoCurveOnSurface
    names.push_back(QObject::tr("Invalid Curve On Surface"));           //    BRepCheck_InvalidCurveOnSurface
    names.push_back(QObject::tr("Invalid Curve On Closed Surface"));    //    BRepCheck_InvalidCurveOnClosedSurface
    names.push_back(QObject::tr("Invalid Same Range Flag"));            //    BRepCheck_InvalidSameRangeFlag
    names.push_back(QObject::tr("Invalid Same Parameter Flag"));        //    BRepCheck_InvalidSameParameterFlag
    names.push_back(QObject::tr("Invalid Degenerated Flag"));           //    BRepCheck_InvalidDegeneratedFlag
    names.push_back(QObject::tr("Free Edge"));                          //    BRepCheck_FreeEdge
    names.push_back(QObject::tr("Invalid MultiConnexity"));             //    BRepCheck_InvalidMultiConnexity
    names.push_back(QObject::tr("Invalid Range"));                      //    BRepCheck_InvalidRange
    names.push_back(QObject::tr("Empty Wire"));                         //    BRepCheck_EmptyWire
    names.push_back(QObject::tr("Redundant Edge"));                     //    BRepCheck_RedundantEdge
    names.push_back(QObject::tr("Self Intersecting Wire"));             //    BRepCheck_SelfIntersectingWire
    names.push_back(QObject::tr("No Surface"));                         //    BRepCheck_NoSurface
    names.push_back(QObject::tr("Invalid Wire"));                       //    BRepCheck_InvalidWire
    names.push_back(QObject::tr("Redundant Wire"));                     //    BRepCheck_RedundantWire
    names.push_back(QObject::tr("Intersecting Wires"));                 //    BRepCheck_IntersectingWires
    names.push_back(QObject::tr("Invalid Imbrication Of Wires"));       //    BRepCheck_InvalidImbricationOfWires
    names.push_back(QObject::tr("Empty Shell"));                        //    BRepCheck_EmptyShell
    names.push_back(QObject::tr("Redundant Face"));                     //    BRepCheck_RedundantFace
    names.push_back(QObject::tr("Unorientable Shape"));                 //    BRepCheck_UnorientableShape
    names.push_back(QObject::tr("Not Closed"));                         //    BRepCheck_NotClosed
    names.push_back(QObject::tr("Not Connected"));                      //    BRepCheck_NotConnected
    names.push_back(QObject::tr("Sub Shape Not In Shape"));             //    BRepCheck_SubshapeNotInShape
    names.push_back(QObject::tr("Bad Orientation"));                    //    BRepCheck_BadOrientation
    names.push_back(QObject::tr("Bad Orientation Of Sub Shape"));       //    BRepCheck_BadOrientationOfSubshape
    names.push_back(QObject::tr("Invalid Tolerance Value"));            //    BRepCheck_InvalidToleranceValue
    names.push_back(QObject::tr("Check Failed"));                       //    BRepCheck_CheckFail

    return names;
}

QString checkStatusToString(const int &index)
{
    static QVector<QString> names = buildCheckStatusStringVector();
    if (index == -1)
    {
        return QString(QObject::tr("No Result"));
    }
    if (index > 33 || index < 0)
    {
        QString message(QObject::tr("Out Of Enum Range:") + QStringLiteral(" "));
        QString number;
        number.setNum(index);
        message += number;
        return message;
    }
    return names.at(index);
}

QVector<QString> buildBOPCheckResultVector()
{
  QVector<QString> results;
  results.push_back(QObject::tr("Boolean operation: Unknown check"));               //BOPAlgo_CheckUnknown
  results.push_back(QObject::tr("Boolean operation: Bad type"));                    //BOPAlgo_BadType
  results.push_back(QObject::tr("Boolean operation: Self-intersection found"));     //BOPAlgo_SelfIntersect
  results.push_back(QObject::tr("Boolean operation: Edge too small"));              //BOPAlgo_TooSmallEdge
  results.push_back(QObject::tr("Boolean operation: Non-recoverable face"));        //BOPAlgo_NonRecoverableFace
  results.push_back(QObject::tr("Boolean operation: Incompatibility of vertex"));   //BOPAlgo_IncompatibilityOfVertex
  results.push_back(QObject::tr("Boolean operation: Incompatibility of edge"));     //BOPAlgo_IncompatibilityOfEdge
  results.push_back(QObject::tr("Boolean operation: Incompatibility of face"));     //BOPAlgo_IncompatibilityOfFace
  results.push_back(QObject::tr("Boolean operation: Aborted"));                     //BOPAlgo_OperationAborted
  results.push_back(QObject::tr("Boolean operation: GeomAbs_C0"));                  //BOPAlgo_GeomAbs_C0
  results.push_back(QObject::tr("Boolean operation: Invalid curve on surface"));    //BOPAlgo_InvalidCurveOnSurface
  results.push_back(QObject::tr("Boolean operation: Not valid"));                   //BOPAlgo_NotValid

  return results;
}

QString getBOPCheckString(const BOPAlgo_CheckStatus &status)
{
  static QVector<QString> strings = buildBOPCheckResultVector();
  int index = static_cast<int>(status);
  if (index < 0 || index > strings.size())
    index = 0;
  return strings.at(index);
}

ResultEntry::ResultEntry()
{
    viewProviderRoot = nullptr;
    boxSep = nullptr;
    boxSwitch = nullptr;
    parent = nullptr;
    children.clear();
    selectionStrings.clear();
}

ResultEntry::~ResultEntry()
{
    if (boxSep && viewProviderRoot)
        viewProviderRoot->removeChild(boxSep);
    if (viewProviderRoot)
        viewProviderRoot->unref();
    qDeleteAll(children);
}

void ResultEntry::buildEntryName()
{
    ResultEntry* parentEntry = this;
    while (parentEntry->parent) {
        ResultEntry* temp = parentEntry->parent;
        if (!temp->parent)
            break;
        parentEntry = parentEntry->parent;
    }

  QString stringOut;
  QTextStream stream(&stringOut);
  TopTools_IndexedMapOfShape shapeMap;
  int index(-1);

  switch (this->shape.ShapeType())
  {
  case TopAbs_COMPOUND:
      TopExp::MapShapes(parentEntry->shape, TopAbs_COMPOUND, shapeMap);
      stream << "Compound";
      break;
  case TopAbs_COMPSOLID:
      TopExp::MapShapes(parentEntry->shape, TopAbs_COMPSOLID, shapeMap);
      stream << "CompSolid";
      break;
  case TopAbs_SOLID:
      TopExp::MapShapes(parentEntry->shape, TopAbs_SOLID, shapeMap);
      stream << "Solid";
      break;
  case TopAbs_SHELL:
      TopExp::MapShapes(parentEntry->shape, TopAbs_SHELL, shapeMap);
      stream << "Shell";
      break;
  case TopAbs_WIRE:
      TopExp::MapShapes(parentEntry->shape, TopAbs_WIRE, shapeMap);
      stream << "Wire";
      break;
  case TopAbs_FACE:
      TopExp::MapShapes(parentEntry->shape, TopAbs_FACE, shapeMap);
      stream << "Face";
      break;
  case TopAbs_EDGE:
      TopExp::MapShapes(parentEntry->shape, TopAbs_EDGE, shapeMap);
      stream << "Edge";
      break;
  case TopAbs_VERTEX:
      TopExp::MapShapes(parentEntry->shape, TopAbs_VERTEX, shapeMap);
      stream << "Vertex";
      break;
  default:
      stream << "Unexpected shape type";
      break;
  }

  index = shapeMap.FindIndex(this->shape);
  stream << index;
  this->name = stringOut;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////

ResultModel::ResultModel(QObject *parent) : QAbstractItemModel(parent)
{
    root = nullptr;
}

ResultModel::~ResultModel()
{
    if (root)
        delete root;
}

QModelIndex ResultModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!root)
        return {};
    ResultEntry *parentNode = nodeFromIndex(parent);
    if (!parentNode)
        return {};
    return createIndex(row, column, parentNode->children.at(row));
}

QModelIndex ResultModel::parent(const QModelIndex &child) const
{
    ResultEntry *childNode = nodeFromIndex(child);
    if (!childNode)
        return {};
    ResultEntry *parentNode = childNode->parent;
    if (!parentNode)
        return {};
    ResultEntry *grandParentNode = parentNode->parent;
    if (!grandParentNode)
        return {};
    int row = grandParentNode->children.indexOf(parentNode);
    return createIndex(row, 0, parentNode);
}

int ResultModel::rowCount(const QModelIndex &parent) const
{
    ResultEntry *parentNode = nodeFromIndex(parent);
    if (!parentNode)
        return 0;
    return parentNode->children.size();
}

int ResultModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return 3;
}

QVariant ResultModel::data(const QModelIndex &index, int role) const
{
    if (role != Qt::DisplayRole)
        return {};
    ResultEntry *node = nodeFromIndex(index);
    if (!node)
        return {};
    switch (index.column())
    {
    case 0:
        return QVariant(node->name);
    case 1:
        return QVariant(node->type);
    case 2:
        return QVariant(node->error);
    }
    return {};
}

QVariant ResultModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section)
    {
    case 0:
        return QVariant(QString(tr("Name")));
    case 1:
        return QVariant(QString(tr("Type")));
    case 2:
        return QVariant(QString(tr("Error")));
    }
    return {};
}

void ResultModel::setResults(ResultEntry *resultsIn)
{
    this->beginResetModel();
    if (root)
        delete root;
    root = resultsIn;
    this->endResetModel();
}

ResultEntry* ResultModel::getEntry(const QModelIndex &index)
{
    return nodeFromIndex(index);
}

ResultEntry* ResultModel::nodeFromIndex(const QModelIndex &index) const
{
    if (index.isValid())
        return static_cast<ResultEntry *>(index.internalPointer());
    else
        return root;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////

TaskCheckGeometryResults::TaskCheckGeometryResults(QWidget *parent) : QWidget(parent)
{
    this->setWindowTitle(tr("Check Geometry Results"));
    setupInterface();
    setupFunctionMap();
}

TaskCheckGeometryResults::~TaskCheckGeometryResults()
{
    cancelWorker();
    try {
        Gui::Selection().clearSelection();
    }
    catch (const Py::Exception&) {
        Base::PyException e; // extract the Python error text
        e.ReportException();
    }
}

void TaskCheckGeometryResults::setupInterface()
{
    message = new QLabel(this);
    message->setText(tr("Check is running..."));
    model = new ResultModel(this);
    treeView = new QTreeView(this);
    treeView->setModel(model);
    treeView->setSelectionMode(QAbstractItemView::SingleSelection);
    treeView->setSelectionBehavior(QAbstractItemView::SelectRows);
    connect(treeView->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, &TaskCheckGeometryResults::currentRowChanged);

    QVBoxLayout *layout = new QVBoxLayout();
    layout->addWidget(message);
    layout->addWidget(treeView);
    this->setLayout(layout);
}

namespace
{

Part::GeometryCheck::Options checkOptions()
{
    Part::GeometryCheck::Options o;
    o.bop = PartParams::getCheckGeometryRunBOPCheck();
    // The Single-threaded box of the panel stores RunSingleThreaded. This
    // read a key of another name, RunBOPCheckSingleThreaded, which nothing
    // writes: the box did nothing.
    o.singleThreaded = PartParams::getCheckGeometryRunSingleThreaded();
    o.argumentType = PartParams::getCheckGeometryArgumentTypeMode();
    o.selfInter = PartParams::getCheckGeometrySelfInterMode();
    o.smallEdge = PartParams::getCheckGeometrySmallEdgeMode();
    o.rebuildFace = PartParams::getCheckGeometryRebuildFaceMode();
    o.continuity = PartParams::getCheckGeometryContinuityMode();
    o.tangent = PartParams::getCheckGeometryTangentMode();
    o.mergeVertex = PartParams::getCheckGeometryMergeVertexMode();
    o.mergeEdge = PartParams::getCheckGeometryMergeEdgeMode();
    o.curveOnSurface = PartParams::getCheckGeometryCurveOnSurfaceMode();
    return o;
}

nlohmann::json optionsJson(const Part::GeometryCheck::Options& o)
{
    return nlohmann::json {{"bop", o.bop},
                           {"singleThreaded", o.singleThreaded},
                           {"argumentType", o.argumentType},
                           {"selfInter", o.selfInter},
                           {"smallEdge", o.smallEdge},
                           {"rebuildFace", o.rebuildFace},
                           {"continuity", o.continuity},
                           {"tangent", o.tangent},
                           {"mergeVertex", o.mergeVertex},
                           {"mergeEdge", o.mergeEdge},
                           {"curveOnSurface", o.curveOnSurface}};
}

}  // namespace

void TaskCheckGeometryResults::goCheck()
{
    Gui::WaitCursor wc;
    auto selection = Gui::Selection().getSelection();
    cancelWorker();
    items.clear();

    int selectedCount(0);
    for(const auto &sel :  selection) {
        selectedCount++;
        TopoDS_Shape shape = Part::Feature::getShape(sel.pObject,sel.SubName,true);
        if (shape.IsNull())
            continue;
        auto vp = Gui::Application::Instance->getViewProvider(sel.pObject);
        if (!vp || !vp->getRoot())
            continue;
        Item item;
        item.object = sel.pObject;
        item.sub = sel.SubName ? sel.SubName : "";
        item.shape = shape;
        item.separator = vp->getRoot();
        QTextStream baseStream(&item.baseName);
        baseStream << QString::fromUtf8(sel.DocName);
        baseStream << "." << QString::fromUtf8(sel.FeatName);
        buildShapeContent(sel.pObject, item.baseName, shape);
        items.push_back(item);
    }
    this->selectedCount = selectedCount;
    const Part::GeometryCheck::Options options = checkOptions();
    if (checkInWorkers(options)) {
        message->setText(tr("Checking %n shape(s) in a worker process...", "",
                            static_cast<int>(items.size())));
        model->setResults(new ResultEntry());
        return;
    }

    std::string scopeName {tr("Boolean operation check...").toStdString()};
    Handle(Message_ProgressIndicator) theProgress = new BOPProgressIndicator(tr("Check geometry"),
                                                                             Gui::getMainWindow());
    Message_ProgressRange theRange(theProgress->Start());
    Message_ProgressScope theScope(theRange,
                                   TCollection_AsciiString(scopeName.c_str()),
                                   static_cast<int>(items.size()));
    theScope.Show();
    ResultEntry *theRoot = new ResultEntry();
    int invalidShapes = 0;
    int checkedCount = 0;
    for (const auto& item : items) {
        std::string label = tr("Checking").toStdString() + " ";
        label += item.object->Label.getStrValue();
        label += "...";
        Message_ProgressScope theInnerScope(theScope.Next(), TCollection_AsciiString(label.c_str()), 1);
        theInnerScope.Show();
        std::vector<Part::GeometryCheck::Finding> found;
        try {
            found = Part::GeometryCheck::check(item.shape, options, theInnerScope.Next());
        }
        catch (const Standard_Failure& e) {
            FC_ERR(item.baseName.toStdString() << ": " << e.GetMessageString());
        }
        theInnerScope.Close();
        ++checkedCount;
        invalidShapes += putFindings(found, item, theRoot);
        if (theScope.UserBreak())
            break;
    }
    showResults(theRoot, checkedCount, invalidShapes);
}

void TaskCheckGeometryResults::showResults(ResultEntry* theRoot, int checkedCount,
                                           int invalidShapes)
{
    model->setResults(theRoot);
    treeView->expandAll();
    treeView->header()->resizeSections(QHeaderView::ResizeToContents);
    QString aMessage {tr("%1 processed out of %2 selected").arg(checkedCount).arg(selectedCount)};
    aMessage += QLatin1String("\n ") + tr("%n invalid shapes.", "", invalidShapes);
    message->setText(aMessage);
}

int TaskCheckGeometryResults::putFindings(const std::vector<Part::GeometryCheck::Finding>& found,
                                          const Item& item, ResultEntry* theRoot)
{
    // The entries the findings name, on the shape this process holds: each
    // by its type and index among the checked shape's
    using Source = Part::GeometryCheck::Source;
    const bool logErrors = PartParams::getCheckGeometryLogErrors();
    std::vector<ResultEntry*> made;
    made.reserve(found.size());
    int invalid = 0;
    for (const auto& f : found) {
        ResultEntry* entry = new ResultEntry();
        entry->viewProviderRoot = item.separator;
        entry->viewProviderRoot->ref();
        const auto type = static_cast<TopAbs_ShapeEnum>(f.type);
        if (f.parent < 0 || f.parent >= static_cast<int>(made.size())) {
            ++invalid;
            entry->parent = theRoot;
            entry->shape = item.shape;
            entry->name = item.baseName;
            entry->type = shapeEnumToString(type);
            entry->error = tr("Invalid");
            goSetupResultBoundingBox(entry);
            theRoot->children.push_back(entry);
            made.push_back(entry);
            continue;
        }
        ResultEntry* parent = made[f.parent];
        TopTools_IndexedMapOfShape map;
        TopExp::MapShapes(item.shape, type, map);
        if (f.index >= 1 && f.index <= map.Extent()) {
            entry->shape = map.FindKey(f.index);
        }
        else {
            entry->shape = item.shape;
        }
        entry->parent = parent;
        entry->buildEntryName();
        entry->type = shapeEnumToString(type);
        if (f.source == Source::Brep) {
            entry->error = checkStatusToString(f.status);
            dispatchError(entry, static_cast<BRepCheck_Status>(f.status));
        }
        else {
            entry->error = getBOPCheckString(static_cast<BOPAlgo_CheckStatus>(f.status));
            goSetupResultBoundingBox(entry);
            if (type == TopAbs_FACE || type == TopAbs_EDGE || type == TopAbs_VERTEX) {
                goSetupResultTypedSelection(entry, entry->shape, type);
            }
            /*log BOPCheck errors to report view*/
            if (logErrors) {
                std::clog << parent->name.toStdString().c_str() << " : "
                          << entry->name.toStdString().c_str() << " : "
                          << entry->type.toStdString().c_str() << " : "
                          << entry->error.toStdString().c_str() << std::endl;
            }
        }
        parent->children.push_back(entry);
        made.push_back(entry);
    }
    return invalid;
}

bool TaskCheckGeometryResults::checkInWorkers(const Part::GeometryCheck::Options& options)
{
    // docs/WorkerProcess.md sec 25: the window free while a worker checks,
    // CheckCommand of Preferences/Workers ("Check geometry in a worker
    // process" in the Document preferences), on unless set off
    auto group = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Workers");
    if (items.empty() || checkHere || !group->GetBool("CheckCommand", true)
        || !App::WorkerPool::supported()) {
        return false;
    }
    App::Document* doc = items.front().object->getDocument();
    nlohmann::json list = nlohmann::json::array();
    std::vector<std::string> reads;
    for (const auto& item : items) {
        if (item.object->getDocument() != doc) {
            return false;  // a pool is a document's
        }
        list.push_back({{"object", item.object->getNameInDocument()}, {"sub", item.sub}});
        reads.emplace_back(item.object->getNameInDocument());
    }
    App::WorkerPool* pool = App::WorkerPool::of(*doc);
    if (!pool || !pool->start()) {
        return false;
    }
    static long serial = 0;
    App::WorkerPool::Job job;
    job.kind = "part.check";
    job.tag = "part.check." + std::to_string(++serial);
    // What it reads: an edit of one of them, or of what they are made from,
    // drops it, and the check is to be run again
    job.reads = reads;
    job.inputs = nlohmann::json {{"items", list}, {"options", optionsJson(options)}}.dump();
    QPointer<TaskCheckGeometryResults> self(this);
    auto timer = std::make_shared<QElapsedTimer>();
    timer->start();
    job.done = [self, timer](App::WorkerPool::Result& r) {
        if (!self) {
            return;
        }
        self->workerDone(r, timer->elapsed() / 1000.0);
    };
    const std::string tag = job.tag;
    if (!pool->submit(std::move(job))) {
        return false;
    }
    workerTag = tag;
    workerDocument = doc->getName();
    return true;
}

void TaskCheckGeometryResults::workerDone(App::WorkerPool::Result& r, double seconds)
{
    workerTag.clear();
    workerDocument.clear();
    using Status = App::WorkerPool::Result::Status;
    if (r.status != Status::Done) {
        if (r.status == Status::Cancelled) {
            message->setText(tr("The design changed while a worker checked it: %1. Check again.")
                                 .arg(QString::fromUtf8(r.error.c_str())));
            return;
        }
        if (r.died) {
            message->setText(tr("The worker process checking it ended before it was done: "
                                "an OCCT failure on this geometry. Not tried here, where it "
                                "could end this session too."));
            return;
        }
        // Failed or unfit: here, as it was asked for
        FC_LOG("checked here: " << r.error);
        QTimer::singleShot(0, this, [this]() {
            Base::StateLocker here(checkHere);
            goCheck();
        });
        return;
    }
    nlohmann::json output = nlohmann::json::parse(r.output, nullptr, false);
    const nlohmann::json results = output.is_object() ? output.value("results", nlohmann::json())
                                                      : nlohmann::json();
    ResultEntry* theRoot = new ResultEntry();
    int invalidShapes = 0;
    int checkedCount = 0;
    for (std::size_t i = 0; i < items.size() && results.is_array() && i < results.size(); ++i) {
        std::vector<Part::GeometryCheck::Finding> found;
        for (const auto& f : results[i].value("findings", nlohmann::json::array())) {
            if (!f.is_array() || f.size() != 5) {
                continue;
            }
            Part::GeometryCheck::Finding finding;
            finding.parent = f[0].get<int>();
            finding.type = f[1].get<int>();
            finding.index = f[2].get<int>();
            finding.source = static_cast<Part::GeometryCheck::Source>(f[3].get<int>());
            finding.status = f[4].get<int>();
            found.push_back(finding);
        }
        ++checkedCount;
        invalidShapes += putFindings(found, items[i], theRoot);
    }
    showResults(theRoot, checkedCount, invalidShapes);
    if (auto mw = Gui::getMainWindow()) {
        mw->showMessage(tr("Geometry checked by a worker in %1 s").arg(seconds, 0, 'f', 1), 8000);
    }
}

void TaskCheckGeometryResults::cancelWorker()
{
    if (workerTag.empty()) {
        return;
    }
    if (auto doc = App::GetApplication().getDocument(workerDocument.c_str())) {
        if (auto pool = App::WorkerPool::of(*doc, false)) {
            const std::string tag = workerTag;
            pool->cancel([tag](const std::string& t) { return t == tag; });
        }
    }
    workerTag.clear();
    workerDocument.clear();
}

void TaskCheckGeometryResults::buildShapeContent(App::DocumentObject *pObject, const QString &baseName, const TopoDS_Shape &shape)
{

    bool advancedShapeContent = PartParams::getCheckGeometryAdvancedShapeContent();
    int decimals = static_cast<int>(App::UnitsParams::getDecimals());
    std::ostringstream stream;
    if (!shapeContentString.empty())
        stream << std::endl << std::endl;
    stream << tr("Checked object").toStdString() << ": ";
    Base::PyGILStateLocker lock;
    try {
        PyObject* module = PyImport_ImportModule("BasicShapes.ShapeContent");
        if (!module) {
            throw Py::Exception();
        }
        Py::Tuple args(3);
        args.setItem(0, Py::asObject(pObject->getPyObject()));
        args.setItem(1, Py::Long(decimals));
        args.setItem(2, Py::Boolean(advancedShapeContent));
        Py::Module shapecontent(module, true);
        Py::String result(shapecontent.callMemberFunction("buildShapeContent", args));
        stream << result.as_std_string("utf-8");
    }
    catch (Base::PyException&) { //script had runtime error so fall back on OCCT method
        stream << baseName.toUtf8().constData() << std::endl;
        BRepTools_ShapeSet set;
        set.Add(shape);
        set.DumpExtent(stream);
    }
    shapeContentString += stream.str();
}

QString TaskCheckGeometryResults::getShapeContentString()
{
  return QString::fromStdString(shapeContentString);
}

void TaskCheckGeometryResults::dispatchError(ResultEntry *entry, const BRepCheck_Status &stat)
{
    std::vector<FunctionMapType>::iterator mapIt;
    for (mapIt = functionMap.begin(); mapIt != functionMap.end(); ++mapIt)
    {
        if (std::get<0>(*mapIt) == entry->shape.ShapeType() && std::get<1>(*mapIt) == stat)
        {
            (std::get<2>(*mapIt))(entry);
            return;
        }
    }
    goSetupResultBoundingBox(entry);
    ParameterGrp::handle group = App::GetApplication().GetUserParameter().
    GetGroup("BaseApp")->GetGroup("Preferences")->GetGroup("Mod")->GetGroup("Part")->GetGroup("CheckGeometry");
    bool logErrors = PartParams::getCheckGeometryLogErrors(); //log errors to report view

    /*log BRepCheck errors to report view*/
    if (logErrors){
        std::clog << entry->parent->name.toStdString().c_str() << " : "
                  << entry->name.toStdString().c_str() << " : "
                  << entry->type.toStdString().c_str() << " : "
                  << entry->error.toStdString().c_str() << " (BRepCheck)"
                  << std::endl;
    }
}

void TaskCheckGeometryResults::setupFunctionMap()
{
    functionMap.emplace_back(TopAbs_SHELL, BRepCheck_NotClosed, goSetupResultShellNotClosed);
    functionMap.emplace_back(TopAbs_WIRE, BRepCheck_NotClosed, goSetupResultWireNotClosed);
    functionMap.emplace_back(TopAbs_VERTEX, BRepCheck_InvalidPointOnCurve, goSetupResultInvalidPointCurve);
    functionMap.emplace_back(TopAbs_FACE, BRepCheck_IntersectingWires, goSetupResultIntersectingWires);
    functionMap.emplace_back(TopAbs_EDGE, BRepCheck_InvalidCurveOnSurface, goSetupResultInvalidCurveSurface);
    functionMap.emplace_back(TopAbs_EDGE, BRepCheck_InvalidSameParameterFlag, goSetupResultInvalidSameParameterFlag);
    functionMap.emplace_back(TopAbs_FACE, BRepCheck_UnorientableShape, goSetupResultUnorientableShapeFace);
}

void TaskCheckGeometryResults::currentRowChanged (const QModelIndex &current, const QModelIndex &previous)
{
    Gui::Selection().clearSelection();
    if (previous.isValid())
    {
        ResultEntry *entry = model->getEntry(previous);
        if (entry)
        {
            if (entry->boxSwitch)
                entry->boxSwitch->whichChild.setValue(SO_SWITCH_NONE);
        }
    }
    if (current.isValid())
    {
        ResultEntry *entry = model->getEntry(current);
        if (entry)
        {
            if (entry->boxSwitch)
                entry->boxSwitch->whichChild.setValue(0);
            QStringList::Iterator stringIt;
            for (stringIt = entry->selectionStrings.begin(); stringIt != entry->selectionStrings.end(); ++stringIt)
            {
                //need unique delimiter.
                QString doc, object, sub;
                if (!this->split((*stringIt), doc, object, sub))
                    continue;
                Gui::Selection().addSelection(doc.toUtf8(), object.toUtf8(), sub.toUtf8());
            }
        }
    }
}

bool TaskCheckGeometryResults::split(QString &input, QString &doc, QString &object, QString &sub)
{
    QStringList strings = input.split(QStringLiteral("."));
    if (strings.size() != 3)
        return false;
    doc = strings.at(0);
    object = strings.at(1);
    sub = strings.at(2);
    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////

QString PartGui::buildSelectionName(const ResultEntry *entry, const TopoDS_Shape &shape)
{
    const ResultEntry *parentEntry = entry;
    while(parentEntry->parent)
    {
        ResultEntry *temp = parentEntry->parent;
        if (!temp->parent)
          break;
        parentEntry = parentEntry->parent;
    }

    QString stringOut;
    QTextStream stream(&stringOut);
    stream << parentEntry->name;
    stream << '.';
    TopTools_IndexedMapOfShape shapeMap;
    int index(-1);

    switch (shape.ShapeType())
    {
    case TopAbs_FACE:
        TopExp::MapShapes(parentEntry->shape, TopAbs_FACE, shapeMap);
        stream << "Face";
        break;
    case TopAbs_EDGE:
        TopExp::MapShapes(parentEntry->shape, TopAbs_EDGE, shapeMap);
        stream << "Edge";
        break;
    case TopAbs_VERTEX:
        TopExp::MapShapes(parentEntry->shape, TopAbs_VERTEX, shapeMap);
        stream << "Vertex";
        break;
    default:
        stream << "Unexpected shape type";
        break;
    }

    index = shapeMap.FindIndex(shape);
    stream << index;
    return stringOut;
}

void PartGui::goSetupResultTypedSelection(ResultEntry* entry, const TopoDS_Shape& shape, TopAbs_ShapeEnum type)
{
    TopExp_Explorer it;
    for (it.Init(shape, type); it.More(); it.Next())
    {
        QString name = buildSelectionName(entry, (it.Current()));
        if (!name.isEmpty())
            entry->selectionStrings.append(name);
    }
}

void PartGui::goSetupResultBoundingBox(ResultEntry *entry)
{
    //empty compound throws bounding box error. Mantis #0001426
    try
    {
      Bnd_Box boundingBox;
      BRepBndLib::Add(entry->shape, boundingBox);
      Standard_Real xmin, ymin, zmin, xmax, ymax, zmax;
      boundingBox.Get(xmin, ymin, zmin, xmax, ymax, zmax);
      SbVec3f boundCenter((xmax - xmin)/2 + xmin, (ymax - ymin)/2 + ymin, (zmax - zmin)/2 + zmin);

      entry->boxSep = new SoSeparator();
      entry->viewProviderRoot->addChild(entry->boxSep);
      entry->boxSwitch = new SoSwitch();
      entry->boxSep->addChild(entry->boxSwitch);
      SoGroup *group = new SoGroup();
      entry->boxSwitch->addChild(group);
      entry->boxSwitch->whichChild.setValue(SO_SWITCH_NONE);
      SoDrawStyle *dStyle = new SoDrawStyle();
      dStyle->style.setValue(SoDrawStyle::LINES);
      dStyle->linePattern.setValue(0xc0c0);
      group->addChild(dStyle);
      SoMaterial *material = new SoMaterial();
      material->diffuseColor.setValue(255.0, 255.0, 255.0);
      material->ambientColor.setValue(255.0, 255.0, 255.0);
      group->addChild(material);

      SoResetTransform *reset = new SoResetTransform();
      group->addChild(reset);

      SoTransform *position = new SoTransform();
      position->translation.setValue(boundCenter);
      group->addChild(position);

      SoCube *cube = new SoCube();
      cube->width.setValue(xmax - xmin);
      cube->height.setValue(ymax - ymin);
      cube->depth.setValue(zmax - zmin);
      group->addChild(cube);
    }
    catch (const Standard_Failure &){}
}

void PartGui::goSetupResultShellNotClosed(ResultEntry *entry)
{
    ShapeAnalysis_FreeBounds shellCheck(entry->shape);
    TopoDS_Compound closedWires = shellCheck.GetClosedWires();
    TopoDS_Compound openWires = shellCheck.GetOpenWires();

    goSetupResultTypedSelection(entry, closedWires, TopAbs_EDGE);
    goSetupResultTypedSelection(entry, openWires, TopAbs_EDGE);

    goSetupResultBoundingBox(entry);
}

void PartGui::goSetupResultWireNotClosed(ResultEntry *entry)
{
    goSetupResultTypedSelection(entry, entry->shape, TopAbs_EDGE);
    goSetupResultBoundingBox(entry);
}

void PartGui::goSetupResultInvalidPointCurve(ResultEntry *entry)
{
    goSetupResultTypedSelection(entry, entry->shape, TopAbs_VERTEX);
    goSetupResultBoundingBox(entry);
}

void PartGui::goSetupResultIntersectingWires(ResultEntry *entry)
{
    goSetupResultTypedSelection(entry, entry->shape, TopAbs_FACE);
    goSetupResultBoundingBox(entry);
}

void PartGui::goSetupResultInvalidCurveSurface(ResultEntry *entry)
{
    goSetupResultTypedSelection(entry, entry->shape, TopAbs_EDGE);
    goSetupResultBoundingBox(entry);
}

void PartGui::goSetupResultInvalidSameParameterFlag(ResultEntry *entry)
{
    goSetupResultTypedSelection(entry, entry->shape, TopAbs_EDGE);
    goSetupResultBoundingBox(entry);
}

void PartGui::goSetupResultUnorientableShapeFace(ResultEntry *entry)
{
    goSetupResultTypedSelection(entry, entry->shape, TopAbs_FACE);
    goSetupResultBoundingBox(entry);
}

////////////////////////////////////////////////////////////////////////////////////////////////

TaskCheckGeometryDialog::TaskCheckGeometryDialog()
    : widget(nullptr), contentLabel(nullptr), okBtn(nullptr), settingsBtn(nullptr), resultsBtn(nullptr)
{
    ParameterGrp::handle group = App::GetApplication().GetUserParameter().
    GetGroup("BaseApp")->GetGroup("Preferences")->GetGroup("Mod")->GetGroup("Part")->GetGroup("CheckGeometry");
    bool expandShapeContent = PartParams::getCheckGeometryExpandShapeContent();

    this->setButtonPosition(TaskDialog::South);
    widget = new TaskCheckGeometryResults();

    taskbox = new Gui::TaskView::TaskBox(
        Gui::BitmapFactory().pixmap("Part_CheckGeometry"),
        widget->windowTitle(), true, nullptr);
    taskbox->groupLayout()->addWidget(widget);
    Content.push_back(taskbox);

    contentLabel = new QTextEdit();
    contentLabel->setText(widget->getShapeContentString());
    shapeContentBox = new Gui::TaskView::TaskBox(Gui::BitmapFactory().pixmap("Part_CheckGeometry"),
        tr("Shape Content"), true, nullptr);
    shapeContentBox->groupLayout()->addWidget(contentLabel);
    if (!expandShapeContent){
        shapeContentBox->hideGroupBox();
    }
    Content.push_back(shapeContentBox);

    settingsBox = new Gui::TaskView::TaskBox(Gui::BitmapFactory().pixmap("Part_CheckGeometry"),
        tr("Settings"), true, nullptr);
    Content.push_back(settingsBox);

    autoRunCheckBox = new QCheckBox();
    autoRunCheckBox->setText(tr("Skip settings page"));
    autoRunCheckBox->setToolTip(
        tr("Skip this settings page and run the geometry check automatically.")
        + QStringLiteral("\n")
        + tr("Default: false"));
    autoRunCheckBox->setChecked(PartParams::getCheckGeometryAutoRun());
    connect(autoRunCheckBox, &QCheckBox::toggled,
            this, &TaskCheckGeometryDialog::onAutoRunCheckBoxToggled);
    settingsBox->groupLayout()->addWidget(autoRunCheckBox);

    runBOPCheckBox = new QCheckBox();
    runBOPCheckBox->setText(tr("Run boolean operation check"));
    runBOPCheckBox->setToolTip(tr(
        "Extra boolean operations check that can sometimes find errors that\n"
        "the standard BRep geometry check misses. These errors do not always\n"
        "mean the checked object is unusable.  Default: false"));
    runBOPCheckBox->setChecked(PartParams::getCheckGeometryRunBOPCheck());
    connect(runBOPCheckBox, &QCheckBox::toggled,
            this, &TaskCheckGeometryDialog::onRunBOPCheckBoxToggled);
    settingsBox->groupLayout()->addWidget(runBOPCheckBox);

    runSingleThreadedCheckBox = new QCheckBox();
    runSingleThreadedCheckBox->setText(tr("Single-threaded"));
    runSingleThreadedCheckBox->setToolTip(tr(
        "Run the geometry check in a single thread.  This is slower,\n"
        "but more stable.  Default: false"));
    runSingleThreadedCheckBox->setChecked(PartParams::getCheckGeometryRunSingleThreaded());
    connect(runSingleThreadedCheckBox, &QCheckBox::toggled,
            this, &TaskCheckGeometryDialog::onRunSingleThreadedCheckBoxToggled);
    settingsBox->groupLayout()->addWidget(runSingleThreadedCheckBox);

    logErrorsCheckBox = new QCheckBox();
    logErrorsCheckBox->setText(tr("Log errors"));
    logErrorsCheckBox->setToolTip(tr("Log errors to report view.  Default: true"));
    logErrorsCheckBox->setChecked(PartParams::getCheckGeometryLogErrors());
    connect(logErrorsCheckBox, &QCheckBox::toggled,
            this, &TaskCheckGeometryDialog::onLogErrorsCheckBoxToggled);
    settingsBox->groupLayout()->addWidget(logErrorsCheckBox);

    expandShapeContentCheckBox = new QCheckBox();
    expandShapeContentCheckBox->setText(tr("Expand shape content"));
    expandShapeContentCheckBox->setToolTip(tr(
        "Expand shape content.  Changes will take effect next time you use \n"
        "the check geometry tool.  Default: false"));
    expandShapeContentCheckBox->setChecked(PartParams::getCheckGeometryExpandShapeContent());
    connect(expandShapeContentCheckBox, &QCheckBox::toggled,
            this, &TaskCheckGeometryDialog::onExpandShapeContentCheckBoxToggled);
    settingsBox->groupLayout()->addWidget(expandShapeContentCheckBox);

    advancedShapeContentCheckBox = new QCheckBox();
    advancedShapeContentCheckBox->setText(tr("Advanced shape content"));
    advancedShapeContentCheckBox->setToolTip(tr(
        "Show advanced shape content.  Changes will take effect next time you use \n"
        "the check geometry tool.  Default: false"));
    advancedShapeContentCheckBox->setChecked(PartParams::getCheckGeometryAdvancedShapeContent());
    connect(advancedShapeContentCheckBox, &QCheckBox::toggled,
            this, &TaskCheckGeometryDialog::onAdvancedShapeContentCheckBoxToggled);
    settingsBox->groupLayout()->addWidget(advancedShapeContentCheckBox);

    settingsBox->groupLayout()->addWidget(new QLabel(tr("\nIndividual boolean operation checks:")));

    argumentTypeModeCheckBox = new QCheckBox();
    argumentTypeModeCheckBox->setText(QStringLiteral("  ") + tr("Bad type"));
    argumentTypeModeCheckBox->setToolTip(tr("Check for bad argument types.  Default: true"));
    argumentTypeModeCheckBox->setChecked(PartParams::getCheckGeometryArgumentTypeMode());
    connect(argumentTypeModeCheckBox, &QCheckBox::toggled,
            this, &TaskCheckGeometryDialog::onArgumentTypeModeCheckBoxToggled);
    settingsBox->groupLayout()->addWidget(argumentTypeModeCheckBox);

    selfInterModeCheckBox = new QCheckBox();
    selfInterModeCheckBox->setText(QStringLiteral("  ") + tr("Self-intersect"));
    selfInterModeCheckBox->setToolTip(tr("Check for self-intersections.  Default: true"));
    selfInterModeCheckBox->setChecked(PartParams::getCheckGeometrySelfInterMode());
    connect(selfInterModeCheckBox, &QCheckBox::toggled,
            this, &TaskCheckGeometryDialog::onSelfInterModeCheckBoxToggled);
    settingsBox->groupLayout()->addWidget(selfInterModeCheckBox);

    smallEdgeModeCheckBox = new QCheckBox();
    smallEdgeModeCheckBox->setText(QStringLiteral("  ") + tr("Too small edge"));
    smallEdgeModeCheckBox->setToolTip(tr("Check for edges that are too small.  Default: true"));
    smallEdgeModeCheckBox->setChecked(PartParams::getCheckGeometrySmallEdgeMode());
    connect(smallEdgeModeCheckBox, &QCheckBox::toggled,
            this, &TaskCheckGeometryDialog::onSmallEdgeModeCheckBoxToggled);
    settingsBox->groupLayout()->addWidget(smallEdgeModeCheckBox);

    rebuildFaceModeCheckBox = new QCheckBox();
    rebuildFaceModeCheckBox->setText(QStringLiteral("  ") + tr("Nonrecoverable face"));
    rebuildFaceModeCheckBox->setToolTip(tr("Check for nonrecoverable faces.  Default: true"));
    rebuildFaceModeCheckBox->setChecked(PartParams::getCheckGeometryRebuildFaceMode());
    connect(rebuildFaceModeCheckBox, &QCheckBox::toggled,
            this, &TaskCheckGeometryDialog::onRebuildFaceModeCheckBoxToggled);
    settingsBox->groupLayout()->addWidget(rebuildFaceModeCheckBox);

    continuityModeCheckBox = new QCheckBox();
    continuityModeCheckBox->setText(QStringLiteral("  ") + tr("Continuity"));
    continuityModeCheckBox->setToolTip(tr("Check for continuity.  Default: true"));
    continuityModeCheckBox->setChecked(PartParams::getCheckGeometryContinuityMode());
    connect(continuityModeCheckBox, &QCheckBox::toggled,
            this, &TaskCheckGeometryDialog::onContinuityModeCheckBoxToggled);
    settingsBox->groupLayout()->addWidget(continuityModeCheckBox);

    tangentModeCheckBox = new QCheckBox();
    tangentModeCheckBox->setText(QStringLiteral("  ") + tr("Incompatibility of face"));
    tangentModeCheckBox->setToolTip(tr("Check for incompatible faces.  Default: true"));
    tangentModeCheckBox->setChecked(PartParams::getCheckGeometryTangentMode());
    connect(tangentModeCheckBox, &QCheckBox::toggled,
            this, &TaskCheckGeometryDialog::onTangentModeCheckBoxToggled);
    settingsBox->groupLayout()->addWidget(tangentModeCheckBox);

    mergeVertexModeCheckBox = new QCheckBox();
    mergeVertexModeCheckBox->setText(QStringLiteral("  ") + tr("Incompatibility of vertex"));
    mergeVertexModeCheckBox->setToolTip(tr("Check for incompatible vertices.  Default: true"));
    mergeVertexModeCheckBox->setChecked(PartParams::getCheckGeometryMergeVertexMode());
    connect(mergeVertexModeCheckBox, &QCheckBox::toggled,
            this, &TaskCheckGeometryDialog::onMergeVertexModeCheckBoxToggled);
    settingsBox->groupLayout()->addWidget(mergeVertexModeCheckBox);

    mergeEdgeModeCheckBox = new QCheckBox();
    mergeEdgeModeCheckBox->setText(QStringLiteral("  ") + tr("Incompatibility of edge"));
    mergeEdgeModeCheckBox->setToolTip(tr("Check for incompatible edges.  Default: true"));
    mergeEdgeModeCheckBox->setChecked(PartParams::getCheckGeometryMergeEdgeMode());
    connect(mergeEdgeModeCheckBox, &QCheckBox::toggled,
            this, &TaskCheckGeometryDialog::onMergeEdgeModeCheckBoxToggled);
    settingsBox->groupLayout()->addWidget(mergeEdgeModeCheckBox);

    curveOnSurfaceModeCheckBox = new QCheckBox();
    curveOnSurfaceModeCheckBox->setText(QStringLiteral("  ") + tr("Invalid curve on surface"));
    curveOnSurfaceModeCheckBox->setToolTip(tr("Check for invalid curves on surfaces.  Default: true"));
    curveOnSurfaceModeCheckBox->setChecked(PartParams::getCheckGeometryCurveOnSurfaceMode());
    connect(curveOnSurfaceModeCheckBox, &QCheckBox::toggled,
            this, &TaskCheckGeometryDialog::onCurveOnSurfaceModeCheckBoxToggled);
    settingsBox->groupLayout()->addWidget(curveOnSurfaceModeCheckBox);
    if (PartParams::getCheckGeometryAutoRun()){
        settingsBox->hide();
        widget->goCheck();
        contentLabel->setText(widget->getShapeContentString());
    } else {
        taskbox->hide();
        shapeContentBox->hide();
    }
}

bool TaskCheckGeometryDialog::accept()
{
    settingsBtn->setEnabled(true);
    settingsBox->hide();
    shapeContentBox->show();
    taskbox->show();
    widget->goCheck();
    QScrollBar *v = contentLabel->verticalScrollBar();
    v->setValue(v->maximum()); //scroll to bottom
    int curval = v->value(); //save position
    contentLabel->setText(widget->getShapeContentString());
    v->setValue(curval+(v->maximum()-curval)/5);
    return false;
}

bool TaskCheckGeometryDialog::reject()
{
    return true;
}

void TaskCheckGeometryDialog::onClicked(QAbstractButton *btn)
{
    /** when ok (run check) is clicked or when close is clicked
     *  the appropriate accept() or reject() is called already
     *  all we need to do here is enable / disable / show / hide
     *  ui elements
     */

    if(btn == okBtn){
        settingsBtn->setEnabled(true);
    } else if (btn == settingsBtn){
        settingsBtn->setEnabled(false);
        taskbox->hide();
        shapeContentBox->hide();
        settingsBox->show();
        resultsBtn->setEnabled(true);
    } else if (btn == resultsBtn){
        settingsBtn->setEnabled(true);
        taskbox->show();
        shapeContentBox->show();
        settingsBox->hide();
        resultsBtn->setEnabled(false);
    }
}

void TaskCheckGeometryDialog::modifyStandardButtons(QDialogButtonBox* box)
{
    okBtn = box->button(QDialogButtonBox::Ok);
    okBtn->setText(tr("Run check"));
    settingsBtn = box->addButton(tr("Settings"),QDialogButtonBox::ActionRole);
    ParameterGrp::handle group = App::GetApplication().GetUserParameter().
    GetGroup("BaseApp")->GetGroup("Preferences")->GetGroup("Mod")->GetGroup("Part")->GetGroup("CheckGeometry");
    if(!PartParams::getCheckGeometryAutoRun())
        settingsBtn->setEnabled(false);
    resultsBtn = box->addButton(tr("Results"),QDialogButtonBox::ActionRole);
    resultsBtn->setEnabled(false);
    connect(box, &QDialogButtonBox::clicked, this, &TaskCheckGeometryDialog::onClicked);
}

void TaskCheckGeometryDialog::onAutoRunCheckBoxToggled(bool isOn)
{
    ParameterGrp::handle group = App::GetApplication().GetUserParameter().
    GetGroup("BaseApp")->GetGroup("Preferences")->GetGroup("Mod")->GetGroup("Part")->GetGroup("CheckGeometry");
    PartParams::setCheckGeometryAutoRun(isOn);
}

void TaskCheckGeometryDialog::onRunBOPCheckBoxToggled(bool isOn)
{
    ParameterGrp::handle group = App::GetApplication().GetUserParameter().
    GetGroup("BaseApp")->GetGroup("Preferences")->GetGroup("Mod")->GetGroup("Part")->GetGroup("CheckGeometry");
    PartParams::setCheckGeometryRunBOPCheck(isOn);
}

void TaskCheckGeometryDialog::onRunSingleThreadedCheckBoxToggled(bool isOn)
{
    ParameterGrp::handle group = App::GetApplication().GetUserParameter().
    GetGroup("BaseApp")->GetGroup("Preferences")->GetGroup("Mod")->GetGroup("Part")->GetGroup("CheckGeometry");
    PartParams::setCheckGeometryRunSingleThreaded(isOn);
}

void TaskCheckGeometryDialog::onLogErrorsCheckBoxToggled(bool isOn)
{
    ParameterGrp::handle group = App::GetApplication().GetUserParameter().
    GetGroup("BaseApp")->GetGroup("Preferences")->GetGroup("Mod")->GetGroup("Part")->GetGroup("CheckGeometry");
    PartParams::setCheckGeometryLogErrors(isOn);
}

void TaskCheckGeometryDialog::onArgumentTypeModeCheckBoxToggled(bool isOn)
{
    ParameterGrp::handle group = App::GetApplication().GetUserParameter().
    GetGroup("BaseApp")->GetGroup("Preferences")->GetGroup("Mod")->GetGroup("Part")->GetGroup("CheckGeometry");
    PartParams::setCheckGeometryArgumentTypeMode(isOn);
}

void TaskCheckGeometryDialog::onExpandShapeContentCheckBoxToggled(bool isOn)
{
    ParameterGrp::handle group = App::GetApplication().GetUserParameter().
    GetGroup("BaseApp")->GetGroup("Preferences")->GetGroup("Mod")->GetGroup("Part")->GetGroup("CheckGeometry");
    PartParams::setCheckGeometryExpandShapeContent(isOn);
}

void TaskCheckGeometryDialog::onAdvancedShapeContentCheckBoxToggled(bool isOn)
{
    ParameterGrp::handle group = App::GetApplication().GetUserParameter().
    GetGroup("BaseApp")->GetGroup("Preferences")->GetGroup("Mod")->GetGroup("Part")->GetGroup("CheckGeometry");
    PartParams::setCheckGeometryAdvancedShapeContent(isOn);
}

void TaskCheckGeometryDialog::onSelfInterModeCheckBoxToggled(bool isOn)
{
    ParameterGrp::handle group = App::GetApplication().GetUserParameter().
    GetGroup("BaseApp")->GetGroup("Preferences")->GetGroup("Mod")->GetGroup("Part")->GetGroup("CheckGeometry");
    PartParams::setCheckGeometrySelfInterMode(isOn);
}

void TaskCheckGeometryDialog::onSmallEdgeModeCheckBoxToggled(bool isOn)
{
    ParameterGrp::handle group = App::GetApplication().GetUserParameter().
    GetGroup("BaseApp")->GetGroup("Preferences")->GetGroup("Mod")->GetGroup("Part")->GetGroup("CheckGeometry");
    PartParams::setCheckGeometrySmallEdgeMode(isOn);
}

void TaskCheckGeometryDialog::onRebuildFaceModeCheckBoxToggled(bool isOn)
{
    ParameterGrp::handle group = App::GetApplication().GetUserParameter().
    GetGroup("BaseApp")->GetGroup("Preferences")->GetGroup("Mod")->GetGroup("Part")->GetGroup("CheckGeometry");
    PartParams::setCheckGeometryRebuildFaceMode(isOn);
}

void TaskCheckGeometryDialog::onContinuityModeCheckBoxToggled(bool isOn)
{
    ParameterGrp::handle group = App::GetApplication().GetUserParameter().
    GetGroup("BaseApp")->GetGroup("Preferences")->GetGroup("Mod")->GetGroup("Part")->GetGroup("CheckGeometry");
    PartParams::setCheckGeometryContinuityMode(isOn);
}

void TaskCheckGeometryDialog::onTangentModeCheckBoxToggled(bool isOn)
{
    ParameterGrp::handle group = App::GetApplication().GetUserParameter().
    GetGroup("BaseApp")->GetGroup("Preferences")->GetGroup("Mod")->GetGroup("Part")->GetGroup("CheckGeometry");
    PartParams::setCheckGeometryTangentMode(isOn);
}

void TaskCheckGeometryDialog::onMergeVertexModeCheckBoxToggled(bool isOn)
{
    ParameterGrp::handle group = App::GetApplication().GetUserParameter().
    GetGroup("BaseApp")->GetGroup("Preferences")->GetGroup("Mod")->GetGroup("Part")->GetGroup("CheckGeometry");
    PartParams::setCheckGeometryMergeVertexMode(isOn);
}

void TaskCheckGeometryDialog::onMergeEdgeModeCheckBoxToggled(bool isOn)
{
    ParameterGrp::handle group = App::GetApplication().GetUserParameter().
    GetGroup("BaseApp")->GetGroup("Preferences")->GetGroup("Mod")->GetGroup("Part")->GetGroup("CheckGeometry");
    PartParams::setCheckGeometryMergeEdgeMode(isOn);
}

void TaskCheckGeometryDialog::onCurveOnSurfaceModeCheckBoxToggled(bool isOn)
{
    ParameterGrp::handle group = App::GetApplication().GetUserParameter().
    GetGroup("BaseApp")->GetGroup("Preferences")->GetGroup("Mod")->GetGroup("Part")->GetGroup("CheckGeometry");
    PartParams::setCheckGeometryCurveOnSurfaceMode(isOn);
}

TaskCheckGeometryDialog::~TaskCheckGeometryDialog()
{
  if (widget)
  {
    delete widget;
    widget = nullptr;
  }
  if (contentLabel)
  {
    delete contentLabel;
    contentLabel = nullptr;
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////

BOPProgressIndicator::BOPProgressIndicator (const QString& title, QWidget* parent)
{
    steps = 0;
    canceled = false;

    myProgress = new QProgressDialog(parent);
    myProgress->setWindowTitle(title);
    myProgress->setAttribute(Qt::WA_DeleteOnClose);
}

BOPProgressIndicator::~BOPProgressIndicator ()
{
    myProgress->close();
}

#if OCC_VERSION_HEX < 0x070500
Standard_Boolean BOPProgressIndicator::Show (const Standard_Boolean theForce)
{
    if (theForce) {
        steps = 0;
        canceled = false;

        time.start();
        myProgress->show();

        myProgress->setRange(0, 0);
        myProgress->setValue(0);
    }
    else {
        Handle(TCollection_HAsciiString) aName = GetScope(1).GetName(); //current step
        if (!aName.IsNull())
            myProgress->setLabelText (QString::fromUtf8(aName->ToCString()));
    }

    return Standard_True;
}
#else
void BOPProgressIndicator::Show (const Message_ProgressScope& theScope,
                                 const Standard_Boolean isForce)
{
    Standard_CString aName = theScope.Name(); //current step
    myProgress->setLabelText (QString::fromUtf8(aName));

    if (isForce) {
        myProgress->show();
    }

    QCoreApplication::processEvents();
}

void BOPProgressIndicator::Reset()
{
    steps = 0;
    canceled = false;

    time.start();

    myProgress->setRange(0, 0);
    myProgress->setValue(0);
}
#endif

Standard_Boolean BOPProgressIndicator::UserBreak()
{
    QThread *currentThread = QThread::currentThread();
    if (currentThread == myProgress->thread()) {
        // this is needed to check the status outside BOPAlgo_ArgumentAnalyzer
        //
        // Hint: We must make sure to do this only when calling from the GUI
        // thread because when calling it from a worker thread the thrown
        // exception isn't handled anywhere and thus std::terminate is called
        if (canceled)
            return Standard_True;

        // it suffices to update only every second
        // to avoid to unnecessarily process events
        steps++;
        myProgress->setValue(steps);
        if (time.elapsed() > 1000) {
            time.restart();
            QCoreApplication::processEvents();

            canceled = myProgress->wasCanceled();
            return canceled;
        }
    }

    return Standard_False;
}

#include "moc_TaskCheckGeometry.cpp"
