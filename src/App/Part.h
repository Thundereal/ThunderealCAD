/***************************************************************************
 *   Copyright (c) 2014 Jürgen Riegel <juergen.riegel@web.de>              *
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


#ifndef APP_Part_H
#define APP_Part_H

#include "GeoFeature.h"
#include "OriginGroupExtension.h"
#include "PropertyElementAppearance.h"
#include "PropertyLinks.h"
#include "PropertyStandard.h"
#include "PropertyAppearance.h"
#include "AuxGroup.h"


namespace App
{


/** Base class of all geometric document objects.
 */
class AppExport Part : public App::GeoFeature, public App::OriginGroupExtension,
                       public App::AuxGroupOwner
{
    PROPERTY_HEADER_WITH_EXTENSIONS(App::Part);

public:
    /// type of the part
    PropertyString Type;

    /** @name base properties of all Assembly Items
    * These properties correspond mostly to the meta information
    * in the App::Document class
    */
    //@{
    /// Id e.g. Part number
    App::PropertyString  Id;
    /// material descriptions
    App::PropertyLink    Material;
    /// Meta descriptions
    App::PropertyMap     Meta;

    /** License string
    * Holds the short license string for the Item, e.g. CC-BY
    * for the Creative Commons license suit.
    */
    App::PropertyString  License;
    /// License description/contract URL
    App::PropertyString  LicenseURL;
    //@}

    // Private property holding object link for color override
    App::PropertyLinkSubHidden ColoredElements;
    /** @name What the part lays over what it holds
     *
     * As a link does (App::LinkAppearance, docs/ShapeAppearanceDesign.md
     * sec 14.6.4): the store, whose names are paths, and the two names over
     * it the property editor shows. ColoredElements is a name over it too.
     */
    //@{
    App::PropertyElementAppearance ElementAppearance;
    App::PropertyBool OverrideMaterial;
    App::PropertyAppearanceList ShapeAppearance;
    //@}

    /** How an existing object is added (docs/AssemblyObjectModel.md sec 2)
     *
     * Normal moves it in. Auto moves it in, or adds an App::Link to it that
     * this part owns when it cannot be moved in: it is in another document,
     * another geometry group holds it or something its placement depends
     * on, or it is a child already. Link always adds an owned link.
     * Removing an owned link deletes it, whatever the mode.
     */
    App::PropertyEnumeration GroupMode;
    /// The group owning the elements this part declares (getElementsGroup())
    App::PropertyLink Elements;
    enum GroupModeValue {
        GroupModeNormal,
        GroupModeAuto,
        GroupModeLink,
    };

    /// Constructor
    Part();
    ~Part() override;

    /// returns the type name of the ViewProvider
    const char* getViewProviderName() const override {
        return "Gui::ViewProviderPart";
    }


    void handleChangedPropertyType(Base::XMLReader &reader, const char *TypeName, App::Property *prop) override;
    /// The `Uid` of a file from before sec 36 is the part's UID
    /// (docs/TransactionLogJournal.md sec 36.7)
    void handleChangedPropertyName(Base::XMLReader &reader, const char *TypeName, const char *PropName) override;
    bool hasUidAttribute() const override {return true;}

    /** @name Group mode */
    //@{
    /** What to add in place of \a objs, by GroupMode
     *
     * Called by the group extension before it adds. An object that is to
     * be linked is replaced by a new App::Link this part owns, placed where
     * the object is. An owned link of this part is passed as it is, so the
     * call can be repeated.
     */
    std::vector<DocumentObject*> prepareObjectsToAdd(const std::vector<DocumentObject*> &objs);
    /// Called by the group extension once \a objs are out of Group: deletes
    /// the links this part owns, or disowns one moving to another group
    void onObjectsRemoved(const std::vector<DocumentObject*> &objs);
    /// Whether \a obj is an App::Link this part owns
    bool isOwnedLink(const DocumentObject *obj) const;
    /// Whether Auto mode would add a link in place of \a obj
    bool needsLink(const DocumentObject *obj) const;
    //@}

    /** @name Aux groups */
    //@{
    /// The role of the group of components, which takes what no other group takes
    static const char *PartsRole;
    bool isAuxGroupAllowed(const AuxGroup *group, const DocumentObject *obj) const override;
    /// The aux group of the given role, or null
    AuxGroup *getAuxGroup(const char *role) const;
    /** Add the Parts group, or return the one there is
     *
     * The group is optional (docs/AssemblyObjectModel.md sec 3.1): its
     * presence is the setting.
     */
    virtual AuxGroup *addPartsGroup();
    /// Remove the Parts group, if any; throws when this part needs it
    virtual void removePartsGroup();
    AuxGroup *getElementsGroup(bool create = false) override;
    /** Where a group of this role is listed among the groups
     *
     * The groups come right after Origin in this order: Elements, Joints,
     * Physics, Simulations, Views, a group of any other role or none, and
     * Parts last (docs/AssemblyObjectModel.md sec 3).
     */
    static int getAuxGroupRank(const AuxGroup *group);
    //@}

    /** What a part owns (docs/TransactionLogJournal.md sec 37.9): its origin,
     * and its aux groups of a role and its Elements group, made with it and
     * removed with it; a group without a role is the user's. Pasted onto a
     * live part, each group stands for the live one's of its role and
     * follows the part.
     */
    std::vector<DocumentObject*> getOwnedObjects() const override;
    DocumentObject* getOwnedCounterpart(const DocumentObject* owned,
                                        const DocumentObject* liveOwner,
                                        OwnedPaste& how) const override;

protected:
    void onChanged(const Property *prop) override;
    void onDocumentRestored() override;
    /// The type of the Parts group addPartsGroup() makes
    virtual const char *getPartsGroupType() const {
        return "App::PartAuxGroup";
    }

private:
    /// The names over the looks are being given what the store has
    bool mirroringLooks {false};

public:

    /**
     * Returns the part which contains this object.
     * In case this object does not belong to any Part, 0 is returned.
     * @param obj       the object to search for
     * @param recursive: whether to recursively find any grand parent Part container
     */
    static App::Part* getPartOfObject (const DocumentObject* obj, bool recursive=true);

    PyObject *getPyObject() override;
};

//using PartPython = App::FeaturePythonT<Part>;

} //namespace App


#endif // APP_Part_H
