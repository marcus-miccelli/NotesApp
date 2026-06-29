# Tabbed Notes (Windows-Terminal style) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn quickNote from one-note-per-window into Windows-Terminal-style windows that hold multiple notes as tabs (add/activate/reorder/close/rename), with the note name living in its tab.

**Architecture:** Persistence gains a `windows[]` array (each a geometry + ordered note-id list + active index) beside the existing `notes[]`; `NoteMeta` slims to identity-only. The GUI `NoteWin` becomes a tab group with one RichEdit body per tab (active shown, rest hidden) and a custom-drawn tab strip in the existing title bar. Data-model layers are TDD-tested headlessly; GUI layers are verified by clean build + launch + screenshot.

**Tech Stack:** C11, Win32 + RichEdit 4.1, cJSON, MinGW-w64 (gcc + windres) + GNU Make.

## Global Constraints

- Language: C11 (`-std=c11 -Wall -Wextra`), zero warnings.
- `make test` (headless `tests.exe`) must stay green after every data-model task. It links only `LOGIC_SRC` (paths, store, prefs, cJSON, markdown, md4c, app) — NOT `note_window.c`. So Tasks 1-4 keep tests green even while `make app` is intentionally broken; `make app` is restored from Task 5 onward.
- `make app` builds `quicknote.exe` and must build clean with zero warnings from Task 5 onward.
- Windows-only GUI. Build/run on Windows 10 19045. To rebuild when the exe is locked: `taskkill //IM quicknote.exe //F` (ignore failure) before `make app`.
- Note files stay plain Markdown on disk; the note name is written as the leading `# name` line of the `.md`.
- Commit after each task. Commit-message trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.
- Existing layout constants stay: `PAD_OUTER 8`, `REGION_GAP 8`, `SIDEBAR_W 40`, `TITLEBAR_H 34`, `WINBTN_W 46`, `NUM_BTNS 5`, `NUM_WBTNS 3`.

---

### Task 1: Slim NoteMeta + window structures + in-memory window CRUD

**Files:**
- Modify: `src/prefs.h`
- Modify: `src/prefs.c`
- Modify: `src/app.c:45-60` (`app_new_note` — drop geometry/open)
- Test: `tests/test_prefs.c`, `tests/test_app.c`

**Interfaces:**
- Produces:
  - `typedef struct { char id[16]; char file[24]; char name[64]; char color[16]; } NoteMeta;`
  - `#define WIN_MAX_TABS 32`
  - `typedef struct { char id[16]; int x,y,w,h; int active; char tabs[WIN_MAX_TABS][16]; int ntabs; } WinMeta;`
  - `Prefs` gains `WinMeta* windows; size_t wcount, wcap;`
  - `WinMeta* prefs_add_window(Prefs* p, const char* id);`
  - `WinMeta* prefs_find_window(Prefs* p, const char* id);`
  - `bool     prefs_remove_window(Prefs* p, const char* id);`

- [ ] **Step 1: Update the header**

In `src/prefs.h`, replace the `NoteMeta` struct and `Prefs` struct and add window declarations:

```c
typedef struct {
    char id[16];
    char file[24];
    char name[64];     /* note title; shown in tab + tray; default "Untitled N" */
    char color[16];
} NoteMeta;

#define WIN_MAX_TABS 32
typedef struct {
    char id[16];                    /* stable window id */
    int  x, y, w, h;
    int  active;                    /* index into tabs[] */
    char tabs[WIN_MAX_TABS][16];    /* ordered note ids */
    int  ntabs;
} WinMeta;

typedef struct {
    int version;
    char theme[16];
    NoteMeta* notes;   size_t count,  cap;
    WinMeta*  windows; size_t wcount, wcap;
} Prefs;
```

Add the window declarations next to the existing note ones:

```c
WinMeta* prefs_add_window(Prefs* p, const char* id);
WinMeta* prefs_find_window(Prefs* p, const char* id);
bool     prefs_remove_window(Prefs* p, const char* id);
```

- [ ] **Step 2: Write failing tests**

In `tests/test_prefs.c`, the existing note checks reference removed fields (`m->x`, `m->open`). Replace the body of `test_prefs` with this (keeps the load/corrupt coverage, drops geometry/open from notes, adds window CRUD):

```c
void test_prefs(void) {
    Prefs p;
    prefs_init_default(&p);
    CHECK(p.version == 1);
    CHECK_STR(p.theme, "dark");
    CHECK(p.count == 0);
    CHECK(p.wcount == 0);

    NoteMeta* m = prefs_add_note(&p, "a1b2c3", "a1b2c3.md");
    CHECK(m != NULL);
    CHECK(p.count == 1);
    strcpy(m->color, "amber");
    strcpy(m->name, "Hello");

    /* window CRUD */
    WinMeta* w = prefs_add_window(&p, "w0001");
    CHECK(w != NULL);
    CHECK(p.wcount == 1);
    w->x = 10; w->y = 20; w->w = 300; w->h = 400;
    w->ntabs = 1; strcpy(w->tabs[0], "a1b2c3"); w->active = 0;
    CHECK(prefs_find_window(&p, "w0001") == w);
    CHECK(prefs_find_window(&p, "nope") == NULL);

    const char* path = "C:\\tmp\\sntest_prefs.json";
    CHECK(prefs_save(&p, path));
    prefs_free(&p);

    Prefs q;
    CHECK(prefs_load(&q, path));
    CHECK(q.count == 1);
    NoteMeta* r = prefs_find(&q, "a1b2c3");
    CHECK(r != NULL);
    CHECK_STR(r->file, "a1b2c3.md");
    CHECK_STR(r->color, "amber");
    CHECK_STR(r->name, "Hello");

    WinMeta* rw = prefs_find_window(&q, "w0001");
    CHECK(rw != NULL);
    CHECK(rw->x == 10 && rw->y == 20 && rw->w == 300 && rw->h == 400);
    CHECK(rw->ntabs == 1 && rw->active == 0);
    CHECK_STR(rw->tabs[0], "a1b2c3");

    CHECK(prefs_remove_window(&q, "w0001"));
    CHECK(q.wcount == 0);
    CHECK(prefs_remove(&q, "a1b2c3"));
    CHECK(q.count == 0);
    prefs_free(&q);

    /* missing file -> false but usable default */
    Prefs z;
    CHECK(!prefs_load(&z, "C:\\tmp\\sntest_does_not_exist.json"));
    CHECK(z.version == 1 && z.count == 0 && z.wcount == 0);
    prefs_free(&z);

    /* corrupt file -> false + defaults + a .bak backup with the original bytes */
    const char* corrupt_path = "C:\\tmp\\sntest_prefs_corrupt.json";
    const char* bak_path = "C:\\tmp\\sntest_prefs_corrupt.json.bak";
    const char* corrupt_bytes = "{ this is : not json";
    remove(bak_path);
    FILE* cf = fopen(corrupt_path, "wb");
    CHECK(cf != NULL);
    if (cf) { fputs(corrupt_bytes, cf); fclose(cf); }

    Prefs c;
    CHECK(!prefs_load(&c, corrupt_path));
    CHECK(c.version == 1 && c.count == 0);
    prefs_free(&c);

    FILE* bf = fopen(bak_path, "rb");
    CHECK(bf != NULL);
    if (bf) {
        char rbuf[64]; size_t rn = fread(rbuf, 1, sizeof rbuf - 1, bf);
        rbuf[rn] = '\0'; fclose(bf);
        CHECK_STR(rbuf, corrupt_bytes);
    }
    remove(corrupt_path);
    remove(bak_path);
}
```

`tests/test_prefs.c` needs `#include <stdio.h>` for `FILE`/`remove` (add if not present).

In `tests/test_app.c`, line 20 `CHECK(m->open == true);` references a removed field. Replace it with:

```c
    CHECK_STR(m->file, "");   /* placeholder removed below; see step */
```

Actually replace line 20 with a check that does not use removed fields:

```c
    CHECK(strlen(m->id) == 8);
```

- [ ] **Step 3: Run tests, verify they fail to compile**

Run: `make test`
Expected: compile errors (missing `prefs_add_window`, `wcount`, etc.) — confirms the tests exercise the new API.

- [ ] **Step 4: Implement in prefs.c**

In `src/prefs.c`:

Update `prefs_init_default` to zero the window arrays:

```c
void prefs_init_default(Prefs* p) {
    p->version = 1;
    snprintf(p->theme, sizeof p->theme, "%s", "dark");
    p->notes = NULL; p->count = 0; p->cap = 0;
    p->windows = NULL; p->wcount = 0; p->wcap = 0;
}
```

Update `prefs_free`:

```c
void prefs_free(Prefs* p) {
    free(p->notes);   p->notes = NULL;   p->count = 0;  p->cap = 0;
    free(p->windows); p->windows = NULL; p->wcount = 0; p->wcap = 0;
}
```

Slim `prefs_add_note` (drop geometry/open assignment):

