CC      := gcc
# -MMD -MP emit per-object .d files so edits to a header recompile every .c
# that includes it (without this, e.g. growing a struct in a shared header
# silently mismatches sizeof across translation units).
CFLAGS  := -std=c11 -Wall -Wextra -O2 -MMD -MP -Isrc -Ithird_party/md4c -Ithird_party/cjson
LDLIBS  := -lgdi32 -lcomctl32 -lole32 -lshell32 -ldwmapi

# Build-temp fix: cc1/collect2 need a writable temp dir, obtained via the
# Windows GetTempPath env vars (TMP/TEMP). GNU Make scrubs these from recipe
# environments, and some shells default them to an unwritable location, so pin
# a repo-local temp dir for every recipe. Created at parse time below.
BUILD_TMP   := .build-tmp
export TMP  := $(BUILD_TMP)
export TEMP := $(BUILD_TMP)
$(shell mkdir -p $(BUILD_TMP))

# GUI app sources (added to as tasks land)
APP_SRC := src/main.c src/note_window.c src/tray.c
APP_OBJ := $(APP_SRC:.c=.o)

# Pure-logic sources compiled into the test binary (added to as tasks land)
LOGIC_SRC := src/paths.c src/store.c src/prefs.c third_party/cjson/cJSON.c src/markdown.c third_party/md4c/md4c.c src/app.c
TEST_SRC  := tests/runner.c tests/test_smoke.c tests/test_paths.c tests/test_store.c tests/test_prefs.c tests/test_markdown.c tests/test_app.c
TEST_OBJ  := $(TEST_SRC:.c=.o) $(LOGIC_SRC:.c=.o)

.PHONY: all app test clean
all: app

LOGIC_OBJ := $(LOGIC_SRC:.c=.o)

app: stickynotes.exe
stickynotes.exe: $(APP_OBJ) $(LOGIC_OBJ)
	$(CC) $(CFLAGS) -mwindows -o $@ $^ $(LDLIBS)

test: tests.exe
	./tests.exe
tests.exe: $(TEST_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -Itests -c -o $@ $<

DEPS := $(APP_OBJ:.o=.d) $(TEST_OBJ:.o=.d)
-include $(DEPS)

clean:
	rm -f stickynotes.exe tests.exe $(APP_OBJ) $(TEST_OBJ) $(DEPS)
