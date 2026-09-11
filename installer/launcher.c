// Stand-in for a game's Easy Anti-Cheat bootstrapper.
//
// Steam starts an EAC title through <Game>.exe, which is the anti-cheat
// bootstrapper rather than the game. That bootstrapper loads the live EAC
// module, and EAC will not map an unsigned EOS SDK into the process it
// protects: the game logs "Failed to load ... (GetLastError=193)" and never
// reaches EOS at all, whichever Steam backend sits underneath.
//
// Parking that exe aside and dropping this in its place keeps Steam's half of
// the launch exactly as it was - Steam still starts the app it knows about,
// still counts it as running, and still hands the game auth tickets - while the
// game itself comes up without the anti-cheat, so the proxy can do its job.
//
// Which exe to start comes from EasyAntiCheat\Settings.json, the same file the
// bootstrapper itself reads, so this needs no configuration of its own.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define PATHBUF 1024

// How to turn the log categories on, after three routes that do not work and
// one observation that says which one does.
//
// The command line is not the problem. The proxy logs the game's own command
// line from inside the process, so what arrives is measured rather than
// assumed, and -LogCmds arrives whole in both spellings, -LogCmds="a b" and
// "-LogCmds=a b". Neither raises a category. -abslog, on the same allow list
// the executable carries and differing only in having no spaces in its value,
// works every time. So does nothing else: -ini:Engine:[Core.Log]:X=Verbose, one
// spaceless argument per category and global=Verbose alongside, left the log
// smaller than the run before it.
//
// What does work is the config file, and there is one session that proves it.
// Immediately after the last plugin mounts, at the same line of startup where
// every later session goes straight on to LogNFORDenoise, that one prints
//
//   LogHAL: Log category LogRedpointEOSNetworking verbosity has been raised...
//   ... six more, LogNet and LogHandshake and LogNetDriver among them ...
//   LogOnline: Verbose: OSS: [OnlineSubsystemRedpointEOS].bEnabled is not set
//
// and that last line is a category answering at a verbosity it does not have by
// default. The seven named match no list this launcher has ever passed, and two
// of them were never asked for at all, which is what a hand written [Core.Log]
// section looks like. So log suppression in this build is alive and reads its
// config; the section simply does not survive to be read twice.
//
// It does not have to. The game rewrites Saved\Config\WindowsClient\Engine.ini
// whenever its handshake component saves a client id, which drops anything it
// does not recognise, but it reads the file at startup first. Writing the
// section immediately before starting the game puts it there for exactly the
// moment it is read, and the rewrite afterwards costs nothing because the next
// launch writes it again.
//
// global is deliberately not in this list. The raise lines are the signal and
// they are unambiguous on their own, where global=Verbose would bury them.
#define CORE_LOG_SECTION                                                            \
    "[Core.Log]\r\n"                                                                \
    "LogNet=Verbose\r\n"                                                            \
    "LogNetDriver=Verbose\r\n"                                                      \
    "LogHandshake=Verbose\r\n"                                                      \
    "LogGlobalStatus=Verbose\r\n"                                                   \
    "LogMatchmaking=Verbose\r\n"                                                    \
    "LogRedpointEOSNetworking=Verbose\r\n"                                          \
    "LogRedpointEOSAntiCheat=Verbose\r\n"

// Where the engine writes its log. Kept for its own sake now rather than as a
// probe: it puts the engine's account and the proxy's log in one place, and it
// is the one argument here known to survive the allow list. Skipped when the
// path would need quoting, since a quoted argument is what does not survive.
#define ABSLOG_NAME "eos-proxy-engine.log"

static void Fail(const char* what) {
    char msg[PATHBUF + 256];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE,
        "%s\n\n"
        "This is eos-proxy's stand-in for the anti-cheat launcher. The original\n"
        "is next to it as <name>.eosbak - rename it back to undo this, or run\n"
        "eos-proxy-setup.exe --remove-auth on the game folder.", what);
    MessageBoxA(NULL, msg, "EOS Proxy launcher", MB_ICONERROR);
}

