#ifndef PMI_CALLOUT_H
#define PMI_CALLOUT_H

#include <string>

#include <gp_Ax3.hxx>
#include <gp_Pnt.hxx>

#include <App/PropertyGeo.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>
#include <Mod/Part/App/PropertyTopoShape.h>

#include "Annotation.h"

namespace PMI
{

class Drawing;
class TolerancedFeature;

/** What an annotation looks like (docs/GDandT.md sec 6): drawn from its
 * semantics on every recompute, never the other way round.
 *
 * The drawing lies in Plane -- x the reading direction, y up, z toward the
 * reader -- with a leader to the feature. Plane is placed from the feature
 * (AutoPlace) until the user moves it. Presentation is the result: strokes as
 * edges and fills as faces, in the part's coordinates, which the view draws,
 * STEP writes tessellated and a drawing reads. EquivalentText is the CAx-IF
 * "equivalent unicode string" of the annotation, for checking.
 */
class PMIExport Callout: public Annotation
{
    PROPERTY_HEADER_WITH_OVERRIDE(PMI::Callout);

public:
    Callout();

    App::PropertyPlacement Plane;
    App::PropertyLength TextHeight;
    App::PropertyBool AutoPlace;
    App::PropertyBool ShowLeader;
    Part::PropertyPartShape Presentation;
    App::PropertyString EquivalentText;

    enum LeaderStyle
    {
        NoLeader,
        /// Ending in an arrowhead: frames, dimensions, notes
        ArrowLeader,
        /// Ending in a filled triangle on the feature: a datum (ISO 5459)
        TriangleLeader,
    };

    /// The toleranced feature the callout is drawn to, or null
    virtual TolerancedFeature* calloutFeature() const = 0;
    /// Draw the callout in its plane's coordinates at text height \a h
    virtual void layout(Drawing& d, double h) const = 0;
    /// The CAx-IF equivalent string
    virtual std::u32string equivalentText() const = 0;
    virtual LeaderStyle leaderStyle() const
    {
        return ArrowLeader;
    }

    /// Lay the callout out again: Plane (if AutoPlace), Presentation,
    /// EquivalentText
    void updatePresentation();

    /// The plane a callout on \a feature takes: the face's own if planar,
    /// else the standard view looking most squarely at it; \a target is the
    /// point on the feature the leader goes to
    static bool defaultPlane(const TolerancedFeature* feature, double h, gp_Ax3& plane,
                             gp_Pnt& target);
    /// The point on the feature a leader goes to
    static bool targetPoint(const TolerancedFeature* feature, gp_Pnt& target);

    /// A number as a drawing writes it: \a decimals places, or (-1) as few
    /// as it needs
    static std::u32string number(double value, int decimals = -1);

protected:
    void onChanged(const App::Property* prop) override;
    void onDocumentRestored() override;
    /// How many callouts before this one in the set land near \a mine on
    /// a parallel plane
    int autoSlot(const gp_Ax3& mine, double h) const;

private:
    bool _placing = false;
};

}  // namespace PMI

#endif  // PMI_CALLOUT_H
