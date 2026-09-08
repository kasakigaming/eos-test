#pragma once

#include "pak.h"

// Steam auth backends.
//
// The EOS proxy on its own is not enough. Nearly every game only reaches the
// EOS networking calls after ISteamUser::GetAuthTicketForWebApi has handed it a
// ticket, and that needs gbe_fork, uc-online2 or SLSsteam underneath. This is
// the part of the installer that puts one of them in place.
//
// A backend arrives in two pieces. Its config ships inside this exe, packed
// from installer\payload\<backend> by tools\pack.c; those text files are
// templates, with {APPID} and friends substituted as they are written, so the
// backend lands already pointed at the game instead of needing a config pass by
// hand afterwards. The emulator binary is downloaded from the project's own
// GitHub release at install time - see fetch.h for why it is not embedded.
//
// Included by setup.c after the utility section, so PathJoin, FileExists,
// ErrText, ReadFileBytes and g_lastError are all in scope.

#define AUTH_MANIFEST  "eos-proxy-auth.txt"
#define AUTH_BAK_EXT   ".eosbak"
#define AUTH_SLS_FILE  "SLSsteam-config.yaml"
#define AUTH_MAX_FILES 512

typedef enum { AUTH_GBE = 0, AUTH_UC = 1, AUTH_SLS = 2, AUTH_COUNT = 3 } AuthBackend;

typedef struct {
    const char* id;      // token written into the manifest
    const char* label;   // what the menu shows
    const char* dir;     // installer\payload\<dir>
    const char* res;     // RCDATA name, NULL when there is nothing to copy
    const char* flag;    // command line verb, without the dashes
    const char* url;     // upstream project

    // Where the emulator itself comes from. NULL for a backend that has no
    // files to fetch. The API is asked for the latest release and the first
    // asset whose name contains assetPart is taken; pinned is the fallback for
    // when that request fails, and goes stale on its own eventually.
    const char* api;
    const char* assetPart;
    const char* pinned;

    // What to pull out of the unpacked archive. The hints only rank candidates,
    // so a rearranged release still resolves as long as the name holds.
    const char* wanted;
    const char* hint1;
    const char* hint2;
} AuthInfo;

static const AuthInfo g_authInfo[AUTH_COUNT] = {
    { "gbe_fork", "gbe_fork", "gbe_fork", "GBEFORK", "gbe",
      "https://github.com/Detanup01/gbe_fork",
      "https://api.github.com/repos/Detanup01/gbe_fork/releases/latest",
      // Full name on purpose: a bare "emu-win-release" also matches the
      // -vs22 asset, which is listed first.
      "emu-win-release.7z",
      "https://github.com/Detanup01/gbe_fork/releases/latest/download/emu-win-release.7z",
      // The experimental build is the one that hands out the web API ticket the
      // proxy is waiting on. The regular build does not.
      "steam_api64.dll", "experimental", "x64" },

    { "uc-online2", "uc-online2", "uc-online2", "UCONLINE", "uc",
      "https://github.com/UnionCrax-Team/uc-online2",
      "https://api.github.com/repos/UnionCrax-Team/uc-online2/releases/latest",
      "-release.zip",
      NULL,                       // asset name carries the version, so no pin
      "steam_api64.dll", "x64", NULL },

    { "SLSsteam", "SLSsteam", NULL, NULL, "sls",
      "https://github.com/AceSLS/SLSsteam",
      NULL, NULL, NULL,
      NULL, NULL, NULL },
};

// ------------------------------------------------------------- resources

static const unsigned char* g_authPak[AUTH_COUNT];
static DWORD                g_authPakLen[AUTH_COUNT];
static const unsigned char* g_slsTemplate;
static DWORD                g_slsTemplateLen;

static const unsigned char* LoadRes(const char* name, DWORD* outLen) {
    *outLen = 0;
    HRSRC r = FindResourceA(NULL, name, RT_RCDATA);
    if (!r) return NULL;
    HGLOBAL h = LoadResource(NULL, r);
    if (!h) return NULL;
    const unsigned char* p = (const unsigned char*)LockResource(h);
    if (!p) return NULL;
    *outLen = SizeofResource(NULL, r);
    return *outLen ? p : NULL;
}

// ---------------------------------------------------------- pak reading

typedef struct {
    const char*          path;      // not NUL terminated
    DWORD                pathLen;
    const unsigned char* data;
    DWORD                dataLen;
    DWORD                flags;
} PakEntry;

typedef struct {
    const unsigned char* p;
    const unsigned char* end;
    DWORD                left;
} PakCursor;