// Own folder, without the trailing backslash.
static BOOL SelfDir(char* out, size_t cap) {
    char path[PATHBUF];
    DWORD n = GetModuleFileNameA(NULL, path, sizeof(path));
    if (!n || n >= sizeof(path)) return FALSE;
    char* slash = strrchr(path, '\\');
    if (!slash) return FALSE;
    *slash = 0;
    _snprintf_s(out, cap, _TRUNCATE, "%s", path);
    return TRUE;
}

static void PathJoin(char* out, size_t cap, const char* dir, const char* name) {
    size_t n = strlen(dir);
    if (n && (dir[n - 1] == '\\' || dir[n - 1] == '/'))
        _snprintf_s(out, cap, _TRUNCATE, "%s%s", dir, name);
    else
        _snprintf_s(out, cap, _TRUNCATE, "%s\\%s", dir, name);
}

// The one field this needs out of Settings.json.
static BOOL JsonGetString(const char* text, size_t len, const char* key,
                          char* out, size_t cap) {
    char needle[64];
    _snprintf_s(needle, sizeof(needle), _TRUNCATE, "\"%s\"", key);
    size_t nl = strlen(needle);

    for (size_t i = 0; i + nl < len; i++) {
        if (_strnicmp(text + i, needle, nl) != 0) continue;

        size_t j = i + nl;
        while (j < len && (text[j] == ' ' || text[j] == '\t')) j++;
        if (j >= len || text[j] != ':') continue;
        j++;
        while (j < len && (text[j] == ' ' || text[j] == '\t')) j++;
        if (j >= len || text[j] != '"') continue;
        j++;

        size_t o = 0;
        while (j < len && text[j] != '"' && o + 1 < cap) {
            if (text[j] == '\\' && j + 1 < len) j++;    // \\ and \/ both land as is
            out[o++] = text[j++];
        }
        out[o] = 0;
        return o > 0;
    }
    return FALSE;
}

static BOOL ReadSettingsExe(const char* root, char* out, size_t cap) {
    char cfg[PATHBUF];
    PathJoin(cfg, sizeof(cfg), root, "EasyAntiCheat\\Settings.json");

    HANDLE h = CreateFileA(cfg, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    char buf[1u << 16];
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &got, NULL) && got > 0;
    CloseHandle(h);
    if (!ok) return FALSE;
    buf[got] = 0;

    return JsonGetString(buf, got, "executable", out, cap);
}

// Where the engine keeps the config it writes for itself, which is not in the
// game folder: %LOCALAPPDATA%\<Project>\Saved\Config\<Platform>\Engine.ini. The
// project name is the first segment of the path Settings.json gives for the
// game executable, so this needs no more configuration than the rest of the
// launcher does. The platform folder is whichever one the engine has already
// made - a client build writes WindowsClient, a generic one Windows - and if it
// has made neither there is nothing here worth guessing at.
static BOOL ProjectEngineIni(const char* rel, char* out, size_t cap) {
    char local[PATHBUF], project[PATHBUF], dir[PATHBUF];

    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", local, sizeof(local));
    if (!n || n >= sizeof(local)) return FALSE;

    size_t i = 0;
    while (rel[i] && rel[i] != '\\' && rel[i] != '/' && i + 1 < sizeof(project)) i++;
    if (!i) return FALSE;
    memcpy(project, rel, i);
    project[i] = 0;

    static const char* platforms[] = { "WindowsClient", "Windows" };
    for (int p = 0; p < 2; p++) {
        _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s\\%s\\Saved\\Config\\%s",
                    local, project, platforms[p]);
        DWORD attr = GetFileAttributesA(dir);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            _snprintf_s(out, cap, _TRUNCATE, "%s\\Engine.ini", dir);
            return TRUE;
        }
    }
    return FALSE;
}

