#ifndef PMI_API_H
#define PMI_API_H

#include <string>
#include <vector>

#include <Mod/PMI/PMIGlobal.h>

namespace App
{
class DocumentObject;
}

namespace PMI
{

class AnnotationSet;
class TolerancedFeature;

/** What the commands do, App side: headless, scriptable, tested
 * (docs/GDandT.md sec 3, 4). The Gui commands are one call each to these.
 */
namespace Api
{
/** A face picked on a part, as an annotation refers to it: the solid body
 * of a PartDesign Body that holds the face, and the face's name in its
 * shape -- a pick lands on the Body's tip feature, (Body, "Hole.Face6"),
 * and must become (Solid, "Face7"). A face of a shape that is no Body stays
 * as it is. False, with \a why, if it is no face of a solid.
 */
PMIExport bool solidReference(App::DocumentObject* obj, const std::string& subname,
                              App::DocumentObject*& solid, std::string& face, std::string& why);

/** The set annotating \a part (a Body, one of its solid bodies, or another
 * shape), made if \a create: in the Body, or beside the shape.
 */
PMIExport AnnotationSet* getSet(App::DocumentObject* part, bool create = true);

/** The set's toleranced feature on exactly these faces of \a solid, made if
 * there is none (sec 4.2: one per face set, shared).
 */
PMIExport TolerancedFeature* feature(AnnotationSet* set, App::DocumentObject* solid,
                                     const std::vector<std::string>& faces,
                                     const char* kind = nullptr);

/// The first datum letter the solid has not used (ASME skips I, O, Q)
PMIExport std::string nextDatumLetter(AnnotationSet* set, App::DocumentObject* solid);
}  // namespace Api

}  // namespace PMI

#endif  // PMI_API_H