static DWORD PakCount(const unsigned char* pak, DWORD len) {
    if (!pak || len < PAK_MAGIC_LEN + 4) return 0;
    if (memcmp(pak, PAK_MAGIC, strlen(PAK_MAGIC)) != 0) return 0;
    DWORD n;
    memcpy(&n, pak + PAK_MAGIC_LEN, 4);
    return n;
}

static BOOL PakOpen(const unsigned char* pak, DWORD len, PakCursor* c) {
    c->left = PakCount(pak, len);
    c->p    = pak + PAK_MAGIC_LEN + 4;
    c->end  = pak + len;
    return c->left > 0;
}

static BOOL PakNext(PakCursor* c, PakEntry* e) {
    if (!c->left || (size_t)(c->end - c->p) < 12) return FALSE;
    memcpy(&e->pathLen, c->p + 0, 4);
    memcpy(&e->dataLen, c->p + 4, 4);
    memcpy(&e->flags,   c->p + 8, 4);
    c->p += 12;
    if ((size_t)(c->end - c->p) < (size_t)e->pathLen + (size_t)e->dataLen) { c->left = 0; return FALSE; }
    e->path = (const char*)c->p;  c->p += e->pathLen;
    e->data = c->p;               c->p += e->dataLen;
    c->left--;
    return TRUE;
}

// We wrote the archive ourselves, but it decides where files land, so it is
// still checked before any path is joined onto a game folder.
static BOOL PakPathSafe(const char* p, DWORD len) {
    if (len == 0 || len >= PATHBUF - 8) return FALSE;
    if (p[0] == '\\' || p[0] == '/') return FALSE;
    if (len > 1 && p[1] == ':') return FALSE;
    for (DWORD i = 0; i + 1 < len; i++)
        if (p[i] == '.' && p[i + 1] == '.') return FALSE;
    return TRUE;
}

// Whether this build carries files for a backend. Config templates only, these
// days: the emulators themselves are downloaded.
static BOOL AuthBundled(AuthBackend b) {
    if (b == AUTH_SLS) return g_slsTemplateLen > 0;
    return PakCount(g_authPak[b], g_authPakLen[b]) > 0;
}

// Whether the installer can put this backend in place at all, by download or
// out of its own resources.
static BOOL AuthAvailable(AuthBackend b) {
    return g_authInfo[b].api != NULL || AuthBundled(b);
}

static void AuthLoadPayloads(void) {
    for (int i = 0; i < AUTH_COUNT; i++) {
        if (!g_authInfo[i].res) continue;
        g_authPak[i] = LoadRes(g_authInfo[i].res, &g_authPakLen[i]);
    }
    g_slsTemplate = LoadRes("SLSCONF", &g_slsTemplateLen);
}

// --------------------------------------------------------------- helpers

static const char* StrIStr(const char* hay, const char* needle) {
    size_t n = strlen(needle);
    if (!n) return hay;
    for (const char* p = hay; *p; p++)
        if (_strnicmp(p, needle, n) == 0) return p;
    return NULL;
}

static BOOL AllDigits(const char* s) {
    if (!*s) return FALSE;
    for (; *s; s++) if (*s < '0' || *s > '9') return FALSE;
    return TRUE;
}

