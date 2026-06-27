CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -O2 -Isrc -Ithird_party/md4c -Ithird_party/cjson
LDLIBS  := -lgdi32 -lcomctl32 -lole32 -lshell32

# Build-temp fix: cc1/collect2 need a writable temp dir, obtained via the
# Windows GetTempPath env vars (TMP/TEMP). GNU Make scrubs these from recipe
# environments, and some shells default them to an unwritable location, so pin
# a repo-local temp dir for every recipe. Created at parse time below.
BUILD_TMP   := .build-tmp
export TMP  := $(BUILD_TMP)
export TEMP := $(BUILD_TMP)
$(shell mkdir -p $(BUILD_TMP))

# GUI app sources (added to as tasks land)
APP_SRC := src/main.c
APP_OBJ := $(APP_SRC:.c=.o)

# Pure-logic sources compiled into the test binary (added to as tasks land)
LOGIC_SRC := src/paths.c src/store.c src/prefs.c third_party/cjson/cJSON.c src/markdown.c third_party/md4c/md4c.c src/app.c
TEST_SRC  := tests/runner.c tests/test_smoke.c tests/test_paths.c tests/test_store.c tests/test_prefs.c tests/test_markdown.c tests/test_app.c
TEST_OBJ  := $(TEST_SRC:.c=.o) $(LOGIC_SRC:.c=.o)

.PHONY: all app test clean
all: app

app: stickynotes.exe
stickynotes.exe: $(APP_OBJ)
	$(CC) $(CFLAGS) -mwindows -o $@ $^ $(LDLIBS)

test: tests.exe
	./tests.exe
tests.exe: $(TEST_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -Itests -c -o $@ $<

clean:
	rm -f stickynotes.exe tests.exe $(APP_OBJ) $(TEST_OBJ)
