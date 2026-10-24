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


#ifndef GUI_MERGEDOCUMENTS_H
#define GUI_MERGEDOCUMENTS_H

#include <Base/Persistence.h>
#include <map>
#include <vector>
#include <fastsignals/signal.h>

namespace zipios {
class ZipInputStream;
}
namespace App {
class Document;
class DocumentObject;
}

namespace Gui {
class Document;
class GuiExport MergeDocuments : public Base::Persistence
{
public:
    explicit MergeDocuments(App::Document* doc);
    ~MergeDocuments() override;
    unsigned int getMemSize () const override;
    std::vector<App::DocumentObject*> importObjects(std::istream&);
    /// The name each object had in the stream -> its name here, after
    /// importObjects()
    const std::map<std::string, std::string>& names() const
    {
        return nameMap;
    }
    /** The UIDs of the objects of a stream importObjects() takes, read off
     * its Document.xml alone (docs/TransactionLog.md sec 37.9): whether a
     * paste may clash, asked before anything is read. `from` is the
     * document it was copied from (the `From` attribute), empty if none.
     * An object with no UID (a stream from before UIDs) clashes with
     * nothing: an import gives it one of its own.
     */
    static std::vector<std::string> scanUids(std::istream& input, std::string& from);
    void importObject(const std::vector<App::DocumentObject*>& o, Base::XMLReader & r);
    void exportObject(const std::vector<App::DocumentObject*>& o, Base::Writer & w);
    void Save (Base::Writer & w) const override;
    void Restore(Base::XMLReader &r) override;
    void SaveDocFile (Base::Writer & w) const override;
    void RestoreDocFile(Base::Reader & r) override;

private:
    zipios::ZipInputStream* stream{nullptr};
    App::Document* appdoc;
    Gui::Document* document;
    std::vector<App::DocumentObject*> objects;
    std::map<std::string, std::string> nameMap;
    using Connection = fastsignals::connection;
    Connection connectExport;
    Connection connectImport;
};

} // namespace Gui

#endif // GUI_MERGEDOCUMENTS_H