```c
NoteMeta* prefs_add_note(Prefs* p, const char* id, const char* file) {
    if (p->count == p->cap) {
        size_t nc = p->cap ? p->cap * 2 : 8;
        NoteMeta* n = realloc(p->notes, nc * sizeof(NoteMeta));
        if (!n) return NULL;
        p->notes = n; p->cap = nc;
    }
    NoteMeta* m = &p->notes[p->count++];
    memset(m, 0, sizeof *m);
    copy_str(m->id, sizeof m->id, id, "");
    copy_str(m->file, sizeof m->file, file, "");
    snprintf(m->color, sizeof m->color, "%s", "slate");
    return m;
}
```

Add window CRUD (mirrors the note ones):

```c
WinMeta* prefs_add_window(Prefs* p, const char* id) {
    if (p->wcount == p->wcap) {
        size_t nc = p->wcap ? p->wcap * 2 : 4;
        WinMeta* n = realloc(p->windows, nc * sizeof(WinMeta));
        if (!n) return NULL;
        p->windows = n; p->wcap = nc;
    }
    WinMeta* w = &p->windows[p->wcount++];
    memset(w, 0, sizeof *w);
    copy_str(w->id, sizeof w->id, id, "");
    w->x = 200; w->y = 200; w->w = 480; w->h = 360;   /* 4:3 default */
    w->active = 0; w->ntabs = 0;
    return w;
}

WinMeta* prefs_find_window(Prefs* p, const char* id) {
    for (size_t i = 0; i < p->wcount; i++)
        if (strcmp(p->windows[i].id, id) == 0) return &p->windows[i];
    return NULL;
}

bool prefs_remove_window(Prefs* p, const char* id) {
    for (size_t i = 0; i < p->wcount; i++) {
        if (strcmp(p->windows[i].id, id) == 0) {
            memmove(&p->windows[i], &p->windows[i+1],
                    (p->wcount - i - 1) * sizeof(WinMeta));
            p->wcount--;
            return true;
        }
    }
    return false;
}
```

In `src/app.c`, update `app_new_note` to drop the removed fields (remove the `m->open = true;` line at the end and keep the rest). The function becomes:

```c
NoteMeta* app_new_note(AppState* a) {
    char id[16], file[24];
    app_gen_id(id);
    snprintf(file, sizeof file, "%s.md", id);
    NoteMeta* m = prefs_add_note(&a->prefs, id, file);
    if (!m) return NULL;
    snprintf(m->name, sizeof m->name, "Untitled %d", app_next_untitled(a));
    char path[260];
    app_note_path(a, m, path, sizeof path);
    if (!store_write_note(path, "", 0)) {
        prefs_remove(&a->prefs, id);
        return NULL;
    }
    return m;
}
```

Note: `prefs_load`/`prefs_save` still reference removed `NoteMeta` fields (`m->x`, `m->open`) — they are fixed in Task 2. For THIS task to compile, temporarily delete the four `m->x/y/w/h` lines and the `m->open` line from `prefs_load` (lines ~106-112) and the matching `cJSON_AddNumber/Bool` lines for x/y/w/h/open in `prefs_save` (lines ~129-134). Keep id/file/name/color. (Task 2 replaces these bodies wholesale, so this is just to keep the build moving.)

- [ ] **Step 5: Run tests, verify pass**

Run: `make test`
Expected: all tests PASS (count printed, 0 failures). `make app` will NOT build yet — that is expected until Task 5.

- [ ] **Step 6: Commit**

```bash
git add src/prefs.h src/prefs.c src/app.c tests/test_prefs.c tests/test_app.c
git commit -m "$(printf 'feat(prefs): slim NoteMeta + window structs + window CRUD\n\nGeometry/open move off NoteMeta onto a new WinMeta (geometry + ordered\ntab ids + active index). Add prefs_add/find/remove_window.\n\nCo-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>')"
```

---

### Task 2: Persist windows[] in JSON (schema version 2)

**Files:**
- Modify: `src/prefs.c` (`prefs_load`, `prefs_save`)
- Test: `tests/test_prefs.c` (round-trip already added in Task 1 now must actually persist)

**Interfaces:**
- Consumes: `WinMeta`, `Prefs.windows` from Task 1.
- Produces: JSON with top-level `"windows"` array; `prefs_save` writes `"version": 2`.

- [ ] **Step 1: Confirm the round-trip test fails**

The Task-1 test already saves a window and reloads it. With Task-1's stripped save/load (no window serialization), `prefs_find_window(&q, "w0001")` returns NULL after reload.

Run: `make test`
Expected: FAIL at `CHECK(rw != NULL)` in `test_prefs`.

- [ ] **Step 2: Implement save**

In `src/prefs.c`, replace `prefs_save` with:

```c
bool prefs_save(const Prefs* p, const char* json_path) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 2);
    cJSON_AddStringToObject(root, "theme", p->theme);

    cJSON* arr = cJSON_AddArrayToObject(root, "notes");
    for (size_t i = 0; i < p->count; i++) {
        const NoteMeta* m = &p->notes[i];
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", m->id);
        cJSON_AddStringToObject(o, "file", m->file);
        cJSON_AddStringToObject(o, "name", m->name);
        cJSON_AddStringToObject(o, "color", m->color);
        cJSON_AddItemToArray(arr, o);
    }

    cJSON* warr = cJSON_AddArrayToObject(root, "windows");
    for (size_t i = 0; i < p->wcount; i++) {
        const WinMeta* w = &p->windows[i];
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", w->id);
        cJSON_AddNumberToObject(o, "x", w->x);
        cJSON_AddNumberToObject(o, "y", w->y);
        cJSON_AddNumberToObject(o, "w", w->w);
        cJSON_AddNumberToObject(o, "h", w->h);
        cJSON_AddNumberToObject(o, "active", w->active);
        cJSON* tabs = cJSON_AddArrayToObject(o, "tabs");
        for (int t = 0; t < w->ntabs; t++)
            cJSON_AddItemToArray(tabs, cJSON_CreateString(w->tabs[t]));
        cJSON_AddItemToArray(warr, o);
    }

    char* text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) return false;
    FILE* f = fopen(json_path, "wb");
    if (!f) { free(text); return false; }
    fputs(text, f);
    fclose(f);
    free(text);
    return true;
}
```

- [ ] **Step 3: Implement load**

In `src/prefs.c`, replace the notes-loading block and add window loading. Replace from `p->version = json_int(...)` through `cJSON_Delete(root); return true;` with:

```c
    p->version = json_int(root, "version", 1);
    copy_str(p->theme, sizeof p->theme, json_str(root, "theme", "dark"), "dark");

    const cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "notes");
    const cJSON* it = NULL;
    cJSON_ArrayForEach(it, arr) {
        const char* id = json_str(it, "id", NULL);
        const char* file = json_str(it, "file", NULL);
        if (!id || !file) continue;
        NoteMeta* m = prefs_add_note(p, id, file);
        if (!m) break;
        copy_str(m->name, sizeof m->name, json_str(it, "name", ""), "");
        copy_str(m->color, sizeof m->color, json_str(it, "color", "slate"), "slate");
    }

    const cJSON* warr = cJSON_GetObjectItemCaseSensitive(root, "windows");
    const cJSON* wit = NULL;
    cJSON_ArrayForEach(wit, warr) {
        const char* id = json_str(wit, "id", NULL);
        if (!id) continue;
        WinMeta* w = prefs_add_window(p, id);
        if (!w) break;
        w->x = json_int(wit, "x", 200);
        w->y = json_int(wit, "y", 200);
        w->w = json_int(wit, "w", 480);
        w->h = json_int(wit, "h", 360);
        w->active = json_int(wit, "active", 0);
        const cJSON* tabs = cJSON_GetObjectItemCaseSensitive(wit, "tabs");
        const cJSON* tit = NULL;
        w->ntabs = 0;
        cJSON_ArrayForEach(tit, tabs) {
            if (w->ntabs >= WIN_MAX_TABS) break;
            if (cJSON_IsString(tit) && tit->valuestring)
                copy_str(w->tabs[w->ntabs++], sizeof w->tabs[0], tit->valuestring, "");
        }
        if (w->active >= w->ntabs) w->active = w->ntabs ? w->ntabs - 1 : 0;
    }
    /* migration for v1 files happens in Task 3, inserted here */

    cJSON_Delete(root);
    return true;
```

- [ ] **Step 4: Run tests, verify pass**

Run: `make test`
Expected: PASS, including the window round-trip checks.

- [ ] **Step 5: Commit**

```bash
git add src/prefs.c
git commit -m "$(printf 'feat(prefs): serialize windows[] at schema version 2\n\nCo-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>')"
```

---

### Task 3: Migrate v1 prefs (per-note geometry/open) to v2 windows

**Files:**
- Modify: `src/prefs.c` (`prefs_load` — add migration after the windows loop)
- Test: `tests/test_prefs.c` (add `test_prefs_migration` or extend; register in runner if new)

**Interfaces:**
- Consumes: `prefs_add_window`, note loading from Tasks 1-2.
- Produces: after loading a v1 file, every note that was `open:true` in v1 has a 1-tab window using that note's old `x,y,w,h`.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_prefs.c` a second function, and declare+call it in the runner. First the test:

