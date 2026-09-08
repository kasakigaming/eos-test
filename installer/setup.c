// EOS Proxy Setup - automated installer for the EOS proxy DLL.
//
// Single file installer: the proxy DLL is embedded as an RCDATA resource, so
// this exe is everything the user needs. It finds games that ship the EOS SDK,
// renames the original EOSSDK-Win64-Shipping.dll to .yes and drops the proxy
// in its place. Several games can be done in one go, and folders outside the
// Steam libraries can be added by hand and are remembered between runs.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EOS_DLL      "EOSSDK-Win64-Shipping.dll"
#define EOS_BAK      "EOSSDK-Win64-Shipping.yes"
// Every forwarded export inside the proxy stores this name, and the real SDK
// never contains it. That is how the two are told apart.
#define PROXY_MARKER EOS_BAK
#define CONFIG_NAME  "eos-proxy-setup.txt"

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

// ------------------------------------------------------- embedded proxy DLL

static const unsigned char* g_proxy = NULL;
static DWORD g_proxyLen = 0;

static BOOL LoadEmbeddedProxy(void) {
    HRSRC res = FindResourceA(NULL, "PROXYDLL", RT_RCDATA);
    if (!res) return FALSE;
    HGLOBAL h = LoadResource(NULL, res);
    if (!h) return FALSE;
    g_proxy = (const unsigned char*)LockResource(h);
    g_proxyLen = SizeofResource(NULL, res);
    return g_proxy != NULL && g_proxyLen > 0;
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

static DWORD g_lastError = 0;   // kept around for the "retry as admin" offer

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
static void RelaunchElevated(const char* verb, char folders[][PATHBUF], int count) {
    char exe[PATHBUF];
    if (!GetModuleFileNameA(NULL, exe, sizeof(exe))) return;

    char params[4096];
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

static void PrintReminder(void) {
    printf("\n"
        "  Reminder: most games also need ISteamUser::GetAuthTicketForWebApi to\n"
        "  succeed before they touch EOS networking. Use one of:\n"
        "    - uc-online2        https://github.com/UnionCrax-Team/uc-online2\n"
        "    - gbe_fork          https://github.com/Detanup01/gbe_fork\n"
        "    - SLSsteam (Linux)  https://github.com/AceSLS/SLSsteam\n"
        "  If something goes wrong, read epic_proxy.log next to the game exe.\n");
}

// Runs one verb over many folders, then offers a single elevated retry for the
// ones that were refused for lack of rights.
static void RunBatch(BOOL remove, char folders[][PATHBUF], int count, BOOL askAdmin) {
    static char denied[MAX_TARGETS][PATHBUF];
    int ok = 0, failed = 0, deniedCount = 0;

    for (int i = 0; i < count; i++) {
        if (remove ? Uninstall(folders[i]) : Install(folders[i])) {
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
            if (fgets(line, sizeof(line), stdin) && (line[0] == 'y' || line[0] == 'Y'))
                RelaunchElevated(remove ? "--uninstall" : "--install", denied, deniedCount);
        } else {
            printf("  (run as administrator to fix the %d access denied folder(s))\n", deniedCount);
        }
    }

    if (!remove && ok) PrintReminder();
}

// -------------------------------------------------------------------- shell

// Accepts a folder, or a file inside one, so dragging the DLL or the game exe
// onto the window works too.
static BOOL NormalizeFolder(char* path) {
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
        printf("   [%d] %-34.34s  %s\n", i + 1, g_targets[i].name, StatusText(st));
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

        printf("\n  install : 1   or  1,3,5   or  2-4   or  a  (all)\n"
                 "  remove  : u1  or  u1,3    or  ua\n"
                 "  m add a folder to search    c clear saved folders\n"
                 "  r rescan                    q quit\n  > ");

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

        BOOL remove = (line[0] == 'u' || line[0] == 'U');
        int n = PickTargets(remove ? line + 1 : line, picked, MAX_TARGETS);
        if (n == 0) { printf("  X  nothing selected.\n"); continue; }

        printf("\n  %s %d folder(s)...\n", remove ? "Removing from" : "Installing into", n);
        RunBatch(remove, picked, n, TRUE);
    }
}

static void Usage(void) {
    printf(
        "\n  Usage:\n"
        "    setup.exe                             interactive, scans Steam libraries\n"
        "    setup.exe <folder> [<folder> ...]     install into those folders\n"
        "    setup.exe --install <folder>...       same, no confirmation\n"
        "    setup.exe --uninstall <folder>...     restore the original DLL\n"
        "    setup.exe --status <folder>...        report what is installed\n"
        "    setup.exe --all                       install into every game found\n"
        "    setup.exe --extract <file>            just write the proxy DLL somewhere\n"
        "    --yes    do not ask for confirmation\n"
        "    --pause  keep the window open when done\n"
        "\n  Folders added with 'm' are remembered in %s next to this exe.\n"
        "  A folder can be one game or a parent holding several.\n", CONFIG_NAME);
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
             "  ==============================\n");

    if (!LoadEmbeddedProxy()) {
        printf("\n  X  FATAL: this exe was built without the embedded proxy DLL.\n");
        return 2;
    }
    printf("  embedded proxy: %lu bytes\n", g_proxyLen);

    const char* verb = NULL;
    static char folders[MAX_TARGETS][PATHBUF];
    int folderCount = 0;
    BOOL pause = OwnsConsole();
    BOOL assumeYes = FALSE;

    for (int i = 1; i < argc; i++) {
        if (_stricmp(argv[i], "--pause") == 0) { pause = TRUE; continue; }
        if (_stricmp(argv[i], "--yes") == 0 || _stricmp(argv[i], "-y") == 0) { assumeYes = TRUE; continue; }
        if (_stricmp(argv[i], "--help") == 0 || _stricmp(argv[i], "-h") == 0 ||
            _stricmp(argv[i], "/?") == 0) { Usage(); return 0; }
        if (argv[i][0] == '-') { verb = argv[i]; continue; }
        if (folderCount < MAX_TARGETS) {
            _snprintf_s(folders[folderCount], PATHBUF, _TRUNCATE, "%s", argv[i]);
            Trim(folders[folderCount]);
            folderCount++;
        }
    }

    int rc = 0;
    int valid = 0;
    BOOL remove = FALSE;

    if (verb && _stricmp(verb, "--extract") == 0) {
        if (folderCount != 1) { Usage(); rc = 1; }
        else {
            DWORD err = WriteProxyTo(folders[0]);
            if (err) { printf("\n  X  failed: %s\n", ErrText(err)); rc = 1; }
            else       printf("\n  OK wrote %s\n", folders[0]);
        }
        goto done;
    }

    if (verb && _stricmp(verb, "--all") == 0) {
        LoadRoots();
        Rescan();
        folderCount = 0;
        for (int i = 0; i < g_targetCount && folderCount < MAX_TARGETS; i++)
            _snprintf_s(folders[folderCount++], PATHBUF, _TRUNCATE, "%s", g_targets[i].path);
        if (!folderCount) { printf("\n  X  no EOS games found.\n"); rc = 1; goto done; }
        printf("\n  Installing into all %d folder(s) found.\n", folderCount);
        RunBatch(FALSE, folders, folderCount, !assumeYes);
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

    if (verb && _stricmp(verb, "--status") == 0) {
        for (int i = 0; i < folderCount; i++)
            printf("\n  %s\n  status: %s\n", folders[i], StatusText(GetStatus(folders[i])));
        goto done;
    }

    remove = (verb && _stricmp(verb, "--uninstall") == 0);

    // A bare folder list is usually drag and drop, so confirm before writing.
    if (!verb && !assumeYes) {
        printf("\n  Install the proxy into these %d folder(s)?\n", folderCount);
        for (int i = 0; i < folderCount; i++) printf("    %s\n", folders[i]);
        printf("  [Y/n] ");
        char a[16];
        if (fgets(a, sizeof(a), stdin) && (a[0] == 'n' || a[0] == 'N')) { rc = 1; goto done; }
    }

    RunBatch(remove, folders, folderCount, TRUE);

done:
    if (pause) {
        printf("\n  Press Enter to close...");
        (void)getchar();
    }
    return rc;
}