static BOOL MkDirsFor(const char* path) {
    char buf[PATHBUF];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s", path);
    char* last = strrchr(buf, '\\');
    if (!last) return TRUE;
    *last = 0;

    for (char* p = buf; *p; p++) {
        if (*p != '\\' || p == buf) continue;
        if (p[-1] == ':') continue;                        // C:\ is not ours to create
        *p = 0;
        if (!DirExists(buf)) CreateDirectoryA(buf, NULL);
        *p = '\\';
    }
    if (DirExists(buf)) return TRUE;
    if (CreateDirectoryA(buf, NULL)) return TRUE;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

static DWORD AuthWriteBytes(const char* path, const void* data, DWORD len) {
    if (!MkDirsFor(path)) return GetLastError();
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();

    const unsigned char* p = (const unsigned char*)data;
    DWORD off = 0;
    while (off < len) {
        DWORD w = 0;
        if (!WriteFile(h, p + off, len - off, &w, NULL) || w == 0) {
            DWORD e = GetLastError();
            CloseHandle(h);
            return e ? e : ERROR_WRITE_FAULT;
        }
        off += w;
    }
    CloseHandle(h);
    return 0;
}

// Downloading and unpacking a backend release. Needs the helpers above.
#include "fetch.h"

// ------------------------------------------------------- game identity

typedef struct {
    char appid[16];
    char account[128];
    char steamid[32];
    char game[160];
    char dir[PATHBUF];
} AuthVars;

// Pulls "key" "value" out of a Valve KeyValues blob. Good enough for the two
// fields an appmanifest is read for.
//
// Quoted tokens are walked one at a time rather than in pairs, because a block
// name ("AppState") is a lone token followed by a brace and pairing from the
// top of the file would be one token out from there on.
static BOOL VdfGet(const char* text, size_t len, const char* key, char* out, size_t cap) {
    size_t keyLen = strlen(key);
    BOOL valueIsNext = FALSE;

    for (size_t i = 0; i < len; i++) {
        if (text[i] != '"') continue;

        size_t s = ++i;
        while (i < len && text[i] != '"') i++;
        if (i >= len) break;
        size_t n = i - s;

        if (valueIsNext) {
            if (n >= cap) n = cap - 1;
            memcpy(out, text + s, n);
            out[n] = 0;
            return TRUE;
        }
        if (n == keyLen && _strnicmp(text + s, key, keyLen) == 0) valueIsNext = TRUE;
    }
    return FALSE;
}

static BOOL ReadAppIdFile(const char* dir, char* out, size_t cap) {
    char path[PATHBUF];
    PathJoin(path, sizeof(path), dir, "steam_appid.txt");
    size_t len = 0;
    unsigned char* buf = ReadFileBytes(path, 64, &len);
    if (!buf) return FALSE;

    char tmp[64];
    size_t n = len < sizeof(tmp) - 1 ? len : sizeof(tmp) - 1;
    memcpy(tmp, buf, n);
    tmp[n] = 0;
    free(buf);

    Trim(tmp);
    if (!AllDigits(tmp)) return FALSE;
    _snprintf_s(out, cap, _TRUNCATE, "%s", tmp);
    return TRUE;
}

// A game under <library>\steamapps\common\<installdir> can be traced back to
// its AppID through that library's appmanifest_*.acf files.
static BOOL AppIdFromSteam(const char* folder, char* out, size_t cap) {
    const char* marker = "\\steamapps\\common\\";
    const char* hit = StrIStr(folder, marker);
    if (!hit) return FALSE;

    char lib[PATHBUF];
    size_t libLen = (size_t)(hit - folder);
    if (libLen == 0 || libLen >= sizeof(lib)) return FALSE;
    memcpy(lib, folder, libLen);
    lib[libLen] = 0;

    const char* nameStart = hit + strlen(marker);
    char installDir[PATHBUF];
    size_t k = 0;
    while (nameStart[k] && nameStart[k] != '\\' && k + 1 < sizeof(installDir)) {
        installDir[k] = nameStart[k];
        k++;
    }
    installDir[k] = 0;
    if (!k) return FALSE;

    char pattern[PATHBUF];
    PathJoin(pattern, sizeof(pattern), lib, "steamapps\\appmanifest_*.acf");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    BOOL found = FALSE;
    do {
        char acf[PATHBUF];
        PathJoin(acf, sizeof(acf), lib, "steamapps");
        PathJoin(acf, sizeof(acf), acf, fd.cFileName);

        size_t len = 0;
        unsigned char* buf = ReadFileBytes(acf, 1u << 18, &len);
        if (!buf) continue;

        char dirVal[PATHBUF], idVal[32];
        if (VdfGet((char*)buf, len, "installdir", dirVal, sizeof(dirVal)) &&
            _stricmp(dirVal, installDir) == 0 &&
            VdfGet((char*)buf, len, "appid", idVal, sizeof(idVal)) &&
            AllDigits(idVal)) {
            _snprintf_s(out, cap, _TRUNCATE, "%s", idVal);
            found = TRUE;
        }
        free(buf);
    } while (!found && FindNextFileA(h, &fd));

    FindClose(h);
    return found;
}

static BOOL AuthFindAppId(const char* folder, char* out, size_t cap) {
    // A steam_appid.txt already there wins: something has decided what this game
    // is. Binaries\Win64 layouts mean looking a few levels up as well.
    char dir[PATHBUF];
    _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s", folder);
    for (int up = 0; up < 5; up++) {
        if (ReadAppIdFile(dir, out, cap)) return TRUE;
        char* slash = strrchr(dir, '\\');
        if (!slash || slash == dir) break;
        *slash = 0;
    }
    return AppIdFromSteam(folder, out, cap);
}

static void AuthSteamUser(char* name, size_t nameCap, char* id, size_t idCap) {
    _snprintf_s(name, nameCap, _TRUNCATE, "%s", "Player");
    _snprintf_s(id, idCap, _TRUNCATE, "%s", "76561197960287930");   // gbe_fork's own default

    HKEY k;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        char buf[128];
        DWORD type = 0, size = sizeof(buf) - 1;
        if (RegQueryValueExA(k, "AutoLoginUser", NULL, &type, (BYTE*)buf, &size) == ERROR_SUCCESS &&
            type == REG_SZ && size > 1) {
            buf[size < sizeof(buf) ? size : sizeof(buf) - 1] = 0;
            if (buf[0]) _snprintf_s(name, nameCap, _TRUNCATE, "%s", buf);
        }
        RegCloseKey(k);
    }
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam\\ActiveProcess", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD v = 0, type = 0, size = sizeof(v);
        if (RegQueryValueExA(k, "ActiveUser", NULL, &type, (BYTE*)&v, &size) == ERROR_SUCCESS &&
            type == REG_DWORD && v)
            _snprintf_s(id, idCap, _TRUNCATE, "%llu", 76561197960265728ULL + (ULONGLONG)v);
        RegCloseKey(k);
    }
}