```c
void test_prefs_migration(void) {
    /* hand-write a v1 prefs: two notes, one open (with geometry), one closed */
    const char* path = "C:\\tmp\\sntest_prefs_v1.json";
    const char* v1 =
        "{\"version\":1,\"theme\":\"dark\",\"notes\":["
        "{\"id\":\"open01\",\"file\":\"open01.md\",\"name\":\"Open\","
        "\"x\":11,\"y\":22,\"w\":333,\"h\":444,\"color\":\"slate\",\"open\":true},"
        "{\"id\":\"shut01\",\"file\":\"shut01.md\",\"name\":\"Shut\","
        "\"x\":1,\"y\":2,\"w\":3,\"h\":4,\"color\":\"slate\",\"open\":false}"
        "]}";
    FILE* f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) { fputs(v1, f); fclose(f); }

    Prefs p;
    CHECK(prefs_load(&p, path));
    CHECK(p.count == 2);                 /* both notes kept */
    CHECK(p.wcount == 1);                /* one window for the open note */
    WinMeta* w = &p.windows[0];
    CHECK(w->ntabs == 1);
    CHECK_STR(w->tabs[0], "open01");
    CHECK(w->x == 11 && w->y == 22 && w->w == 333 && w->h == 444);
    CHECK(prefs_find(&p, "shut01") != NULL);   /* closed note still present */
    prefs_free(&p);
    remove(path);
}
```

Register it: in `tests/runner.c` add a `void test_prefs_migration(void);` declaration and a call alongside `test_prefs()`. (Open `tests/runner.c` and mirror how `test_prefs` is declared and invoked.)

- [ ] **Step 2: Run test, verify it fails**

Run: `make test`
Expected: FAIL at `CHECK(p.wcount == 1)` (migration not implemented; a v1 file has no `windows`).

- [ ] **Step 3: Implement migration**

In `src/prefs.c`, the load must read legacy per-note geometry/open WITHOUT storing it on `NoteMeta` (those fields are gone). Read them locally during the note loop only when migrating. Replace the note-loading loop and add migration. Use this approach: when there is no `windows` array (or `version < 2`), build windows from the v1 note objects.

Replace the note loop + the migration placeholder with:

```c
    int has_windows = cJSON_GetObjectItemCaseSensitive(root, "windows") != NULL;
    int legacy = (p->version < 2) || !has_windows;

    const cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "notes");
    const cJSON* it = NULL;
    cJSON_ArrayForEach(it, arr) {
        const char* id = json_str(it, "id", NULL);
        const char* file = json_str(it, "file", NULL);
        if (!id || !file) continue;
        NoteMeta* m = prefs_add_note(p, id, file);
        if (!m) break;
        copy_str(m->name, sizeof m->name, json_str(it, "name", ""), "");
        copy_str(m->color, sizeof m->color, json_str(it, "color", "slate"), "slate");

        if (legacy && json_bool(it, "open", true)) {
            /* one window per previously-open note, keeping its geometry */
            char wid[16];
            snprintf(wid, sizeof wid, "mw%zu", p->wcount);
            WinMeta* w = prefs_add_window(p, wid);
            if (w) {
                w->x = json_int(it, "x", 200);
                w->y = json_int(it, "y", 200);
                w->w = json_int(it, "w", 480);
                w->h = json_int(it, "h", 360);
                w->ntabs = 1; w->active = 0;
                copy_str(w->tabs[0], sizeof w->tabs[0], id, "");
            }
        }
    }

    if (!legacy) {
        const cJSON* warr = cJSON_GetObjectItemCaseSensitive(root, "windows");
        const cJSON* wit = NULL;
        cJSON_ArrayForEach(wit, warr) {
            const char* id = json_str(wit, "id", NULL);
            if (!id) continue;
            WinMeta* w = prefs_add_window(p, id);
            if (!w) break;
            w->x = json_int(wit, "x", 200);
            w->y = json_int(wit, "y", 200);
            w->w = json_int(wit, "w", 480);
            w->h = json_int(wit, "h", 360);
            w->active = json_int(wit, "active", 0);
            const cJSON* tabs = cJSON_GetObjectItemCaseSensitive(wit, "tabs");
            const cJSON* tit = NULL;
            w->ntabs = 0;
            cJSON_ArrayForEach(tit, tabs) {
                if (w->ntabs >= WIN_MAX_TABS) break;
                if (cJSON_IsString(tit) && tit->valuestring)
                    copy_str(w->tabs[w->ntabs++], sizeof w->tabs[0], tit->valuestring, "");
            }
            if (w->active >= w->ntabs) w->active = w->ntabs ? w->ntabs - 1 : 0;
        }
    }

    cJSON_Delete(root);
    return true;
```

(This replaces both the Task-2 note loop and the Task-2 window loop; the window loop now runs only in the non-legacy branch.)

- [ ] **Step 4: Run tests, verify pass**

Run: `make test`
Expected: PASS, including `test_prefs_migration` and the Task-2 round-trip (a v2 file has a `windows` array, so `legacy` is false and the explicit-window branch runs).

- [ ] **Step 5: Commit**

```bash
git add src/prefs.c tests/test_prefs.c tests/runner.c
git commit -m "$(printf 'feat(prefs): migrate v1 per-note geometry/open to v2 windows\n\nCo-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>')"
```

---

### Task 4: App-level window helpers

**Files:**
- Modify: `src/app.h`, `src/app.c`
- Test: `tests/test_app.c`

**Interfaces:**
- Consumes: `prefs_add_window`, `app_new_note`, `app_gen_id`.
- Produces:
  - `WinMeta* app_new_window(AppState* a);` — new window id + one fresh note as its single tab; returns the window.
  - `WinMeta* app_open_note_in_window(AppState* a, const char* note_id);` — new window whose single tab is an existing note; returns the window (NULL if note unknown).
  - `WinMeta* app_window_of_note(AppState* a, const char* note_id);` — the window whose tabs[] contains note_id, or NULL.

- [ ] **Step 1: Write failing tests**

Append to `tests/test_app.c` (inside `test_app`, before `app_shutdown(&a);`):

```c
    /* window helpers */
    WinMeta* w = app_new_window(&a);
    CHECK(w != NULL);
    CHECK(w->ntabs == 1);
    CHECK(strlen(w->tabs[0]) == 8);             /* a fresh note id */
    CHECK(app_window_of_note(&a, w->tabs[0]) == w);

    NoteMeta* extra = app_new_note(&a);
    CHECK(extra != NULL);
    char extra_id[16]; strcpy(extra_id, extra->id);
    CHECK(app_window_of_note(&a, extra_id) == NULL);   /* not in any window */
    WinMeta* w2 = app_open_note_in_window(&a, extra_id);
    CHECK(w2 != NULL && w2 != w);
    CHECK(w2->ntabs == 1);
    CHECK_STR(w2->tabs[0], extra_id);
    CHECK(app_open_note_in_window(&a, "nosuch") == NULL);
```

- [ ] **Step 2: Run, verify fail to compile**

Run: `make test`
Expected: compile error — `app_new_window` undeclared.

- [ ] **Step 3: Declare in app.h**

Add to `src/app.h` after `app_new_note`:

```c
WinMeta* app_new_window(AppState* a);
WinMeta* app_open_note_in_window(AppState* a, const char* note_id);
WinMeta* app_window_of_note(AppState* a, const char* note_id);
```

- [ ] **Step 4: Implement in app.c**

Add a window-id generator and the three helpers to `src/app.c`:

```c
static void app_gen_win_id(AppState* a, char* out16) {
    /* "wXXXXXXX" hex, distinct from note ids and other window ids */
    for (;;) {
        char id[16]; id[0] = 'w';
        static const char hex[] = "0123456789abcdef";
        for (int i = 1; i < 8; i++) id[i] = hex[rand() & 0xF];
        id[8] = '\0';
        if (!prefs_find_window(&a->prefs, id)) { strcpy(out16, id); return; }
    }
}

WinMeta* app_new_window(AppState* a) {
    NoteMeta* m = app_new_note(a);
    if (!m) return NULL;
    char nid[16]; strcpy(nid, m->id);   /* copy: app_new_note may realloc notes */
    char wid[16]; app_gen_win_id(a, wid);
    WinMeta* w = prefs_add_window(&a->prefs, wid);
    if (!w) { app_delete_note(a, nid); return NULL; }
    w->ntabs = 1; w->active = 0;
    strcpy(w->tabs[0], nid);
    return w;
}

WinMeta* app_open_note_in_window(AppState* a, const char* note_id) {
    if (!prefs_find(&a->prefs, note_id)) return NULL;
    char wid[16]; app_gen_win_id(a, wid);
    WinMeta* w = prefs_add_window(&a->prefs, wid);
    if (!w) return NULL;
    w->ntabs = 1; w->active = 0;
    snprintf(w->tabs[0], sizeof w->tabs[0], "%s", note_id);
    return w;
}

WinMeta* app_window_of_note(AppState* a, const char* note_id) {
    for (size_t i = 0; i < a->prefs.wcount; i++) {
        WinMeta* w = &a->prefs.windows[i];
        for (int t = 0; t < w->ntabs; t++)
            if (strcmp(w->tabs[t], note_id) == 0) return w;
    }
    return NULL;
}
```

