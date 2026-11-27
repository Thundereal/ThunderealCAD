// SPDX-License-Identifier: LGPL-2.1-or-later

#include "WallExchange.h"

using namespace Fluid::Solver;

bool WallExchange::longest(double& found, std::string& why)
{
    std::unique_lock lock(mutex);
    changed.wait(lock, [this] { return isReady || fluidEnded; });
    if (!isReady) {
        why = fluidWhy.empty() ? std::string("the fluid's run ended before it began") : fluidWhy;
        return false;
    }
    found = stretch;
    return true;
}

bool WallExchange::piece(double time,
                         double length,
                         const std::vector<WallState>& states,
                         std::vector<WallLoad>& loads,
                         std::string& why)
{
    std::unique_lock lock(mutex);
    if (!fluidEnded) {
        askedTime = time;
        askedLength = length;
        askedStates = states;
        answered = false;
        asking = true;
        changed.notify_all();
        changed.wait(lock, [this] { return answered || fluidEnded; });
    }
    if (!answered) {
        why = fluidWhy.empty() ? std::string("the fluid's run ended") : fluidWhy;
        return false;
    }
    loads = answer;
    return true;
}

void WallExchange::done()
{
    const std::lock_guard lock(mutex);
    moverDone = true;
    changed.notify_all();
}

void WallExchange::ready(double longest)
{
    const std::lock_guard lock(mutex);
    stretch = longest;
    isReady = true;
    changed.notify_all();
}

bool WallExchange::next(double& time, double& length, std::vector<WallState>& states)
{
    std::unique_lock lock(mutex);
    changed.wait(lock, [this] { return asking || moverDone; });
    if (!asking) {
        return false;
    }
    asking = false;
    time = askedTime;
    length = askedLength;
    states = askedStates;
    return true;
}

void WallExchange::pushed(const std::vector<WallLoad>& loads)
{
    const std::lock_guard lock(mutex);
    answer = loads;
    answered = true;
    changed.notify_all();
}

void WallExchange::ended(const std::string& why)
{
    const std::lock_guard lock(mutex);
    fluidEnded = true;
    fluidWhy = why;
    changed.notify_all();
}