// The name a human would call this game. TheIsle\Binaries\Win64 is "TheIsle",
// the same way the game listing decides what to show.
static void AuthGameName(const char* folder, char* out, size_t cap) {
    char dir[PATHBUF];
    _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s", folder);

    for (int up = 0; up < 4 && IsGenericFolder(BaseName(dir)); up++) {
        char* slash = strrchr(dir, '\\');
        if (!slash || slash == dir) break;
        *slash = 0;
    }
    _snprintf_s(out, cap, _TRUNCATE, "%s", BaseName(dir));
}

// Returns FALSE when the AppID could not be worked out, which is fatal for
// every backend: none of them can produce a ticket for an unknown app.
static BOOL AuthFillVars(const char* folder, const char* appidOverride, AuthVars* v) {
    memset(v, 0, sizeof(*v));
    _snprintf_s(v->dir, sizeof(v->dir), _TRUNCATE, "%s", folder);
    AuthGameName(folder, v->game, sizeof(v->game));
    AuthSteamUser(v->account, sizeof(v->account), v->steamid, sizeof(v->steamid));

    if (appidOverride && AllDigits(appidOverride)) {
        _snprintf_s(v->appid, sizeof(v->appid), _TRUNCATE, "%s", appidOverride);
        return TRUE;
    }
    return AuthFindAppId(folder, v->appid, sizeof(v->appid));
}

static unsigned char* AuthSubstitute(const unsigned char* in, DWORD inLen,
                                     const AuthVars* v, DWORD* outLen) {
    struct { const char* key; const char* val; } sub[] = {
        { "{APPID}",        v->appid   },
        { "{ACCOUNT_NAME}", v->account },
        { "{STEAM_ID}",     v->steamid },
        { "{GAME_NAME}",    v->game    },
        { "{GAME_DIR}",     v->dir     },
    };
    const int subCount = (int)(sizeof(sub) / sizeof(sub[0]));

    size_t cap = (size_t)inLen + 256, len = 0;
    unsigned char* out = (unsigned char*)malloc(cap);
    if (!out) return NULL;

    for (DWORD i = 0; i < inLen; ) {
        const char* rep = NULL;
        size_t keyLen = 0;
        if (in[i] == '{') {
            for (int s = 0; s < subCount; s++) {
                size_t n = strlen(sub[s].key);
                if ((size_t)(inLen - i) >= n && memcmp(in + i, sub[s].key, n) == 0) {
                    rep = sub[s].val;
                    keyLen = n;
                    break;
                }
            }
        }
        size_t need = rep ? strlen(rep) : 1;
        if (len + need + 1 > cap) {
            size_t ncap = (len + need + 1) * 2;
            unsigned char* grown = (unsigned char*)realloc(out, ncap);
            if (!grown) { free(out); return NULL; }
            out = grown;
            cap = ncap;
        }
        if (rep) { memcpy(out + len, rep, need); len += need; i += (DWORD)keyLen; }
        else     { out[len++] = in[i++]; }
    }
    *outLen = (DWORD)len;
    return out;
}

// ------------------------------------------------------------- manifest

// One record per file the installer wrote, so removal puts the folder back
// exactly as it was instead of guessing from file names.
typedef struct {
    char rel[PATHBUF];
    BOOL hadOriginal;      // an existing file was parked as <name>.eosbak
} AuthFileRec;

static void AuthManifestPath(const char* folder, char* out, size_t cap) {
    PathJoin(out, cap, folder, AUTH_MANIFEST);
}

