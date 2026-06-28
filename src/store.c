#include "store.h"
#include <stdio.h>
#include <stdlib.h>

bool store_write_note(const char* path, const char* text, size_t len) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    size_t w = (len > 0) ? fwrite(text, 1, len, f) : 0;
    fclose(f);
    return w == len;
}

bool store_read_note(const char* path, char** out_text, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return false; }
    fseek(f, 0, SEEK_SET);
    char* buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return false; }
    size_t r = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[r] = '\0';
    *out_text = buf;
    *out_len = r;
    return true;
}

bool store_delete_note(const char* path) {
    if (remove(path) == 0) return true;
    /* treat already-absent as success */
    FILE* f = fopen(path, "rb");
    if (!f) return true;
    fclose(f);
    return false;
}
