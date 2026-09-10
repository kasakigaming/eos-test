// EOS Proxy Setup - automated installer for the EOS proxy DLL.
//
// Single file installer: the proxy DLL is embedded as an RCDATA resource, so
// this exe is everything the user needs. It finds games that ship the EOS SDK,
// renames the original EOSSDK-Win64-Shipping.dll to .yes and drops the proxy
// in its place. Several games can be done in one go, and folders outside the
// Steam libraries can be added by hand and are remembered between runs.
//
// The proxy alone does not make a game work, so one pick installs it together
// with a Steam auth backend - see auth.h for that half, and fetch.h for why
// those backends are downloaded rather than carried in here.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define EOS_DLL      "EOSSDK-Win64-Shipping.dll"
#define EOS_BAK      "EOSSDK-Win64-Shipping.yes"
// Every forwarded export inside the proxy stores this name, and the real SDK
// never contains it. That is how the two are told apart.
#define PROXY_MARKER EOS_BAK
#define CONFIG_NAME  "eos-proxy-setup.txt"

// Printed on every run, so a log or a screenshot says which build produced it.
// Keep it in step with the VERSIONINFO block in setup.rc.
#define SETUP_VERSION "t79"
#define SETUP_BUILD   "test 7, 2026-09-10"

#define PATHBUF      1024
#define MAX_TARGETS  128
#define MAX_ROOTS    64
#define MAX_PROBE    (8u * 1024u * 1024u)   // anything bigger is the real SDK
#define SCAN_DEPTH   5

// ---------------------------------------------------------------- utilities

static const unsigned char* MemFind(const unsigned char* hay, size_t hayLen,
                                    const char* needle, size_t needleLen) {
    if (needleLen == 0 || hayLen < needleLen) return NULL;
    for (size_t i = 0; i + needleLen <= hayLen; i++)
        if (hay[i] == (unsigned char)needle[0] && memcmp(hay + i, needle, needleLen) == 0)
            return hay + i;
    return NULL;
}

static void PathJoin(char* out, size_t cap, const char* dir, const char* name) {
    size_t n = strlen(dir);
    if (n && (dir[n - 1] == '\\' || dir[n - 1] == '/'))
        _snprintf_s(out, cap, _TRUNCATE, "%s%s", dir, name);
    else
        _snprintf_s(out, cap, _TRUNCATE, "%s\\%s", dir, name);
}

static const char* BaseName(const char* path) {
    const char* slash = strrchr(path, '\\');
    return (slash && slash[1]) ? slash + 1 : path;
}

// Folder names that say nothing about which game this is, so the parent name
// is used for the listing instead (TheIsle\Binaries\Win64 -> "TheIsle").
static BOOL IsGenericFolder(const char* name) {
    static const char* generic[] = {
        "binaries", "bin", "bin64", "win64", "winx64", "x64", "engine",
        "plugins", "game", "build", "release", "data", "shipping"
    };
    for (int i = 0; i < (int)(sizeof(generic) / sizeof(generic[0])); i++)
        if (_stricmp(name, generic[i]) == 0) return TRUE;
    return FALSE;
}

static void Trim(char* s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' ||
                 s[n - 1] == '\t' || s[n - 1] == '"'))
        s[--n] = 0;
    char* p = s;
    while (*p == ' ' || *p == '\t' || *p == '"') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

static BOOL FileExists(const char* path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL DirExists(const char* path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static const char* ErrText(DWORD err) {
    static char buf[512];
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL, err, 0, buf, sizeof(buf) - 1, NULL);
    while (n && (buf[n - 1] == '\r' || buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = 0;
    if (!n) _snprintf_s(buf, sizeof(buf), _TRUNCATE, "error %lu", err);
    return buf;
}

// Reads at most maxBytes of a file. Returns a malloc'd buffer, caller frees.
static unsigned char* ReadFileBytes(const char* path, size_t maxBytes, size_t* outLen) {
    *outLen = 0;
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size)) { CloseHandle(h); return NULL; }

    size_t want = (size.QuadPart > (LONGLONG)maxBytes) ? maxBytes : (size_t)size.QuadPart;
    unsigned char* buf = (unsigned char*)malloc(want ? want : 1);
    if (!buf) { CloseHandle(h); return NULL; }

    size_t got = 0;
    while (got < want) {
        DWORD chunk = (DWORD)((want - got > 0x100000) ? 0x100000 : (want - got));
        DWORD read = 0;
        if (!ReadFile(h, buf + got, chunk, &read, NULL) || read == 0) break;
        got += read;
    }
    CloseHandle(h);
    *outLen = got;
    return buf;
}

static BOOL GetFileSize64(const char* path, ULONGLONG* out) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return FALSE;
    *out = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    return TRUE;
}

// Kept around for the "retry as admin" offer, and set by auth.h as well.
static DWORD g_lastError = 0;

// The Steam auth backends. Needs the utilities above and g_lastError.
#include "auth.h"

// ------------------------------------------------------- embedded proxy DLL

static const unsigned char* g_proxy = NULL;
static DWORD g_proxyLen = 0;

static BOOL LoadEmbeddedProxy(void) {
    g_proxy = LoadRes("PROXYDLL", &g_proxyLen);
    return g_proxy != NULL;
}

static DWORD WriteProxyTo(const char* path) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();

    DWORD off = 0;
    while (off < g_proxyLen) {
        DWORD written = 0;
        if (!WriteFile(h, g_proxy + off, g_proxyLen - off, &written, NULL) || written == 0) {
            DWORD e = GetLastError();
            CloseHandle(h);
            return e ? e : ERROR_WRITE_FAULT;
        }
        off += written;
    }
    CloseHandle(h);
    return 0;
}

// --------------------------------------------------------- file classifying

typedef enum { K_MISSING = 0, K_PROXY, K_REAL } FileKind;

