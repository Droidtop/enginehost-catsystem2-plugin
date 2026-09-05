# CatSystem2

An engine for CatSystem2 games, written from the formats rather than wrapped
around anything: there is no open CatSystem2 engine to wrap.

Today it is a scene player. It reads a game folder as the original engine does -
loose files layered over its ".int" archives, decrypted with the key kept inside
the game's own executable - and plays a scene script: the background, the
characters standing on it, and the dialogue, drawn on the virtual screen the
game was authored for. It does not run KCS, the compiled system script every
retail release boots into, so there is no title screen, no menus and no save
system of the game's own yet, and no sound.

## Building and running

Needs gcc, SDL2, SDL2_ttf and zlib.

    make
    ./bin/catsystem2 "/path/to/a/game" --steps 2 --shot frame.png

`--shot` draws one frame into a PNG and opens no window, which is how the engine
is checked on a machine with no screen and in CI. Without it a window opens and
a click or a key advances the script. `--script <name>` plays one script instead
of the game's entry point, and `--list <archive>` prints an archive's contents.

## Layout

    src/      the engine. No Android, no Java, no frontend beyond SDL2.
    tools/    generators for the data tables the engine carries.
    makefile  the desktop build.

The Android plugin is a separate branch that compiles these same sources and
adds nothing to them; see the enginehost plugin repository's `plugin-core`.

## Attribution

The formats were worked out with the help of the public documentation in
[TriggersTools.CatSystem2](https://github.com/trigger-segfault/TriggersTools.CatSystem2)
(MIT). No source code from that project, and none from the proprietary
CatSystem2 runtime, is included here. See THIRD_PARTY.md.
