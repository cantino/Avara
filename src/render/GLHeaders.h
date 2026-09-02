/*
    Pulls in the OpenGL headers appropriate for the current platform.

    Desktop builds load entry points through GLAD. Emscripten targets WebGL2,
    which is GLES 3.0, and provides those headers directly with no loader.
*/

#pragma once

#if defined(__EMSCRIPTEN__)
    #include <GLES3/gl3.h>
    #include <GLES2/gl2ext.h>
#else
    #include <glad/glad.h>
#endif
