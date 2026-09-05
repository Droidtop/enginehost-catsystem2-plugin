# CatSystem2 plugin for Enginehost

This branch is the wrapper, and only the wrapper. The engine it wraps is on
`main`: C, with no Android in it, buildable and runnable on Linux on its own.

What is here is what Android needs and the engine must not know about: the
plugin class Enginehost loads, the JNI bridge that hands it the buffer of pixels
the engine draws into, the CMake file that compiles the engine's own sources,
the capabilities the plugin advertises, and the bundle metadata and keys it is
signed with. Nothing here reads a game file or draws a pixel.

Nothing on this branch builds by itself, deliberately: there is no engine here
to compile. A release line - `plugin/<version>` - is this branch merged onto
the engine, and that is what CI builds and publishes.
