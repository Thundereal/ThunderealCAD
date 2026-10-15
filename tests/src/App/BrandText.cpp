// SPDX-License-Identifier: LGPL-2.1-or-later

#include "gtest/gtest.h"

#include <App/Application.h>
#include <App/BrandText.h>

// The strings are upstream's and this fork's own, as the program shows them.
class BrandTextTest: public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto& config = App::Application::Config();
        auto it = config.find("ExeName");
        hadName = it != config.end();
        if (hadName) {
            savedName = it->second;
        }
        config["ExeName"] = "Thundereal";
    }

    void TearDown() override
    {
        auto& config = App::Application::Config();
        if (hadName) {
            config["ExeName"] = savedName;
        }
        else {
            config.erase("ExeName");
        }
    }

    static QString brand(const char* text)
    {
        return App::brandText(QString::fromUtf8(text));
    }

    bool hadName = false;
    std::string savedName;
};

TEST_F(BrandTextTest, thisProgramTakesTheName)
{
    EXPECT_TRUE(App::isRebranded());
    EXPECT_EQ(brand("You must restart FreeCAD for changes to take effect."),
              QStringLiteral("You must restart Thundereal for changes to take effect."));
    EXPECT_EQ(brand("It will be installed in FreeCAD's macros directory."),
              QStringLiteral("It will be installed in Thundereal's macros directory."));
    EXPECT_EQ(brand("Extend the FreeCAD 3D Window to a Oculus Rift"),
              QStringLiteral("Extend the Thundereal 3D Window to a Oculus Rift"));
    EXPECT_EQ(brand("IFC export in FreeCAD is done by IfcOpenShell. If not, FreeCAD will only "
                    "export IFC2x3."),
              QStringLiteral("IFC export in Thundereal is done by IfcOpenShell. If not, "
                             "Thundereal will only export IFC2x3."));
    EXPECT_EQ(brand("<p>a wide range of formats can be imported into FreeCAD.</p>"),
              QStringLiteral("<p>a wide range of formats can be imported into Thundereal.</p>"));
    EXPECT_EQ(brand("<FreeCAD installation directory>/Resources"),
              QStringLiteral("<Thundereal installation directory>/Resources"));
    EXPECT_EQ(brand("Serve this FreeCAD session to an AI agent"),
              QStringLiteral("Serve this Thundereal session to an AI agent"));
}

TEST_F(BrandTextTest, upstreamKeepsItsName)
{
    const char* kept[] = {
        "The FreeCAD forum, where you can find help from other users",
        "See the FreeCAD Wiki for details about the image.",
        "Frequently Asked Questions on the FreeCAD website",
        "Donate to FreeCAD development",
        "fetched from the FreeCAD wiki, hosted on FreeCAD's GitHub account",
        "Do this only once to sketches created in FreeCAD older than v0.15",
        "This was the standard behaviour before FreeCAD 0.19",
        "Heals faulty Draft objects saved with an earlier version of FreeCAD.",
        "Supported FreeCAD Versions",
        "You won't be able to edit the file with an older FreeCAD version.",
        "The format every FreeCAD version can open.",
        "Installing FreeCAD Addon '{}'",
        "FreeCAD would not be possible without the contributions of",
        "However, at FreeCAD, we believe having a building should not be mandatory",
        "WARNING: This addon requires FreeCAD ",
        "FreeCAD Dark",
        "FreeCAD macro",
        "FreeCAD Tool Library",
        // A name, a path, a link, a compound
        "Use FreeCAD.ActiveDocument.Objects",
        "Use materials from .FreeCAD/Materials directory",
        "Submit a PR at https://github.com/FreeCAD/FreeCAD-addons",
        "FreeCAD-VR",
        "Displayed in the Addon Manager's list of Addons. Should not include the word \"FreeCAD\".",
        "<p><b>Tip</b>: You are currently using FreeCAD version %1. Consider using the <a "
        "href=\"https://github.com/FreeCAD/FreeCAD/releases\">latest development version "
        "%2</a>, which brings all the latest improvements to FreeCAD.</p>",
        "Values between 6 and 8 are considered the best trade-off among FreeCAD users.",
    };
    for (const char* text : kept) {
        EXPECT_EQ(brand(text), QString::fromUtf8(text)) << text;
    }
}

