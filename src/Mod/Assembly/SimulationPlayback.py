# SPDX-License-Identifier: LGPL-2.1-or-later
# /**************************************************************************
#                                                                           *
#    Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
#                                                                           *
#    This file is part of FreeCAD.                                          *
#                                                                           *
#    FreeCAD is free software: you can redistribute it and/or modify it     *
#    under the terms of the GNU Lesser General Public License as            *
#    published by the Free Software Foundation, either version 2.1 of the   *
#    License, or (at your option) any later version.                        *
#                                                                           *
#    FreeCAD is distributed in the hope that it will be useful, but         *
#    WITHOUT ANY WARRANTY; without even the implied warranty of             *
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
#    Lesser General Public License for more details.                        *
#                                                                           *
#    You should have received a copy of the GNU Lesser General Public       *
#    License along with FreeCAD. If not, see                                *
#    <https://www.gnu.org/licenses/>.                                       *
#                                                                           *
# **************************************************************************/

"""Playback: how a run's frames play and its video goes (docs/
AssemblyDynamics.md sec 8.6), and how its curves are drawn so that a peak
narrower than a pixel is still seen.

A run's frames are its output frames, an output step apart, and with the
study's ImpactFrames the frames through each impact between them, as close
as the impact's force needs -- microseconds apart, where a steel block lands
on a lever. Played an output step a video frame (Normal), an impact is over
between two video frames, unseen. Auto plays at Normal where the frames are
an output step apart and slows where they come closer -- as far as each
frame shown for a video frame where the contact forces swing the most in
the run, as much less as they swing less -- the speed eased into and out
of: no more than EASE times from one video frame to the next. The plots go by the video's time
(window()): the run at Normal's scale, a part played slower stretched as
much as it is slower, with a timeline under them whose ticks, an even step
of the run's time apart, spread as the speed eases in and close up as it
eases out (slowSpans())."""

import bisect
import math