static FileKind ClassifyDll(const char* path) {
    ULONGLONG size;
    if (!FileExists(path) || !GetFileSize64(path, &size)) return K_MISSING;
    if (size > MAX_PROBE) return K_REAL;              // real SDK is tens of MB

    size_t len = 0;
    unsigned char* buf = ReadFileBytes(path, MAX_PROBE, &len);
    if (!buf) return K_REAL;                          // unreadable: assume original
    FileKind kind = MemFind(buf, len, PROXY_MARKER, strlen(PROXY_MARKER)) ? K_PROXY : K_REAL;
    free(buf);
    return kind;
}

static BOOL SameAsEmbedded(const char* path) {
    ULONGLONG size;
    if (!GetFileSize64(path, &size) || size != g_proxyLen) return FALSE;
    size_t len = 0;
    unsigned char* buf = ReadFileBytes(path, g_proxyLen, &len);
    if (!buf) return FALSE;
    BOOL same = (len == g_proxyLen) && memcmp(buf, g_proxy, g_proxyLen) == 0;
    free(buf);
    return same;
}

// The proxy is x64 only, so warn if the game's own SDK is not.
static BOOL IsAmd64Pe(const char* path) {
    size_t len = 0;
    unsigned char* buf = ReadFileBytes(path, 4096, &len);
    if (!buf || len < 0x40) { free(buf); return TRUE; }   // unknown: do not nag
    DWORD peOff = *(DWORD*)(buf + 0x3C);
    BOOL ok = TRUE;
    if (peOff < len && len - peOff > 6 && memcmp(buf + peOff, "PE\0\0", 4) == 0)
        ok = (*(WORD*)(buf + peOff + 4) == 0x8664);
    free(buf);
    return ok;
}

// --------------------------------------------------------------- app status

typedef enum {
    ST_NOT_INSTALLED,      // real SDK present, no backup
    ST_INSTALLED,          // proxy + backup, matches this build
    ST_INSTALLED_OLD,      // proxy + backup, a different build
    ST_REINSTALLED,        // real SDK back in place but a backup lingers
    ST_PROXY_ONLY,         // proxy present, original gone
    ST_BACKUP_ONLY,        // original parked in .yes, no dll at all
    ST_NOTHING             // neither file: not an EOS folder
} Status;

static Status GetStatus(const char* folder) {
    char dll[PATHBUF], bak[PATHBUF];
    PathJoin(dll, sizeof(dll), folder, EOS_DLL);
    PathJoin(bak, sizeof(bak), folder, EOS_BAK);

    FileKind kind = ClassifyDll(dll);
    BOOL hasBak = FileExists(bak);

    if (kind == K_PROXY)
        return hasBak ? (SameAsEmbedded(dll) ? ST_INSTALLED : ST_INSTALLED_OLD) : ST_PROXY_ONLY;
    if (kind == K_REAL)
        return hasBak ? ST_REINSTALLED : ST_NOT_INSTALLED;
    return hasBak ? ST_BACKUP_ONLY : ST_NOTHING;
}

static const char* StatusText(Status s) {
    switch (s) {
    case ST_NOT_INSTALLED: return "not installed";
    case ST_INSTALLED:     return "installed (up to date)";
    case ST_INSTALLED_OLD: return "installed (different build)";
    case ST_REINSTALLED:   return "game files were restored - reinstall needed";
    case ST_PROXY_ONLY:    return "BROKEN - original DLL is missing";
    case ST_BACKUP_ONLY:   return "original backed up, proxy missing";
    default:               return "no EOS SDK here";
    }
}

// ------------------------------------------------------------ install logic

static BOOL Install(const char* folder) {
    char dll[PATHBUF], bak[PATHBUF];
    PathJoin(dll, sizeof(dll), folder, EOS_DLL);
    PathJoin(bak, sizeof(bak), folder, EOS_BAK);
    g_lastError = 0;

    Status st = GetStatus(folder);
    printf("\n  %s\n  status: %s\n", folder, StatusText(st));

    if (st == ST_NOTHING) {
        printf("  -> nothing to do: no %s in this folder.\n", EOS_DLL);
        return FALSE;
    }
    if (st == ST_PROXY_ONLY) {
        printf("  -> refusing: %s is the proxy but %s is gone.\n", EOS_DLL, EOS_BAK);
        printf("     Verify the game files in Steam, then run this again.\n");
        return FALSE;
    }
    if (st == ST_INSTALLED) {
        printf("  -> already up to date, nothing to do.\n");
        return TRUE;
    }

    // Park the original as .yes when there is one to park.
    BOOL renamed = FALSE;
    if (st == ST_NOT_INSTALLED || st == ST_REINSTALLED) {
        if (!IsAmd64Pe(dll))
            printf("  !  warning: %s is not 64-bit. This proxy is x64 only.\n", EOS_DLL);

        if (st == ST_REINSTALLED) {
            // The DLL Steam just restored matches the current game build, so it
            // becomes the new backup.
            printf("  -  replacing the stale %s\n", EOS_BAK);
            if (!DeleteFileA(bak)) {
                g_lastError = GetLastError();
                printf("  X  could not delete %s: %s\n", EOS_BAK, ErrText(g_lastError));
                return FALSE;
            }
        }
        printf("  -  %s  ->  %s\n", EOS_DLL, EOS_BAK);
        if (!MoveFileA(dll, bak)) {
            g_lastError = GetLastError();
            printf("  X  rename failed: %s\n", ErrText(g_lastError));
            return FALSE;
        }
        renamed = TRUE;
    }

    printf("  -  writing proxy %s (%lu bytes)\n", EOS_DLL, g_proxyLen);
    DWORD err = WriteProxyTo(dll);
    if (err) {
        g_lastError = err;
        printf("  X  write failed: %s\n", ErrText(err));
        if (renamed) {                     // leave the game in a working state
            DeleteFileA(dll);
            if (MoveFileA(bak, dll))
                printf("  -  rolled back, original restored.\n");
            else
                printf("  !  ROLLBACK FAILED. Rename %s back to %s by hand.\n", EOS_BAK, EOS_DLL);
        }
        return FALSE;
    }

    printf("  OK installed.\n");
    return TRUE;
}