`app_new_window` uses `rand()` via `app_gen_win_id`; `app_gen_id` already seeds `rand` on first call, and `app_new_note` (called first in `app_new_window`) calls `app_gen_id`, so the seed is set. Keep `#include` set as-is.

- [ ] **Step 5: Run tests, verify pass**

Run: `make test`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/app.h src/app.c tests/test_app.c
git commit -m "$(printf 'feat(app): window helpers (new window, open note in window, window-of-note)\n\nCo-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>')"
```

---

### Task 5: Port note_window to a window-group model (single tab, no strip, no title box)

This restores `make app`. Goal: the GUI opens per-WinMeta, holds an array of tabs (just one for now), drops the H1 title box, and draws the active note's name as plain text in the title bar. No `+`, reorder, or rename yet.

**Files:**
- Modify: `src/note_window.h`, `src/note_window.c`
- Modify: `src/main.c` (startup opens windows, not notes)
- Modify: `src/tray.c` (reopen/new map to windows)

**Interfaces:**
- Consumes: `WinMeta`, `app_window_of_note`, `app_new_window` (Tasks 1-4).
- Produces:
  - `HWND note_window_open(AppState* app, WinMeta* win);` (signature change: WinMeta, not NoteMeta)
  - `HWND note_window_find_open_window(const char* win_id);`
  - `HWND note_window_find_by_note(const char* note_id);` (registry scan: returns the window hosting that note, or NULL)
  - `void note_window_activate_note(HWND hwnd, const char* note_id);` (focus + switch to that tab)

- [ ] **Step 1: Update note_window.h**

Open `src/note_window.h`. Change the open signature and add the new lookups. Replace the `note_window_open` declaration and add:

```c
HWND note_window_open(AppState* app, WinMeta* win);
HWND note_window_find_open_window(const char* win_id);
HWND note_window_find_by_note(const char* note_id);
void note_window_activate_note(HWND hwnd, const char* note_id);
void note_window_close(HWND hwnd);
void note_window_register_class(HINSTANCE hInst);
```

(Keep whatever include of `app.h`/`prefs.h` already exists so `WinMeta` is visible.)

- [ ] **Step 2: Replace the NoteWin struct + helpers in note_window.c**

In `src/note_window.c`, replace the `NoteWin` typedef (lines ~62-71) with the tab-group form and add a tab struct:

```c
typedef struct {
    char id[16];      /* note id */
    HWND edit;        /* this tab's body RichEdit */
} NoteTab;

typedef struct {
    AppState* app;
    char  win_id[16];          /* resolve WinMeta* fresh via prefs_find_window */
    NoteTab tab[WIN_MAX_TABS];
    int   ntabs;
    int   active;
    HWND  btns[NUM_BTNS];
    HWND  wbtns[NUM_WBTNS];
    int   whot[NUM_WBTNS];
    int   active_fmt[NUM_BTNS];
} NoteWin;
```

Replace the `nw_meta` helper (resolves a note) with a window resolver and active-tab accessors:

```c
static WinMeta* nw_win(NoteWin* nw) { return prefs_find_window(&nw->app->prefs, nw->win_id); }
static NoteTab* nw_cur(NoteWin* nw) { return (nw->ntabs > 0) ? &nw->tab[nw->active] : NULL; }
static HWND     nw_edit(NoteWin* nw) { NoteTab* t = nw_cur(nw); return t ? t->edit : NULL; }
static NoteMeta* nw_note_meta(NoteWin* nw, int i) {
    return prefs_find(&nw->app->prefs, nw->tab[i].id);
}
```

Throughout `note_window.c`, every existing use of `nw->edit` becomes `nw_edit(nw)` (active tab's body), and every `nw_meta(nw)` (the old note resolver) is rewritten to resolve the active tab's `NoteMeta` via `prefs_find(&nw->app->prefs, nw_cur(nw)->id)`. Delete the old `nw->title` field and all references to it (the title box is removed). The format-button/caption-button code (`nw->btns`, `nw->wbtns`, `nw->whot`, `nw->active_fmt` replacing `nw->active`) is unchanged except the rename from `nw->active[]` to `nw->active_fmt[]` for the sidebar highlight (the struct now uses `active` for the active tab index).

Apply these renames consistently:
- `nw->active[i]` (sidebar highlight) -> `nw->active_fmt[i]` everywhere in `nw_draw_button`, `nw_clear_toggles`, `nw_update_toggles`, `nw_wrap_inline`.
- `nw->edit` -> `nw_edit(nw)` in `nw_update_toggles`, `nw_caret_on_heading`, `nw_body_text` calls, `nw_wrap_inline`, `nw_list_prefix`, `nw_handle_enter`, `nw_apply_format`, `nw_reformat_now`, WM_SETFOCUS, WM_NOTIFY, WM_TIMER.

- [ ] **Step 3: Rewrite content load/save for the active tab and remove the title box**

Replace `nw_load_content` and `nw_save_content` to operate on a given tab index (no title control):

```c
/* Load tab i's note: "# name\n\n body" -> name is metadata, body fills the edit. */
static void nw_load_tab(NoteWin* nw, int i) {
    NoteMeta* m = prefs_find(&nw->app->prefs, nw->tab[i].id);
    if (!m) return;
    char path[260];
    app_note_path(nw->app, m, path, sizeof path);
    char* txt = NULL; size_t n = 0;
    const char* body = "";
    if (store_read_note(path, &txt, &n)) {
        body = txt;
        if (n >= 2 && txt[0] == '#' && txt[1] == ' ') {
            size_t k = 2; while (k < n && txt[k] != '\n' && txt[k] != '\r') k++;
            if (k < n && txt[k] == '\r') k++;
            if (k < n && txt[k] == '\n') k++;
            size_t j = k;
            if (j < n && txt[j] == '\r') j++;
            if (j < n && txt[j] == '\n') k = j + 1;
            body = txt + k;
        }
    }
    SetWindowTextA(nw->tab[i].edit, body);
    free(txt);
}

/* Save tab i's body, prefixing the current name as "# name". */
static void nw_save_tab(NoteWin* nw, int i) {
    NoteMeta* m = prefs_find(&nw->app->prefs, nw->tab[i].id);
    if (!m) return;
    int bl = GetWindowTextLengthA(nw->tab[i].edit);
    char* b = malloc((size_t)bl + 1); if (!b) return;
    GetWindowTextA(nw->tab[i].edit, b, bl + 1);
    int tl = (int)strlen(m->name);
    size_t need = (tl > 0 ? (size_t)tl + 4 : 0) + (size_t)bl + 1;
    char* buf = malloc(need); if (!buf) { free(b); return; }
    size_t off = 0;
    if (tl > 0) off += (size_t)sprintf(buf + off, "# %s\n\n", m->name);
    memcpy(buf + off, b, (size_t)bl); off += (size_t)bl;
    char path[260];
    app_note_path(nw->app, m, path, sizeof path);
    store_write_note(path, buf, off);
    free(buf); free(b);
}
```

Replace the existing `nw_save_content(nw)` calls (in `nw_reformat_now`, WM_TIMER, `note_window_close`) with `nw_save_tab(nw, nw->active)`. Delete `nw_style_title` and `nw_load_content`/`nw_save_content`.

- [ ] **Step 4: Create one edit per tab; lay out + paint the tab name**

In `WM_CREATE`, remove the title RichEdit creation block. After creating the sidebar + caption buttons and extending the frame, create the per-tab edits from the WinMeta tab list. Replace the title/edit creation with:

```c
        /* one body RichEdit per tab; active shown, rest hidden */
        WinMeta* w = prefs_find_window(&nw->app->prefs, nw->win_id);
        nw->ntabs = w ? w->ntabs : 0;
        if (nw->ntabs > WIN_MAX_TABS) nw->ntabs = WIN_MAX_TABS;
        nw->active = (w && w->active < nw->ntabs) ? w->active : 0;
        for (int i = 0; i < nw->ntabs; i++) {
            snprintf(nw->tab[i].id, sizeof nw->tab[i].id, "%s", w->tabs[i]);
            nw->tab[i].edit = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL
                  | (i == nw->active ? WS_VISIBLE : 0),
                0, 0, 100, 100, hwnd, (HMENU)ID_EDIT, cs->hInstance, NULL);
            SendMessageW(nw->tab[i].edit, EM_SETBKGNDCOLOR, 0, (LPARAM)COL_BG);
            CHARFORMAT2W cf; memset(&cf, 0, sizeof cf);
            cf.cbSize = sizeof cf; cf.dwMask = CFM_COLOR; cf.crTextColor = COL_TEXT;
            SendMessageW(nw->tab[i].edit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
            SendMessageW(nw->tab[i].edit, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK);
            nw_load_tab(nw, i);
            nw_apply_format_edit(nw, i);    /* style each loaded body */
        }
