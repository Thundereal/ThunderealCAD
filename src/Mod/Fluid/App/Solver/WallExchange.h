// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include <Mod/Fluid/FluidGlobal.h>

#include "Backend.h"

namespace Fluid::Solver
{

// Where the fluid's run and the run of what moves its driven walls meet
// (docs/FluidSimulation.md sec 12.9): two threads, each waiting for the
// other in turn. The fluid's session is its WallDriver; the mover calls
// longest() once and then piece() for each stretch, in time order, and
// done() when it has no more. Either side ending -- over, failed, stopped
// -- lets the other go: its call answers false.
class FluidExport WallExchange: public WallDriver
{
public:
    // The mover's side.
    // Waits until the fluid is set up: the longest stretch to ask for, s.
    // false when the fluid's run ended before it was.
    bool longest(double& stretch, std::string& why);
    // The walls' states at 'time'; waits for the fluid to pass time +
    // length and gives what it pushed each wall with on the way. false
    // when the fluid's run has ended: why, where it said.
    bool piece(double time,
               double length,
               const std::vector<WallState>& states,
               std::vector<WallLoad>& loads,
               std::string& why);
    // No more stretches: the fluid's run ends where it is.
    void done();

    // The fluid's side.
    void ready(double longest) override;
    bool next(double& time, double& length, std::vector<WallState>& states) override;
    void pushed(const std::vector<WallLoad>& loads) override;
    void ended(const std::string& why) override;

private:
    std::mutex mutex;
    std::condition_variable changed;
    bool isReady = false;
    double stretch = 0.0;
    bool asking = false;    // a stretch waits for the fluid to take it
    bool answered = false;  // the fluid's loads for the stretch last taken
    bool moverDone = false;
    bool fluidEnded = false;
    std::string fluidWhy;
    double askedTime = 0.0;
    double askedLength = 0.0;
    std::vector<WallState> askedStates;
    std::vector<WallLoad> answer;
};

}  // namespace Fluid::Solver