static BOOL Uninstall(const char* folder) {
    char dll[PATHBUF], bak[PATHBUF];
    PathJoin(dll, sizeof(dll), folder, EOS_DLL);
    PathJoin(bak, sizeof(bak), folder, EOS_BAK);
    g_lastError = 0;

    Status st = GetStatus(folder);
    printf("\n  %s\n  status: %s\n", folder, StatusText(st));

    if (st == ST_NOT_INSTALLED || st == ST_NOTHING) {
        printf("  -> nothing to remove.\n");
        return TRUE;
    }

    if (st == ST_INSTALLED || st == ST_INSTALLED_OLD || st == ST_PROXY_ONLY) {
        printf("  -  deleting proxy %s\n", EOS_DLL);
        if (!DeleteFileA(dll)) {
            g_lastError = GetLastError();
            printf("  X  delete failed: %s\n", ErrText(g_lastError));
            return FALSE;
        }
    }
    if (st == ST_PROXY_ONLY) {
        printf("  !  no %s to restore. Verify the game files in Steam.\n", EOS_BAK);
        return FALSE;
    }

    if (st == ST_REINSTALLED) {
        printf("  -  removing leftover %s (the real DLL is already in place)\n", EOS_BAK);
        if (!DeleteFileA(bak)) {
            g_lastError = GetLastError();
            printf("  X  delete failed: %s\n", ErrText(g_lastError));
            return FALSE;
        }
    } else {
        printf("  -  %s  ->  %s\n", EOS_BAK, EOS_DLL);
        if (!MoveFileA(bak, dll)) {
            g_lastError = GetLastError();
            printf("  X  restore failed: %s\n", ErrText(g_lastError));
            return FALSE;
        }
    }

    printf("  OK removed, game restored to stock.\n");
    return TRUE;
}

// ------------------------------------------------------------- game finding

typedef struct {
    char path[PATHBUF];
    char name[128];
} Target;

static Target g_targets[MAX_TARGETS];
static int    g_targetCount = 0;

static void AddTarget(const char* path, const char* name) {
    if (g_targetCount >= MAX_TARGETS) return;
    for (int i = 0; i < g_targetCount; i++)
        if (_stricmp(g_targets[i].path, path) == 0) return;
    _snprintf_s(g_targets[g_targetCount].path, PATHBUF, _TRUNCATE, "%s", path);
    _snprintf_s(g_targets[g_targetCount].name, 128, _TRUNCATE, "%s", name);
    g_targetCount++;
}

static BOOL HasEos(const char* dir) {
    char probe[PATHBUF];
    PathJoin(probe, sizeof(probe), dir, EOS_DLL);
    if (FileExists(probe)) return TRUE;
    PathJoin(probe, sizeof(probe), dir, EOS_BAK);
    return FileExists(probe);
}

// Depth limited hunt for the EOS SDK. Stops descending once a folder matches.
static void ScanDir(const char* dir, int depth, const char* label) {
    if (HasEos(dir)) { AddTarget(dir, label); return; }
    if (depth <= 0) return;

    char pattern[PATHBUF];
    PathJoin(pattern, sizeof(pattern), dir, "*");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;

        char sub[PATHBUF];
        PathJoin(sub, sizeof(sub), dir, fd.cFileName);
        ScanDir(sub, depth - 1, label);
    } while (FindNextFileA(h, &fd) && g_targetCount < MAX_TARGETS);

    FindClose(h);
}

// Handles both shapes of folder the user can point at: a single game
// (C:\Games\TheIsle) or a folder full of games (steamapps\common, D:\Games).
static void ScanRoot(const char* root) {
    if (HasEos(root)) { AddTarget(root, BaseName(root)); return; }

    char pattern[PATHBUF];
    PathJoin(pattern, sizeof(pattern), root, "*");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;

        char sub[PATHBUF];
        PathJoin(sub, sizeof(sub), root, fd.cFileName);
        // TheIsle\Binaries\Win64 should be listed as TheIsle, not Binaries.
        const char* label = IsGenericFolder(fd.cFileName) ? BaseName(root) : fd.cFileName;
        ScanDir(sub, SCAN_DEPTH, label);
    } while (FindNextFileA(h, &fd) && g_targetCount < MAX_TARGETS);

    FindClose(h);
}

