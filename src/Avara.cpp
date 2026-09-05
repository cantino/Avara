/*
    Copyright ©1994-1996, Juri Munkki
    All rights reserved.

    File: Avara.c
    Created: Sunday, November 13, 1994, 21:18
    Modified: Monday, September 2, 1996, 17:39
*/

#include "AvaraTCP.h"
#include "CAvaraApp.h"
#include "CAvaraGame.h"
#include "CPlayerManager.h"
#include "CBSPPart.h"
#include "FastMat.h"
#include "Preferences.h"
#include "BasePath.h"
#include "Logging.h"
#include "signal.h"
#if (!defined(__linux__) || defined(__GLIBC__)) && !defined(__EMSCRIPTEN__)
#include "signalhandling.hpp"
#endif
#ifdef _WIN32
#include <Windows.h>
#include <ShellAPI.h>
typedef enum PROCESS_DPI_AWARENESS {
    PROCESS_DPI_UNAWARE = 0,
    PROCESS_SYSTEM_DPI_AWARE = 1,
    PROCESS_PER_MONITOR_DPI_AWARE = 2
} PROCESS_DPI_AWARENESS;

HRESULT(WINAPI *SetProcessDpiAwareness)(PROCESS_DPI_AWARENESS dpiAwareness);
#endif

void SetHiDPI() {
#ifdef _WIN32
    void *shcoreDLL = SDL_LoadObject("SHCORE.DLL");
    SetProcessDpiAwareness =
        (HRESULT(WINAPI *)(PROCESS_DPI_AWARENESS))SDL_LoadFunction(shcoreDLL, "SetProcessDpiAwareness");
    SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
#endif
}

#include <SDL2/SDL.h>
#include <nanogui/nanogui.h>
#include <sstream>
#include <string>

#if (!defined(__linux__) || defined(__GLIBC__)) && !defined(__EMSCRIPTEN__)
SignalHandling sh;
#endif

using namespace nanogui;

void NullLogger(void *userdata, int category, SDL_LogPriority priority, const char *message) {}

// combine 'defaultArgs' and command-line arguments
std::vector<std::string> combinedArgs(std::string defaultArgs, int argc, char* argv[]) {
    std::vector<std::string> args;
    // first parse/insert the defaultArgs
    std::stringstream ss(defaultArgs);
    std::string arg, text;
    while (std::getline(ss, text, ' ')) {
        // look for quoted text
        if (text[0] == '\'') {
            arg = text.substr(1);
            // wait for the entire quoted string
            continue;
        } else if (arg.size() > 0) {
            // append to existing until we find the final quote
            arg += ' ';
            size_t lastChar = text.size() - 1;
            if (text[lastChar] == '\'') {
                arg += text.substr(0, lastChar);
            } else {
                arg += text;
                continue;
            }
        } else {
            arg = text;
        }
        args.push_back(arg);
        arg = "";
    }

    // now add actual command-line arguments (inserted AFTER defaultArgs so they override)
    for (int i = 1; i < argc; i++) {
        args.push_back(std::string(argv[i]));
    }
    return args;
}

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#include "AssetManager.h"
#include "CNetManager.h"
#include <json.hpp>
#include <cstring>

// Hooks for the page around the canvas. Hosting on the web is much easier with
// a real button in the page chrome than with the small in-canvas one, and the
// room code has to reach the page so it can be shown and shared.
extern "C" {

EMSCRIPTEN_KEEPALIVE void avara_web_start_match() {
    if (gApplication) {
        CAvaraGame *game = ((CAvaraAppImpl *)gApplication)->GetGame();
        if (game) {
            game->SendStartCommand();
        }
    }
}

// The page owns the canvas size: it fits the window, minus the toolbar, and
// re-renders at the new resolution rather than scaling a fixed-size buffer.
EMSCRIPTEN_KEEPALIVE void avara_web_resize(int w, int h) {
    if (gApplication && w > 0 && h > 0) {
        SDL_SetWindowSize(((CApplication *)gApplication)->sdlWindow(), w, h);
    }
}

// A hidden tab gets no animation frames, and in a lockstep match a player who
// switches tabs stalls everyone else, so the page keeps the loop turning from
// a worker clock while it is hidden.
EMSCRIPTEN_KEEPALIVE void avara_web_tick() {
    nanogui::pump_mainloop();
}

// Chat is streamed a character at a time so everyone can watch you type, and a
// carriage return is what commits the line -- and runs it, if it starts with a
// slash. Sending the whole line at once keeps the same protocol, so /load and
// friends work from the page exactly as they do from the in-canvas roster.
EMSCRIPTEN_KEEPALIVE void avara_web_chat(const char *text) {
    if (!gApplication || text == NULL) {
        return;
    }
    CNetManager *net = ((CAvaraAppImpl *)gApplication)->GetNet();
    if (!net) {
        return;
    }
    char clear = '\x1B';
    net->SendRosterMessage(1, &clear);
    size_t len = strlen(text);
    if (len > 0) {
        net->SendRosterMessage(len, (char *)text);
    }
    char endline = 13;
    net->SendRosterMessage(1, &endline);
}

// The ready checkmark, which is what the in-canvas Start/Ready button sends.
EMSCRIPTEN_KEEPALIVE void avara_web_ready() {
    if (gApplication) {
        CNetManager *net = ((CAvaraAppImpl *)gApplication)->GetNet();
        if (net) {
            net->SendRosterMessage(checkMark_utf8);
        }
    }
}

// Loading by exact set and tag, which is what the page's picker knows; the
// /load command matches on a substring instead.
EMSCRIPTEN_KEEPALIVE void avara_web_load_level(const char *set, const char *tag) {
    if (gApplication && set && tag) {
        CNetManager *net = ((CAvaraAppImpl *)gApplication)->GetNet();
        if (net) {
            net->SendLoadLevel(set, tag);
        }
    }
}

// The page's level picker is populated from the sets that were actually
// packaged into the build, so it can never offer a level nobody has.
EMSCRIPTEN_KEEPALIVE char *avara_web_level_json() {
    nlohmann::json sets = nlohmann::json::array();
    for (auto &setName : AssetManager::GetAvailablePackages()) {
        auto manifest = AssetManager::GetManifest(setName);
        if (!manifest) {
            continue;
        }
        nlohmann::json levels = nlohmann::json::array();
        for (auto const &entry : (*manifest)->levelDirectory) {
            levels.push_back({{"name", entry.levelName}, {"tag", entry.alfPath}});
        }
        if (levels.empty()) {
            continue;
        }
        sets.push_back({{"set", setName}, {"levels", levels}});
    }
    return strdup(sets.dump().c_str());
}

}  // extern "C"
#endif

