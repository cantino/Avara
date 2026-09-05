/*
    nanogui/nanogui.cpp -- Basic initialization and utility routines

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/screen.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <commdlg.h>
#  include <prsht.h>
#endif

#include <nanogui/opengl.h>
#include <map>
#include <thread>
#include <chrono>
#include <iostream>
#include <algorithm>

#if !defined(_WIN32)
#  include <locale.h>
#  include <signal.h>
#  include <sys/dir.h>
#endif

NAMESPACE_BEGIN(nanogui)

extern std::vector<Screen *> __nanogui_screens;

double gStartTime = 0.0;

void setTime(double t) {
    gStartTime = t;
}

double getTime() {
    return ((double)SDL_GetTicks() / (double)1000.0) - gStartTime;
}

void init() {
    #if !defined(_WIN32)
        /* Avoid locale-related number parsing issues */
        setlocale(LC_NUMERIC, "C");
    #endif

#if defined(__EMSCRIPTEN__)
    /* SDL_INIT_EVERYTHING pulls in haptic and sensor subsystems that the
       emscripten port does not implement, and one failure fails them all. */
    const Uint32 initFlags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS |
                             SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER;
#else
    const Uint32 initFlags = SDL_INIT_EVERYTHING;
#endif
    if (SDL_Init(initFlags) != 0)
        throw std::runtime_error(std::string("Could not initialize SDL: ") + SDL_GetError());

    setTime(0);
}

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

static bool mainloop_active = false;
int throttle = 0;

/* One pass of the main loop: draw every visible screen, then handle input.
   Factored out so platforms that cannot block the calling thread -- the web,
   where the browser owns the event loop -- can drive it a frame at a time.
   Passing draw=false runs everything but the rendering, which is what a
   hidden browser tab wants: the simulation and the network have to keep up
   with the other players, but there is nothing on screen to update. */
static void mainloop_iteration(bool draw) {
    SDL_Event theEvent;

    int numScreens = 0;
    for(auto screen : __nanogui_screens) {
        if (!screen->visible()) {
            continue;
        }
        screen->idle();
        if (draw) {
            screen->drawAll();
        }
        numScreens++;
    }

    if (numScreens == 0) {
        /* Give up if there was nothing to draw */
        mainloop_active = false;
        return;
    }

#if defined(__EMSCRIPTEN__)
    /* The browser drives the frame clock, so never block waiting for input:
       drain whatever has queued up since the last animation frame. */
    while (SDL_PollEvent(&theEvent)) {
#else
    /* Wait for mouse/keyboard or empty refresh events */
    if (SDL_WaitEventTimeout(&theEvent, throttle)) {  // uses SDL_PollEvent(&theEvent) when throttle == 0
#endif
        if (theEvent.type == SDL_QUIT) {
            mainloop_active = false;
        }
        for(auto screen : __nanogui_screens) {
            screen->handleSDLEvent(theEvent);
        }
    }
}

#if defined(__EMSCRIPTEN__)
static void mainloop_frame() { mainloop_iteration(true); }
#endif

void mainloop(int refresh) {
    throttle = refresh;
    if (mainloop_active)
        throw std::runtime_error("Main loop is already running!");

    mainloop_active = true;

#if defined(__EMSCRIPTEN__)
    /* Hand the loop to requestAnimationFrame. The third argument unwinds the
       caller's stack without tearing down the runtime, so main() does not run
       its shutdown path on the way out. */
    emscripten_set_main_loop(mainloop_frame, 0, 1);
#else
    while (mainloop_active) {
        mainloop_iteration(true);
    }

    /* Process events once more */
    SDL_Event theEvent;
    SDL_PollEvent(&theEvent);
#endif
}

#if defined(__EMSCRIPTEN__)
/* Run one iteration from outside requestAnimationFrame. A hidden tab gets no
   animation frames at all, which in a lockstep match stalls everyone else, so
   the page drives the loop from a worker clock instead while it is hidden. */
void pump_mainloop() {
    if (mainloop_active) {
        mainloop_iteration(false);  // nothing to see; keep the simulation going
    }
}
#endif

void leave() {
    mainloop_active = false;
}

bool active() {
    return mainloop_active;
}

void shutdown() {
    SDL_Quit();
}

uint32_t utf8_decode(char *p, size_t len) {
    uint32_t codepoint = 0;
    size_t i = 0;

    if (!len)
        return 0;

    for (; i < len; ++i) {
        if (i == 0) {
            codepoint = (0xff >> len) & *p;
        }
        else {
            codepoint <<= 6;
            codepoint |= 0x3f & *p;
        }
        if (!*p)
            return 0;
        p++;
    }

    return codepoint;
}

std::array<char, 8> utf8(int c) {
    std::array<char, 8> seq;
    int n = 0;
    if (c < 0x80) n = 1;
    else if (c < 0x800) n = 2;
    else if (c < 0x10000) n = 3;
    else if (c < 0x200000) n = 4;
    else if (c < 0x4000000) n = 5;
    else if (c <= 0x7fffffff) n = 6;
    seq[n] = '\0';
    switch (n) {
        case 6: seq[5] = 0x80 | (c & 0x3f); c = c >> 6; c |= 0x4000000;
        case 5: seq[4] = 0x80 | (c & 0x3f); c = c >> 6; c |= 0x200000;
        case 4: seq[3] = 0x80 | (c & 0x3f); c = c >> 6; c |= 0x10000;
        case 3: seq[2] = 0x80 | (c & 0x3f); c = c >> 6; c |= 0x800;
        case 2: seq[1] = 0x80 | (c & 0x3f); c = c >> 6; c |= 0xc0;
        case 1: seq[0] = c;
    }
    return seq;
}

int __nanogui_get_image(NVGcontext *ctx, const std::string &name, uint8_t *data, uint32_t size) {
    static std::map<std::string, int> iconCache;
    auto it = iconCache.find(name);
    if (it != iconCache.end())
        return it->second;
    int iconID = nvgCreateImageMem(ctx, 0, data, size);
    if (iconID == 0)
        throw std::runtime_error("Unable to load resource data.");
    iconCache[name] = iconID;
    return iconID;
}

void Object::decRef(bool dealloc) const noexcept {
    --m_refCount;
    if (m_refCount == 0 && dealloc) {
        delete this;
    } else if (m_refCount < 0) {
        fprintf(stderr, "Internal error: Object reference count < 0!\n");
        abort();
    }
}

Object::~Object() { }

NAMESPACE_END(nanogui)
