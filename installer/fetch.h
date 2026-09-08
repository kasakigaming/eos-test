#pragma once

#include <urlmon.h>
#include <wininet.h>

// Fetching a Steam auth backend at install time.
//
// The emulators are not embedded in this exe on purpose. Antivirus reliably
// flags them - Defender quarantines gbe_fork's release archive the moment it
// lands on disk - and an installer carrying those bytes gets deleted along with
// them before the user ever runs it. Downloading at install time keeps this exe
// clean, and when the AV does eat the download there is a real error message to
// show instead of a mystery.
//
// Everything here leans on what Windows already ships: URLDownloadToFileA for
// the transfer (TLS and redirects handled for us) and tar.exe, which is bsdtar
// and reads both the .zip and the .7z that these projects publish.
//
// Included by auth.h, so the utilities from setup.c are in scope.

#define FETCH_CACHE_DIR "eos-proxy\\cache"

// --------------------------------------------------------------- transport

static BOOL NetDownload(const char* url, const char* dest) {
    DeleteUrlCacheEntryA(url);      // a stale copy would defeat "latest"
    if (!MkDirsFor(dest)) return FALSE;
    DeleteFileA(dest);

    HRESULT hr = URLDownloadToFileA(NULL, url, dest, 0, NULL);
    if (FAILED(hr)) {
        g_lastError = (DWORD)hr;
        return FALSE;
    }
    return FileExists(dest);
}

// Small responses (the GitHub API listing) go through a temp file rather than a
// second transport, so there is only one thing that can break.
static unsigned char* NetGetText(const char* url, size_t* outLen) {
    char tmp[PATHBUF], dir[PATHBUF];
    if (!GetTempPathA(sizeof(dir), dir)) return NULL;
    PathJoin(tmp, sizeof(tmp), dir, "eos-proxy-api.json");

    if (!NetDownload(url, tmp)) return NULL;
    unsigned char* buf = ReadFileBytes(tmp, 4u << 20, outLen);
    DeleteFileA(tmp);
    return buf;
}

// ------------------------------------------------------------ github api

// Last path segment of a URL, which is '/' separated rather than '\'.
static const char* UrlLeaf(const char* url) {
    const char* slash = strrchr(url, '/');
    return (slash && slash[1]) ? slash + 1 : url;
}

// Picks the download URL of the first asset whose file name contains namePart.
// Hand rolled rather than a JSON parser: the field is unambiguous and the
// alternative is dragging a dependency into a single file installer.
static BOOL GhFindAsset(const char* json, size_t len, const char* namePart,
                        char* out, size_t cap) {
    const char* key = "\"browser_download_url\":\"";
    size_t keyLen = strlen(key);

    for (size_t i = 0; i + keyLen < len; i++) {
        if (memcmp(json + i, key, keyLen) != 0) continue;

        size_t s = i + keyLen;
        size_t e = s;
        while (e < len && json[e] != '"') e++;
        if (e >= len || e - s >= cap) continue;

        char url[PATHBUF];
        size_t n = e - s;
        if (n >= sizeof(url)) continue;
        memcpy(url, json + s, n);
        url[n] = 0;

        if (StrIStr(UrlLeaf(url), namePart)) {
            _snprintf_s(out, cap, _TRUNCATE, "%s", url);
            return TRUE;
        }
        i = e;
    }
    return FALSE;
}

// ------------------------------------------------------------- cache dir

// Under LOCALAPPDATA rather than next to the exe: setup.exe is often run from
// Downloads or from inside Program Files, and only one of those is writable.
static BOOL CacheDir(char* out, size_t cap) {
    char base[PATHBUF];
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof(base));
    if (!n || n >= sizeof(base)) return FALSE;
    PathJoin(out, cap, base, FETCH_CACHE_DIR);
    return TRUE;
}

