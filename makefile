# The CatSystem2 engine, built for this machine.
#
# The same sources are what the Android plugin compiles, through
# app/src/main/cpp/CMakeLists.txt; nothing here is Android-specific and nothing
# there is engine code. Build with "make", then:
#
#   ./bin/catsystem2 "<game folder>" --shot frame.png
#
# which draws one frame without opening a window, so the engine can be checked
# on a machine with no screen.

TARGET   := catsystem2
SRCDIR   := src
BUILDDIR := bin

SOURCES  := $(wildcard $(SRCDIR)/*.c)
OBJECTS  := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SOURCES))
DEPS     := $(OBJECTS:.o=.d)

CC       ?= gcc
# The engine itself needs nothing but zlib. SDL2 is the desktop frontend's
# window, used by src/main.c alone; the Android wrapper compiles everything
# except that file and links no SDL at all.
CFLAGS   := -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -O2 -g -MMD -MP -Ithird_party $(shell sdl2-config --cflags)
LDFLAGS  := $(shell sdl2-config --libs) -lz -lm

.PHONY: all clean check

all: $(BUILDDIR)/$(TARGET)

$(BUILDDIR)/$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# Draws the opening of a game into a PNG. GAME is the game folder:
#   make check GAME="/path/to/Labyrinth of Grisaia"
check: $(BUILDDIR)/$(TARGET)
	@test -n "$(GAME)" || (echo 'set GAME to a game folder' && false)
	$(BUILDDIR)/$(TARGET) "$(GAME)" --steps 2 --shot $(BUILDDIR)/check.png
	@echo "wrote $(BUILDDIR)/check.png"

clean:
	rm -f $(OBJECTS) $(DEPS) $(BUILDDIR)/$(TARGET)

-include $(DEPS)
