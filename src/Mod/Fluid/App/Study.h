// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <memory>
#include <string>

#include <App/DocumentObject.h>
#include <App/GroupExtension.h>
#include <App/PropertyFile.h>
#include <App/PropertyGeo.h>
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>

#include <Mod/Fluid/FluidGlobal.h>

#include "ResultsArchive.h"
#include "Solver/Backend.h"

namespace Fluid
{

/// A fluid study (docs/FluidSimulation.md secs 5.2, 11): the parts that are
/// its walls, in Walls; the fluid at the start and the monitors, in Group;
/// the run and its kept results.
class FluidExport Study: public App::DocumentObject, public App::GroupExtension
{
    PROPERTY_HEADER_WITH_OVERRIDE(Fluid::Study);

public:
    Study();
    ~Study() override;

    /// The parts the fluid cannot pass, each as its shape where it stands
    /// (a Link or a sub-assembly as the shape it shows)
    App::PropertyLinkList Walls;
    App::PropertyTime TimeEnd;
    App::PropertyTime OutputInterval;
    /// Gravity, in the document's coordinates, mm/s^2
    App::PropertyVector Gravity;
    /// The size of what is to be resolved: a particle backend's spacing, a
    /// mesh backend's base cell. 0: a twentieth of the fluid's smallest side
    App::PropertyLength Resolution;
    /// Whether the space's side against gravity is open to the air: the
    /// fluid that leaves through it is gone. The other sides are closed.
    App::PropertyBool OpenTop;
    /// The backend the study runs on, by name; empty: the first that can
    /// run here
    App::PropertyString Backend;
    /// The last run's results, an archive (ResultsArchive.h)
    App::PropertyFileIncluded Results;
    /// How the last run ended, in words
    App::PropertyString Status;

    /// The study as a backend is given it. Throws Base::ValueError naming
    /// what cannot be taken: a wall or a fluid without a solid shape.
    Solver::Model buildModel() const;

    /// Starts a run in a thread of its own; why says what is wrong when the
    /// answer is not Ok. One run at a time.
    Solver::Status start(std::string& why);
    struct Progress
    {
        bool running = false;  // a run was started and not finished
        bool done = false;     // its thread is through: finish() will not wait
        double time = 0.0;     // of the last frame, s
        std::size_t frames = 0;
    };
    Progress progress() const;
    void cancel();
    /// Waits for the run and keeps its results in Results; a run cancelled
    /// or failed keeps none and leaves the ones before. why as start()'s.
    Solver::Status finish(std::string& why);
    /// start() and finish().
    Solver::Status run(std::string& why);

    /// The kept results, opened when first asked for; nullptr when there
    /// are none. Throws as ArchiveReader does.
    const ArchiveReader* results() const;
    /// Whether the kept results are of another model than the study is now.
    bool isStale() const;

    const char* getViewProviderName() const override
    {
        return "Gui::ViewProviderDocumentObjectGroup";
    }
    PyObject* getPyObject() override;

protected:
    void onChanged(const App::Property* prop) override;
    void unsetupObject() override;

private:
    struct Run;
    std::unique_ptr<Run> active;
    mutable std::unique_ptr<ArchiveReader> reader;
    mutable std::string readerPath;
};

/// A volume full of fluid when the run starts: the solid of Volume, where it
/// stands. A child of a Study.
class FluidExport InitialFluid: public App::DocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(Fluid::InitialFluid);

public:
    InitialFluid();

    App::PropertyLink Volume;
    App::PropertyDensity Density;
    App::PropertyDynamicViscosity Viscosity;

    const char* getViewProviderName() const override
    {
        return "Gui::ViewProviderDocumentObject";
    }
};

/// What a run reports at each frame and keeps: the height of the fluid's
/// surface above Point, or the pressure at it. A child of a Study.
class FluidExport Monitor: public App::DocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(Fluid::Monitor);

public:
    Monitor();

    App::PropertyEnumeration Kind;
    /// In the document's coordinates
    App::PropertyPosition Point;

    static const char* KindEnums[];

    const char* getViewProviderName() const override
    {
        return "Gui::ViewProviderDocumentObject";
    }
};

}  // namespace Fluid