// Defender does not always delete what it dislikes. Often it leaves the file
// at full size and fails every attempt to open it, and each tool then reports
// its own baffling error - 7zr says "Can't open as archive", tar says nothing
// useful. Asking the OS directly is the only way to tell that apart from a
// corrupt download.
static BOOL FileBlockedByAv(const char* path) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return FALSE; }

    DWORD e = GetLastError();
    return e == ERROR_VIRUS_INFECTED || e == ERROR_VIRUS_DELETED;
}

// Steam emulators are flagged as riskware more or less universally. It is a
// false positive, but no amount of retrying gets past it, so the only useful
// thing to print is exactly what to type.
static void ReportAvBlock(const char* what, const char* gameFolder) {
    char cache[PATHBUF];
    if (!CacheDir(cache, sizeof(cache))) _snprintf_s(cache, sizeof(cache), _TRUNCATE, "%%LOCALAPPDATA%%\\eos-proxy");

    printf("\n  !  The antivirus is blocking %s.\n", what);
    printf("     Steam emulators trip nearly every scanner. Windows leaves the file\n"
           "     on disk but refuses to let anything read it, so this cannot be\n"
           "     worked around from here - it needs your say so.\n\n"
           "     In a PowerShell running as administrator:\n\n"
           "       Add-MpPreference -ExclusionPath \"%s\"\n", cache);
    if (gameFolder && gameFolder[0])
        printf("       Add-MpPreference -ExclusionPath \"%s\"\n", gameFolder);
    printf("\n     then run this again. To undo it later, the same command with\n"
           "     Remove-MpPreference. On another scanner, exclude those two paths\n"
           "     the way that product does it.\n");
}

// --------------------------------------------------------------- unpacking

static BOOL HaveTar(char* out, size_t cap) {
    char found[PATHBUF];
    if (!SearchPathA(NULL, "tar.exe", NULL, sizeof(found), found, NULL)) return FALSE;
    _snprintf_s(out, cap, _TRUNCATE, "%s", found);
    return TRUE;
}

static BOOL RunHidden(const char* cmdline, DWORD* exitCode) {
    char mutableCmd[2048];
    _snprintf_s(mutableCmd, sizeof(mutableCmd), _TRUNCATE, "%s", cmdline);

    STARTUPINFOA si = { 0 };
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = { 0 };

    if (!CreateProcessA(NULL, mutableCmd, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        g_lastError = GetLastError();
        return FALSE;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}

// The official standalone 7z extractor. Public domain, 600KB, and the only
// thing on hand that reads the BCJ2 filtered archives gbe_fork publishes -
// bsdtar handles plain .7z but gives up on those. Fetched only when a .7z
// actually needs it, and kept in the cache afterwards.
#define SEVENZR_URL "https://www.7-zip.org/a/7zr.exe"

static BOOL Ensure7zr(char* out, size_t cap) {
    char cache[PATHBUF];
    if (!CacheDir(cache, sizeof(cache))) return FALSE;
    PathJoin(out, cap, cache, "7zr.exe");
    if (FileExists(out)) return TRUE;

    printf("  -  fetching 7zr.exe, the only extractor that reads this archive\n");
    if (!NetDownload(SEVENZR_URL, out)) {
        printf("  X  could not download %s (0x%08lX)\n", SEVENZR_URL, g_lastError);
        return FALSE;
    }
    return FileExists(out);
}

static BOOL EndsWithI(const char* s, const char* suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && _stricmp(s + n - m, suffix) == 0;
}

// tar.exe first because it is already on the machine; 7zr only when it is a
// .7z that bsdtar could not make sense of.
static BOOL RunExtractor(const char* archive, const char* destDir) {
    if (!DirExists(destDir) && !MkDirsFor(destDir) && !CreateDirectoryA(destDir, NULL)) {
        g_lastError = GetLastError();
        printf("  X  could not create %s: %s\n", destDir, ErrText(g_lastError));
        return FALSE;
    }
    if (!DirExists(destDir)) CreateDirectoryA(destDir, NULL);

    char cmd[2048];
    DWORD rc = 1;

    char tar[PATHBUF];
    if (HaveTar(tar, sizeof(tar))) {
        _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "\"%s\" -xf \"%s\" -C \"%s\"",
                    tar, archive, destDir);
        if (RunHidden(cmd, &rc) && rc == 0) return TRUE;
    }

    if (!EndsWithI(archive, ".7z")) {
        printf("  X  tar.exe could not unpack %s (exit %lu).\n", BaseName(archive), rc);
        return FALSE;
    }

    char zr[PATHBUF];
    if (!Ensure7zr(zr, sizeof(zr))) return FALSE;

    _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "\"%s\" x -y -bso0 -bsp0 -o\"%s\" \"%s\"",
                zr, destDir, archive);
    if (!RunHidden(cmd, &rc)) {
        printf("  X  could not run 7zr.exe: %s\n", ErrText(g_lastError));
        return FALSE;
    }
    if (rc != 0) {
        printf("  X  7zr.exe failed (exit %lu) unpacking %s\n", rc, BaseName(archive));
        return FALSE;
    }
    return TRUE;
}