// Puts the [Core.Log] section back, keeping everything else the file holds. Any
// section of that name already there is dropped first, because the game does
// not always rewrite the file between runs and two copies of it would be one
// too many. A failure anywhere here is silent on purpose: the log categories
// are a diagnostic, and the game still has to start without them.
static void WriteCoreLog(const char* ini) {
    char buf[1u << 16];
    DWORD got = 0;

    HANDLE h = CreateFileA(ini, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        if (!ReadFile(h, buf, sizeof(buf) - 1, &got, NULL)) got = 0;
        CloseHandle(h);
    }
    buf[got] = 0;

    // Drop an existing [Core.Log], from its header to the next section header.
    char* start = strstr(buf, "[Core.Log]");
    if (start) {
        char* end = start;
        while ((end = strchr(end, '\n')) != NULL) {
            end++;
            if (*end == '[') break;
        }
        if (end) memmove(start, end, strlen(end) + 1);
        else *start = 0;
        got = (DWORD) strlen(buf);
    }

    // Keep the section on a line of its own whatever the file ended with.
    const char* lead = (got && buf[got - 1] != '\n') ? "\r\n" : "";

    h = CreateFileA(ini, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    DWORD written;
    if (got) WriteFile(h, buf, got, &written, NULL);
    if (*lead) WriteFile(h, lead, (DWORD) strlen(lead), &written, NULL);
    WriteFile(h, CORE_LOG_SECTION, (DWORD) strlen(CORE_LOG_SECTION), &written, NULL);
    CloseHandle(h);
}

// The control probe of the run: an allow-listed argument that needs no quoting,
// so that a run which produces this file has proved the command line is read.
// %LOCALAPPDATA% is where the engine already keeps its own logs, and it is the
// one path here that is normally free of spaces. When it is not, the probe is
// left out rather than quoted, because a quoted control answers a different
// question than the one being asked.
static BOOL AbsLogArg(char* out, size_t cap) {
    char local[PATHBUF];
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", local, sizeof(local));
    if (!n || n >= sizeof(local)) return FALSE;
    if (strchr(local, ' ')) return FALSE;
    _snprintf_s(out, cap, _TRUNCATE, "-abslog=%s\\" ABSLOG_NAME, local);
    return TRUE;
}

// Everything after our own program name, so launch options set in Steam still
// reach the game.
static const char* OwnArgs(void) {
    const char* cmd = GetCommandLineA();
    if (*cmd == '"') {
        cmd++;
        while (*cmd && *cmd != '"') cmd++;
        if (*cmd == '"') cmd++;
    } else {
        while (*cmd && *cmd != ' ' && *cmd != '\t') cmd++;
    }
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    return cmd;
}

int main(void) {
    char root[PATHBUF], rel[PATHBUF], exe[PATHBUF], dir[PATHBUF];

    if (!SelfDir(root, sizeof(root))) {
        Fail("Could not work out which folder this is running from.");
        return 1;
    }
    if (!ReadSettingsExe(root, rel, sizeof(rel))) {
        Fail("Could not read the game executable out of\n"
             "EasyAntiCheat\\Settings.json.");
        return 1;
    }

    PathJoin(exe, sizeof(exe), root, rel);
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
        char msg[PATHBUF + 64];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE, "The game executable is not there:\n%s", exe);
        Fail(msg);
        return 1;
    }

    // The game is started from its own folder, the way its launcher does it.
    _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s", exe);
    char* slash = strrchr(dir, '\\');
    if (slash) *slash = 0;

    // Before the game starts, so it is there for the one moment it is read.
    char ini[PATHBUF];
    if (ProjectEngineIni(rel, ini, sizeof(ini))) WriteCoreLog(ini);

    char abslog[PATHBUF];
    if (!AbsLogArg(abslog, sizeof(abslog))) abslog[0] = 0;

    char cmd[PATHBUF * 3];
    const char* args = OwnArgs();
    _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "\"%s\"%s%s%s%s",
                exe, *abslog ? " " : "", abslog, *args ? " " : "", args);

    STARTUPINFOA si = { 0 };
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = { 0 };

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        char msg[PATHBUF + 64];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "Could not start the game (error %lu):\n%s", GetLastError(), exe);
        Fail(msg);
        return 1;
    }

    // Waiting matters: Steam watches this process to decide whether the game is
    // still running, and stops handing out auth tickets once it exits.
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}