static BOOL GetSteamRoot(char* out, size_t cap) {
    struct { HKEY root; const char* key; const char* val; } probes[] = {
        { HKEY_CURRENT_USER,  "Software\\Valve\\Steam",              "SteamPath"   },
        { HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath" },
        { HKEY_LOCAL_MACHINE, "SOFTWARE\\Valve\\Steam",              "InstallPath" },
    };
    for (int i = 0; i < 3; i++) {
        HKEY k;
        if (RegOpenKeyExA(probes[i].root, probes[i].key, 0, KEY_READ, &k) != ERROR_SUCCESS)
            continue;
        DWORD type = 0, size = (DWORD)cap - 1;
        LONG r = RegQueryValueExA(k, probes[i].val, NULL, &type, (BYTE*)out, &size);
        RegCloseKey(k);
        if (r == ERROR_SUCCESS && type == REG_SZ && size > 1) {
            out[size < cap ? size : cap - 1] = 0;
            for (char* p = out; *p; p++) if (*p == '/') *p = '\\';
            if (DirExists(out)) return TRUE;
        }
    }
    return FALSE;
}

// Pulls every "path" entry out of steamapps\libraryfolders.vdf, so games on a
// second drive are found too.
static int GetSteamLibraries(char libs[][PATHBUF], int maxLibs) {
    char root[PATHBUF];
    if (!GetSteamRoot(root, sizeof(root))) return 0;

    int count = 0;
    _snprintf_s(libs[count++], PATHBUF, _TRUNCATE, "%s", root);

    char vdf[PATHBUF];
    PathJoin(vdf, sizeof(vdf), root, "steamapps\\libraryfolders.vdf");
    size_t len = 0;
    unsigned char* buf = ReadFileBytes(vdf, 1u << 20, &len);
    if (!buf) return count;

    char* text = (char*)buf;
    for (size_t i = 0; i < len && count < maxLibs; i++) {
        if (text[i] != '"') continue;

        size_t keyStart = ++i;
        while (i < len && text[i] != '"') i++;
        if (i >= len) break;
        size_t keyLen = i - keyStart;
        if (keyLen != 4 || _strnicmp(text + keyStart, "path", 4) != 0) continue;

        while (++i < len && text[i] != '"') ;
        if (i >= len) break;
        size_t valStart = ++i;
        while (i < len && text[i] != '"') i++;
        if (i >= len) break;

        char val[PATHBUF];
        size_t o = 0;
        for (size_t j = valStart; j < i && o + 1 < sizeof(val); j++) {
            if (text[j] == '\\' && j + 1 < i && text[j + 1] == '\\') j++;   // "\\" -> "\"
            val[o++] = text[j];
        }
        val[o] = 0;

        BOOL dup = FALSE;
        for (int k = 0; k < count; k++) if (_stricmp(libs[k], val) == 0) dup = TRUE;
        if (!dup && DirExists(val))
            _snprintf_s(libs[count++], PATHBUF, _TRUNCATE, "%s", val);
    }
    free(buf);
    return count;
}

static void ScanSteam(void) {
    char libs[MAX_ROOTS][PATHBUF];
    int n = GetSteamLibraries(libs, MAX_ROOTS);
    if (n == 0) {
        printf("  (Steam not found in the registry - use 'm' to add a folder)\n");
        return;
    }
    for (int i = 0; i < n; i++) {
        char common[PATHBUF];
        PathJoin(common, sizeof(common), libs[i], "steamapps\\common");
        if (!DirExists(common)) continue;
        printf("  scanning %s\n", common);
        ScanRoot(common);
    }
}

// ------------------------------------------------- saved manual search paths

static char g_cfgPath[PATHBUF];
static char g_roots[MAX_ROOTS][PATHBUF];
static int  g_rootCount = 0;

static void ExeFolder(char* out, size_t cap) {
    out[0] = 0;
    char exe[PATHBUF];
    if (!GetModuleFileNameA(NULL, exe, sizeof(exe))) return;
    char* slash = strrchr(exe, '\\');
    if (!slash) return;
    *slash = 0;
    _snprintf_s(out, cap, _TRUNCATE, "%s", exe);
}

static void LoadRoots(void) {
    char dir[PATHBUF];
    ExeFolder(dir, sizeof(dir));
    if (!dir[0]) return;
    PathJoin(g_cfgPath, sizeof(g_cfgPath), dir, CONFIG_NAME);

    g_rootCount = 0;
    size_t len = 0;
    unsigned char* buf = ReadFileBytes(g_cfgPath, 1u << 16, &len);
    if (!buf) return;

    char line[PATHBUF];
    size_t o = 0;
    for (size_t i = 0; i <= len; i++) {
        char c = (i < len) ? (char)buf[i] : '\n';
        if (c == '\r') continue;
        if (c != '\n') {
            if (o + 1 < sizeof(line)) line[o++] = c;
            continue;
        }
        line[o] = 0;
        o = 0;
        Trim(line);
        if (line[0] && line[0] != '#' && g_rootCount < MAX_ROOTS)
            _snprintf_s(g_roots[g_rootCount++], PATHBUF, _TRUNCATE, "%s", line);
    }
    free(buf);
}

static void SaveRoots(void) {
    if (!g_cfgPath[0]) return;
    if (g_rootCount == 0) { DeleteFileA(g_cfgPath); return; }

    HANDLE h = CreateFileA(g_cfgPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("  !  could not save %s: %s\n", CONFIG_NAME, ErrText(GetLastError()));
        return;
    }
    const char* header =
        "# Extra folders EOS Proxy Setup searches, one per line.\r\n"
        "# Either a single game folder or a folder that holds several games.\r\n";
    DWORD w;
    WriteFile(h, header, (DWORD)strlen(header), &w, NULL);
    for (int i = 0; i < g_rootCount; i++) {
        WriteFile(h, g_roots[i], (DWORD)strlen(g_roots[i]), &w, NULL);
        WriteFile(h, "\r\n", 2, &w, NULL);
    }
    CloseHandle(h);
}

static BOOL AddRoot(const char* path) {
    for (int i = 0; i < g_rootCount; i++)
        if (_stricmp(g_roots[i], path) == 0) return FALSE;
    if (g_rootCount >= MAX_ROOTS) { printf("  !  saved folder list is full.\n"); return FALSE; }
    _snprintf_s(g_roots[g_rootCount++], PATHBUF, _TRUNCATE, "%s", path);
    SaveRoots();
    return TRUE;
}

static void ScanSavedRoots(void) {
    for (int i = 0; i < g_rootCount; i++) {
        if (!DirExists(g_roots[i])) {
            printf("  (saved folder is gone: %s)\n", g_roots[i]);
            continue;
        }
        printf("  scanning %s\n", g_roots[i]);
        ScanRoot(g_roots[i]);
    }
}

// Covers dropping this exe straight into the game folder.
static void ScanExeFolder(void) {
    char dir[PATHBUF];
    ExeFolder(dir, sizeof(dir));
    if (!dir[0]) return;
    // A proxy DLL with no backup next to setup.exe is the build output, not a
    // game. Listing it as a broken install would only confuse.
    if (GetStatus(dir) == ST_PROXY_ONLY) return;
    ScanRoot(dir);
}

static void Rescan(void) {
    g_targetCount = 0;
    printf("\n  Scanning for games...\n");
    ScanExeFolder();
    ScanSteam();
    ScanSavedRoots();
}

// ---------------------------------------------------------------- elevation

static BOOL IsElevated(void) {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return FALSE;
    TOKEN_ELEVATION el;
    DWORD size = sizeof(el);
    BOOL ok = GetTokenInformation(token, TokenElevation, &el, sizeof(el), &size);
    CloseHandle(token);
    return ok && el.TokenIsElevated;
}

// Re-runs this exe as admin for every folder that hit a permissions wall.
static void RelaunchElevated(const char* verb, const char* appid,
                             char folders[][PATHBUF], int count) {
    char exe[PATHBUF];
    if (!GetModuleFileNameA(NULL, exe, sizeof(exe))) return;

    char params[4096];
    if (appid && appid[0])
        _snprintf_s(params, sizeof(params), _TRUNCATE, "%s --appid %s", verb, appid);
    else
        _snprintf_s(params, sizeof(params), _TRUNCATE, "%s", verb);
    for (int i = 0; i < count; i++) {
        char one[PATHBUF + 4];
        _snprintf_s(one, sizeof(one), _TRUNCATE, " \"%s\"", folders[i]);
        if (strlen(params) + strlen(one) + 16 >= sizeof(params)) {
            printf("  !  too many folders for one elevated run, doing the first %d.\n", i);
            break;
        }
        strcat_s(params, sizeof(params), one);
    }
    strcat_s(params, sizeof(params), " --yes --pause");

    SHELLEXECUTEINFOA sei = { 0 };
    sei.cbSize = sizeof(sei);
    sei.lpVerb = "runas";
    sei.lpFile = exe;
    sei.lpParameters = params;
    sei.nShow = SW_SHOWNORMAL;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;

    if (!ShellExecuteExA(&sei)) {
        printf("  X  elevation cancelled or failed.\n");
        return;
    }
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, INFINITE);
        CloseHandle(sei.hProcess);
    }
}

