/***************************************************************************
 *   Copyright (c) 2009 Werner Mayer <wmayer[at]users.sourceforge.net>     *
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

#include "VRMLObject.h"
#include "Document.h"
#include "DocumentObjectPy.h"
#include "FileBlobManager.h"
#include <Base/FileInfo.h>
#include <Base/Reader.h>
#include <Base/Stream.h>
#include <Base/Writer.h>


using namespace App;

PROPERTY_SOURCE(App::VRMLObject, App::GeoFeature)


VRMLObject::VRMLObject()
{
    ADD_PROPERTY_TYPE(VrmlFile,(nullptr),"",Prop_None,"Included file with the VRML definition");
    ADD_PROPERTY_TYPE(Urls,(""),"",static_cast<PropertyType>(Prop_ReadOnly|Prop_Output|Prop_Transient),
        "Resource files loaded by the VRML file");
    ADD_PROPERTY_TYPE(Resources,(""),"",static_cast<PropertyType>(Prop_ReadOnly|Prop_Output),
        "Resource files loaded by the VRML file");
    Urls.setSize(0);
    Resources.setSize(0);
}

short VRMLObject::mustExecute() const
{
    return 0;
}

void VRMLObject::onChanged(const App::Property* prop)
{
    if (prop == &VrmlFile) {
        std::string orig = VrmlFile.getOriginalFileName();
        if (!orig.empty()) {
            // store the path name of the VRML file
            Base::FileInfo fi(orig);
            this->vrmlPath = fi.dirPath();
        }
    }
    else if (prop == &Urls && !restoringUrls) {
        // save the relative paths to the resource files in the project file.
        // Not while a restore fills Urls with the transient copies: the
        // saved Resources stand, and deriving them again from those paths
        // dropped every one to `<Obj>/` (Urls.setSize() makes empty ones)
        // or flattened its subdirectories.
        Resources.setSize(Urls.getSize());
        const std::vector<std::string>& urls = Urls.getValues();
        int index=0;
        for (std::vector<std::string>::const_iterator it = urls.begin(); it != urls.end(); ++it, ++index) {
            std::string output = getRelativePath(this->vrmlPath, *it);
            Resources.set1Value(index, output);
        }
    }
    GeoFeature::onChanged(prop);
}

PyObject *VRMLObject::getPyObject()
{
    if (PythonObject.is(Py::_None())){
        // ref counter is set to 1
        PythonObject = Py::Object(new DocumentObjectPy(this),true);
    }
    return Py::new_reference_to(PythonObject); 
}

std::string VRMLObject::getRelativePath(const std::string& prefix, const std::string& resource) const
{
    std::string str;
    std::string intname = this->getNameInDocument();
    if (!prefix.empty()) {
        if (resource.substr(0, prefix.size()) == prefix) {
            std::string suffix = resource.substr(prefix.size());
            str = intname + suffix;
        }
    }

    if (str.empty()) {
        Base::FileInfo fi(resource);
        str = intname + "/" + fi.fileName();
    }

    return str;
}

std::string VRMLObject::fixRelativePath(const std::string& name, const std::string& resource) const
{
    // the part before the first '/' must match with object's internal name
    std::string::size_type pos = resource.find('/');
    if (pos != std::string::npos) {
        std::string prefix = resource.substr(0, pos);
        std::string suffix = resource.substr(pos);
        if (prefix != name) {
            return name + suffix;
        }
    }
    return resource;
}

void VRMLObject::makeDirectories(const std::string& path, const std::string& subdir)
{
    std::string::size_type pos = subdir.find('/');
    while (pos != std::string::npos) {
        std::string sub = subdir.substr(0, pos);
        std::string dir = path + "/" + sub;
        Base::FileInfo fi(dir);
        if (!fi.createDirectory())
            break;
        pos = subdir.find('/', pos+1);
    }
}

bool VRMLObject::keepsItsName(const std::string& resource)
{
    // What a VRML Inline names, which Coin opens by its name
    static const char* const scenes[] = {"wrl", "wrz", "vrml", "iv", "x3d", "x3dv", "x3dz", "gz"};
    Base::FileInfo fi(resource);
    for (const char* ext : scenes) {
        if (fi.hasExtension(ext)) {
            return true;
        }
    }
    return false;
}

std::string VRMLObject::archiveName(const std::string& resource) const
{
    // Save always wrote `<this object's name>.<resource>`, the resource
    // starting with that name too (getRelativePath(), fixRelativePath()).
    // Read the name off the resource, so that an object restored under
    // another name still finds the entries saved under its old one.
    std::string::size_type pos = resource.find('/');
    std::string owner = pos != std::string::npos ? resource.substr(0, pos)
                                                 : std::string(getNameInDocument());
    return owner + "." + resource;
}

void VRMLObject::Save (Base::Writer &writer) const
{
    App::GeoFeature::Save(writer);

    // save also the inline files if there
    const std::vector<std::string>& urls = Resources.getValues();
    for (const auto & url : urls) {
        writer.addFile(archiveName(url).c_str(), this);
    }

    this->index = 0;
}

void VRMLObject::Restore(Base::XMLReader &reader)
{
    App::GeoFeature::Restore(reader);
    // Empty slots for RestoreDocFile() to fill, the saved Resources kept
    restoringUrls = true;
    Urls.setSize(Resources.getSize());
    restoringUrls = false;

    // restore also the inline files if there. Under the name Save wrote
    // them: they were registered by the bare resource before
    // (docs/HandsOnQueue.md entry 212), which matched no entry, so no
    // resource was ever restored.
    const std::vector<std::string>& urls = Resources.getValues();
    for(const auto & url : urls) {
        reader.addFile(archiveName(url).c_str(), this);
    }

    this->index = 0;
}

void VRMLObject::SaveDocFile (Base::Writer &writer) const
{
    // store the inline files of the VRML file
    if (this->index < Urls.getSize()) {
        std::string url = Urls[this->index];

        Base::FileInfo fi(url);
        // it can happen that the transient directory has changed after
        // saving the 'URLs' in RestoreDocFile() and then we have to
        // try again with the new transient directory -- under the neutral
        // name RestoreDocFile() gives a texture, or the resource's own.
        if (!fi.exists()) {
            std::string path = getDocument()->TransientDir.getValue();
            url = path + "/" + Resources[this->index];
            fi.setFile(FileBlobManager::neutralName(url));
            if (!fi.exists()) {
                fi.setFile(url);
            }
        }

        this->index++;
        Base::ifstream file(fi, std::ios::in | std::ios::binary);
        if (file) {
            writer.Stream() << file.rdbuf();
        }
    }
}

void VRMLObject::RestoreDocFile(Base::Reader &reader)
{
    // Which resource this entry is: by its name, the order Save wrote them
    // in being the fallback
    const std::string entry = reader.getFileName();
    int which = -1;
    for (int i = 0; i < Resources.getSize(); ++i) {
        const std::string name = archiveName(Resources[i]);
        if (entry.size() >= name.size()
                && entry.compare(entry.size() - name.size(), name.size(), name) == 0) {
            which = i;
            break;
        }
    }
    if (which < 0) {
        which = this->index;
    }
    if (which >= Resources.getSize()) {
        return;
    }

    std::string path = getDocument()->TransientDir.getValue();
    std::string url = Resources[which];
    std::string intname = this->getNameInDocument();
    url = fixRelativePath(intname, url);
    makeDirectories(path, url);

    // A texture goes under a neutral name (docs/HandsOnQueue.md entries
    // 206, 212): the close deletes it with the directory. The view provider
    // points the scene at it.
    std::string file = path + "/" + url;
    if (!keepsItsName(url)) {
        file = FileBlobManager::neutralName(file);
    }
    Base::FileInfo fi(file);
    restoringUrls = true;
    Resources.set1Value(which, url);
    Urls.set1Value(which, file);
    restoringUrls = false;
    this->index++;

    Base::ofstream out(fi, std::ios::out | std::ios::binary);
    if (out) {
        reader >> out.rdbuf();
        out.close();
    }

    // after restoring all inline files reload the VRML file
    if (this->index == Urls.getSize()) {
        VrmlFile.touch();
    }
}
