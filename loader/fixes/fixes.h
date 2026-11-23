// Built-in game fixes (SMLoader 1.4.0). See fixes.cpp.
#pragma once

// Reads the [fixes] section of loader.ini and applies the enabled fixes.
// Every fix checks the game's code first and leaves it alone if it isn't the
// code it knows. Call once, early, before the game runs physics or networking.
void InstallGameFixes(const wchar_t* iniPath, void (*log)(const char* fmt, ...));

// Byte-signature search in the exe's code; -1 matches any byte. Returns the
// match, or nullptr unless there is exactly one (*matches receives the count).
unsigned char* FindCode(const int* sig, size_t len, int* matches);
bool PatchCode(void* at, const void* bytes, size_t len);