// ------------------------------------------------------------- batch runner

// Everything the installer can do to a folder. One enum so the batch runner,
// the menu and the elevated relaunch all talk about the same set of verbs.
typedef enum {
    ACT_SETUP = 0,       // proxy DLL and an auth backend, the whole job
    ACT_INSTALL,         // proxy DLL only
    ACT_UNINSTALL,       // proxy DLL and anything installed under it, out
    ACT_GBE,             // auth backend in
    ACT_UC,
    ACT_SLS,
    ACT_AUTH_REMOVE      // auth backend out
} Action;

// Which backend the whole-job action installs. Asked once per run in the menu,
// set by --gbe/--uc/--sls on the command line.
static AuthBackend g_setupBackend = AUTH_GBE;
static BOOL        g_setupBackendChosen = FALSE;

static const char* ActionVerb(Action a) {
    switch (a) {
    case ACT_UNINSTALL:   return "--uninstall";
    case ACT_INSTALL:     return "--proxy-only";
    case ACT_GBE:         return "--gbe";
    case ACT_UC:          return "--uc";
    case ACT_SLS:         return "--sls";
    case ACT_AUTH_REMOVE: return "--remove-auth";
    default:              return "--install";
    }
}

static const char* ActionTitle(Action a) {
    switch (a) {
    case ACT_UNINSTALL:   return "Removing everything from";
    case ACT_INSTALL:     return "Installing the proxy into";
    case ACT_GBE:         return "Installing gbe_fork into";
    case ACT_UC:          return "Installing uc-online2 into";
    case ACT_SLS:         return "Writing the SLSsteam config for";
    case ACT_AUTH_REMOVE: return "Removing the auth backend from";
    default:              return "Setting up";
    }
}

static BOOL RunOne(Action a, const char* folder, const char* appid) {
    switch (a) {
    case ACT_INSTALL:     return Install(folder);
    case ACT_GBE:         return AuthInstall(folder, AUTH_GBE, appid);
    case ACT_UC:          return AuthInstall(folder, AUTH_UC, appid);
    case ACT_SLS:         return AuthInstall(folder, AUTH_SLS, appid);
    case ACT_AUTH_REMOVE: return AuthRemove(folder);

    // Removal goes in the opposite order to installation: the backend first,
    // so its .eosbak files go back before the proxy's do.
    case ACT_UNINSTALL: {
        BOOL auth = AuthRemove(folder);
        BOOL prox = Uninstall(folder);
        return auth && prox;
    }

    // The whole job. A game with the proxy but no ticket underneath is the most
    // common way for this to look installed and still not work, so one action
    // does both and is what the menu and drag and drop use.
    default: {
        if (!Install(folder)) return FALSE;
        return AuthInstall(folder, g_setupBackend, appid);
    }
    }
}

// Asks once which backend the whole-job action should use, then remembers it.
static void ChooseBackend(void) {
    if (g_setupBackendChosen) return;

    printf("\n  Which Steam auth backend? The proxy needs one of these underneath\n"
             "  it before a game will touch EOS networking.\n\n");
    for (int i = 0; i < AUTH_COUNT; i++) {
        const char* note = (i == AUTH_SLS)      ? "Linux only, writes the config here"
                         : AuthAvailable((AuthBackend)i) ? "downloaded from the project on first use"
                         : "not available in this build";
        printf("    [%d] %-11s %s\n", i + 1, g_authInfo[i].label, note);
    }
    printf("\n  [1] > ");

    char line[64];
    if (fgets(line, sizeof(line), stdin)) {
        Trim(line);
        int pick = atoi(line);
        if (pick >= 1 && pick <= AUTH_COUNT) g_setupBackend = (AuthBackend)(pick - 1);
    }
    g_setupBackendChosen = TRUE;
    printf("  -> %s\n", g_authInfo[g_setupBackend].label);
}

// The proxy is only half the job, so say so - but only for the folders that
// still have nothing underneath it.
static void PrintReminder(char folders[][PATHBUF], int count) {
    int missing = 0;
    for (int i = 0; i < count; i++)
        if (_stricmp(AuthStatusText(folders[i]), "none") == 0) missing++;
    if (!missing) return;

    printf("\n"
        "  %d of those folder(s) have no Steam auth backend yet. Most games only\n"
        "  reach the EOS networking calls once ISteamUser::GetAuthTicketForWebApi\n"
        "  returns a ticket, which needs one of:\n", missing);
    for (int i = 0; i < AUTH_COUNT; i++) {
        const char* state = (i == AUTH_SLS)               ? "Linux only, config written here"
                          : AuthAvailable((AuthBackend)i) ? "downloaded on first use"
                                                          : "not available in this build";
        printf("    %-12s %-32s %s\n", g_authInfo[i].label, state, g_authInfo[i].url);
    }
    printf("  Install one from the menu (g/o/s) or with --gbe / --uc / --sls.\n"
           "  If something goes wrong, read epic_proxy.log next to the game exe.\n");
}

