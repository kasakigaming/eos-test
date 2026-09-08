// Build-time packer. Turns installer\payload\<backend> into build\<backend>.pak,
// which setup.rc then embeds into eos-proxy-setup.exe.
//
//   pack.exe <source folder> <output .pak>
//
// A missing or empty source folder is not an error: it produces an empty pak,
// so the installer builds fine before anyone drops the third party binaries in
// and simply reports that backend as "not bundled".
//
// Files whose name starts with '_' are documentation for whoever fills the
// payload folder and are left out of the archive.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../installer/pak.h"

#define MAXENT 512
#define PATHBUF 1024

typedef struct {
    char  rel[PATHBUF];      // path inside the game folder
    char  full[PATHBUF];
    DWORD size;
    DWORD flags;
} Entry;

static Entry g_ent[MAXENT];
static int   g_count = 0;
static int   g_skipped = 0;

static void PathJoin(char* out, size_t cap, const char* dir, const char* name) {
    size_t n = strlen(dir);
    if (!n)                                    _snprintf_s(out, cap, _TRUNCATE, "%s", name);
    else if (dir[n - 1] == '\\' || dir[n - 1] == '/') _snprintf_s(out, cap, _TRUNCATE, "%s%s", dir, name);
    else                                       _snprintf_s(out, cap, _TRUNCATE, "%s\\%s", dir, name);
}

static void Walk(const char* dir, const char* rel) {
    char pattern[PATHBUF];
    PathJoin(pattern, sizeof(pattern), dir, "*");

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

        char full[PATHBUF], sub[PATHBUF];
        PathJoin(full, sizeof(full), dir, fd.cFileName);
        PathJoin(sub, sizeof(sub), rel, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            Walk(full, sub);
            continue;
        }
        if (fd.cFileName[0] == '_') continue;              // notes for the packer, not payload
        if (fd.nFileSizeHigh) {                            // >4GB, nothing sane is that big
            printf("  ! skipping oversized %s\n", sub);
            g_skipped++;
            continue;
        }
        if (g_count >= MAXENT) {
            printf("  ! payload holds more than %d files, skipping %s\n", MAXENT, sub);
            g_skipped++;
            continue;
        }

        Entry* e = &g_ent[g_count++];
        _snprintf_s(e->rel, PATHBUF, _TRUNCATE, "%s", sub);
        _snprintf_s(e->full, PATHBUF, _TRUNCATE, "%s", full);
        e->size  = fd.nFileSizeLow;
        e->flags = PakIsTextName(fd.cFileName) ? PAK_FLAG_TEXT : 0;
    } while (FindNextFileA(h, &fd));

    FindClose(h);
}

static int WriteAll(HANDLE h, const void* data, DWORD len) {
    const unsigned char* p = (const unsigned char*)data;
    DWORD off = 0;
    while (off < len) {
        DWORD w = 0;
        if (!WriteFile(h, p + off, len - off, &w, NULL) || w == 0) return 0;
        off += w;
    }
    return 1;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        printf("usage: pack.exe <source folder> <output.pak>\n");
        return 1;
    }
    const char* src = argv[1];
    const char* out = argv[2];

    DWORD attr = GetFileAttributesA(src);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
        Walk(src, "");

    HANDLE h = CreateFileA(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("  X cannot write %s (error %lu)\n", out, GetLastError());
        return 1;
    }

    char magic[PAK_MAGIC_LEN];
    memset(magic, 0, sizeof(magic));
    memcpy(magic, PAK_MAGIC, strlen(PAK_MAGIC));

    DWORD count = (DWORD)g_count;
    int ok = WriteAll(h, magic, PAK_MAGIC_LEN) && WriteAll(h, &count, sizeof(count));

    ULONGLONG total = 0;
    for (int i = 0; ok && i < g_count; i++) {
        Entry* e = &g_ent[i];
        DWORD pathLen = (DWORD)strlen(e->rel);

        unsigned char* buf = (unsigned char*)malloc(e->size ? e->size : 1);
        if (!buf) { ok = 0; break; }

        HANDLE f = CreateFileA(e->full, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD got = 0;
        if (f != INVALID_HANDLE_VALUE) {
            while (got < e->size) {
                DWORD r = 0;
                if (!ReadFile(f, buf + got, e->size - got, &r, NULL) || r == 0) break;
                got += r;
            }
            CloseHandle(f);
        }
        if (got != e->size) {
            printf("  X could not read %s\n", e->full);
            free(buf);
            ok = 0;
            break;
        }

        ok = WriteAll(h, &pathLen, sizeof(pathLen)) &&
             WriteAll(h, &e->size, sizeof(e->size)) &&
             WriteAll(h, &e->flags, sizeof(e->flags)) &&
             WriteAll(h, e->rel, pathLen) &&
             (e->size == 0 || WriteAll(h, buf, e->size));
        free(buf);
        total += e->size;
        printf("  + %-52s %8lu%s\n", e->rel, e->size,
               (e->flags & PAK_FLAG_TEXT) ? "  (template)" : "");
    }
    CloseHandle(h);

    if (!ok) {
        DeleteFileA(out);
        printf("  X failed writing %s\n", out);
        return 1;
    }
    printf("  %s: %d file(s), %llu bytes%s\n", out, g_count, total,
           g_count ? "" : "  (payload folder empty - backend will be reported as not bundled)");
    return g_skipped ? 1 : 0;
}
