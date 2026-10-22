/***************************************************************************
 *   Copyright (c) 2010 Werner Mayer <wmayer[at]users.sourceforge.net>     *
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
# include <stack>
#endif

#include <App/Document.h>
#include <App/DocumentObject.h>
#include <Base/Reader.h>
#include <Base/Writer.h>
#include <Gui/Application.h>
#include <Gui/Document.h>
#include <Gui/ViewProvider.h>

#include "MergeDocuments.h"


using namespace Gui;
namespace sp = std::placeholders;

namespace Gui {

class XMLMergeReader : public Base::XMLReader
{
public:
    XMLMergeReader(std::map<std::string, std::string>& name, Base::Reader &reader)
      : Base::XMLReader(reader), nameMap(name)
    {}

    void addName(const char* s1, const char* s2) override
    {
        nameMap[s1] = s2;
    }
    const char* getName(const char* name) const override
    {
        std::map<std::string, std::string>::const_iterator it = nameMap.find(name);
        if (it != nameMap.end())
            return it->second.c_str();
        else
            return name;
    }
    bool doNameMapping() const override
    {
        return true;
    }
protected:


private:
    std::map<std::string, std::string>& nameMap;
    using PropertyTag = std::pair<std::string, std::string>;
    std::stack<PropertyTag> propertyStack;
};
}

MergeDocuments::MergeDocuments(App::Document* doc) : appdoc(doc)
{
    //NOLINTBEGIN
    connectExport = doc->signalExportObjects.connect
        (std::bind(&MergeDocuments::exportObject, this, sp::_1, sp::_2));
    connectImport = doc->signalImportObjects.connect
        (std::bind(&MergeDocuments::importObject, this, sp::_1, sp::_2));
    document = Gui::Application::Instance->getDocument(doc);
    //NOLINTEND
}

MergeDocuments::~MergeDocuments()
{
    connectExport.disconnect();
    connectImport.disconnect();
}

unsigned int MergeDocuments::getMemSize () const
{
    return 0;
}

namespace
{
/// The value of `attr="..."` in the tag text `tag`, empty if none
std::string attribute(const std::string& tag, const char* attr)
{
    const std::string key = std::string(" ") + attr + "=\"";
    auto pos = tag.find(key);
    if (pos == std::string::npos)
        return {};
    pos += key.size();
    auto end = tag.find('"', pos);
    return end == std::string::npos ? std::string() : tag.substr(pos, end - pos);
}
}  // namespace

std::vector<std::string> MergeDocuments::scanUids(std::istream& input, std::string& from)
{
    from.clear();
    std::vector<std::string> uids;
    zipios::ZipInputStream zip(input);
    // Document.xml is the first entry; the object list is its first part,
    // read up to its end and no further (the properties, the shapes' files)
    std::string text;
    char buf[65536];
    while (zip) {
        zip.read(buf, sizeof(buf));
        text.append(buf, static_cast<std::size_t>(zip.gcount()));
        if (text.find("</Objects>") != std::string::npos)
            break;
    }
    auto doc = text.find("<Document ");
    if (doc != std::string::npos)
        from = attribute(text.substr(doc, text.find('>', doc) - doc), "From");
    auto end = text.find("</Objects>");
    for (auto pos = text.find("<Object "); pos != std::string::npos && pos < end;
         pos = text.find("<Object ", pos + 1)) {
        const std::string tag = text.substr(pos, text.find('>', pos) - pos);
        std::string uid = attribute(tag, "uid");
        if (!uid.empty())
            uids.push_back(std::move(uid));
    }
    return uids;
}

std::vector<App::DocumentObject*>
MergeDocuments::importObjects(std::istream& input)
{
    this->nameMap.clear();
    // Owned here: an import that throws (a clash settlement that cannot
    // read a value, sec 36.4) leaves neither the stream nor a dangling
    // pointer to it behind
    std::unique_ptr<zipios::ZipInputStream> owned(new zipios::ZipInputStream(input));
    this->stream = owned.get();
    struct Reset
    {
        zipios::ZipInputStream*& stream;
        ~Reset() { stream = nullptr; }
    } reset {this->stream};
    Base::ZipReader zreader(*this->stream,std::string("<memory>"));
    XMLMergeReader reader(this->nameMap, zreader);
    return appdoc->importObjects(reader);
}

void MergeDocuments::importObject(const std::vector<App::DocumentObject*>& o, Base::XMLReader & r)
{
    objects = o;
    for (auto it : objects) {
        Gui::ViewProvider* vp = document->getViewProvider(it);
        if (vp) vp->hide();
    }
    Restore(r);

    r.readFiles();
}

void MergeDocuments::exportObject(const std::vector<App::DocumentObject*>& o, Base::Writer & w)
{
    objects = o;
    Save(w);
}

void MergeDocuments::Save (Base::Writer & w) const
{
    w.addFile("GuiDocument.xml", this);
}

void MergeDocuments::Restore(Base::XMLReader &r)
{
    r.addFile("GuiDocument.xml", this);
}

void MergeDocuments::SaveDocFile (Base::Writer & w) const
{
    document->exportObjects(objects, w);
}

void MergeDocuments::RestoreDocFile(Base::Reader & r)
{
    document->importObjects(objects, r, nameMap);
}
