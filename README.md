# CatSystem2

An engine for CatSystem2 games, written from the formats rather than wrapped
around anything: there is no open CatSystem2 engine to wrap.

Today it is a scene player. It reads a game folder as the original engine does -
loose files layered over its ".int" archives, decrypted with the key kept inside
the game's own executable - and plays a scene script: the background, the
characters standing on it, the dialogue, and the music, effects and voices the
script calls for, on the virtual screen the game was authored for. It does not
run KCS, the compiled system script every retail release boots into, so there is
no title screen, no menus and no save system of the game's own yet.

## Building and running

Needs gcc, zlib, and SDL2 for the window and the sound device. The engine itself
needs only zlib: it mixes sound but opens no device, because the two frontends
open different ones. SDL2 is used by `src/main.c` alone, and the Android wrapper
compiles everything except that file, links no SDL, and hands the same mixer to
AAudio instead.

    make
    ./bin/catsystem2 "/path/to/a/game" --steps 2 --shot frame.png

`--shot` draws one frame into a PNG and opens no window, which is how the engine
is checked on a machine with no screen and in CI. Without it a window opens and
a click or a key advances the script. `--script <name>` plays one script instead
of the game's entry point, `--silent` opens no sound device, and
`--list <archive>` prints an archive's contents.

## Layout

    src/          the engine, and src/main.c the desktop frontend.
    third_party/  stb_truetype.h and stb_vorbis.c, vendored: glyphs and Ogg.
    tools/        generators for the data tables the engine carries.
    makefile      the desktop build.

The Android plugin is a separate branch that compiles these same sources and
adds nothing to them; see the enginehost plugin repository's `plugin-core`.

## Attribution

The formats were worked out with the help of the public documentation in
[TriggersTools.CatSystem2](https://github.com/trigger-segfault/TriggersTools.CatSystem2)
(MIT). No source code from that project, and none from the proprietary
CatSystem2 runtime, is included here. See THIRD_PARTY.md.

## This branch

This is a release line: the engine on `main` with the Enginehost wrapper from
`plugin-core` merged onto it. The wrapper adds `app/`, which compiles these same
sources for Android and shows what they draw; it adds nothing to the engine and
repeats none of it. CI builds and signs the bundle from here.
