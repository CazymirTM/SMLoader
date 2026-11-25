// SMContent (SMLoader 1.5.0): add or edit game data in memory. See content.cpp
// and docs/CONTENT.md.
#pragma once
#include <stdint.h>
#include "../../include/smloader.h"

// The API entry points (SMLoaderAPI.ContentAdd / ContentEdit / ContentAddIcon).
int Content_Add(const char* gamePath, const char* text, size_t len);
int Content_Edit(const char* gamePath, SMContentEditFn fn, void* user);
int Content_AddIcon(const char* itemUuid, const unsigned char* bgra);

// Call once, after every mod's SMMod_Init: installs the hooks if any mod
// registered content, otherwise does nothing.
void Content_Install(const wchar_t* loaderDir, void (*log)(const char* fmt, ...),
                     int (*patchImport)(const char* dll, const char* func, void* hook, void** original));

// Code hooks, shared with the rest of the loader. ScanUnique: the only match of
// a byte pattern ("48 8B ?? ..") in the game's code, or null. Hook: redirect a
// function; `steal` = whole, position-independent instructions at its start
// (at least 14 bytes). *original receives the function to call next.
uint8_t* Code_ScanUnique(const char* sig, size_t steal);
bool     Code_Hook(uint8_t* at, size_t steal, const void* hook, void** original);