static BOOL AuthReadManifest(const char* folder, char* backend, size_t bcap,
                             char* appid, size_t acap,
                             AuthFileRec* files, int maxFiles, int* count) {
    if (backend && bcap) backend[0] = 0;
    if (appid && acap)   appid[0] = 0;
    if (count) *count = 0;

    char path[PATHBUF];
    AuthManifestPath(folder, path, sizeof(path));
    size_t len = 0;
    unsigned char* buf = ReadFileBytes(path, 1u << 18, &len);
    if (!buf) return FALSE;

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
        if (!line[0] || line[0] == '#') continue;

        if (_strnicmp(line, "backend=", 8) == 0) {
            if (backend) _snprintf_s(backend, bcap, _TRUNCATE, "%s", line + 8);
        } else if (_strnicmp(line, "appid=", 6) == 0) {
            if (appid) _snprintf_s(appid, acap, _TRUNCATE, "%s", line + 6);
        } else if ((line[0] == '+' || line[0] == '*') && line[1]) {
            if (files && count && *count < maxFiles) {
                _snprintf_s(files[*count].rel, PATHBUF, _TRUNCATE, "%s", line + 1);
                files[*count].hadOriginal = (line[0] == '*');
                (*count)++;
            }
        }
    }
    free(buf);
    return backend ? backend[0] != 0 : TRUE;
}

static DWORD AuthWriteManifest(const char* folder, const char* backend, const char* appid,
                               const AuthFileRec* files, int count) {
    static char body[128 * 1024];
    int n = _snprintf_s(body, sizeof(body), _TRUNCATE,
        "# Written by eos-proxy-setup.exe. It is how the installer knows what it\r\n"
        "# put here and what to put back. Deleting it makes removal manual.\r\n"
        "backend=%s\r\n"
        "appid=%s\r\n"
        "# + installed   * installed over an original parked as <name>" AUTH_BAK_EXT "\r\n",
        backend, appid);
    if (n < 0) return ERROR_INSUFFICIENT_BUFFER;

    for (int i = 0; i < count; i++) {
        size_t used = strlen(body);
        if (_snprintf_s(body + used, sizeof(body) - used, _TRUNCATE,
                        "%c%s\r\n", files[i].hadOriginal ? '*' : '+', files[i].rel) < 0)
            return ERROR_INSUFFICIENT_BUFFER;
    }

    char path[PATHBUF];
    AuthManifestPath(folder, path, sizeof(path));
    return AuthWriteBytes(path, body, (DWORD)strlen(body));
}

// ------------------------------------------------------------- install

static void AuthUndo(const char* folder, const AuthFileRec* rec, int n) {
    for (int i = n - 1; i >= 0; i--) {
        char full[PATHBUF], bak[PATHBUF];
        PathJoin(full, sizeof(full), folder, rec[i].rel);
        _snprintf_s(bak, sizeof(bak), _TRUNCATE, "%s" AUTH_BAK_EXT, full);
        DeleteFileA(full);
        if (rec[i].hadOriginal) MoveFileA(bak, full);
    }
}

// Folders the payload created are only worth keeping while they hold files.
// RemoveDirectory refusing a non-empty folder is exactly the wanted stop.
static void AuthPruneDirs(const char* folder, const AuthFileRec* rec, int n) {
    size_t rootLen = strlen(folder);
    for (int i = n - 1; i >= 0; i--) {
        char full[PATHBUF];
        PathJoin(full, sizeof(full), folder, rec[i].rel);
        for (;;) {
            char* slash = strrchr(full, '\\');
            if (!slash) break;
            *slash = 0;
            if (strlen(full) <= rootLen) break;
            if (!RemoveDirectoryA(full)) break;
        }
    }
}

