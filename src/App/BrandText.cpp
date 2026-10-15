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
// FreeCAD: the credits, upstream's own voice, its releases.
bool speaksForUpstream(const QString& text)
{
    static const QRegularExpression re(QStringLiteral(
        "would(?:n't| not) be possible without"
        "|\\bFreeCAD version %\\d"
        "|github\\.com/FreeCAD/FreeCAD/releases"
        "|\\bpart of FreeCAD\\b"
        "|\\bat FreeCAD, we\\b"
        "|\\bmade with FreeCAD\\b"
        "|\\brequires FreeCAD\\s*$"));
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
        "|[Dd]ocumentation|community|users|GitHub|[Vv]ersions?\\b|older\\b"
        "|v?\\d+\\.\\d)"));
    static const QRegularExpression before(QStringLiteral(
        "\\b(?:earlier|older|previous|newer|later|specific|future)\\s+versions?\\s+of\\s+"
        "(?:the\\s+)?\\z"));
    return after.match(text.mid(pos + upstreamLength)).hasMatch()
        || before.match(text.left(pos)).hasMatch();
}

// The FreeCAD at pos only says the thing belongs to FreeCAD, and reads as true
// without it, for either program: the names of the themes and of two formats,
// as a whole string, and an add-on -- "a FreeCAD Addon", "the FreeCAD Addon
// Manager".
bool onlyOwns(const QString& text, qsizetype pos)
{
    static const QRegularExpression whole(
        QStringLiteral("^FreeCAD (?:Classic|Dark|Light|macro|Tool|Tool Library)$"));
    static const QRegularExpression addon(QStringLiteral("\\A [Aa]ddons?\\b"));
    return (pos == 0 && whole.match(text).hasMatch())
        || addon.match(text.mid(pos + upstreamLength)).hasMatch();
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
    int dropped = 0;    ///< FreeCAD that only said whose the thing is, left out
};

// The FreeCAD at pos left out of out, which holds the text before it, with the
// space after it; "a FreeCAD Addon" is "an Addon". Returns where the text
// goes on.
qsizetype dropWord(const QString& text, qsizetype pos, QString& out)
{
    qsizetype next = pos + upstreamLength;
    if (next < text.size() && text.at(next) == u' ') {
        ++next;
    }
    if (out.endsWith(QLatin1String(" a ")) || out == QLatin1String("a ")) {
        out.insert(out.size() - 1, u'n');
    }
    else if (out.endsWith(QLatin1String(" A ")) || out == QLatin1String("A ")) {
        out.insert(out.size() - 1, u'n');
    }
    else if (out.isEmpty() && next < text.size()) {
        // What is left starts the string: "FreeCAD macro" is "Macro"
        out += text.at(next).toUpper();
        ++next;
    }
    return next;
}

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
        else if (onlyOwns(text, pos)) {
            out.text += QStringView(text).mid(last, pos - last);
            last = dropWord(text, pos, out.text);
            ++out.dropped;
            continue;
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

// Every FreeCAD in text the name, but in a name, a path or a link. A hyphen is
// no sign of a name here: text this is asked of means the program by every
// FreeCAD, and a translation joins words with one ("FreeCAD-Datei").
QString nameEvery(const QString& text, const QString& name)
{
    static const QRegularExpression word(QStringLiteral("\\bFreeCAD\\b"));
    QString out;
    qsizetype last = 0;
    auto it = word.globalMatch(text);
    while (it.hasNext()) {
        const qsizetype pos = it.next().capturedStart();
        out += QStringView(text).mid(last, pos - last);
        out += isPartOfName(text, pos, false) ? QString(upstreamName) : name;
        last = pos + upstreamLength;
    }
    out += QStringView(text).mid(last);
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
    if (english.kept > 0 || english.rephrased > 0 || (english.named > 0) == (english.dropped > 0)) {
        return translation;
    }
    if (english.named > 0) {
        return nameEvery(translation, name);
    }
    // Every FreeCAD only said whose the thing is: left out, with the space or
    // hyphen that joined it to its neighbour ("FreeCAD-Makro", "macro FreeCAD")
    static const QRegularExpression word(QStringLiteral("\\bFreeCAD\\b"));
    QString out;
    qsizetype last = 0;
    auto it = word.globalMatch(translation);
    while (it.hasNext()) {
        const qsizetype pos = it.next().capturedStart();
        if (isPartOfName(translation, pos, false)) {
            continue;
        }
        out += QStringView(translation).mid(last, pos - last);
        last = pos + upstreamLength;
        if (last < translation.size()
            && (translation.at(last) == u' ' || translation.at(last) == u'-')) {
            ++last;
        }
        else if (out.endsWith(u' ')) {
            out.chop(1);
        }
    }
    out += QStringView(translation).mid(last);
    return out;
}

QString App::brandEvery(const QString& text)
{
    const QString name = programName();
    if (name == upstreamName) {
        return text;
    }
    return nameEvery(text, name);
}

std::string App::programUrl()
{
    const auto& config = App::Application::Config();
    auto it = config.find("ProgramUrl");
    if (it == config.end() || it->second.empty()) {
        return "https://www.freecad.org";
    }
    return it->second;
}

std::string App::createdBy()
{
    return "Created by " + programName().toStdString() + " <" + programUrl() + ">";
}