// Runs one action over many folders, then offers a single elevated retry for
// the ones that were refused for lack of rights.
static void RunBatch(Action a, char folders[][PATHBUF], int count, BOOL askAdmin,
                     const char* appid) {
    static char denied[MAX_TARGETS][PATHBUF];
    int ok = 0, failed = 0, deniedCount = 0;

    for (int i = 0; i < count; i++) {
        // Install and Uninstall print the folder with their own status line.
        if (a != ACT_SETUP && a != ACT_INSTALL && a != ACT_UNINSTALL)
            printf("\n  %s\n", folders[i]);
        if (RunOne(a, folders[i], appid)) {
            ok++;
        } else {
            failed++;
            if (g_lastError == ERROR_ACCESS_DENIED && deniedCount < MAX_TARGETS)
                _snprintf_s(denied[deniedCount++], PATHBUF, _TRUNCATE, "%s", folders[i]);
        }
    }

    printf("\n  ---- %d done, %d failed ----\n", ok, failed);

    if (deniedCount && !IsElevated()) {
        if (askAdmin) {
            printf("\n  %d folder(s) need administrator rights. Retry as admin? [y/N] ", deniedCount);
            char line[16];
            if (fgets(line, sizeof(line), stdin) && (line[0] == 'y' || line[0] == 'Y')) {
                // The backend choice has to survive the relaunch, or the
                // elevated run would quietly fall back to the default.
                char verb[64];
                if (a == ACT_SETUP)
                    _snprintf_s(verb, sizeof(verb), _TRUNCATE, "--install --%s",
                                g_authInfo[g_setupBackend].flag);
                else
                    _snprintf_s(verb, sizeof(verb), _TRUNCATE, "%s", ActionVerb(a));
                RelaunchElevated(verb, appid, denied, deniedCount);
            }
        } else {
            printf("  (run as administrator to fix the %d access denied folder(s))\n", deniedCount);
        }
    }

    if (a == ACT_INSTALL && ok) PrintReminder(folders, count);
}

// -------------------------------------------------------------------- shell

// Accepts a folder, or a file inside one, so dragging the DLL or the game exe
// onto the window works too.
// Everything downstream walks paths a component at a time and separates them
// with backslashes, so a path handed in on the command line - or dragged onto
// the exe from a shell that writes forward slashes - is turned into one full
// backslash path here, before anything looks at it.
static BOOL NormalizeFolder(char* path) {
    char full[PATHBUF];
    for (char* p = path; *p; p++) if (*p == '/') *p = '\\';

    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '\\' && path[n - 2] != ':') path[--n] = 0;

    if (GetFullPathNameA(path, sizeof(full), full, NULL))
        _snprintf_s(path, PATHBUF, _TRUNCATE, "%s", full);

    if (DirExists(path)) return TRUE;
    if (FileExists(path)) {
        char* slash = strrchr(path, '\\');
        if (slash) { *slash = 0; return TRUE; }
    }
    return FALSE;
}

// "1,3,5", "2-4", or "a" for all. Returns how many 1-based indices were written.
static int ParseSelection(const char* s, int* out, int maxOut, int count) {
    int n = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == 'a' || *s == 'A' || *s == '*') {
        for (int i = 1; i <= count && n < maxOut; i++) out[n++] = i;
        return n;
    }
    while (*s && n < maxOut) {
        while (*s && (*s < '0' || *s > '9')) s++;
        if (!*s) break;

        int lo = 0;
        while (*s >= '0' && *s <= '9') lo = lo * 10 + (*s++ - '0');
        int hi = lo;
        if (*s == '-') {
            s++;
            hi = 0;
            while (*s >= '0' && *s <= '9') hi = hi * 10 + (*s++ - '0');
            if (hi < lo) { int t = lo; lo = hi; hi = t; }
        }
        for (int i = lo; i <= hi && n < maxOut; i++) {
            if (i < 1 || i > count) continue;
            BOOL dup = FALSE;
            for (int k = 0; k < n; k++) if (out[k] == i) dup = TRUE;
            if (!dup) out[n++] = i;
        }
    }
    return n;
}

static void ListTargets(void) {
    printf("\n  Found %d folder(s) with the EOS SDK:\n\n", g_targetCount);
    for (int i = 0; i < g_targetCount; i++) {
        Status st = GetStatus(g_targets[i].path);
        printf("   [%d] %-34.34s  proxy: %s\n", i + 1, g_targets[i].name, StatusText(st));
        printf("       %-34.34s  auth:  %s\n", "", AuthStatusText(g_targets[i].path));
        printf("       %s\n", g_targets[i].path);
    }
    if (g_rootCount) {
        printf("\n  Saved extra folders (%d):\n", g_rootCount);
        for (int i = 0; i < g_rootCount; i++)
            printf("       %s\n", g_roots[i]);
    }
}

// Collects the selected targets into a folder array for RunBatch.
static int PickTargets(const char* sel, char out[][PATHBUF], int maxOut) {
    int idx[MAX_TARGETS];
    int n = ParseSelection(sel, idx, MAX_TARGETS, g_targetCount);
    int c = 0;
    for (int i = 0; i < n && c < maxOut; i++)
        _snprintf_s(out[c++], PATHBUF, _TRUNCATE, "%s", g_targets[idx[i] - 1].path);
    return c;
}

static void ManualPath(void) {
    printf("\n  Folder to add. It can be one game (C:\\Games\\TheIsle) or a folder\n"
             "  holding several games (D:\\Games). Drag and drop works.\n  > ");
    char path[PATHBUF];
    if (!fgets(path, sizeof(path), stdin)) return;
    Trim(path);
    if (!path[0]) return;
    if (!NormalizeFolder(path)) { printf("  X  not a folder: %s\n", path); return; }

    int before = g_targetCount;
    ScanRoot(path);
    int found = g_targetCount - before;

    if (found == 0) {
        printf("  X  no %s found under that folder (searched %d levels deep).\n",
               EOS_DLL, SCAN_DEPTH);
        printf("     Point at the folder that holds the DLL, or its parent.\n");
        return;
    }

    printf("  OK found %d game folder(s) here.\n", found);
    if (AddRoot(path))
        printf("  -  saved to %s, it will be searched next time too.\n", CONFIG_NAME);
}