```

Add a per-edit format pass helper (factor the body of `nw_apply_format` to take an HWND, keeping the existing logic). Add near `nw_apply_format`:

```c
static void nw_apply_format_edit(NoteWin* nw, int i);  /* fwd */
```

and make `nw_apply_format(nw)` call `nw_apply_format_edit(nw, nw->active)`. `nw_apply_format_edit` is the old `nw_apply_format` body with `nw->edit` replaced by `nw->tab[i].edit`.

Update `nw_layout` to size all tab edits to the body rect (only the active is visible) and drop the title control:

```c
    for (int i = 0; i < nw->ntabs; i++)
        MoveWindow(nw->tab[i].edit, bd.left, bd.top,
                   bd.right - bd.left, bd.bottom - bd.top, TRUE);
```

In `WM_PAINT`, after drawing the dividers, draw the active note's name as plain text in the title bar (placeholder for the strip in Task 6):

```c
        NoteMeta* am = (nw->ntabs > 0) ? prefs_find(&nw->app->prefs, nw->tab[nw->active].id) : NULL;
        if (am) {
            RECT tr; int dummy;
            nw_regions(hwnd, &tr, NULL, &dummy, &dummy);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, COL_TEXT);
            HFONT f = CreateFontW(-18,0,0,0,FW_SEMIBOLD,0,0,0,0,0,0,0,0,L"Segoe UI");
            HFONT old = (HFONT)SelectObject(hdc, f);
            wchar_t wname[128];
            MultiByteToWideChar(CP_ACP, 0, am->name, -1, wname, 128);
            DrawTextW(hdc, wname, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(hdc, old); DeleteObject(f);
        }
```

In `WM_DESTROY`, the per-tab edits are child windows destroyed automatically; just keep freeing `nw`. In `WM_NOTIFY`/`WM_TIMER`/`WM_SETFOCUS`, replace `ID_TITLE` handling (deleted) and use `nw_edit(nw)`. Remove the `ID_TITLE` branch in WM_NOTIFY entirely. Remove `#define ID_TITLE`.

- [ ] **Step 5: Update open/close + registry to be window-keyed**

Replace the registry (keyed by note id) to key by window id, and rewrite `note_window_open`/`note_window_close` + add the new lookups. Replace the registry section and the open/close functions:

```c
#define NW_REGISTRY_CAP 64
typedef struct { char win_id[16]; HWND hwnd; } NwRegEntry;
static NwRegEntry s_registry[NW_REGISTRY_CAP];

static void nw_registry_add(const char* win_id, HWND hwnd) {
    for (int i = 0; i < NW_REGISTRY_CAP; i++)
        if (s_registry[i].hwnd == NULL) {
            snprintf(s_registry[i].win_id, sizeof s_registry[i].win_id, "%s", win_id);
            s_registry[i].hwnd = hwnd; return;
        }
}
static void nw_registry_remove(HWND hwnd) {
    for (int i = 0; i < NW_REGISTRY_CAP; i++)
        if (s_registry[i].hwnd == hwnd) { s_registry[i].hwnd = NULL; s_registry[i].win_id[0] = '\0'; return; }
}
HWND note_window_find_open_window(const char* win_id) {
    for (int i = 0; i < NW_REGISTRY_CAP; i++)
        if (s_registry[i].hwnd && strcmp(s_registry[i].win_id, win_id) == 0)
            return s_registry[i].hwnd;
    return NULL;
}
HWND note_window_find_by_note(const char* note_id) {
    for (int i = 0; i < NW_REGISTRY_CAP; i++) {
        if (!s_registry[i].hwnd) continue;
        NoteWin* nw = nw_get(s_registry[i].hwnd);
        if (!nw) continue;
        for (int t = 0; t < nw->ntabs; t++)
            if (strcmp(nw->tab[t].id, note_id) == 0) return s_registry[i].hwnd;
    }
    return NULL;
}
```

```c
HWND note_window_open(AppState* app, WinMeta* win) {
    NoteWin* nw = calloc(1, sizeof *nw);
    if (!nw) return NULL;
    nw->app = app;
    snprintf(nw->win_id, sizeof nw->win_id, "%s", win->id);
    HWND h = CreateWindowExW(0, NOTE_CLASS, L"quickNote",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VISIBLE,
        win->x, win->y, win->w, win->h,
        NULL, NULL, GetModuleHandleW(NULL), nw);
    if (h) nw_registry_add(nw->win_id, h);
    else   free(nw);
    return h;
}

void note_window_close(HWND hwnd) {
    NoteWin* nw = nw_get(hwnd);
    if (nw) {
        WinMeta* w = nw_win(nw);
        if (w) {
            nw_save_geometry(hwnd, w);          /* see step 6 */
            for (int i = 0; i < nw->ntabs; i++) nw_save_tab(nw, i);
            w->active = nw->active;
            /* window stays in prefs.windows with its tab list (reopened next launch) */
        }
    }
    DestroyWindow(hwnd);
}
```

Add `note_window_activate_note` (used by the tray in Task 11; safe to add now):

```c
void note_window_activate_note(HWND hwnd, const char* note_id) {
    NoteWin* nw = nw_get(hwnd);
    if (!nw) return;
    for (int t = 0; t < nw->ntabs; t++) {
        if (strcmp(nw->tab[t].id, note_id) == 0) {
            if (t != nw->active) {
                ShowWindow(nw->tab[nw->active].edit, SW_HIDE);
                nw->active = t;
                ShowWindow(nw->tab[t].edit, SW_SHOW);
                InvalidateRect(hwnd, NULL, TRUE);
            }
            break;
        }
    }
    SetForegroundWindow(hwnd);
    if (nw_edit(nw)) SetFocus(nw_edit(nw));
}
```

- [ ] **Step 6: Update nw_save_geometry to take a WinMeta**

Change `nw_save_geometry(HWND, NoteMeta*)` to `nw_save_geometry(HWND, WinMeta*)` (same body, writing `w->x/y/w/h`). Update its caller in `WM_EXITSIZEMOVE` to resolve `nw_win(nw)`.

- [ ] **Step 7: Update main.c startup**

Open `src/main.c`. Where it currently loops notes and calls `note_window_open(app, &note)`, change it to loop windows. Replace that loop with:

```c
    if (app.prefs.wcount == 0) {
        WinMeta* w = app_new_window(&app);     /* first run: one window, one note */
        if (w) note_window_open(&app, w);
    } else {
        for (size_t i = 0; i < app.prefs.wcount; i++)
            note_window_open(&app, &app.prefs.windows[i]);
    }
```

(If `main.c` references `app_new_note`/note open for the empty case, replace with the above.)

- [ ] **Step 8: Update tray.c open paths (minimal, full behavior in Task 11)**

Open `src/tray.c`. Change "New Note" to open a new window:

```c
    WinMeta* w = app_new_window(app);
    if (w) note_window_open(app, w);
```

For the per-note reopen menu entries, change the handler to: if `note_window_find_by_note(id)` returns a window, `note_window_activate_note(win, id)`; else `app_open_note_in_window(app, id)` then `note_window_open`. (The tray currently calls `note_window_find_open`/`note_window_open(app, meta)`; update those call sites to the window-based equivalents. The tray menu still lists `prefs.notes[]` names.)

Remove any remaining references to the deleted `note_window_find_open(id)`.

- [ ] **Step 9: Build + run + verify**

Run: `taskkill //IM quicknote.exe //F; make test && make app`
Expected: `make test` PASS; `make app` builds `quicknote.exe` with zero warnings.

Launch and screenshot:
```bash
./quicknote.exe &
sleep 2
```
Capture a screenshot (full-screen) and confirm: one window opens, dark, two dividers, qN cell, format sidebar, native-style – ▢ ✕, the active note's **name drawn in the title bar** (no separate H1 box below), body editable. Close it.

- [ ] **Step 10: Commit**

```bash
git add src/note_window.h src/note_window.c src/main.c src/tray.c
git commit -m "$(printf 'refactor(ui): window-group model, per-tab body, drop title box\n\nWindows now open per WinMeta with one RichEdit per tab (single tab for\nnow). Registry + open/close keyed by window id. Name drawn in the title\nbar. make app restored.\n\nCo-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>')"
```

---

### Task 6: Tab strip rendering + hit rectangles

Draw all tabs and a `+` in the title bar; compute their rects once for both paint and hit-testing. No interactions yet (Task 7).

**Files:**
- Modify: `src/note_window.c`

**Interfaces:**
- Consumes: `NoteWin` tab list.
- Produces:
  - `#define DRAG_GAP 36`, `PLUS_W 28`, `TAB_MIN 48`, `TAB_MAX 200`, `TAB_CLOSE_W 18`, `DRAG_THRESHOLD 6`.
  - `typedef enum { HIT_NONE, HIT_TAB, HIT_CLOSE, HIT_PLUS } TabHit;`
  - `static TabHit nw_tab_hit(NoteWin* nw, HWND hwnd, int cx, int cy, int* out_idx);` — classify a client point in the strip.
  - `static void nw_tab_rect(NoteWin* nw, HWND hwnd, int i, RECT* r);` — the i-th tab's rect.
  - `static void nw_plus_rect(NoteWin* nw, HWND hwnd, RECT* r);`