static BOOL AuthRemove(const char* folder) {
    g_lastError = 0;

    char backend[64], appid[16];
    static AuthFileRec rec[AUTH_MAX_FILES];
    int count = 0;
    if (!AuthReadManifest(folder, backend, sizeof(backend), appid, sizeof(appid),
                          rec, AUTH_MAX_FILES, &count)) {
        printf("  -> no auth backend installed here by this installer.\n");
        return TRUE;
    }

    printf("  -  removing %s (%d file(s))\n", backend, count);
    BOOL ok = TRUE;
    for (int i = 0; i < count; i++) {
        char full[PATHBUF], bak[PATHBUF];
        PathJoin(full, sizeof(full), folder, rec[i].rel);
        _snprintf_s(bak, sizeof(bak), _TRUNCATE, "%s" AUTH_BAK_EXT, full);

        if (FileExists(full) && !DeleteFileA(full)) {
            DWORD e = GetLastError();
            if (!g_lastError) g_lastError = e;
            printf("  X  could not delete %s: %s\n", rec[i].rel, ErrText(e));
            ok = FALSE;
            continue;
        }
        if (!rec[i].hadOriginal) continue;

        if (FileExists(bak)) {
            if (!MoveFileA(bak, full)) {
                DWORD e = GetLastError();
                if (!g_lastError) g_lastError = e;
                printf("  X  could not restore %s: %s\n", rec[i].rel, ErrText(e));
                ok = FALSE;
            }
        } else if (!FileExists(full)) {
            printf("  !  %s had a backup but it is gone. Verify the game files.\n", rec[i].rel);
        }
    }
    AuthPruneDirs(folder, rec, count);

    char path[PATHBUF];
    AuthManifestPath(folder, path, sizeof(path));
    if (ok && !DeleteFileA(path) && FileExists(path)) {
        g_lastError = GetLastError();
        printf("  X  could not delete %s: %s\n", AUTH_MANIFEST, ErrText(g_lastError));
        ok = FALSE;
    }

    if (ok) printf("  OK %s removed.\n", backend);
    return ok;
}

static BOOL AuthSlsConfig(const char* folder, const char* appidOverride) {
    g_lastError = 0;

    if (!g_slsTemplateLen) {
        printf("  X  the SLSsteam template is missing from this build.\n");
        return FALSE;
    }

    AuthVars v;
    if (!AuthFillVars(folder, appidOverride, &v)) {
        printf("  X  could not work out the Steam AppID for this folder.\n");
        printf("     Pass it yourself:  setup.exe --sls --appid <number> \"%s\"\n", folder);
        return FALSE;
    }

    DWORD outLen = 0;
    unsigned char* out = AuthSubstitute(g_slsTemplate, g_slsTemplateLen, &v, &outLen);
    if (!out) { printf("  X  out of memory.\n"); return FALSE; }

    // Anything already installed here is replaced, the same as a real backend:
    // running two of them at once is not a thing.
    char had[64];
    if (AuthReadManifest(folder, had, sizeof(had), NULL, 0, NULL, 0, NULL)) {
        printf("  -  %s is already installed here, replacing it\n", had);
        if (!AuthRemove(folder)) { free(out); return FALSE; }
    }

    char path[PATHBUF];
    PathJoin(path, sizeof(path), folder, AUTH_SLS_FILE);
    DWORD err = AuthWriteBytes(path, out, outLen);
    free(out);

    if (err) {
        g_lastError = err;
        printf("  X  write failed: %s\n", ErrText(err));
        return FALSE;
    }

    // Tracked like the others so the listing shows it and "remove auth" clears
    // it, even though nothing in the game folder was actually patched.
    AuthFileRec rec;
    _snprintf_s(rec.rel, PATHBUF, _TRUNCATE, "%s", AUTH_SLS_FILE);
    rec.hadOriginal = FALSE;
    err = AuthWriteManifest(folder, g_authInfo[AUTH_SLS].id, v.appid, &rec, 1);
    if (err) {
        g_lastError = err;
        printf("  X  could not write %s: %s\n", AUTH_MANIFEST, ErrText(err));
        DeleteFileA(path);
        return FALSE;
    }

    printf("  OK wrote %s\n", path);
    printf("     SLSsteam is Linux only, so there is nothing to copy into the game\n"
           "     folder from here. On the Linux side put that file at\n"
           "     ~/.config/SLSsteam/config.yaml (merge it if you already have one)\n"
           "     and start Steam through SLSsteam. AppID %s is already filled in.\n", v.appid);
    return TRUE;
}

// ------------------------------------------------- fetching the emulator

