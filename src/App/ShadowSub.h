// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************************************
 *                                                                                                 *
 *   Copyright (c) 2026 Zheng, Lei (realthunder) <realthunder.dev@gmail.com>                       *
 *                                                                                                 *
 *   This file is part of the FreeCAD CAx development system.                                      *
 *                                                                                                 *
 *   This library is free software; you can redistribute it and/or modify it under the terms of    *
 *   the GNU Lesser General Public License as published by the Free Software Foundation; either    *
 *   version 2.1 of the License, or (at your option) any later version.                            *
 *                                                                                                 *
 *   This library is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;     *
 *   without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.     *
 *   See the GNU Lesser General Public License for more details.                                   *
 *                                                                                                 *
 *   You should have received a copy of the GNU Lesser General Public License along with this      *
 *   library; if not, write to the Free Software Foundation, Inc., 51 Franklin Street,             *
 *   Fifth Floor, Boston, MA  02110-1301  USA                                                      *
 *                                                                                                 *
 **************************************************************************************************/

#ifndef APP_SHADOWSUB_H
#define APP_SHADOWSUB_H

// Apart from PropertyLinks.h so that ObjectIdentifier.h, which the sandbox
// image compiles against its own stand-in properties (FC_EXPR_IMAGE,
// docs/ExpressionImage.md), can hold one without the property classes.

#include <string>
#include <utility>
#include <vector>

#ifndef FC_NO_QT
#include "MappedName.h"
#else
namespace Data
{
/// The image (FC_NO_QT) has no Qt and no element map: a reference there
/// holds no string ids, and this stands in for the list it would hold
struct ElementIDRefs
{
};
}  // namespace Data
#endif

namespace App
{

/** A sub-element reference as resolved: the new style (mapped) name in
 * first, the old style one in second -- a pair, as it always was -- and
 * the string ids the element's name holds.
 *
 * The mapped name names string ids by number, as text; nothing held
 * them, so a string table compacted while the element was gone dropped
 * what the reference names, and the element coming back was named anew
 * (docs/TransactionLog.md sec 27.75). The ids are the element map's for
 * the element, which include the names it was made from, not only those
 * its text shows. They are saved beside the shadow and held again from
 * the numbers once the reference registers against its target: no
 * lookup, no parse of the name. Never compared: two shadows are the same
 * when their names are.
 */
struct ShadowSub : std::pair<std::string, std::string>
{
    using Names = std::pair<std::string, std::string>;
    ShadowSub() = default;
    ShadowSub(std::string newName, std::string oldName)
        : Names(std::move(newName), std::move(oldName))
    {}
    ShadowSub(const Names& names) : Names(names) {}
    ShadowSub(Names&& names) : Names(std::move(names)) {}

    bool operator==(const ShadowSub& other) const
    {
        return static_cast<const Names&>(*this) == static_cast<const Names&>(other);
    }
    bool operator!=(const ShadowSub& other) const
    {
        return !(*this == other);
    }

    /// The ids held, from the target's element map
    Data::ElementIDRefs sids;
    /// Read from a file and not yet held: ids of the owner's table, which
    /// is the target's for a reference within one table. Held when the
    /// reference registers.
    std::vector<long> savedIds;

    /** A reference whose names are another table's -- into another
     * document, or through a link to one (docs/TransactionLog.md sec
     * 27.76 item 5): the element part of 'first' with its string ids
     * imported into the owner's table. This is what the file keeps; the
     * working form, 'first', is looked up from it in the target's table
     * when the reference registers. Empty for a reference within one
     * table.
     */
    std::string stored;
    /// The owner's ids 'stored' names, held
    Data::ElementIDRefs storedIds;
    /// 'first' is not yet looked up from 'stored': restored and not
    /// registered, or looked up and missing (then 'first' is empty)
    bool pending = false;

    /** The mapped name a file gave beside the element of an
     * expression's path (docs/TransactionLog.md sec 31.22), until the
     * path registers. An expression says its element in its text, and
     * a path with no name beside it takes the one that element has
     * where it registers; with this, it registers as it was saved, as
     * a link does.
     */
    std::string given;
};


}  // namespace App

#endif  // APP_SHADOWSUB_H