TEST_F(BrandTextTest, thisBuildAndOtherFreeCAD)
{
    EXPECT_EQ(brand("<b>This file will not open in any other FreeCAD.</b>"),
              QStringLiteral("<b>This file will not open in any FreeCAD.</b>"));
    EXPECT_EQ(brand("The compact format is this FreeCAD's own."),
              QStringLiteral("The compact format is Thundereal's own."));
    EXPECT_EQ(brand("Smaller files that load faster, readable by this FreeCAD only."),
              QStringLiteral("Smaller files that load faster, readable by Thundereal only."));
    EXPECT_EQ(brand("Save writes the current one (schema %ld), which only this FreeCAD reads, "
                    "to '%s'. To update this file for other FreeCAD versions, use Save As."),
              QStringLiteral("Save writes the current one (schema %ld), which only Thundereal "
                             "reads, to '%s'. To update this file for FreeCAD versions, use "
                             "Save As."));
    EXPECT_EQ(App::brandText("this FreeCAD was built without the pyodide sandbox host"),
              "Thundereal was built without the pyodide sandbox host");
}

TEST_F(BrandTextTest, translations)
{
    // Every FreeCAD in the source means this program: so it does in the translation.
    EXPECT_EQ(App::brandTranslation(
                  QStringLiteral("You must restart FreeCAD for changes to take effect."),
                  QStringLiteral("Sie muessen FreeCAD neu starten (FreeCAD.Gui).")),
              QStringLiteral("Sie muessen Thundereal neu starten (FreeCAD.Gui)."));
    // A translation's compound is the program too.
    EXPECT_EQ(App::brandTranslation(QStringLiteral("Unhandled FreeCAD exception."),
                                    QStringLiteral("Unbehandelte FreeCAD-Ausnahme.")),
              QStringLiteral("Unbehandelte Thundereal-Ausnahme."));
    // A source that names upstream, or reads differently only in English, leaves
    // its translation alone.
    EXPECT_EQ(App::brandTranslation(QStringLiteral("The FreeCAD Forum"),
                                    QStringLiteral("Das FreeCAD-Forum")),
              QStringLiteral("Das FreeCAD-Forum"));
    EXPECT_EQ(App::brandTranslation(
                  QStringLiteral("<b>This file will not open in any other FreeCAD.</b>"),
                  QStringLiteral("<b>Diese Datei oeffnet kein anderes FreeCAD.</b>")),
              QStringLiteral("<b>Diese Datei oeffnet kein anderes FreeCAD.</b>"));
    EXPECT_EQ(App::brandTranslation(QStringLiteral("Loading from the FreeCAD wiki into FreeCAD"),
                                    QStringLiteral("Laden aus dem FreeCAD-Wiki in FreeCAD")),
              QStringLiteral("Laden aus dem FreeCAD-Wiki in FreeCAD"));
}

TEST_F(BrandTextTest, upstreamNameChangesNothing)
{
    App::Application::Config()["ExeName"] = "FreeCAD";
    EXPECT_FALSE(App::isRebranded());
    EXPECT_EQ(brand("You must restart FreeCAD."), QStringLiteral("You must restart FreeCAD."));
    EXPECT_EQ(App::brandText("only this FreeCAD reads"), "only this FreeCAD reads");
    EXPECT_EQ(App::brandTranslation(QStringLiteral("restart FreeCAD"),
                                    QStringLiteral("FreeCAD neu starten")),
              QStringLiteral("FreeCAD neu starten"));
}
