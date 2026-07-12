CC      := gcc
WINDRES := windres
VERSION := 1.0.0
# -MMD -MP emit per-object .d files so edits to a header recompile every .c
# that includes it (without this, e.g. growing a struct in a shared header
# silently mismatches sizeof across translation units).
CFLAGS  := -std=c11 -Wall -Wextra -O2 -MMD -MP -Isrc -Ithird_party/md4c -Ithird_party/cjson
LDLIBS  := -lgdi32 -lcomctl32 -lole32 -lshell32 -ldwmapi -ld2d1 -ldwrite

# Build-temp fix: cc1/collect2 need a writable temp dir, obtained via the
# Windows GetTempPath env vars (TMP/TEMP). GNU Make scrubs these from recipe
# environments, and some shells default them to an unwritable location, so pin
# a repo-local temp dir for every recipe. Created at parse time below.
BUILD_TMP   := .build-tmp
export TMP  := $(BUILD_TMP)
export TEMP := $(BUILD_TMP)
$(shell mkdir -p $(BUILD_TMP))

# GUI app sources (added to as tasks land)
APP_SRC := src/main.c src/note_window.c src/tray.c src/gfx_d2d.c
APP_OBJ := $(APP_SRC:.c=.o)
RES_OBJ := assets/app_res.o   # compiled .rc (app icon)

# Pure-logic sources compiled into the test binary (added to as tasks land)
LOGIC_SRC := src/paths.c src/store.c src/prefs.c third_party/cjson/cJSON.c src/markdown.c third_party/md4c/md4c.c src/app.c
TEST_SRC  := tests/runner.c tests/test_smoke.c tests/test_paths.c tests/test_store.c tests/test_prefs.c tests/test_markdown.c tests/test_app.c
TEST_OBJ  := $(TEST_SRC:.c=.o) $(LOGIC_SRC:.c=.o)

.PHONY: all app test clean installer
all: app

LOGIC_OBJ := $(LOGIC_SRC:.c=.o)

app: quicknote.exe
quicknote.exe: $(APP_OBJ) $(LOGIC_OBJ) $(RES_OBJ)
	$(CC) $(CFLAGS) -mwindows -o $@ $^ $(LDLIBS)

# Resource (app icon). windres resolves the ICON path relative to the .rc dir
# and #includes via -I.
$(RES_OBJ): assets/app.rc assets/quicknote.ico assets/app.manifest src/resource.h
	$(WINDRES) -Isrc -Iassets -o $@ assets/app.rc

test: tests.exe
	./tests.exe
tests.exe: $(TEST_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -Itests -c -o $@ $<

DEPS := $(APP_OBJ:.o=.d) $(TEST_OBJ:.o=.d)
-include $(DEPS)

# Installer (Inno Setup 6). Recipes run under MSYS2 bash, so fallback paths
# are POSIX-style and quoted. Checked in order: PATH, the winget per-user
# location, then the classic machine-wide "Program Files (x86)" location.
# The per-user dir is resolved with `cygpath -F 28` (CSIDL_LOCAL_APPDATA,
# via the Windows API) because recipe environments here scrub Windows env
# vars like LOCALAPPDATA — the same phenomenon as TMP/TEMP at the top of
# this file. Both candidates are overridable (used by the error-path test).
# MSYS2_ARG_CONV_EXCL="/D" stops the MSYS runtime from rewriting the
# /DAppVersion switch into a Windows path (which ISCC would then reject as
# a second script filename).
ISCC_LOCAL := $(shell cygpath -u -F 28 2>/dev/null)/Programs/Inno Setup 6/ISCC.exe
ISCC_PF    := /c/Program Files (x86)/Inno Setup 6/ISCC.exe

installer: quicknote.exe
	@ISCC="$$(command -v ISCC || true)"; \
	for p in "$(ISCC_LOCAL)" "$(ISCC_PF)"; do \
	  if [ -z "$$ISCC" ] && [ -x "$$p" ]; then ISCC="$$p"; fi; \
	done; \
	if [ -z "$$ISCC" ]; then \
	  echo "error: ISCC.exe not found — install Inno Setup 6: winget install JRSoftware.InnoSetup"; \
	  exit 1; \
	fi; \
	MSYS2_ARG_CONV_EXCL="/D" "$$ISCC" /DAppVersion="$(VERSION)" installer/quicknote.iss

clean:
	rm -f quicknote.exe stickynotes.exe tests.exe $(APP_OBJ) $(TEST_OBJ) $(RES_OBJ) $(DEPS)
