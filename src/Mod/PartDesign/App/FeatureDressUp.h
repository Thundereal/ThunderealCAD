/***************************************************************************
 *   Copyright (c) 2010 Juergen Riegel <FreeCAD@juergen-riegel.net>        *
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


#ifndef PARTDESIGN_DressUp_H
#define PARTDESIGN_DressUp_H

#include <map>

#include "FeatureAddSub.h"

namespace PartDesign
{

class PartDesignExport DressUp : public PartDesign::FeatureAddSub
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesign::DressUp);

public:
    DressUp();

    /**
     * Base feature and it's subelements to which dressup operation will be applied to.
     * Unlike Feature::BaseFeature it includes Sublinks and set not only inside a body.
     * But for consistency if BaseFeature is nonzero this links to the same body as it.
     */
    App::PropertyLinkSub Base;
    App::PropertyBool SupportTransform;
    Part::PropertyPartShape   DressUpShape;
    /** The edges of the last recompute whose fillet or chamfer failed or
     * whose reference is gone, left out of the result (docs/HandsOnQueue.md
     * entry 143); not saved
     */
    App::PropertyStringList FailedEdges;
    /// The faces those edges' fillets or chamfers had when they last were
    /// made; not saved
    Part::PropertyPartShape FailedShape;

    short mustExecute() const override;
    /// updates the Placement property from the Placement of the BaseFeature
    void positionByBaseFeature();
    /**
     * Returns the BaseFeature property's object if it's set otherwise returns Base's
     * feature property object otherwise feature property's object (if any)
     * @param silent if couldn't determine the base feature and silent == true,
     *               silently return a nullptr, otherwise throw Base::Exception.
     *               Default is false.
     */
    Part::Feature* getBaseObject(bool silent=false) const override;
    /// extracts all edges from the subshapes (including face edges) and furthermore adds
    /// all C0 continuous edges to the vector
    /** The edges of Base in \a shape. A reference that is gone throws,
     * or, given \a missing, is added to it and left out (docs/HandsOnQueue.md
     * entry 143).
     */
    std::vector<TopoShape> getContinuousEdges(const TopoShape &shape,
                                              std::vector<std::string> *missing = nullptr);

    std::vector<TopoShape> getFaces(const TopoShape &shape);

    void getAddSubShape(std::vector<std::pair<Part::TopoShape, Type> > &shapes) override;

    static const std::string &addsubElementPrefix();

    void setPauseRecompute(bool) override {
    }

    void onBaseFeatureRerouted(App::DocumentObject* oldBase,
                               App::DocumentObject* newBase) override;

protected:
    void onChanged(const App::Property* prop) override;

    /** After a build that left out what failed: \a failedNames gets the
     * names of the edges left out -- \a missing, then the failed in
     * \a baseShape's names -- and FailedEdges and FailedShape are set; the
     * faces of the edges made are kept for when they fail later.
     */
    void setFailedEdges(const TopoShape &baseShape, const std::vector<TopoShape> &edges,
                        const Part::TopoShape::FilletPartial &partial,
                        const std::vector<std::string> &missing,
                        std::vector<std::string> &failedNames);
    /// FailedEdges set to \a names, FailedShape to their faces as last made
    void setFailedEdges(const std::vector<std::string> &names);
    /// The faces of \a names' dress-ups not made in this session, found in
    /// the result about to be replaced (docs/HandsOnQueue.md entry 160)
    void seedMadeFaces(const std::vector<std::string> &names);
    /** Where the dress-up failed on some edges (\a failed, indices into
     * \a edges): the slivers at them -- faces narrower than \a maxWidth, see
     * Part::TopoShape::makESliverFree -- taken out of \a baseShape, and
     * \a edges and \a extra replaced by what they are after it (an entry of
     * \a extra null where its shape went). \a removed gets the slivers'
     * names in the old \a baseShape. False, nothing changed, when none is
     * taken out or an edge does not come through whole.
     */
    bool removeSlivers(TopoShape &baseShape, std::vector<TopoShape> &edges,
                       const std::vector<int> &failed, double maxWidth,
                       std::vector<TopoDS_Shape> &extra,
                       std::vector<std::string> &removed) const;
    /** As removeSlivers(), with the features near the failed edges -- a
     * pocket or a boss the dress-up runs into, see
     * Part::TopoShape::makEFeatureFree -- each face within \a reach of one
     * bringing in its feature. \a pockets and \a bosses get what the
     * features took away and added, for putBackFeatures().
     */
    bool removeFeatures(TopoShape &baseShape, std::vector<TopoShape> &edges,
                        const std::vector<int> &failed, double reach,
                        std::vector<TopoDS_Shape> &extra,
                        std::vector<std::string> &removed,
                        TopoShape &pockets, TopoShape &bosses) const;
    /** \a shape with the features removeFeatures() took out put back:
     * \a pockets cut from it, \a bosses fused to it. Null when that fails,
     * changes its number of solids, or is not valid and clean by BOP's check.
     */
    TopoShape putBackFeatures(const TopoShape &shape, const TopoShape &pockets,
                              const TopoShape &bosses) const;
private:
    /// A removal adopted: \a edges and \a extra found in \a freed through
    /// \a history, \a removed named, \a baseShape replaced. False, nothing
    /// changed, when an edge does not come through whole.
    static bool adoptRemoval(TopoShape &baseShape, std::vector<TopoShape> &edges,
                             std::vector<TopoDS_Shape> &extra,
                             std::vector<std::string> &removed, const TopoShape &freed,
                             const opencascade::handle<BRepTools_History> &history,
                             const std::vector<TopoShape> &faces);
protected:
public:
    /** The faces \a edge's dress-up had when last made in this session, or
     * as the file's result had them for an edge missing since the open;
     * null for none. \a edge as Base names it, '?Edge3' for a missing one.
     */
    TopoDS_Shape getMadeFaces(const std::string &edge) const;

protected:
    /// The error of a result that left \a names out, nullptr for none
    static App::DocumentObjectExecReturn *failedEdgesError(const char *what,
                                                           const std::vector<std::string> &names);

private:
    /// The faces of each edge's dress-up as last made, by the edge's name
    std::map<std::string, TopoDS_Shape> madeFaces;
};

} //namespace PartDesign


#endif // PARTDESIGN_DressUp_H
