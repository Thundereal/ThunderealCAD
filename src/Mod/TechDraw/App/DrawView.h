/***************************************************************************
 *   Copyright (c) 2007 Jürgen Riegel <juergen.riegel@web.de>              *
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

#ifndef DrawView_h_
#define DrawView_h_

#include <fastsignals/signal.h>
#include <QCoreApplication>
#include <QRectF>

#include <App/DocumentObject.h>
#include <App/FeaturePython.h>
#include <App/PropertyLinks.h>
#include <App/PropertyUnits.h>
#include <Mod/TechDraw/TechDrawGlobal.h>


namespace TechDraw
{

class DrawPage;
class DrawViewClip;
class DrawViewCollection;
class DrawLeaderLine;
/*class CosmeticVertex;*/

/** Base class of all View Features in the drawing module
 */
class TechDrawExport DrawView : public App::DocumentObject
{
    Q_DECLARE_TR_FUNCTIONS(TechDraw::DrawView)
    PROPERTY_HEADER_WITH_OVERRIDE(TechDraw::DrawView);

public:
    /// Constructor
    DrawView();
    ~DrawView() override;

    App::PropertyDistance X;
    App::PropertyDistance Y;
    App::PropertyBool LockPosition;
    App::PropertyFloatConstraint Scale;

    App::PropertyEnumeration ScaleType;
    App::PropertyAngle Rotation;
    App::PropertyString Caption;

    /** @name methods override Feature */
    //@{
    /// recalculate the Feature
    App::DocumentObjectExecReturn *execute() override;
    void onDocumentRestored() override;
    short mustExecute() const override;
    //@}
    void handleChangedPropertyType(
        Base::XMLReader &reader, const char * TypeName, App::Property * prop) override;
    void Restore(Base::XMLReader& reader) override;

    bool isInClip();
    DrawViewClip* getClipGroup();

    //! the link naming the view that owns this one, for the types that have such a link.
    //! Overriding it is all a view type needs to do to be drawn inside its owner.
    virtual App::PropertyLink* getOwnerProperty() { return nullptr; }
    //! the collection this view is a member of, if any
    DrawViewCollection* getCollection() const;
    //! the view this one is drawn inside: its owner if it has one, otherwise the
    //! collection it belongs to. Answered from the document, so it does not depend on
    //! anything having been drawn yet.
    DrawView* claimParent() const;

    /// returns the type name of the ViewProvider
    const char* getViewProviderName() const override {
        return "TechDrawGui::ViewProviderDrawingView";
    }
    //return PyObject as DrawViewPy
    PyObject *getPyObject() override;

    virtual DrawPage* findParentPage() const;
    virtual std::vector<DrawPage*> findAllParentPages() const;
    virtual int countParentPages() const;
    virtual QRectF getRect() const;                      //must be overridden by derived class
    QRectF getRectAligned() const;
    virtual double autoScale() const;
    virtual double autoScale(double w, double h) const;
    virtual bool checkFit() const;
    virtual bool checkFit(DrawPage*) const;
    virtual void setPosition(double x, double y, bool force = false);
    virtual bool keepUpdated(void);

    fastsignals::signal<void (const DrawView*)> signalGuiPaint;
    void requestPaint(void);

    virtual double getScale(void) const;
    void checkScale(void);

    virtual void handleXYLock(void);
    virtual bool isLocked(void) const;
    virtual bool showLock(void) const;

    std::vector<TechDraw::DrawLeaderLine*> getLeaders(void) const;

    void setScaleAttribute();

    void overrideKeepUpdated(bool s) { m_overrideKeepUpdated = s; }
    bool overrideKeepUpdated(void) { return m_overrideKeepUpdated; }

    /** @name A failure of work done after execute() returned
     * (docs/TechDrawErrors.md 3.1). A view's projection, faces, section or
     * detail cut run on worker threads, and execute() has returned success
     * by the time they fail. Called on the GUI thread once the worker is
     * done. An error is the object's recompute error, as if execute() had
     * returned it; a warning -- a result usable but partial -- leaves the
     * object valid and shows in its tool tip. Both go at the next recompute.
     * An error reported while the object is in its own recompute -- a
     * failure caught there rather than thrown -- is that recompute's.
     */
    //@{
    void reportLateError(const std::string& why);
    void reportLateWarning(const std::string& why);
    /// The warnings since the last recompute, one per line
    const std::string& getLateWarnings() const { return m_lateWarnings; }
    App::DocumentObjectExecReturn* recompute() override;
    //@}

    void translateLabel(std::string context, std::string baseName, std::string uniqueName);

protected:
    void onChanged(const App::Property* prop) override;
    virtual void validateScale();
    std::string pageFeatName;
    bool autoPos;
    bool mouseMove;

    int prefScaleType();
    double prefScale();

private:
    static const char* ScaleTypeEnums[];
    static App::PropertyFloatConstraint::Constraints scaleRange;

    bool m_overrideKeepUpdated;
    std::string m_lateWarnings;
    /// reportLateError() from inside this object's own recompute: returned
    /// as its error when the recompute ends
    std::string m_heldError;
    bool m_inRecompute = false;
};

using DrawViewPython = App::FeaturePythonT<DrawView>;

} //namespace TechDraw

#endif
