// SPDX-License-Identifier: LGPL-2.1-or-later

// A TechDraw reference's saved signature now ends with the centre of the view's
// sources it was taken under: "F 100 -25 5 0 @ 30 10 2.5". A build from before
// that reads the signature with DrawViewPart::sameSignature() as it was; this
// keeps that reading verbatim (main 92b6db803e) and shows it gets from a tagged
// signature exactly the kind and the numbers it gets from the same signature
// untagged -- so an older build compares, and risks, what it always did.

#include "gtest/gtest.h"

#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace
{

// DrawViewPart::sameSignature()'s reading, as main has it: the kind is the
// first character, the numbers are read from the rest until one does not read
std::vector<double> oldRead(const std::string& text)
{
    std::istringstream in(text.substr(1));
    std::vector<double> values;
    double value;
    while (in >> value) {
        values.push_back(value);
    }
    return values;
}

// and its comparison, with the points compared as gp_Pnt::IsEqual would
bool oldSame(const std::string& a, const std::string& b, double tolerance = 1e-7)
{
    if (a.empty() || b.empty() || a[0] != b[0]) {
        return false;
    }
    const std::vector<double> va = oldRead(a);
    const std::vector<double> vb = oldRead(b);
    if (va.size() != vb.size() || va.empty()) {
        return false;
    }
    for (size_t i = 0; i < va.size(); i++) {
        if (std::fabs(va[i] - vb[i]) > tolerance) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST(SignatureTagOldReader, aTaggedFaceReadsAsItsNumbers)
{
    const std::vector<double> want {100, -25, 5, 0};
    EXPECT_EQ(oldRead("F 100 -25 5 0"), want);
    EXPECT_EQ(oldRead("F 100 -25 5 0 @ 30 10 2.5"), want);
}

TEST(SignatureTagOldReader, aTaggedEdgeAndVertexReadAsTheirNumbers)
{
    const std::string edge = "E 1 2 0 4 6 0 2.5 4 0";
    EXPECT_EQ(oldRead(edge + " @ -12.5 7 0"), oldRead(edge));
    EXPECT_EQ(oldRead(edge).size(), 9u);
    const std::string vertex = "V 1.25 -3 0";
    EXPECT_EQ(oldRead(vertex + " @ 1e-17 2.220446049e-17 5"), oldRead(vertex));
    EXPECT_EQ(oldRead(vertex).size(), 3u);
}

TEST(SignatureTagOldReader, anOlderBuildComparesTheNumbersAsBefore)
{
    // the same numbers: the same element, whatever the tags say
    EXPECT_TRUE(oldSame("F 100 -25 5 0 @ 30 10 2.5", "F 100 -25 5 0"));
    EXPECT_TRUE(oldSame("F 100 -25 5 0 @ 30 10 2.5", "F 100 -25 5 0 @ 22.5 5 2.5"));
    // other numbers: another element, as before -- a tag never makes a match
    EXPECT_FALSE(oldSame("F 100 -17.5 0 0 @ 22.5 5 2.5", "F 100 -25 5 0 @ 30 10 2.5"));
    EXPECT_FALSE(oldSame("F 100 -25 5 0 @ 30 10 2.5", "E 100 -25 5 0 @ 30 10 2.5"));
}