- [ ] **Step 1: Add constants + strip geometry helpers**

Near the other layout `#define`s add the constants above and the enum. Add geometry helpers that all share one width computation:

```c
static int nw_strip_left(void)  { return SIDEBAR_W + 1; }
static int nw_strip_right(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    int btns_l = rc.right - NUM_WBTNS * WINBTN_W;
    return btns_l - DRAG_GAP;
}
static int nw_tab_w(NoteWin* nw, HWND hwnd) {
    int avail = nw_strip_right(hwnd) - nw_strip_left() - PLUS_W;
    int n = nw->ntabs > 0 ? nw->ntabs : 1;
    int w = avail / n;
    if (w > TAB_MAX) w = TAB_MAX;
    if (w < TAB_MIN) w = TAB_MIN;
    return w;
}
static void nw_tab_rect(NoteWin* nw, HWND hwnd, int i, RECT* r) {
    int w = nw_tab_w(nw, hwnd), l = nw_strip_left() + i * w;
    SetRect(r, l, 0, l + w, TITLEBAR_H);
}
static void nw_plus_rect(NoteWin* nw, HWND hwnd, RECT* r) {
    int w = nw_tab_w(nw, hwnd), l = nw_strip_left() + nw->ntabs * w;
    SetRect(r, l, 0, l + PLUS_W, TITLEBAR_H);
}
static TabHit nw_tab_hit(NoteWin* nw, HWND hwnd, int cx, int cy, int* out_idx) {
    if (cy < 0 || cy >= TITLEBAR_H) return HIT_NONE;
    for (int i = 0; i < nw->ntabs; i++) {
        RECT r; nw_tab_rect(nw, hwnd, i, &r);
        if (cx >= r.left && cx < r.right) {
            if (out_idx) *out_idx = i;
            if (cx >= r.right - TAB_CLOSE_W) return HIT_CLOSE;
            return HIT_TAB;
        }
    }
    RECT pr; nw_plus_rect(nw, hwnd, &pr);
    if (cx >= pr.left && cx < pr.right) return HIT_PLUS;
    return HIT_NONE;
}
```

- [ ] **Step 2: Draw the strip in WM_PAINT**

Replace the plain-name drawing block added in Task 5 with a tab-strip renderer. Add a helper and call it from `WM_PAINT`:

```c
static void nw_paint_tabs(NoteWin* nw, HWND hwnd, HDC hdc) {
    HFONT f = CreateFontW(-15,0,0,0,FW_SEMIBOLD,0,0,0,0,0,0,0,0,L"Segoe UI"); /* ~H3 */
    HFONT old = (HFONT)SelectObject(hdc, f);
    SetBkMode(hdc, TRANSPARENT);
    for (int i = 0; i < nw->ntabs; i++) {
        RECT r; nw_tab_rect(nw, hwnd, i, &r);
        int isact = (i == nw->active);
        HBRUSH bg = CreateSolidBrush(isact ? COL_TOGGLE : COL_BG);
        FillRect(hdc, &r, bg); DeleteObject(bg);
        if (isact) {                                  /* accent top edge */
            RECT top = { r.left, 0, r.right, 2 };
            HBRUSH ac = CreateSolidBrush(RGB(0x6a,0x9a,0xff));
            FillRect(hdc, &top, ac); DeleteObject(ac);
        }
        NoteMeta* m = prefs_find(&nw->app->prefs, nw->tab[i].id);
        wchar_t wname[128];
        MultiByteToWideChar(CP_ACP, 0, m && m->name[0] ? m->name : "Untitled", -1, wname, 128);
        RECT tr = { r.left + 8, r.top, r.right - TAB_CLOSE_W, r.bottom };
        SetTextColor(hdc, COL_TEXT);
        DrawTextW(hdc, wname, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT xr = { r.right - TAB_CLOSE_W, r.top, r.right, r.bottom };   /* close glyph */
        SetTextColor(hdc, RGB(0xb0,0xb0,0xb8));
        DrawTextW(hdc, L"\x2715", -1, &xr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    RECT pr; nw_plus_rect(nw, hwnd, &pr);            /* + button */
    SetTextColor(hdc, COL_TEXT);
    DrawTextW(hdc, L"+", -1, &pr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, old); DeleteObject(f);
}
```

In `WM_PAINT`, after the dividers, call `nw_paint_tabs(nw, hwnd, hdc);` (replacing the Task-5 name-drawing code).

- [ ] **Step 3: Build + run + verify**

Run: `taskkill //IM quicknote.exe //F; make app`
Launch, screenshot. Confirm: a single tab with the note name + a ✕, a `+` to its right, accent on the active tab, the – ▢ ✕ still top-right with a gap before them.

- [ ] **Step 4: Commit**

```bash
git add src/note_window.c
git commit -m "$(printf 'feat(ui): render Windows-Terminal-style tab strip + hit rects\n\nCo-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>')"
```

---

### Task 7: New tab (+ / Ctrl+N) and tab activation

**Files:**
- Modify: `src/note_window.c`

**Interfaces:**
- Consumes: `nw_tab_hit`, `app_new_note`, `app_apply_format_edit`.
- Produces:
  - `static void nw_add_tab(NoteWin* nw, HWND hwnd);` — new note as a new tab, becomes active.
  - `static void nw_activate(NoteWin* nw, HWND hwnd, int i);` — show tab i, hide previous.
  - HTCLIENT over the strip so clicks arrive; `WM_LBUTTONDOWN` routing.

- [ ] **Step 1: Implement activate + add-tab**

```c
static void nw_sync_winmeta(NoteWin* nw) {        /* mirror runtime tabs into prefs */
    WinMeta* w = nw_win(nw);
    if (!w) return;
    w->ntabs = nw->ntabs; w->active = nw->active;
    for (int i = 0; i < nw->ntabs; i++)
        snprintf(w->tabs[i], sizeof w->tabs[i], "%s", nw->tab[i].id);
}

static void nw_activate(NoteWin* nw, HWND hwnd, int i) {
    if (i < 0 || i >= nw->ntabs || i == nw->active) return;
    ShowWindow(nw->tab[nw->active].edit, SW_HIDE);
    nw->active = i;
    ShowWindow(nw->tab[i].edit, SW_SHOW);
    nw_sync_winmeta(nw);
    InvalidateRect(hwnd, NULL, TRUE);
    SetFocus(nw->tab[i].edit);
    nw_update_toggles(nw);
}

static void nw_add_tab(NoteWin* nw, HWND hwnd) {
    if (nw->ntabs >= WIN_MAX_TABS) return;
    NoteMeta* m = app_new_note(nw->app);
    if (!m) return;
    int i = nw->ntabs++;
    snprintf(nw->tab[i].id, sizeof nw->tab[i].id, "%s", m->id);
    nw->tab[i].edit = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
        WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
        0, 0, 100, 100, hwnd, (HMENU)ID_EDIT, GetModuleHandleW(NULL), NULL);
    SendMessageW(nw->tab[i].edit, EM_SETBKGNDCOLOR, 0, (LPARAM)COL_BG);
    CHARFORMAT2W cf; memset(&cf, 0, sizeof cf);
    cf.cbSize = sizeof cf; cf.dwMask = CFM_COLOR; cf.crTextColor = COL_TEXT;
    SendMessageW(nw->tab[i].edit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    SendMessageW(nw->tab[i].edit, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK);
    nw_load_tab(nw, i);
    nw_layout(hwnd, nw);
    ShowWindow(nw->tab[nw->active].edit, SW_HIDE);
    nw->active = i;
    ShowWindow(nw->tab[i].edit, SW_SHOW);
    nw_sync_winmeta(nw);
    InvalidateRect(hwnd, NULL, TRUE);
    SetFocus(nw->tab[i].edit);
}
```

- [ ] **Step 2: Route clicks — NCHITTEST + LBUTTONDOWN**

In `WM_NCHITTEST`, before returning `HTCAPTION` for the title bar, return `HTCLIENT` when the point is over a tab or the `+`:

```c
        if (pt.y < TITLEBAR_H) {
            int idx; TabHit h = nw_tab_hit(nw, hwnd, pt.x, pt.y, &idx);
            if (h != HIT_NONE) return HTCLIENT;     /* tabs/+ handle the click */
            return HTCAPTION;                       /* bare bar / drag gap */
        }
```

Add a `WM_LBUTTONDOWN` case:

```c
    case WM_LBUTTONDOWN:
        if (nw) {
            int idx = -1;
            TabHit h = nw_tab_hit(nw, hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &idx);
            if (h == HIT_PLUS) { nw_add_tab(nw, hwnd); return 0; }
            if (h == HIT_TAB)  { nw_activate(nw, hwnd, idx); return 0; }
            /* HIT_CLOSE handled in Task 8 */
        }
        return 0;
```

Add Ctrl+N -> new tab in `nw_handle_key`. Replace the existing `'N'` line:

```c
    if (mf->wParam == 'N' && !shift) { nw_add_tab(nw, GetParent((HWND)mf->nmhdr.hwndFrom)); return 1; }
```

Since `nw_handle_key` already has `hwnd`, use it directly instead:

```c
    if (mf->wParam == 'N' && !shift) { nw_add_tab(nw, hwnd); return 1; }
```

(Delete the old `nw_new_note` call for Ctrl+N; `nw_new_note` is repurposed for Alt+N in Task 11 — keep the function for now.)

- [ ] **Step 3: Build + run + verify**

Run: `taskkill //IM quicknote.exe //F; make app`
Launch. Click `+` a few times (and press Ctrl+N): new tabs appear, narrowing as count grows; clicking a tab switches the visible body; typing in one tab and switching preserves each body. Screenshot 3 tabs.

- [ ] **Step 4: Commit**

```bash
git add src/note_window.c
git commit -m "$(printf 'feat(ui): add tabs via + / Ctrl+N and click-to-activate\n\nCo-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>')"
```

---

### Task 8: Close a tab (✕ / middle-click); empty window closes

**Files:**
- Modify: `src/note_window.c`

**Interfaces:**
- Consumes: `nw_tab_hit`, `nw_activate`, `nw_sync_winmeta`.
- Produces: `static void nw_close_tab(NoteWin* nw, HWND hwnd, int i);` — remove tab i (note stays on disk); if it was the last tab, close the window.

- [ ] **Step 1: Implement close**

```c
static void nw_close_tab(NoteWin* nw, HWND hwnd, int i) {
    if (i < 0 || i >= nw->ntabs) return;
    nw_save_tab(nw, i);
    DestroyWindow(nw->tab[i].edit);
    for (int k = i; k < nw->ntabs - 1; k++) nw->tab[k] = nw->tab[k+1];
    nw->ntabs--;
    if (nw->ntabs == 0) {                 /* last tab: close the window */
        WinMeta* w = nw_win(nw);
        if (w) { w->ntabs = 0; prefs_remove_window(&nw->app->prefs, w->id); }
        DestroyWindow(hwnd);
        return;
    }
    if (nw->active >= nw->ntabs) nw->active = nw->ntabs - 1;
    else if (i < nw->active)     nw->active--;
    ShowWindow(nw->tab[nw->active].edit, SW_SHOW);
    nw_layout(hwnd, nw);
    nw_sync_winmeta(nw);
    InvalidateRect(hwnd, NULL, TRUE);
    SetFocus(nw->tab[nw->active].edit);
    nw_update_toggles(nw);
}
```

- [ ] **Step 2: Route close clicks**

In `WM_LBUTTONDOWN`, handle `HIT_CLOSE`:

```c
            if (h == HIT_CLOSE) { nw_close_tab(nw, hwnd, idx); return 0; }
```

Add middle-click close:

```c
    case WM_MBUTTONDOWN:
        if (nw) {
            int idx = -1;
            TabHit h = nw_tab_hit(nw, hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &idx);
            if (h == HIT_TAB || h == HIT_CLOSE) { nw_close_tab(nw, hwnd, idx); return 0; }
        }
        return 0;
```

Note `nw_close_tab` may `DestroyWindow(hwnd)`; after it returns in the message handler, return 0 immediately (do not touch `nw`). The `return 0;` right after the call satisfies this.

Also: the title-bar X already calls `note_window_close` (closes all tabs, notes persist) — verify `note_window_close` (Task 5) still removes the window from prefs on X. Update `note_window_close` to also `prefs_remove_window` so a closed window doesn't reopen on next launch:

```c
    /* in note_window_close, after saving: */
        if (w) prefs_remove_window(&nw->app->prefs, w->id);
```

(Closing the X means "this window is no longer open"; its notes remain in `prefs.notes` and on disk, reachable from the tray.)

- [ ] **Step 3: Build + run + verify**

Run: `taskkill //IM quicknote.exe //F; make app`
Launch. Open 3 tabs, close the middle one with its ✕ (and one with middle-click): remaining tabs reflow, active stays valid. Close the last remaining tab: the window closes. Reopen the app: the closed-by-X window does not return. Screenshot before/after a close.

- [ ] **Step 4: Commit**

```bash
git add src/note_window.c
git commit -m "$(printf 'feat(ui): close tabs (X / middle-click); empty window closes\n\nCo-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>')"
```

---

### Task 9: Drag a tab to reorder

**Files:**
- Modify: `src/note_window.c`

**Interfaces:**
- Consumes: `nw_tab_hit`, `nw_tab_rect`, `nw_tab_w`, `nw_sync_winmeta`.
- Produces: reorder via mouse capture; drag state fields added to `NoteWin`.

- [ ] **Step 1: Add drag state to NoteWin**

Add fields to the `NoteWin` struct: `int drag_tab; int drag_dx; int press_x; int dragging;` (initialize `drag_tab = -1` is unnecessary since `calloc` zeroes; treat `dragging == 0` as inactive).

- [ ] **Step 2: Arm drag on LBUTTONDOWN over a tab**

In the `HIT_TAB` branch of `WM_LBUTTONDOWN`, before/after activating, arm a potential drag and capture the mouse:

```c
            if (h == HIT_TAB) {
                nw_activate(nw, hwnd, idx);
                RECT r; nw_tab_rect(nw, hwnd, idx, &r);
                nw->drag_tab = idx;
                nw->drag_dx  = GET_X_LPARAM(lp) - r.left;
                nw->press_x  = GET_X_LPARAM(lp);
                nw->dragging = 0;
                SetCapture(hwnd);
                return 0;
            }
```

- [ ] **Step 3: Reorder on MOUSEMOVE with capture**

Add to `WM_MOUSEMOVE` (add the case if absent):

```c
    case WM_MOUSEMOVE:
        if (nw && GetCapture() == hwnd && nw->drag_tab >= 0) {
            int x = GET_X_LPARAM(lp);
            if (!nw->dragging && abs(x - nw->press_x) > DRAG_THRESHOLD) nw->dragging = 1;
            if (nw->dragging) {
                int w = nw_tab_w(nw, hwnd);
                int center = x - nw->drag_dx + w / 2;          /* dragged tab center */
                int target = (center - nw_strip_left()) / w;
                if (target < 0) target = 0;
                if (target > nw->ntabs - 1) target = nw->ntabs - 1;
                if (target != nw->drag_tab) {                  /* shift one step */
                    NoteTab moved = nw->tab[nw->drag_tab];
                    if (target > nw->drag_tab)
                        for (int k = nw->drag_tab; k < target; k++) nw->tab[k] = nw->tab[k+1];
                    else
                        for (int k = nw->drag_tab; k > target; k--) nw->tab[k] = nw->tab[k-1];
                    nw->tab[target] = moved;
                    nw->active = target;
                    nw->drag_tab = target;
                    nw_sync_winmeta(nw);
                    InvalidateRect(hwnd, NULL, TRUE);
                }
            }
            return 0;
        }
        return 0;   /* (merge with any existing WM_MOUSEMOVE body) */
```

- [ ] **Step 4: End drag on LBUTTONUP**

```c
    case WM_LBUTTONUP:
        if (nw && GetCapture() == hwnd) {
            ReleaseCapture();
            nw->drag_tab = -1; nw->dragging = 0;
        }
        return 0;
```

