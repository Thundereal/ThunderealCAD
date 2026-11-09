// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 *   FreeCAD is distributed in the hope that it will be useful, but         *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of             *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
 *   Lesser General Public License for more details.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ***************************************************************************/

#include <algorithm>
#include <cctype>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <App/Application.h>
#include <App/DocumentObject.h>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Mod/Material/App/Materials.h>
#include <Mod/Part/App/PartFeature.h>

#include "ContactFriction.h"
#include "MassProperties.h"

using namespace Assembly;

namespace
{

std::string lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

// One file's families and pairs onto the table's: its families tried
// first, its pairs in place of the same pairs'
void readInto(const std::string& path,
              std::vector<FrictionTable::Family>& families,
              std::vector<FrictionTable::Pair>& pairs)
{
    QFile file(QString::fromStdString(path));
    if (!file.exists()) {
        return;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        Base::Console().warning("Assembly: %s cannot be read\n", path.c_str());
        return;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        Base::Console().warning("Assembly: %s is no friction table: %s\n",
                                path.c_str(),
                                error.errorString().toStdString().c_str());
        return;
    }
    const QJsonObject root = document.object();
    std::vector<FrictionTable::Family> read;
    for (const QJsonValue& value : root.value(QStringLiteral("families")).toArray()) {
        const QJsonObject entry = value.toObject();
        FrictionTable::Family family;
        family.name = entry.value(QStringLiteral("name")).toString().toStdString();
        for (const QJsonValue& word : entry.value(QStringLiteral("words")).toArray()) {
            family.words.push_back(lower(word.toString().toStdString()));
        }
        if (!family.name.empty() && !family.words.empty()) {
            read.push_back(std::move(family));
        }
    }
    families.insert(families.begin(), read.begin(), read.end());
    for (const QJsonValue& value : root.value(QStringLiteral("pairs")).toArray()) {
        const QJsonObject entry = value.toObject();
        FrictionTable::Pair pair;
        pair.a = entry.value(QStringLiteral("a")).toString().toStdString();
        pair.b = entry.value(QStringLiteral("b")).toString().toStdString();
        pair.muStatic = entry.value(QStringLiteral("static")).toDouble(-1.0);
        pair.muDynamic = entry.value(QStringLiteral("dynamic")).toDouble(pair.muStatic);
        pair.source = entry.value(QStringLiteral("source")).toString().toStdString();
        if (pair.a.empty() || pair.b.empty() || pair.muStatic < 0.0 || pair.muDynamic < 0.0) {
            Base::Console().warning("Assembly: %s has a pair without its two families and "
                                    "coefficients, left out\n",
                                    path.c_str());
            continue;
        }
        auto same = std::find_if(pairs.begin(), pairs.end(), [&](const FrictionTable::Pair& p) {
            return (p.a == pair.a && p.b == pair.b) || (p.a == pair.b && p.b == pair.a);
        });
        if (same != pairs.end()) {
            *same = pair;
        }
        else {
            pairs.push_back(pair);
        }
    }
}

}  // namespace

const FrictionTable& FrictionTable::instance()
{
    static const FrictionTable table =
        read({App::Application::getHomePath() + "Mod/Assembly/ContactFriction.json",
              App::Application::getUserAppDataDir() + "Assembly/ContactFriction.json"});
    return table;
}

FrictionTable FrictionTable::read(const std::vector<std::string>& paths)
{
    FrictionTable table;
    for (const auto& path : paths) {
        readInto(path, table.families_, table.pairs_);
    }
    return table;
}

std::string FrictionTable::familyOf(const std::string& name,
                                    const std::string& kind,
                                    const std::vector<std::string>& tags) const
{
    std::vector<std::string> said {lower(name), lower(kind)};
    for (const auto& tag : tags) {
        said.push_back(lower(tag));
    }
    for (const Family& family : families_) {
        for (const auto& word : family.words) {
            for (const auto& text : said) {
                if (text.find(word) != std::string::npos) {
                    return family.name;
                }
            }
        }
    }
    return {};
}

std::string FrictionTable::familyOf(App::DocumentObject* component) const
{
    for (App::DocumentObject* object : shapeFeaturesOf(component)) {
        auto* feature = Base::freecad_dynamic_cast<::Part::Feature>(object);
        if (!feature) {
            continue;
        }
        try {
            const Materials::Material& card = feature->ShapeMaterial.getValue();
            // The Default card, every shape's until a material is chosen, and
            // its appearance-only parent are no material (densityOf())
            const QString uuid = card.getUUID();
            if (uuid == QLatin1String("7f9fd73b-50c9-41d8-b7b2-575a030c1eeb")
                || uuid == QLatin1String("5dbb7be6-8b63-479b-ab4c-87be02ead973")) {
                continue;
            }
            const QString kindName = QStringLiteral("KindOfMaterial");
            const std::string kind = card.hasPhysicalProperty(kindName)
                ? card.getPhysicalValueString(kindName).toStdString()
                : std::string();
            std::vector<std::string> tags;
            for (const QString& tag : card.getTags()) {
                tags.push_back(tag.toStdString());
            }
            const std::string family = familyOf(card.getName().toStdString(), kind, tags);
            if (!family.empty()) {
                return family;
            }
        }
        catch (const Base::Exception&) {
            continue;
        }
    }
    return {};
}

const FrictionTable::Pair* FrictionTable::pairOf(const std::string& a, const std::string& b) const
{
    for (const Pair& pair : pairs_) {
        if ((pair.a == a && pair.b == b) || (pair.a == b && pair.b == a)) {
            return &pair;
        }
    }
    return nullptr;
}
