/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei <realthunder.dev@gmail.com>              *
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

#ifndef BASE_AUTOCOLOR_H
#define BASE_AUTOCOLOR_H

#include <algorithm>
#include <cmath>
#include <vector>

namespace Base {

/** The distance of two hues in [0, 1), around the colour wheel */
inline float hueGap(float a, float b)
{
    const float d = std::fabs(a - b);
    return std::min(d, 1.0F - d);
}

/** A new colour's hue, in [0, 1): the one farthest from every hue in
 * \a taken, so that it stands apart from the colours already in use
 * (docs/HandsOnQueue.md entries 209 and 210). Looked for on a grid of
 * \a steps hues, the lowest of equals: the same hues taken always give the
 * same hue. With nothing taken, 0.
 *
 * Shared by the dress-up panels' edge groups, PartDesign's tag colours and
 * the shapes' automatic colour; each says what is taken. Several colours
 * are picked one after another, each added to \a taken before the next
 * (spreadHues()).
 */
inline float farthestHue(const std::vector<float>& taken, int steps = 360)
{
    float best = 0.0F;
    float bestGap = -1.0F;
    for (int i = 0; i < steps; ++i) {
        const float h = float(i) / float(steps);
        float nearest = 1.0F;
        for (float t : taken) {
            nearest = std::min(nearest, hueGap(h, t));
        }
        if (nearest > bestGap + 1e-6F) {
            bestGap = nearest;
            best = h;
        }
    }
    return best;
}

/// \a h, \a s and \a v, each in [0, 1], of the colour \a r, \a g, \a b
inline void rgbToHsv(float r, float g, float b, float& h, float& s, float& v)
{
    const float high = std::max({r, g, b});
    const float low = std::min({r, g, b});
    const float span = high - low;
    v = high;
    s = high > 0.0F ? span / high : 0.0F;
    if (span <= 0.0F) {
        h = 0.0F;
        return;
    }
    if (high == r) {
        h = (g - b) / span;
    }
    else if (high == g) {
        h = 2.0F + (b - r) / span;
    }
    else {
        h = 4.0F + (r - g) / span;
    }
    h /= 6.0F;
    if (h < 0.0F) {
        h += 1.0F;
    }
}

/// The colour of \a h, \a s and \a v, each in [0, 1]
inline void hsvToRgb(float h, float s, float v, float& r, float& g, float& b)
{
    const float sector = std::fmod(h, 1.0F) * 6.0F;
    const int i = int(sector) % 6;
    const float f = sector - std::floor(sector);
    const float p = v * (1.0F - s);
    const float q = v * (1.0F - s * f);
    const float t = v * (1.0F - s * (1.0F - f));
    switch (i) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
}

/** \a count hues picked one after another by farthestHue(), each apart from
 * \a taken and from those picked before it
 */
inline std::vector<float> spreadHues(std::vector<float> taken, int count)
{
    std::vector<float> hues;
    for (int i = 0; i < count; ++i) {
        hues.push_back(farthestHue(taken));
        taken.push_back(hues.back());
    }
    return hues;
}

} // namespace Base

#endif // BASE_AUTOCOLOR_H