static void Interactive(void) {
    LoadRoots();
    Rescan();

    static char picked[MAX_TARGETS][PATHBUF];

    for (;;) {
        if (g_targetCount == 0)
            printf("\n  No EOS games found. Use 'm' to point at a game folder.\n");
        else
            ListTargets();

        printf("\n  set up  : 1   or  1,3,5   or  2-4   or  a  (all)   proxy + auth, all of it\n"
                 "  undo    : u1  or  ua                              back to stock\n"
                 "  parts   : p1 proxy only     g1 gbe_fork%s   o1 uc-online2%s\n"
                 "            s1 SLSsteam config          ug1 auth backend only, out\n"
                 "  m add a folder to search    c clear saved folders\n"
                 "  r rescan                    q quit\n  > ",
               AuthAvailable(AUTH_GBE) ? "" : " (n/a)",
               AuthAvailable(AUTH_UC)  ? "" : " (n/a)");

        char line[PATHBUF];
        if (!fgets(line, sizeof(line), stdin)) return;
        Trim(line);
        if (!line[0]) continue;

        if (line[0] == 'q' || line[0] == 'Q') return;
        if (line[0] == 'r' || line[0] == 'R') { Rescan(); continue; }
        if (line[0] == 'm' || line[0] == 'M') { ManualPath(); continue; }

        if (line[0] == 'c' || line[0] == 'C') {
            if (!g_rootCount) { printf("  (no saved folders)\n"); continue; }
            printf("  Forget all %d saved folder(s)? Games stay patched. [y/N] ", g_rootCount);
            char a[16];
            if (fgets(a, sizeof(a), stdin) && (a[0] == 'y' || a[0] == 'Y')) {
                g_rootCount = 0;
                SaveRoots();
                printf("  OK cleared.\n");
                Rescan();
            }
            continue;
        }

        // u prefixes a removal, and the letter after it picks what to remove:
        // "u1" the proxy, "ug1" whatever auth backend is there.
        // A bare number is the whole job. A letter in front narrows it to one
        // part, and a leading u undoes rather than installs.
        const char* sel = line;
        Action act = ACT_SETUP;
        if (*sel == 'u' || *sel == 'U') {
            sel++;
            act = ACT_UNINSTALL;
            char what = (char)tolower((unsigned char)*sel);
            if (what == 'g' || what == 'o' || what == 's') { sel++; act = ACT_AUTH_REMOVE; }
        } else {
            switch (tolower((unsigned char)*sel)) {
            case 'p': sel++; act = ACT_INSTALL; break;
            case 'g': sel++; act = ACT_GBE;     break;
            case 'o': sel++; act = ACT_UC;      break;
            case 's': sel++; act = ACT_SLS;     break;
            }
        }

        int n = PickTargets(sel, picked, MAX_TARGETS);
        if (n == 0) { printf("  X  nothing selected.\n"); continue; }

        if (act == ACT_SETUP) ChooseBackend();

        printf("\n  %s %d folder(s)...\n", ActionTitle(act), n);
        RunBatch(act, picked, n, TRUE, NULL);
    }
}

static void Usage(void) {
    printf(
        "\n  Usage:\n"
        "    setup.exe                             interactive, scans Steam libraries\n"
        "    setup.exe <folder> [<folder> ...]     set up those folders completely\n"
        "    setup.exe --install <folder>...       same, no confirmation\n"
        "    setup.exe --all                       set up every game found\n"
        "    setup.exe --uninstall <folder>...     put everything back to stock\n"
        "    setup.exe --status <folder>...        report what is installed\n"
        "    setup.exe --extract <file>            just write the proxy DLL somewhere\n"
        "\n  Setting up installs the proxy and one Steam auth backend, because a\n"
        "  game needs both. gbe_fork unless one of these says otherwise:\n"
        "    --gbe / --uc / --sls    pick the backend, or install only that one\n"
        "    --proxy-only            the proxy DLL and nothing else\n"
        "    --remove-auth           take the backend out, leave the proxy\n"
        "    --appid <number>        which game it is, when it cannot be worked out\n"
        "\n  The emulator is downloaded from the project's own GitHub release the\n"
        "  first time it is needed and cached in %%LOCALAPPDATA%%\\eos-proxy.\n"
        "%s%s"
        "\n    --yes    do not ask for confirmation\n"
        "    --pause  keep the window open when done\n"
        "\n  Folders added with 'm' are remembered in %s next to this exe.\n"
        "  A folder can be one game or a parent holding several.\n",
        AuthAvailable(AUTH_GBE) ? "" : "    ! gbe_fork is not available in this build\n",
        AuthAvailable(AUTH_UC)  ? "" : "    ! uc-online2 is not available in this build\n",
        CONFIG_NAME);
}

// Maps a command line verb to an action. Returns FALSE for verbs handled
// elsewhere (--status, --extract, --all) and for anything unknown.
static BOOL VerbToAction(const char* verb, Action* out) {
    static const struct { const char* verb; Action act; } map[] = {
        { "--install",     ACT_SETUP       },
        { "--setup",       ACT_SETUP       },
        { "--proxy-only",  ACT_INSTALL     },
        { "--uninstall",   ACT_UNINSTALL   },
        { "--gbe",         ACT_GBE         },
        { "--uc",          ACT_UC          },
        { "--sls",         ACT_SLS         },
        { "--remove-auth", ACT_AUTH_REMOVE },
    };
    for (int i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++)
        if (_stricmp(verb, map[i].verb) == 0) { *out = map[i].act; return TRUE; }
    return FALSE;
}

// --gbe on its own installs just that backend, but alongside --install or
// --all it is picking which backend the whole job uses.
static void ApplyBackendChoice(Action act) {
    switch (act) {
    case ACT_GBE: g_setupBackend = AUTH_GBE; g_setupBackendChosen = TRUE; break;
    case ACT_UC:  g_setupBackend = AUTH_UC;  g_setupBackendChosen = TRUE; break;
    case ACT_SLS: g_setupBackend = AUTH_SLS; g_setupBackendChosen = TRUE; break;
    default: break;
    }
}

