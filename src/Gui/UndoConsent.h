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

#ifndef GUI_UNDOCONSENT_H
#define GUI_UNDOCONSENT_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <FCGlobal.h>

namespace App
{
struct Actor;
}

namespace Gui
{

/** Asking another user to let their steps be undone with one's own
 * (docs/MultiViewEdit.md sec 12.11): an undo refused for a step of theirs
 * made since that depends on it goes only when they agree.
 *
 * The desktop user is asked in a dialog that does not block; a served user
 * on the connections they have, by the serving tier's transport
 * (setTransport). Once every owner has agreed the undo is made -- the steps
 * of the request, all or none, as the one who asked -- and the one who asked
 * is told how it went; one owner's no ends it. GUI thread.
 */
class GuiExport UndoConsent
{
public:
    struct Request
    {
        /// The document, by name
        std::string doc;
        /// What the undo takes: the steps of the one who asked and theirs
        std::vector<int> steps;
        /// The undo asked for, by name
        std::string what;
        /// The owners' steps among them, by name
        std::vector<std::string> theirs;
        /// Whose agreement it waits for; null for the desktop user
        std::vector<std::shared_ptr<const App::Actor>> owners;
        /// Who asked, null for the desktop user
        std::shared_ptr<const App::Actor> requester;
        /// The connection that asked, 0 for the desktop
        uint64_t requesterClient = 0;
    };

    /** Ask the owners. The request's id, or 0 when one of them cannot be
     * asked now -- a served user with no connection -- with \a why.
     */
    static int ask(Request request, std::string& why);
    /** \a by's answer to request \a id. False, with \a why, for no such
     * request or one that is not theirs to answer.
     */
    static bool answer(int id, bool allow, const App::Actor* by, std::string& why);
    /// The requests waiting for an answer
    static std::size_t pending();

    /// Hand \a json to the connections of \a owner; false when it has none
    using Deliver = std::function<bool(const App::Actor* owner, const std::string& json)>;
    /// Hand \a json to connection \a client
    using Notify = std::function<void(uint64_t client, const std::string& json)>;
    /// The serving tier's transport, installed as it starts serving
    static void setTransport(Deliver deliver, Notify notify);
};

}  // namespace Gui

#endif  // GUI_UNDOCONSENT_H
