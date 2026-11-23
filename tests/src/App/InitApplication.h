#ifndef TEST_APPLICATION_H
#define TEST_APPLICATION_H

#include <array>
#include <App/Application.h>

// The program's own name (tests/CMakeLists.txt passes it as the executables
// get it, src/Main/CMakeLists.txt). A test must start the Application as the
// program does, or it looks for what the program installed per user -- the
// pyodide runtime under <user data>/Pyodide, say -- in another folder: with
// "FreeCAD" hard-coded here, a Thundereal build put its runtime under
// Thundereal\ and the sandbox suites found none ("expression sandbox image is
// not available").
#ifndef FREECAD_APP_NAME
#define FREECAD_APP_NAME "FreeCAD"
#endif

namespace tests
{

static void initApplication()
{
    if (App::Application::GetARGC() == 0) {
        constexpr int argc = 1;
        std::array<const char*, argc> argv {FREECAD_APP_NAME};
        App::Application::Config()["ExeName"] = FREECAD_APP_NAME;
        App::Application::Config()["ExeVendor"] = FREECAD_APP_NAME;
        App::Application::Config()["AppDataSkipVendor"] = "true";
        App::Application::init(argc, const_cast<char**>(argv.data()));  // NOLINT
    }
}

}  // namespace tests

#endif  // TEST_APPLICATION_H
