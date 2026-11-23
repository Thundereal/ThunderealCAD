# SPDX-License-Identifier: LGPL-2.1-or-later

"""OpenFOAM for the fluid workbench, when there is none on this machine
(docs/FluidSimulation.md sec 13, ruling 4): never shipped with us -- it is
GPL -- but offered as a download the user starts. On Linux it is a conda
package, put in a folder of the workbench's own by micromamba (or a conda or
mamba already here): our own build of conda-forge's recipe
(Thundereal/openfoam-feedstock, in the realthunder channel), since
conda-forge's computes viscous flow wrongly (sec 14.2) and the backend
refuses it; what it runs on comes from conda-forge."""

import glob
import os
import platform
import shutil

import FreeCAD as App

translate = App.Qt.translate

# our build of it and no other: conda-forge's own is numbered below 100
PACKAGE = "openfoam=2412=*_100"
CHANNELS = ("realthunder", "conda-forge")
MICROMAMBA = (
    "https://github.com/mamba-org/micromamba-releases/releases/latest/download/micromamba-{}"
)


def downloadFolder():
    """Where the download goes, and the backend looks."""
    return os.path.join(App.getUserAppDataDir(), "Fluid", "openfoam")


def toolsFolder():
    return os.path.join(App.getUserAppDataDir(), "Fluid", "tools")


def supported():
    """Whether the download can be made on this machine; and why not."""
    if platform.system() != "Linux":
        return False, translate(
            "Fluid",
            "The download is for Linux. On Windows OpenFOAM runs under WSL and on macOS "
            "in a container, which the workbench does not drive yet.",
        )
    if platform.machine() not in ("x86_64", "AMD64"):
        return False, translate(
            "Fluid", "conda-forge has OpenFOAM for 64 bit Intel and AMD processors only."
        )
    return True, ""


def condaTool():
    """A micromamba, mamba or conda already on this machine, or None."""
    for name in ("micromamba", "mamba", "conda"):
        found = shutil.which(name)
        if found:
            return found
    return None


def installScript(folder=None, tool=None):
    """The shell script that makes the download: the installer fetched and
    checked when there is none here, the package installed into the folder,
    the parallel layer mended. Its last line says DONE."""
    folder = folder or downloadFolder()
    lines = ["set -e"]
    if tool is None:
        tools = toolsFolder()
        url = MICROMAMBA.format("linux-64")
        tool = os.path.join(tools, "micromamba")
        lines += [
            "mkdir -p '%s'" % tools,
            "echo 'Fetching micromamba'",
            "curl -fL --retry 3 -o '%s.part' '%s'" % (tool, url),
            "curl -fL --retry 3 -o '%s.sha256' '%s.sha256'" % (tool, url),
            # the published sum, against a download cut short or mangled
            "echo \"$(cut -d' ' -f1 '%s.sha256')  %s.part\" | sha256sum -c -" % (tool, tool),
            "mv '%s.part' '%s'" % (tool, tool),
            "chmod +x '%s'" % tool,
        ]
    lines += [
        "echo 'Installing %s from %s into %s'" % (PACKAGE, " and ".join(CHANNELS), folder),
        "'%s' create -y -p '%s' %s --override-channels '%s'"
        % (tool, folder, " ".join("-c " + channel for channel in CHANNELS), PACKAGE),
        "echo DONE",
    ]
    return "\n".join(lines) + "\n"


def fixParallel(folder=None):
    """conda-forge's OpenFOAM looks for its MPI layer in lib/sys-mpich and
    ships it in lib/mpich-<version>, so every program loads the serial
    stand-in and a parallel run stops at once. A link under the name looked
    for mends it. True when the link was made; False when there was nothing
    to mend."""
    lib = os.path.join(folder or downloadFolder(), "lib")
    wanted = os.path.join(lib, "sys-mpich")
    if os.path.lexists(wanted):
        return False
    shipped = sorted(
        d
        for d in glob.glob(os.path.join(lib, "mpich-*"))
        if os.path.exists(os.path.join(d, "libPstream.so"))
    )
    if not shipped:
        return False
    os.symlink(os.path.basename(shipped[-1]), wanted)
    return True


def explanation():
    """What the download is, for the user to agree to."""
    return translate(
        "Fluid",
        "OpenFOAM is the solver this study runs on. It is free software under the GNU "
        "General Public License, made by OpenCFD and others, and is not part of this "
        "program: it runs as programs of its own.\n\n"
        "The download takes OpenFOAM v2412 as packaged by conda-forge and built again by "
        "us with its viscous flow put right, about 230 MB to "
        "fetch and 1.1 GB on disk, into\n{}\n"
        "and nothing outside that folder is changed. Deleting the folder removes it.\n\n"
        "It is installed by a conda, mamba or micromamba found on this machine; when "
        "there is none, micromamba is fetched first from its project's releases on "
        "GitHub, checked against the sum published there, and kept in\n{}",
    ).format(downloadFolder(), toolsFolder())