// A file the scanner has locked reads as a corrupt archive to every extractor,
// so that is checked for first and again afterwards, rather than leaving the
// user staring at "Can't open as archive".
static BOOL Unpack(const char* archive, const char* destDir, const char* gameFolder) {
    if (FileBlockedByAv(archive)) {
        ReportAvBlock(BaseName(archive), gameFolder);
        return FALSE;
    }
    if (RunExtractor(archive, destDir)) return TRUE;

    if (FileBlockedByAv(archive) || !FileExists(archive))
        ReportAvBlock(BaseName(archive), gameFolder);
    return FALSE;
}

// ------------------------------------------------------- finding the file

// Scores a candidate by how many of the wanted path hints it contains, so
// experimental\x64\steam_api64.dll wins over regular\x32\steam_api64.dll
// without this having to know the archive's layout in advance.
typedef struct {
    char best[PATHBUF];
    int  bestScore;
    int  seen;
    const char* name;
    const char* hint1;
    const char* hint2;
} FileHunt;

static void HuntDir(const char* dir, const char* rel, FileHunt* h, int depth) {
    if (depth <= 0) return;

    char pattern[PATHBUF];
    PathJoin(pattern, sizeof(pattern), dir, "*");
    WIN32_FIND_DATAA fd;
    HANDLE fh = FindFirstFileA(pattern, &fd);
    if (fh == INVALID_HANDLE_VALUE) return;

    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;

        char full[PATHBUF], sub[PATHBUF];
        PathJoin(full, sizeof(full), dir, fd.cFileName);
        if (rel[0]) PathJoin(sub, sizeof(sub), rel, fd.cFileName);
        else        _snprintf_s(sub, sizeof(sub), _TRUNCATE, "%s", fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            HuntDir(full, sub, h, depth - 1);
            continue;
        }
        if (_stricmp(fd.cFileName, h->name) != 0) continue;

        h->seen++;
        int score = 0;
        if (h->hint1 && StrIStr(sub, h->hint1)) score += 2;
        if (h->hint2 && StrIStr(sub, h->hint2)) score += 1;
        if (score > h->bestScore || h->best[0] == 0) {
            h->bestScore = score;
            _snprintf_s(h->best, sizeof(h->best), _TRUNCATE, "%s", full);
        }
    } while (FindNextFileA(fh, &fd));

    FindClose(fh);
}

static BOOL FindInTree(const char* root, const char* name,
                       const char* hint1, const char* hint2,
                       char* out, size_t cap, int* seenCount) {
    FileHunt h;
    memset(&h, 0, sizeof(h));
    h.name  = name;
    h.hint1 = hint1;
    h.hint2 = hint2;

    HuntDir(root, "", &h, 8);
    if (seenCount) *seenCount = h.seen;
    if (!h.best[0]) return FALSE;

    _snprintf_s(out, cap, _TRUNCATE, "%s", h.best);
    return TRUE;
}
