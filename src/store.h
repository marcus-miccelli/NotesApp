#ifndef STORE_H
#define STORE_H
#include <stdbool.h>
#include <stddef.h>

bool store_write_note(const char* path, const char* text, size_t len);
bool store_read_note(const char* path, char** out_text, size_t* out_len);
bool store_delete_note(const char* path);

#endif