# The study's PlaybackSpeed, as the C++ Simulation's enumeration has them
Speeds = ("Auto", "Normal", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64", "1/128", "1/256")

# Auto: the speed changes by at most this from one video frame to the next
EASE = 1.5
# Auto: no slower than this of Normal, however close the frames
SLOWEST = 1.0 / 4096.0
# Auto: frames this far either side are averaged for how close they come --
# an impact's frames are closest about its peak, and one pair a fraction of
# a microsecond apart slowed the whole impact to it
SPREAD = 2


def speedFactor(speed):
    """How fast a PlaybackSpeed plays against Normal: 1 for Auto and Normal"""
    if isinstance(speed, str) and speed.startswith("1/"):
        try:
            return 1.0 / float(speed[2:])
        except ValueError:
            pass
    return 1.0


def onOutput(times, step, frame):
    """Whether a frame is at one of the run's output times, not one between
    them through an impact"""
    if not 0 <= frame < len(times) or not step > 0.0:
        return True
    steps = (times[frame] - times[0]) / step
    return abs(steps - round(steps)) < 1e-6


class Playback:
    """The video frames of a run's frames: their times, and how fast each
    plays (the run's seconds a second of video, its rate).

    times: the frames' times, s; step: the output step, s; fps: the video's
    frames a second; speed: a PlaybackSpeed."""

    def __init__(self, times, step, fps, speed="Auto", forces=None, slow=1.0):
        """forces: Auto, a value a frame whose swing says how much to slow
        (the contact forces' total, contactForces()); None, each place the
        frames come closer slowed as much as they do.

        slow: Auto, how slow where the forces swing the most: video frames a
        frame there takes (the study's SlowMotion)"""
        self.times = list(times)
        self.forces = list(forces) if forces is not None else None
        self.slow = slow if slow > 0.0 else 1.0
        self.fps = max(float(fps), 1.0)
        self.step = step if step > 0.0 else self._guessStep()
        # Normal: an output step a video frame
        self.normal = self.step * self.fps
        self.auto = speed == "Auto"
        self.base = self.normal * speedFactor(speed)
        self._played = None
        self._slopes()

    def _guessStep(self):
        gaps = [b - a for a, b in zip(self.times, self.times[1:]) if b > a]
        return max(gaps) if gaps else 1.0

    def _slopes(self):
        """Auto: each gap's own rate, and the largest at each frame the
        easing allows (the least over the gaps of their rate and the run's
        time to them, EASE a video frame)"""
        n = len(self.times)
        self.wanted = []
        self.caps = []
        if not self.auto or n < 2:
            return
        # A video frame slower by EASE takes in r / fps of the run: the rate
        # changes by fps (EASE - 1) / EASE a second of the run
        self.slope = self.fps * (EASE - 1.0) / EASE
        gaps = [b - a for a, b in zip(self.times, self.times[1:])]
        floor = self.base * SLOWEST
        share = self._shares(len(gaps))
        for j in range(len(gaps)):
            lo = max(0, j - SPREAD)
            hi = min(len(gaps), j + SPREAD + 1)
            close = sum(gaps[lo:hi]) / (hi - lo)
            # As slow as the frames come close, by the share of the run's
            # largest swing of the forces this one is: a lever rattling on
            # its posts at 59 N, between a block's bounces, slowed as much
            # as the block's landing at 528 N, and the speed never got back
            # to Normal between the rattles 3 to 5 ms apart
            dense = min(self.base, max(floor, close * self.fps / self.slow))
            self.wanted.append(self.base * (dense / self.base) ** share[j])
        caps = [self.wanted[0]]
        for j in range(1, n - 1):
            caps.append(min(self.wanted[j - 1], self.wanted[j]))
        caps.append(self.wanted[-1])
        for j in range(1, n):
            caps[j] = min(caps[j], caps[j - 1] + self.slope * (self.times[j] - self.times[j - 1]))
        for j in range(n - 2, -1, -1):
            caps[j] = min(caps[j], caps[j + 1] + self.slope * (self.times[j + 1] - self.times[j]))
        self.caps = caps

    def _shares(self, count):
        """Each gap's share of the run's largest swing of the forces: how
        far they go, least to most, within an output step either side of
        it, over the most they go anywhere. All 1 without forces."""
        if self.forces is None or len(self.forces) < len(self.times):
            return [1.0] * count
        swings = []
        for j in range(count):
            a = bisect.bisect_left(self.times, self.times[j] - self.step)
            b = bisect.bisect_right(self.times, self.times[j + 1] + self.step)
            near = self.forces[a:b]
            swings.append(max(near) - min(near) if near else 0.0)
        largest = max(swings) if swings else 0.0
        if not largest > 0.0:
            return [1.0] * count
        return [s / largest for s in swings]

    def rate(self, time):
        """How fast the run plays at time: its seconds a second of video"""
        if not self.auto or len(self.times) < 2:
            return self.base
        j = bisect.bisect_right(self.times, time) - 1
        j = min(max(j, 0), len(self.times) - 2)
        t0, t1 = self.times[j], self.times[j + 1]
        return max(
            self.base * SLOWEST,
            min(
                self.wanted[j],
                self.caps[j] + self.slope * max(0.0, time - t0),
                self.caps[j + 1] + self.slope * max(0.0, t1 - time),
            ),
        )

    def frames(self):
        """The video's frames: (time, rate) each, from the first frame's time
        to the last's. Normal: the output times."""
        if not self.times:
            return []
        start, end = self.times[0], self.times[-1]
        if not end > start:
            return [(start, self.base)]
        out = []
        if not self.auto:
            count = int(math.floor((end - start) * self.fps / self.base + 1e-6))
            out = [(start + k * self.base / self.fps, self.base) for k in range(count + 1)]
        else:
            time = start
            while time < end:
                rate = self.rate(time)
                out.append((time, rate))
                # The rate halfway through the video frame
                time += self.rate(time + 0.5 * rate / self.fps) / self.fps
        if end - out[-1][0] > 1e-9 * (end - start):
            out.append((end, self.rate(end)))
        return out

    def played(self):
        """frames(), kept: the run's time of each video frame"""
        if self._played is None:
            self._played = [t for t, _ in self.frames()]
        return self._played

    def videoTime(self, time):
        """When the video shows the run's time, s from its start: between
        two video frames, on the line between them"""
        played = self.played()
        if len(played) < 2:
            return 0.0
        k = bisect.bisect_right(played, time) - 1
        if k < 0:
            return 0.0
        if k >= len(played) - 1:
            return (len(played) - 1) / self.fps
        return (k + (time - played[k]) / (played[k + 1] - played[k])) / self.fps

    def runTime(self, video):
        """The run's time the video shows at video, s from its start"""
        played = self.played()
        if len(played) < 2:
            return played[0] if played else 0.0
        x = min(max(video * self.fps, 0.0), len(played) - 1.0)
        k = min(int(math.floor(x)), len(played) - 2)
        return played[k] + (x - k) * (played[k + 1] - played[k])

    def duration(self):
        """How long the video goes, s"""
        return max(0, len(self.played()) - 1) / self.fps

    def normalDuration(self):
        """How long the whole run goes at Normal, s"""
        if len(self.times) < 2:
            return 0.0
        return (self.times[-1] - self.times[0]) / self.normal

    def window(self, time, zoom=1.0):
        """The video's time a plot shows about time, (start, end) in s of
        it: as long as the whole run goes at Normal, over zoom. A plot by the
        video's time draws the run at Normal's scale, and a part it plays
        slower at as much more: Auto's slowed parts alone are stretched, not
        the whole of the run. All of it at Normal unzoomed; time in the
        middle where it can be"""
        total = self.duration()
        width = self.normalDuration() / zoom
        # The whole video within round-off: its own ends
        if width >= total * (1.0 - 1e-9) or not width > 0.0:
            return 0.0, total
        low = min(max(self.videoTime(time) - 0.5 * width, 0.0), total - width)
        return low, low + width

    def slowSpans(self):
        """Auto: where the run plays slower than Normal, eased into and out
        of: (the time it begins to slow, the time it is back at Normal, the
        slowest rate) each"""
        if not self.auto:
            return []
        played = self.played()
        rates = [self.rate(t) for t in played]
        # From where it is a tenth slower, and only those it slows to half
        # or less in: a lever's rattle at a fiftieth of the run's largest
        # swing slows it a few percent, not worth a mark
        eased = self.base * 0.9
        spans = []
        k = 0
        while k < len(played):
            if not rates[k] < eased:
                k += 1
                continue
            first = k
            while k < len(played) and rates[k] < eased:
                k += 1
            slowest = min(rates[first:k])
            if slowest < 0.5 * self.base:
                spans.append(
                    (played[max(first - 1, 0)], played[min(k, len(played) - 1)], slowest)
                )
        return spans


def niceStep(least):
    """The least of 1, 2 and 5 times a power of ten that is least or more:
    the run's time between a timeline's ticks"""
    if not least > 0.0:
        return 1.0
    power = 10.0 ** math.floor(math.log10(least))
    for factor in (1.0, 2.0, 5.0, 10.0):
        if factor * power >= least * (1.0 - 1e-12):
            return factor * power
    return 10.0 * power


def contactForces(results):
    """A run's contact sets' forces, each frame's total, N: what Auto slows
    by (Playback's forces); None when the run reports none"""
    times = results.get("Times", []) if results else []
    channels = [
        c for c in (results or {}).get("Channels", []) if c.get("Quantity") == "ContactForce"
    ]
    if not times or not channels:
        return None
    total = [0.0] * len(times)
    for channel in channels:
        for k, value in enumerate(channel["Values"][: len(times)]):
            if value is not None:
                total[k] += value.Length * 1e-3
    return total


def speedText(rate):
    """A rate as against real time: '1/13 real time', '2x real time'"""
    if not rate > 0.0:
        return ""
    if rate >= 0.995:
        return "%.3gx real time" % rate
    return "1/%s real time" % ("%.3g" % (1.0 / rate)).replace("e+0", "e").replace("e+", "e")


def timeText(time, rate, fps):
    """A frame's time, to as many places as one video frame's change shows"""
    step = rate / max(fps, 1.0)
    places = 2 if not step > 0.0 else min(9, max(2, int(math.ceil(-math.log10(step))) + 1))
    return "%.*f s" % (places, time)


def peakSafe(times, values, t0, t1, columns, bridge=()):
    """The points to draw values over times by, between t0 and t1 across
    columns pixels: of the values in each column, the first, the least, the
    largest and the last (M4, Jugel et al. 2014) -- a peak narrower than a
    pixel is drawn to its height, which every other value's line hides. The
    point each side of the window too, the lines to its edges. A list of
    runs of (time, value), a None value ending one -- but at a frame of
    bridge, passed over: a stress series solved at an impact's peak alone
    has none at its other frames, which are not a gap in it."""
    n = min(len(times), len(values))
    if n == 0 or not t1 > t0:
        return []
    first = max(0, bisect.bisect_left(times, t0, 0, n) - 1)
    last = min(n, bisect.bisect_right(times, t1, 0, n) + 1)
    # The values each side, past those passed over
    while first > 0 and values[first] is None and first in bridge:
        first -= 1
    while last < n and values[last - 1] is None and last - 1 in bridge:
        last += 1
    scale = columns / (t1 - t0)
    runs = []
    run = []
    bucket = []

    def flush():
        if not bucket:
            return
        picked = {bucket[0], bucket[-1]}
        picked.add(min(bucket, key=lambda i: values[i]))
        picked.add(max(bucket, key=lambda i: values[i]))
        run.extend((times[i], values[i]) for i in sorted(picked))
        bucket.clear()

    column = None
    for i in range(first, last):
        if values[i] is None:
            if i in bridge:
                continue
            flush()
            if run:
                runs.append(run)
            run = []
            column = None
            continue
        c = int(math.floor((times[i] - t0) * scale))
        if c != column:
            flush()
            column = c
        bucket.append(i)
    flush()
    if run:
        runs.append(run)
    return runs


def impactPeaks(times, step, forces):
    """The frames at the peaks of forces between the output frames: in each
    run of frames between two output frames, those whose force is above the
    one before and not below the one after"""
    peaks = []
    n = min(len(times), len(forces))
    for k in range(1, n - 1):
        if onOutput(times, step, k):
            continue
        if forces[k] > forces[k - 1] and forces[k] >= forces[k + 1]:
            peaks.append(k)
    return peaks
