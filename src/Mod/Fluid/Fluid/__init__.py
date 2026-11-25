# SPDX-License-Identifier: LGPL-2.1-or-later

# What opening a file imports for a type named Fluid::...: the module named
# by the type's prefix. Without it a session that had not imported FluidApp
# itself dropped every Fluid object of the file it opened.
import FluidApp