// Makes sure this backend's emulator file is in the cache, downloading and
// unpacking the upstream release when it is not, and hands back its path.
static BOOL AuthEnsureBinary(AuthBackend b, const char* gameFolder, char* out, size_t cap) {
    const AuthInfo* info = &g_authInfo[b];

    char cache[PATHBUF], home[PATHBUF], unpacked[PATHBUF];
    if (!CacheDir(cache, sizeof(cache))) {
        printf("  X  LOCALAPPDATA is not set, so there is nowhere to cache the download.\n");
        return FALSE;
    }
    PathJoin(home, sizeof(home), cache, info->id);
    PathJoin(unpacked, sizeof(unpacked), home, "unpacked");

    int seen = 0;
    if (FindInTree(unpacked, info->wanted, info->hint1, info->hint2, out, cap, &seen)) {
        printf("  -  using the cached %s\n", out + strlen(unpacked) + 1);
        return TRUE;
    }

    // The API knows what the current release is. The pinned URL exists only for
    // when that call cannot be made, and goes stale on its own eventually.
    char url[PATHBUF];
    url[0] = 0;
    printf("  -  asking github for the latest %s release\n", info->label);

    size_t jsonLen = 0;
    unsigned char* json = NetGetText(info->api, &jsonLen);
    if (json) {
        if (!GhFindAsset((const char*)json, jsonLen, info->assetPart, url, sizeof(url)))
            printf("  !  no asset named like \"%s\" in the latest release\n", info->assetPart);
        free(json);
    } else {
        printf("  !  could not reach the github api\n");
    }
    if (!url[0] && info->pinned) {
        printf("  -  falling back to the pinned download\n");
        _snprintf_s(url, sizeof(url), _TRUNCATE, "%s", info->pinned);
    }
    if (!url[0]) {
        printf("  X  could not work out where to download %s from.\n", info->label);
        printf("     Check the connection, or fetch it by hand from\n     %s\n", info->url);
        return FALSE;
    }

    char archive[PATHBUF];
    PathJoin(archive, sizeof(archive), home, UrlLeaf(url));
    printf("  -  downloading %s\n     %s\n", UrlLeaf(url), url);

    if (!NetDownload(url, archive)) {
        printf("  X  download failed (0x%08lX).\n", g_lastError);
        if (!FileExists(archive)) ReportAvBlock(UrlLeaf(url), gameFolder);
        return FALSE;
    }

    ULONGLONG size = 0;
    if (!GetFileSize64(archive, &size) || size == 0) {
        ReportAvBlock(UrlLeaf(url), gameFolder);
        return FALSE;
    }
    printf("  -  %llu bytes, unpacking\n", size);

    if (!Unpack(archive, unpacked, gameFolder)) return FALSE;

    if (!FindInTree(unpacked, info->wanted, info->hint1, info->hint2, out, cap, &seen)) {
        printf("  X  no %s in that archive after unpacking.\n", info->wanted);
        printf("     Either the release layout changed, or the scanner took it.\n");
        ReportAvBlock(info->wanted, gameFolder);
        return FALSE;
    }

    // Which copy was picked matters - gbe_fork ships several builds of the same
    // file name - so it is printed rather than chosen silently.
    printf("  -  picked %s", out + strlen(unpacked) + 1);
    if (seen > 1) printf("  (out of %d copies in the archive)", seen);
    printf("\n");
    return TRUE;
}

// ------------------------------------------------------------- install

// Writes one file into the game folder, parking any original as .eosbak and
// recording what happened so a later removal undoes it exactly. Returns a
// Win32 error, 0 on success.
static DWORD AuthPlace(const char* folder, const char* rel,
                       const void* data, DWORD len,
                       AuthFileRec* rec, int* count) {
    if (*count >= AUTH_MAX_FILES) return ERROR_TOO_MANY_NAMES;

    char full[PATHBUF], bak[PATHBUF];
    PathJoin(full, sizeof(full), folder, rel);
    _snprintf_s(bak, sizeof(bak), _TRUNCATE, "%s" AUTH_BAK_EXT, full);

    BOOL parked = FALSE;
    if (FileExists(full)) {
        // An .eosbak already sitting there is the game's real file from an
        // earlier install, so it is the one worth keeping.
        if (!FileExists(bak)) {
            if (!MoveFileA(full, bak)) return GetLastError();
            parked = TRUE;
        }
    }

    DWORD err = AuthWriteBytes(full, data, len);
    if (err) {
        if (parked) MoveFileA(bak, full);
        return err;
    }

    _snprintf_s(rec[*count].rel, PATHBUF, _TRUNCATE, "%s", rel);
    rec[*count].hadOriginal = parked;
    (*count)++;
    return 0;
}