Initialize `nw->drag_tab = -1` in `WM_CREATE` after the struct is wired (since `calloc` gives 0, set it explicitly to -1 so tab 0 isn't treated as armed).

- [ ] **Step 5: Build + run + verify**

Run: `taskkill //IM quicknote.exe //F; make app`
Launch. Open 4 tabs, drag the first tab to the right past the others: order updates live, the dragged tab stays active. Reopen the app to confirm the new order persisted. Screenshot mid/after reorder.

- [ ] **Step 6: Commit**

```bash
git add src/note_window.c
git commit -m "$(printf 'feat(ui): drag tabs to reorder within the strip\n\nCo-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>')"
```

---

### Task 10: Rename the active tab (Ctrl+R)

**Files:**
- Modify: `src/note_window.c`

**Interfaces:**
- Consumes: active tab, `NoteMeta.name`, `nw_save_tab`.
- Produces:
  - `static void nw_begin_rename(NoteWin* nw, HWND hwnd);`
  - `static void nw_commit_rename(NoteWin* nw, HWND hwnd, int accept);`
  - A subclassed temp edit (`rename_edit`) that commits on Enter / cancels on Esc / commits on focus loss.

- [ ] **Step 1: Add rename fields + a subclass**

Add to `NoteWin`: `HWND rename_edit; int rename_tab;`. Add `#define ID_RENAME 1003`.

Add a subclass proc that forwards Enter/Esc to the parent and commits on kill-focus:

```c
static LRESULT CALLBACK nw_rename_sub(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                      UINT_PTR id, DWORD_PTR ref) {
    NoteWin* nw = (NoteWin*)ref;
    HWND parent = GetParent(h);
    if (msg == WM_KEYDOWN && wp == VK_RETURN) { nw_commit_rename(nw, parent, 1); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { nw_commit_rename(nw, parent, 0); return 0; }
    if (msg == WM_KILLFOCUS) { if (nw->rename_edit) nw_commit_rename(nw, parent, 1); }
    return DefSubclassProc(h, msg, wp, lp);
}
```

(Forward-declare `nw_commit_rename` above this.)

- [ ] **Step 2: Begin / commit**

```c
static void nw_begin_rename(NoteWin* nw, HWND hwnd) {
    if (nw->rename_edit || nw->ntabs == 0) return;
    int i = nw->active;
    RECT r; nw_tab_rect(nw, hwnd, i, &r);
    NoteMeta* m = prefs_find(&nw->app->prefs, nw->tab[i].id);
    nw->rename_tab = i;
    nw->rename_edit = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        r.left + 4, 5, (r.right - r.left) - 8, TITLEBAR_H - 10,
        hwnd, (HMENU)ID_RENAME, GetModuleHandleW(NULL), NULL);
    HFONT f = CreateFontW(-15,0,0,0,FW_SEMIBOLD,0,0,0,0,0,0,0,0,L"Segoe UI");
    SendMessageW(nw->rename_edit, WM_SETFONT, (WPARAM)f, TRUE);
    SetWindowTextA(nw->rename_edit, m && m->name[0] ? m->name : "");
    SendMessageW(nw->rename_edit, EM_SETSEL, 0, -1);
    SetWindowSubclass(nw->rename_edit, nw_rename_sub, 0, (DWORD_PTR)nw);
    SetFocus(nw->rename_edit);
}

static void nw_commit_rename(NoteWin* nw, HWND hwnd, int accept) {
    if (!nw->rename_edit) return;
    HWND e = nw->rename_edit;
    nw->rename_edit = NULL;                 /* clear first: kill-focus reentry guard */
    if (accept) {
        char buf[64];
        GetWindowTextA(e, buf, sizeof buf);
        char* s = buf; while (*s == ' ') s++;
        size_t n = strlen(s); while (n > 0 && s[n-1] == ' ') s[--n] = '\0';
        if (s[0]) {
            NoteMeta* m = prefs_find(&nw->app->prefs, nw->tab[nw->rename_tab].id);
            if (m) { snprintf(m->name, sizeof m->name, "%s", s); nw_save_tab(nw, nw->rename_tab); }
        }
    }
    DestroyWindow(e);
    InvalidateRect(hwnd, NULL, TRUE);
    if (nw_edit(nw)) SetFocus(nw_edit(nw));
}
```

- [ ] **Step 3: Bind Ctrl+R**

In `nw_handle_key`, add (after the new-tab line):

```c
    if (mf->wParam == 'R' && !shift) { nw_begin_rename(nw, hwnd); return 1; }
```

- [ ] **Step 4: Build + run + verify**

Run: `taskkill //IM quicknote.exe //F; make app`
Launch. Press Ctrl+R: an edit appears over the active tab with the name selected. Type a new name, Enter: the tab label updates, and the `.md`'s `# name` line updates (reopen the note to confirm). Esc cancels. Click away: commits. Screenshot mid-rename.

- [ ] **Step 5: Commit**

```bash
git add src/note_window.c
git commit -m "$(printf 'feat(ui): rename the active tab via Ctrl+R (inline overlay)\n\nCo-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>')"
```

---

### Task 11: Alt+N new window; tray reopen focuses/activates

**Files:**
- Modify: `src/note_window.c` (Alt+N), `src/tray.c` (reopen behavior)

**Interfaces:**
- Consumes: `app_new_window`, `app_open_note_in_window`, `app_window_of_note`, `note_window_find_by_note`, `note_window_find_open_window`, `note_window_activate_note`.
- Produces: Alt+N spawns a new window; tray menu reopen focuses an existing window+tab or opens a new window.

- [ ] **Step 1: Alt+N in note_window**

`nw_handle_key` only runs for Ctrl (`if (!ctrl) return 0;`). Add Alt handling at the top of `nw_handle_key`, before the ctrl gate:

```c
    BOOL alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    if (alt && !ctrl && mf->wParam == 'N') {
        WinMeta* w = app_new_window(nw->app);
        if (w) note_window_open(nw->app, w);
        return 1;
    }
```

Repurpose/replace `nw_new_note` usage: the old function opened a note window. Delete `nw_new_note` if now unused (Alt+N uses `app_new_window` directly). Ensure no remaining callers.

- [ ] **Step 2: Tray reopen behavior**

In `src/tray.c`, the per-note menu command handler becomes:

```c
    /* reopening note `id` from the tray */
    HWND existing = note_window_find_by_note(id);
    if (existing) {
        note_window_activate_note(existing, id);
    } else {
        WinMeta* w = app_open_note_in_window(app, id);
        if (w) note_window_open(app, w);
    }
```

"New Note" stays `app_new_window` + `note_window_open` (from Task 5). Quit unchanged.

- [ ] **Step 3: Build + run + verify**

Run: `taskkill //IM quicknote.exe //F; make app`
Launch. Press Alt+N: a second window opens. In window A open a couple tabs; from the tray, pick a note that is a tab in A: window A comes forward with that tab active. Pick (or first close then pick) a note not open anywhere: a new window opens holding it. Screenshot two windows.

- [ ] **Step 4: Commit**

```bash
git add src/note_window.c src/tray.c
git commit -m "$(printf 'feat(ui): Alt+N new window; tray reopen focuses window+tab\n\nCo-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>')"
```

---

### Task 12: Persistence wiring + docs

Ensure window geometry/order/active save at the right moments and restore on launch; update README + spec status.

**Files:**
- Modify: `src/note_window.c` (save points), `README.md`

**Interfaces:**
- Consumes: `nw_save_geometry(HWND, WinMeta*)`, `nw_sync_winmeta`, `app_shutdown` (already saves prefs).

- [ ] **Step 1: Save geometry on move/size end + sync on changes**

Confirm `WM_EXITSIZEMOVE` calls `nw_save_geometry(hwnd, nw_win(nw))`. Confirm `nw_sync_winmeta` is called after add/close/reorder/activate (Tasks 7-9 already do). On `WM_CLOSE` -> `note_window_close` saves geometry + tabs and removes the window from prefs. `app_shutdown` (in `main.c`'s WM_DESTROY/quit path) calls `prefs_save`. No code change if all present; otherwise add the missing `nw_save_geometry` call in `WM_EXITSIZEMOVE`.

Add a save of active-tab content + geometry on `WM_CLOSE` already covered; additionally persist geometry on maximize/restore by saving in `WM_EXITSIZEMOVE` only (minimized/maximized skipped by `nw_save_geometry`, which checks `IsIconic`/`IsZoomed`).

- [ ] **Step 2: Verify restore round-trip**

Run: `taskkill //IM quicknote.exe //F; make app`
Launch. Open two windows, each with 2-3 tabs in a custom order; move them; rename a tab. Quit the app via the tray (so `app_shutdown` saves). Relaunch: both windows reappear at their positions with the same tabs, order, active tab, and names. Screenshot the restored state.

- [ ] **Step 3: Update README**

In `README.md`, replace the single-note description with the tabbed model: each window holds notes as tabs; `+`/Ctrl+N add a tab, Alt+N opens a window, drag to reorder, ✕/middle-click closes a tab, Ctrl+R renames the active tab, Ctrl+Shift+D deletes the active note. Note the data model: `preferences.json` now has a `windows` array (geometry + tab order + active) alongside `notes`. Keep the formatting-sidebar table.

- [ ] **Step 4: Commit**

```bash
git add src/note_window.c README.md
git commit -m "$(printf 'feat(ui): persist + restore windows (geometry, tab order, active); docs\n\nCo-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>')"
```

- [ ] **Step 5: Mark spec done**

Edit `docs/superpowers/specs/2026-06-29-tabbed-notes-design.md` status to `implemented`. Commit:

```bash
git add docs/superpowers/specs/2026-06-29-tabbed-notes-design.md
git commit -m "$(printf 'docs: mark tabbed-notes spec implemented\n\nCo-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>')"
```

---

## Notes for the implementer

- **Delete (Ctrl+Shift+D)** already exists via `nw_delete_note`. Update it to act on the active note and remove that tab (reuse `nw_close_tab` logic after `app_delete_note`): after deleting the note's file/entry, remove the tab with `nw_close_tab`-style array shift; if it was the last tab, close the window. Fold this into Task 8 if convenient (it shares the close path).
- All per-tab edits share control id `ID_EDIT`; the `WM_NOTIFY` handlers act on the active tab via `nw_edit(nw)`. EN_CHANGE/EN_MSGFILTER/EN_SELCHANGE from a hidden tab won't fire (hidden, not focused), so acting on the active tab is correct.
- The live-format debounce timer (`WM_TIMER`) must format the **active** tab (`nw_apply_format_edit(nw, nw->active)`).
- When `nw_close_tab` or `note_window_close` calls `DestroyWindow(hwnd)`, return from the message immediately; do not dereference `nw` afterward (WM_DESTROY frees it).
```
