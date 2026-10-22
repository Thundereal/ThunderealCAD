/***************************************************************************
 *   Copyright (c) 2023 Abdullah Tahiri <abdullah.tahiri.yo@gmail.com>     *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License (LGPL)   *
 *   as published by the Free Software Foundation; either version 2 of     *
 *   the License, or (at your option) any later version.                   *
 *   for detail see the LICENCE text file.                                 *
 *                                                                         *
 *   FreeCAD is distributed in the hope that it will be useful,            *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with FreeCAD; if not, write to the Free Software        *
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  *
 *   USA                                                                   *
 *                                                                         *
 ***************************************************************************/

#include "PreCompiled.h"

#include "Observer.h"
#include "Console.h"
#include "Exception.h"


namespace Base
{

void subjectWarning(const char* where, const char* message, void* observer)
{
    if (observer) {
        Base::Console().DeveloperWarning(std::string(where), message, observer);
    }
    else {
        Base::Console().DeveloperWarning(std::string(where), message);
    }
}

void subjectNotifyFailed()
{
    try {
        throw;
    }
    catch (Base::Exception& e) {
        Base::Console().Error("Unhandled Base::Exception caught when notifying observer.\n"
                              "The error message is: %s\n",
                              e.what());
    }
    catch (std::exception& e) {
        Base::Console().Error("Unhandled std::exception caught when notifying observer\n"
                              "The error message is: %s\n",
                              e.what());
    }
    catch (...) {
        Base::Console().Error(
            "Unhandled unknown exception caught in when notifying observer.\n");
    }
}

#if !defined(__MINGW32__)
template class BaseExport Observer<const char*>;
template class BaseExport Subject<const char*>;
#endif

}  // namespace Base
