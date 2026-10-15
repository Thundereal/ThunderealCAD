// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PreCompiled.h"

#ifndef _PreComp_
# include <QRegularExpression>
#endif

#include "Application.h"
#include "BrandText.h"

namespace
{

const QLatin1String upstreamName("FreeCAD");
constexpr qsizetype upstreamLength = 7;

QString programName()
{
    const auto& config = App::Application::Config();
    auto it = config.find("ExeName");
    if (it == config.end() || it->second.empty()) {
        return QString(upstreamName);
    }
    return QString::fromStdString(it->second);
}

// A string that speaks for upstream, or about it as a whole, keeps every
// FreeCAD: the credits, upstream's own voice, its releases, the names of its
// formats and of its themes.
bool speaksForUpstream(const QString& text)
{
    static const QRegularExpression re(QStringLiteral(
        "would(?:n't| not) be possible without"
        "|\\bFreeCAD version %\\d"
        "|github\\.com/FreeCAD/FreeCAD/releases"
        "|\\bpart of FreeCAD\\b"
        "|\\bat FreeCAD, we\\b"
        "|\\bmade with FreeCAD\\b"
        "|\\brequires FreeCAD\\s*$"
        "|^FreeCAD (?:Classic|Dark|Light|macro|Tool|Tool Library)$"));
    return re.match(text).hasMatch();
}

// The FreeCAD at pos is part of a name, a path or a link: the Python module
// (FreeCAD.ActiveDocument), a C++ scope (FreeCAD::), a directory (.FreeCAD/Mod),
// a URL (github.com/FreeCAD/FreeCAD-addons), and in English a compound
// (FreeCAD-VR). A translation joins words with a hyphen where English does not
// -- "FreeCAD-Datei" is a FreeCAD file -- so there a hyphen is no sign of a name.
bool isPartOfName(const QString& text, qsizetype pos, bool hyphenJoinsName = true)
{
    // The word itself, in quotes: Should not include the word "FreeCAD"
    if (pos > 0 && pos + upstreamLength < text.size() && text.at(pos - 1) == u'"'
        && text.at(pos + upstreamLength) == u'"') {
        return true;
    }
    if (pos > 0) {
        const QChar before = text.at(pos - 1);
        if (before == u'.' || before == u'/' || before == u'\\' || before == u'@'
            || before == u':' || (hyphenJoinsName && before == u'-')) {
            return true;
        }
    }
    const qsizetype end = pos + upstreamLength;
    if (end < text.size()) {
        const QChar after = text.at(end);
        if (after == u'/' || after == u'\\') {
            return true;
        }
        if ((after == u'.' || after == u':' || (hyphenJoinsName && after == u'-'))
            && end + 1 < text.size()) {
            const QChar next = text.at(end + 1);
            if (next.isLetterOrNumber() || next == u'_' || (after == u':' && next == u':')) {
                return true;
            }
        }
    }
    return false;
}

// The FreeCAD at pos names upstream itself: its sites and its people, its
// add-ons, one of its versions.
bool namesUpstream(const QString& text, qsizetype pos)
{
    static const QRegularExpression after(QStringLiteral(
        "\\A(?:'s)?[ -]?(?:[Ww]iki|[Ff]orums?|[Ww]ebsite|FAQ|[Tt]eam|development"
        "|[Dd]ocumentation|community|users|GitHub|[Aa]ddons?\\b|[Vv]ersions?\\b|older\\b"
        "|v?\\d+\\.\\d)"));
    static const QRegularExpression before(QStringLiteral(
        "\\b(?:earlier|older|previous|newer|later|specific|future)\\s+versions?\\s+of\\s+"
        "(?:the\\s+)?\\z"));
    return after.match(text.mid(pos + upstreamLength)).hasMatch()
        || before.match(text.left(pos)).hasMatch();
}

// "this FreeCAD" ends at pos + 7 and is this build as against upstream: "only
// this FreeCAD reads", "this FreeCAD's own", "readable by this FreeCAD only".
// "this FreeCAD session" is not -- that one keeps its "this".
bool isThisBuild(const QString& text, qsizetype pos)
{
    static const QRegularExpression after(QStringLiteral(
        "\\A(?:'s|\\s+(?:reads|supports|was|cannot|can|only|knows)\\b|\\s*[.,;:)]|\\s*\\z)"));
    return after.match(text.mid(pos + upstreamLength)).hasMatch();
}

// The word before pos is `word`, as a whole word followed by one space.
bool precededBy(const QString& text, qsizetype pos, QLatin1String word)
{
    const qsizetype start = pos - word.size() - 1;
    if (start < 0 || text.at(pos - 1) != u' ' || QStringView(text).mid(start, word.size()) != word) {
        return false;
    }
    return start == 0 || !(text.at(start - 1).isLetterOrNumber() || text.at(start - 1) == u'_');
}

struct Branded
{
    QString text;
    int named = 0;      ///< FreeCAD replaced by the name
    int kept = 0;       ///< FreeCAD that names upstream, left
    int rephrased = 0;  ///< "this FreeCAD" or "other FreeCAD", which only English has
};

Branded brand(const QString& text, const QString& name)
{
    Branded out;
    if (!text.contains(upstreamName)) {
        out.text = text;
        return out;
    }
    if (speaksForUpstream(text)) {
        out.text = text;
        out.kept = 1;
        return out;
    }
    static const QRegularExpression word(QStringLiteral("\\bFreeCAD\\b"));
    qsizetype last = 0;
    auto it = word.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        const qsizetype pos = match.capturedStart();
        if (isPartOfName(text, pos)) {
            out.text += QStringView(text).mid(last, pos + upstreamLength - last);
            ++out.kept;
        }
        else if (precededBy(text, pos, QLatin1String("this")) && isThisBuild(text, pos)) {
            out.text += QStringView(text).mid(last, pos - 5 - last);
            out.text += name;
            ++out.rephrased;
        }
        else if (precededBy(text, pos, QLatin1String("other"))) {
            out.text += QStringView(text).mid(last, pos - 6 - last);
            out.text += upstreamName;
            ++out.rephrased;
        }
        else if (namesUpstream(text, pos)) {
            out.text += QStringView(text).mid(last, pos + upstreamLength - last);
            ++out.kept;
        }
        else {
            out.text += QStringView(text).mid(last, pos - last);
            out.text += name;
            ++out.named;
        }
        last = pos + upstreamLength;
    }
    out.text += QStringView(text).mid(last);
    return out;
}

}  // namespace

bool App::isRebranded()
{
    return programName() != upstreamName;
}

QString App::brandText(const QString& text)
{
    const QString name = programName();
    if (name == upstreamName) {
        return text;
    }
    return brand(text, name).text;
}

std::string App::brandText(const char* text)
{
    if (!text) {
        return {};
    }
    if (!isRebranded()) {
        return text;
    }
    return brandText(QString::fromUtf8(text)).toStdString();
}

QString App::brandTranslation(const QString& source, const QString& translation)
{
    const QString name = programName();
    if (name == upstreamName || !source.contains(upstreamName)) {
        return translation;
    }
    const Branded english = brand(source, name);
    if (english.named == 0 || english.kept > 0 || english.rephrased > 0) {
        return translation;
    }
    static const QRegularExpression word(QStringLiteral("\\bFreeCAD\\b"));
    QString out;
    qsizetype last = 0;
    auto it = word.globalMatch(translation);
    while (it.hasNext()) {
        const qsizetype pos = it.next().capturedStart();
        out += QStringView(translation).mid(last, pos - last);
        out += isPartOfName(translation, pos, false) ? QString(upstreamName) : name;
        last = pos + upstreamLength;
    }
    out += QStringView(translation).mid(last);
    return out;
}
