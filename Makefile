CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -O2 -Isrc -Ithird_party/md4c -Ithird_party/cjson
LDLIBS  := -lgdi32 -lcomctl32 -lole32 -lshell32

# GUI app sources (added to as tasks land)
APP_SRC := src/main.c
APP_OBJ := $(APP_SRC:.c=.o)

# Pure-logic sources compiled into the test binary (added to as tasks land)
LOGIC_SRC := src/paths.c src/store.c src/prefs.c third_party/cjson/cJSON.c src/markdown.c third_party/md4c/md4c.c
TEST_SRC  := tests/runner.c tests/test_smoke.c tests/test_paths.c tests/test_store.c tests/test_prefs.c tests/test_markdown.c
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