int main(int argc, char *argv[]) {
    // Open log file.
    Logging::OpenLog();
    // Check basepath override.
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--basepath") == 0) {
            SetBasePath(argv[++i]);
        }
    }
    // Allow Windows to run in HiDPI mode.
    SetHiDPI();

    // Init SDL and nanogui.
    init();

    // SDL_LogSetOutputFunction(&NullLogger, NULL);

    // Init Avara stuff.
    InitMatrix();
    OpenAvaraTCP();

    // The Avara application itself.
    CAvaraAppImpl *app = new CAvaraAppImpl();

    // process command-line arguments
    std::string connectAddress;
    std::vector<std::string> textCommands;
    bool host = false;
    std::vector<std::string> args = combinedArgs(app->Get<std::string>(kDefaultArgs), argc, argv);
    for (int i = 0; i < args.size(); i++) {
        std::string &arg = args[i];
        if (arg == "-p" || arg == "--port") {
            int port = atoi(args[++i].c_str());  // pre-inc to next arg
            app->Set(kDefaultClientUDPPort, port);
        } else if (arg == "-n" || arg == "--name") {
            app->Set(kPlayerNameTag, args[++i]);
        } else if (arg == "-c" || arg == "--connect") {
            connectAddress = args[++i];
            app->Set(kLastAddress, connectAddress);
        } else if (arg == "-s" || arg == "--serve" ||
                   arg == "-S" || arg == "--Serve") {
            host = true;
            app->Set(kTrackerRegister, arg[1] == 'S' || arg[2] == 'S');
        } else if (arg == "-f" || arg == "--frametime") {
            uint16_t frameTime = atol(args[++i].c_str());  // pre-inc to next arg
            app->GetGame()->SetFrameTime(frameTime);
        } else if (arg == "-i" || arg == "--keys-from-stdin") {
            app->GetGame()->SetKeysFromStdin();
        } else if (arg == "-if" || arg == "--keys-from-file") {
            // redirect a playback file to stdin
            freopen(args[++i].c_str(), "r", stdin);
            app->GetGame()->SetKeysFromStdin();
        } else if (arg == "-o" || arg == "--keys-to-stdout") {
            app->GetGame()->SetKeysToStdout();
        } else if (arg == "-/" || arg == "--command") {
            std::string textCommand = args[++i];
            if (textCommand[0] != '/') {
                textCommand.insert(0, "/");
            }
            textCommands.push_back(textCommand);
        } else if (arg == "--basepath") {
            // skip, it was handled earlier in main()
            i = i + 2;
        } else {
            SDL_Log("Unknown command-line argument '%s'\n", args[i].c_str());
            exit(1);
        }
    }

    if (textCommands.size() > 0) {
        auto p = CPlayerManagerImpl::LocalPlayer();
        auto *tui = ((CAvaraAppImpl *)app)->GetTui();
        for (auto cmd: textCommands) {
            tui->ExecuteMatchingCommand(cmd, p);
        }
    }

    if(host == true) {
        app->GetNet()->ChangeNet(kServerNet, "");
    } else if(connectAddress.size() > 0) {
        app->GetNet()->ChangeNet(kClientNet, connectAddress);
    }
    // outside of the game, use INACTIVE_LOOP_REFRESH (no need to poll when not playing)
    mainloop(INACTIVE_LOOP_REFRESH);

    app->Done();

    // Shut it down!!
    shutdown();
    Logging::CloseLog();
    return 0;
}

#if defined(__IPHONEOS__) || defined(__TVOS__)

#ifndef SDL_MAIN_HANDLED
#ifdef main
#undef main
#endif

int main(int argc, char *argv[])
{
    return SDL_UIKitRunApp(argc, argv, SDL_main);
}
#endif /* !SDL_MAIN_HANDLED */

#endif /* __IPHONEOS__ || __TVOS__ */
