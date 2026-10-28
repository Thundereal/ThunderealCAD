// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
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

#include <cctype>
#include <cstdlib>
#include <deque>

#include <App/Document.h>
#include <App/GeoFeatureGroupExtension.h>
#include <Base/Console.h>
#include <Gui/Selection/Selection.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Part/App/SubShapeBinder.h>

#include "Import.h"


namespace SurfaceGui
{

App::SubObjectT editContext(App::DocumentObject* feature)
{
    if (!feature || !feature->isAttachedToDocument()) {
        return {};
    }
    std::deque<App::DocumentObject*> path {feature};
    for (auto group = App::GeoFeatureGroupExtension::getGroupOfObject(feature); group;
         group = App::GeoFeatureGroupExtension::getGroupOfObject(group)) {
        path.push_front(group);
    }
    if (path.size() == 1) {
        return App::SubObjectT(feature, "");
    }
    std::string sub;
    for (std::size_t i = 1; i < path.size(); ++i) {
        sub += path[i]->getNameInDocument();
        sub += '.';
    }
    return App::SubObjectT(path.front(), sub.c_str());
}

App::SubObjectT importPick(App::DocumentObject* feature, const App::SubObjectT& picked)
{
    try {
        return Part::SubShapeBinder::import(picked,
                                            editContext(feature),
                                            true,
                                            false,
                                            false,
                                            true,
                                            false);
    }
    catch (Base::Exception& e) {
        e.ReportException();
    }
    // as picked, resolved
    return App::SubObjectT(picked.getSubObject(), picked.getOldElementName().c_str());
}

std::string importPython(const App::SubObjectT& picked, const char* edit)
{
    return "Part.importExternalObject(" + picked.getSubObjectPython() + ", " + edit
        + ", makeFace=False)";
}

std::string editContextPython(App::DocumentObject* feature, const char* edit)
{
    return std::string(edit) + " = " + editContext(feature).getSubObjectPython();
}

App::SubObjectT pickOf(const Gui::SelectionChanges& msg)
{
    return msg.pOriginalMsg ? msg.pOriginalMsg->Object : msg.Object;
}

bool refersTo(App::DocumentObject* obj, const std::string& sub, const App::SubObjectT& picked)
{
    App::DocumentObject* target = picked.getSubObject();
    const std::string element = picked.getOldElementName();
    if (!obj || !target) {
        return false;
    }
    if (obj == target && sub == element) {
        return true;
    }
    // a binder of what was picked: the element it binds it as
    auto binder = dynamic_cast<Part::SubShapeBinder*>(obj);
    if (!binder || binder->Support.getSize() != 1) {
        return false;
    }
    // import binds the object picked in, whole, its elements mapped from
    // it; or, where it cannot map them, the one element picked
    const auto& support = binder->Support.getSubListValues().front();
    const auto subs = support.getSubValues(false);
    const std::string bound = subs.empty() ? std::string() : subs.front();
    App::DocumentObject* source = support.getValue();
    std::string srcSub;
    if (source == picked.getObject() && bound == picked.getSubNameNoElement()) {
        srcSub = bound + element;
    }
    else if (source == target && bound.empty()) {
        srcSub = element;
    }
    else {
        return (source == picked.getObject() && bound == picked.getSubNameNoElement() + element)
            || (source == target && bound == element);
    }
    if (element.empty()) {
        // the object picked whole: the binder whole
        return sub.empty();
    }
    try {
        for (const auto& found :
             Part::Feature::getElementFromSource(binder, "", source, srcSub.c_str(), true)) {
            std::string name;
            found.index.appendToStringBuffer(name);
            if (name == sub) {
                return true;
            }
        }
    }
    catch (Base::Exception&) {
    }
    return false;
}

std::pair<App::DocumentObject*, std::string> listedAs(const App::PropertyLinkSubList& list,
                                                      const App::SubObjectT& picked)
{
    const auto& objs = list.getValues();
    const auto subs = list.getSubValues();
    for (std::size_t i = 0; i < objs.size() && i < subs.size(); ++i) {
        if (refersTo(objs[i], subs[i], picked)) {
            return {objs[i], subs[i]};
        }
    }
    return {picked.getSubObject(), picked.getOldElementName()};
}

int elementIndex(const std::string& name)
{
    std::size_t i = name.size();
    while (i > 0 && std::isdigit(static_cast<unsigned char>(name[i - 1]))) {
        --i;
    }
    if (i == name.size() || i == 0) {
        return 0;
    }
    return std::atoi(name.c_str() + i);
}

bool anyBinder(const std::vector<App::PropertyLinkSubList::SubSet>& links)
{
    for (const auto& link : links) {
        if (link.first && link.first->isDerivedFrom<Part::SubShapeBinder>()) {
            return true;
        }
    }
    return false;
}

}  // namespace SurfaceGui