static BOOL AuthInstall(const char* folder, AuthBackend b, const char* appidOverride) {
    g_lastError = 0;
    const AuthInfo* info = &g_authInfo[b];

    if (b == AUTH_SLS) return AuthSlsConfig(folder, appidOverride);

    if (!AuthAvailable(b)) {
        printf("  X  this build of the installer has no way to install %s.\n", info->label);
        printf("     Upstream: %s\n", info->url);
        return FALSE;
    }

    AuthVars v;
    if (!AuthFillVars(folder, appidOverride, &v)) {
        printf("  X  could not work out the Steam AppID for this folder.\n");
        printf("     %s cannot produce a ticket without it. Pass it yourself:\n", info->label);
        printf("       setup.exe --%s --appid <number> \"%s\"\n", info->flag, folder);
        return FALSE;
    }

    // Anything already here is removed first rather than layered on top, so the
    // .eosbak files keep pointing at the game's own originals.
    char had[64];
    if (AuthReadManifest(folder, had, sizeof(had), NULL, 0, NULL, 0, NULL)) {
        printf("  -  %s is already installed here, replacing it\n", had);
        if (!AuthRemove(folder)) return FALSE;
    }

    printf("  -  %s for app %s as \"%s\" (%s)\n",
           info->label, v.appid, v.account, v.steamid);

    static AuthFileRec rec[AUTH_MAX_FILES];
    int done = 0;
    BOOL ok = TRUE;

    // 1. the emulator, fetched from upstream and cached between runs
    if (info->api) {
        char src[PATHBUF];
        if (!AuthEnsureBinary(b, folder, src, sizeof(src))) return FALSE;

        size_t dllLen = 0;
        unsigned char* dll = ReadFileBytes(src, 64u << 20, &dllLen);
        if (!dll || dllLen == 0) {
            free(dll);
            printf("  X  could not read %s back out of the cache.\n", info->wanted);
            ReportAvBlock(info->wanted, folder);
            return FALSE;
        }

        DWORD err = AuthPlace(folder, info->wanted, dll, (DWORD)dllLen, rec, &done);
        free(dll);

        if (err) {
            g_lastError = err;
            printf("  X  writing %s failed: %s\n", info->wanted, ErrText(err));
            ok = FALSE;
        } else {
            printf("  -  %s (%llu bytes%s)\n", info->wanted, (ULONGLONG)dllLen,
                   rec[done - 1].hadOriginal ? ", original parked" : "");
        }
    }

    // 2. the config templates, which do ride along inside this exe
    PakCursor c;
    PakEntry e;
    if (ok && PakOpen(g_authPak[b], g_authPakLen[b], &c)) {
        while (ok && PakNext(&c, &e)) {
            if (!PakPathSafe(e.path, e.pathLen)) {
                printf("  X  refusing a suspicious payload path, aborting.\n");
                ok = FALSE;
                break;
            }

            char rel[PATHBUF];
            memcpy(rel, e.path, e.pathLen);
            rel[e.pathLen] = 0;

            const unsigned char* data = e.data;
            DWORD len = e.dataLen;
            unsigned char* sub = NULL;
            if (e.flags & PAK_FLAG_TEXT) {
                sub = AuthSubstitute(e.data, e.dataLen, &v, &len);
                if (!sub) { printf("  X  out of memory.\n"); ok = FALSE; break; }
                data = sub;
            }

            DWORD err = AuthPlace(folder, rel, data, len, rec, &done);
            free(sub);

            if (err) {
                g_lastError = err;
                printf("  X  writing %s failed: %s\n", rel, ErrText(err));
                ok = FALSE;
                break;
            }
            printf("  -  %s%s\n", rel, (e.flags & PAK_FLAG_TEXT) ? " (config)" : "");
        }
    }

    if (ok) {
        DWORD err = AuthWriteManifest(folder, info->id, v.appid, rec, done);
        if (err) {
            g_lastError = err;
            printf("  X  could not write %s: %s\n", AUTH_MANIFEST, ErrText(err));
            ok = FALSE;
        }
    }

    if (!ok) {
        AuthUndo(folder, rec, done);
        AuthPruneDirs(folder, rec, done);
        printf("  -  rolled back, the folder is as it was.\n");
        return FALSE;
    }

    printf("  OK %s installed (%d file(s)).\n", info->label, done);
    return TRUE;
}

// --------------------------------------------------------------- status

// Short label for the game listing. Backends put there by hand are recognised
// too, so the listing never claims a folder is untouched when it is not.
static const char* AuthStatusText(const char* folder) {
    static char buf[96];

    char backend[64], appid[16];
    if (AuthReadManifest(folder, backend, sizeof(backend), appid, sizeof(appid), NULL, 0, NULL)) {
        if (appid[0]) _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s, app %s", backend, appid);
        else          _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s", backend);
        return buf;
    }

    char probe[PATHBUF];
    PathJoin(probe, sizeof(probe), folder, "steam_settings");
    if (DirExists(probe)) return "gbe_fork (by hand)";

    PathJoin(probe, sizeof(probe), folder, "OnlineFix64.dll");
    if (FileExists(probe)) return "online-fix (by hand)";

    return "none";
}
