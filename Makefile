include NickelHook/NickelHook.mk

override PKGCONF  += Qt5Widgets Qt5Network Qt5Sql
override LIBRARY  := libnickelfootnote.so
override SOURCES  += src/nickelfootnote.cc src/context.cc src/ask.cc src/log.cc src/chainhook.cc src/store.cc src/openai.cc src/ui.cc
override MOCS     += src/ask.h src/openai.h src/ui.h
override CFLAGS   += -Wall -Wextra -Werror
override CXXFLAGS += -Wall -Wextra -Werror -Wno-missing-field-initializers
# Export only what NickelHook and Qt look up (marked visibility("default")).
# An exported global named like a libnickel symbol binds to libnickel's copy:
# build 1-3 declared `QString const *ATTRIBUTE_TITLE` etc., so NickelHook's
# dlsym wrote through it into Nickel's own ATTRIBUTE_TITLE and Nickel crashed
# loading the home screen.
override CXXFLAGS += -fvisibility=hidden

include NickelHook/NickelHook.mk

# Version label for the log and the User-Agent. build.ps1 and CI pass it from
# `git describe`; src/version.h is only rewritten when it changes.
NFN_VERSION ?= dev
$(shell printf '\043define NFN_VERSION "%s"\n' '$(NFN_VERSION)' > src/version.h.tmp; cmp -s src/version.h.tmp src/version.h && rm -f src/version.h.tmp || mv -f src/version.h.tmp src/version.h)

# NickelHook.mk compiles with $^, so headers can't be prerequisites. Drop
# objects older than the shared headers instead.
$(shell for o in src/nickelfootnote.o src/context.o src/ask.o src/log.o src/chainhook.o src/store.o src/openai.o src/ui.o; do for h in src/nickel.h src/context.h src/ask.h src/log.h src/chainhook.h src/store.h src/openai.h src/ui.h src/version.h; do [ $$h -nt $$o ] && rm -f $$o; done; done)
