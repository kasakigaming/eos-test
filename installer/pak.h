#pragma once

// On-disk format of the payload archives embedded in eos-proxy-setup.exe.
// Written by tools\pack.c at build time, read back by installer\auth.h at run
// time. A tiny format on purpose: the installer must stay a single exe with no
// zip library and no dependency on tar.exe being present.
//
//   char  magic[8]        PAK_MAGIC, NUL padded
//   u32   count
//   count entries, each:
//     u32  pathLen        path relative to the game folder, '\' separated
//     u32  dataLen
//     u32  flags          PAK_FLAG_TEXT: substitute {PLACEHOLDER}s on write
//     char path[pathLen]  not NUL terminated
//     byte data[dataLen]

#define PAK_MAGIC      "EOSPAK1"    // 7 chars + NUL = the full 8 byte field
#define PAK_MAGIC_LEN  8
#define PAK_FLAG_TEXT  1u

// Text files are treated as templates. Binaries are copied byte for byte, so
// the test has to be conservative: anything not listed here stays untouched.
static int PakIsTextName(const char* name) {
    static const char* ext[] = { ".ini", ".txt", ".cfg", ".yaml", ".yml", ".json", ".xml" };
    const char* dot = 0;
    for (const char* p = name; *p; p++) if (*p == '.') dot = p;
    if (!dot) return 0;
    for (int i = 0; i < (int)(sizeof(ext) / sizeof(ext[0])); i++) {
        const char* a = dot;
        const char* b = ext[i];
        while (*a && *b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
            if (ca != *b) break;
            a++; b++;
        }
        if (!*a && !*b) return 1;
    }
    return 0;
}