// Launched from Explorer (double click or drag and drop) the console dies with
// the process, so hold it open in that case.
static BOOL OwnsConsole(void) {
    DWORD pids[4];
    return GetConsoleProcessList(pids, 4) == 1;
}

int main(int argc, char** argv) {
    printf("\n  ==============================\n"
             "   EOS Proxy - automatic setup\n"
             "   " SETUP_VERSION "  (" SETUP_BUILD ")\n"
             "  ==============================\n");

    if (!LoadEmbeddedProxy()) {
        printf("\n  X  FATAL: this exe was built without the embedded proxy DLL.\n");
        return 2;
    }
    AuthLoadPayloads();

    printf("  embedded proxy: %lu bytes | backends:", g_proxyLen);
    for (int i = 0; i < AUTH_COUNT; i++)
        if (AuthAvailable((AuthBackend)i)) printf(" %s", g_authInfo[i].label);
    printf("\n");

    const char* verb = NULL;
    const char* backendFlag = NULL;
    const char* appid = NULL;
    static char folders[MAX_TARGETS][PATHBUF];
    int folderCount = 0;
    BOOL pause = OwnsConsole();
    BOOL assumeYes = FALSE;
    BOOL wantAll = FALSE;

    for (int i = 1; i < argc; i++) {
        if (_stricmp(argv[i], "--pause") == 0) { pause = TRUE; continue; }
        if (_stricmp(argv[i], "--yes") == 0 || _stricmp(argv[i], "-y") == 0) { assumeYes = TRUE; continue; }
        if (_stricmp(argv[i], "--all") == 0) { wantAll = TRUE; continue; }
        if (_stricmp(argv[i], "--help") == 0 || _stricmp(argv[i], "-h") == 0 ||
            _stricmp(argv[i], "/?") == 0) { Usage(); return 0; }
        if (_stricmp(argv[i], "--appid") == 0) {
            if (i + 1 < argc) appid = argv[++i];
            else { printf("\n  X  --appid needs a number after it.\n"); return 1; }
            continue;
        }
        if (argv[i][0] == '-') {
            // A backend flag is kept apart: on its own it is the action, and
            // next to --install or --all it only says which backend to use.
            Action pick;
            if (VerbToAction(argv[i], &pick) &&
                (pick == ACT_GBE || pick == ACT_UC || pick == ACT_SLS))
                backendFlag = argv[i];
            else
                verb = argv[i];
            continue;
        }
        if (folderCount < MAX_TARGETS) {
            _snprintf_s(folders[folderCount], PATHBUF, _TRUNCATE, "%s", argv[i]);
            Trim(folders[folderCount]);
            folderCount++;
        }
    }

    int rc = 0;
    int valid = 0;
    Action act = ACT_SETUP;

    if (backendFlag) {
        Action pick;
        VerbToAction(backendFlag, &pick);
        ApplyBackendChoice(pick);
        // Alone it means "just this backend"; with a verb or --all it was only
        // choosing which backend the whole job installs.
        if (!verb && !wantAll) verb = backendFlag;
    }

    if (verb && _stricmp(verb, "--extract") == 0) {
        if (folderCount != 1) { Usage(); rc = 1; }
        else {
            DWORD err = WriteProxyTo(folders[0]);
            if (err) { printf("\n  X  failed: %s\n", ErrText(err)); rc = 1; }
            else       printf("\n  OK wrote %s\n", folders[0]);
        }
        goto done;
    }

    // --status reports rather than acts, and is handled once the folder list
    // has been checked, so it is not an action verb.
    BOOL isStatus = verb && _stricmp(verb, "--status") == 0;

    if (verb && !isStatus && !VerbToAction(verb, &act)) {
        printf("\n  X  unknown option: %s\n", verb);
        Usage();
        rc = 1;
        goto done;
    }

    if (wantAll) {
        LoadRoots();
        Rescan();
        folderCount = 0;
        for (int i = 0; i < g_targetCount && folderCount < MAX_TARGETS; i++)
            _snprintf_s(folders[folderCount++], PATHBUF, _TRUNCATE, "%s", g_targets[i].path);
        if (!folderCount) { printf("\n  X  no EOS games found.\n"); rc = 1; goto done; }
        printf("\n  %s all %d folder(s) found.\n", ActionTitle(act), folderCount);
        RunBatch(act, folders, folderCount, !assumeYes, appid);
        goto done;
    }

    if (folderCount == 0) {
        if (verb) { Usage(); rc = 1; }
        else      Interactive();
        goto done;
    }

    // Drop anything that is not a real folder before touching the disk.
    for (int i = 0; i < folderCount; i++) {
        if (NormalizeFolder(folders[i])) {
            if (valid != i) _snprintf_s(folders[valid], PATHBUF, _TRUNCATE, "%s", folders[i]);
            valid++;
        } else {
            printf("\n  X  not a folder: %s\n", folders[i]);
            rc = 1;
        }
    }
    folderCount = valid;
    if (!folderCount) goto done;

    if (isStatus) {
        for (int i = 0; i < folderCount; i++)
            printf("\n  %s\n  proxy: %s\n  auth:  %s\n", folders[i],
                   StatusText(GetStatus(folders[i])), AuthStatusText(folders[i]));
        goto done;
    }

    // A bare folder list is usually drag and drop, so confirm before writing.
    if (!verb && !assumeYes) {
        printf("\n  Set up the proxy and %s in these %d folder(s)?\n",
               g_authInfo[g_setupBackend].label, folderCount);
        for (int i = 0; i < folderCount; i++) printf("    %s\n", folders[i]);
        printf("  [Y/n] ");
        char a[16];
        if (fgets(a, sizeof(a), stdin) && (a[0] == 'n' || a[0] == 'N')) { rc = 1; goto done; }
    }

    RunBatch(act, folders, folderCount, TRUE, appid);

done:
    if (pause) {
        printf("\n  Press Enter to close...");
        (void)getchar();
    }
    return rc;
}
