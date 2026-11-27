// SPDX-License-Identifier: LGPL-2.1-or-later

#include <FCGlobal.h>

#pragma once

// Fluid
#ifndef FluidExport
# ifdef Fluid_EXPORTS
#  define FluidExport FREECAD_DECL_EXPORT
# else
#  define FluidExport FREECAD_DECL_IMPORT
# endif
#endif

// FluidGui
#ifndef FluidGuiExport
# ifdef FluidGui_EXPORTS
#  define FluidGuiExport FREECAD_DECL_EXPORT
# else
#  define FluidGuiExport FREECAD_DECL_IMPORT
# endif
#endif
