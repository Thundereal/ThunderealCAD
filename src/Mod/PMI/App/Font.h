#ifndef PMI_FONT_H
#define PMI_FONT_H

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <TopoDS_Shape.hxx>

#include <Mod/PMI/PMIGlobal.h>

namespace PMI
{

/** The lettering of an annotation: osifont (ISO 3098), read with FreeType,
 * no Qt -- a headless export writes the same text the view shows
 * (docs/GDandT.md sec 6.1).
 *
 * A glyph is made once for each height, as planar faces with its origin on
 * the baseline, and placed by location: every "0" of a height is one shape.
 * Curves are flattened -- a presentation is drawn and tessellated, never
 * machined.
 */
class PMIExport Font
{
public:
    /// osifont, from the PMI module's resources
    static Font& instance();

    struct Glyph
    {
        /// Faces at the capital height asked, the origin on the baseline;
        /// null for a blank (or a character the font lacks, see missing)
        TopoDS_Shape faces;
        /// At capital height 1
        double advance = 0.0;
        bool missing = false;
    };

    bool isValid() const
    {
        return _valid;
    }
    const std::string& error() const
    {
        return _error;
    }

    /// The glyph at capital \a height: made once for each, and placed by
    /// translation only -- a location that scales is refused by OCCT 8's
    /// algorithms
    const Glyph& glyph(char32_t c, double height = 1.0);
    /// Whether the font draws \a c
    bool has(char32_t c);

    /// The width of \a text at capital height \a height
    double width(const std::u32string& text, double height);

    /// \a text as faces at capital height \a height, the baseline's start at
    /// (x, y) of the XY plane
    TopoDS_Shape text(const std::u32string& text, double height, double x, double y);

    /// UTF-8 to code points
    static std::u32string fromUtf8(const std::string& s);
    /// Code points to UTF-8
    static std::string toUtf8(const std::u32string& s);

private:
    Font();
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    Glyph make(char32_t c, double height);

    struct Private;
    Private* d;
    bool _valid = false;
    std::string _error;
    double _capHeight = 1.0;
    std::map<std::pair<char32_t, double>, Glyph> _cache;
    std::mutex _mutex;
};

}  // namespace PMI

#endif  // PMI_FONT_H
