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

// What to ask the game for, and the form it will actually accept.
//
// Two tests to rule out the obvious route. The proxy logs the game's own
// command line from inside the process, so there is no guessing left about
// what arrives: the whole -LogCmds argument does, every category, quotes and
// all, in both the -LogCmds="a b" form and the "-LogCmds=a b" one. Nothing
// answers it. No "Log category ... has been raised" from the engine, and not a
// line from LogNet, LogNetDriver, LogHandshake, LogRedpointEOSNetworking or
// LogRedpointEOSAntiCheat, through joins that ran their full 73 seconds.
//
// That is not the command line being ignored, and -abslog is the argument that
// proves it. The game's executable carries an allow list of what a shipping
// build accepts,
//
//   -game -log -unattended -nosplash -RenderOffscreen -d3d12 -nullrhi -NoSound
//   -NoLoadingScreen -TILoadTest ... -LogCmds -abslog
//
// which is The Isle's own list, their load-test switches included. -abslog is
// on it and its value has no spaces, and it works: the engine writes its whole
// log to the path named below. So the command line is read, allow-listed
// arguments are honoured, and -LogCmds alone is lost. The space is the only
// thing that separates the two, and -LogCmds cannot be written without one.
//
// The same help text in the executable names the way around it, because the
// config form of the setting needs no spaces at all:
//
//   [Core.Log]
//   [cat]=[level]        foo=verbose
//
// which -ini: sets from the command line, one spaceless argument per category.
// The allow list already carries three -ini:Engine:[...] arguments of The
// Isle's own, so the form is one this build expects to see.
//
// -LogCmds is gone from here on purpose rather than kept as a spare. It is
// inert twice over, and leaving it in would make a run that finally works
// unattributable. global=Verbose rides along as the control: if the -ini: route
// reaches log suppression at all, that alone makes the log enormous, which is
// an answer even if every category below turns out to be compiled out.
#define LOG_INI  " -ini:Engine:[Core.Log]:global=Verbose"                           \
                 " -ini:Engine:[Core.Log]:LogNet=Verbose"                           \
                 " -ini:Engine:[Core.Log]:LogNetDriver=Verbose"                     \
                 " -ini:Engine:[Core.Log]:LogHandshake=Verbose"                     \
                 " -ini:Engine:[Core.Log]:LogGlobalStatus=Verbose"                  \
                 " -ini:Engine:[Core.Log]:LogMatchmaking=Verbose"                   \
                 " -ini:Engine:[Core.Log]:LogRedpointEOSNetworking=Verbose"         \
                 " -ini:Engine:[Core.Log]:LogRedpointEOSAntiCheat=Verbose"

// Where the engine writes its log. Worth keeping now that it is proven to work:
// it puts the engine's account and the proxy's own log in step, and it is the
// one argument here known to survive the allow list. Skipped when the path
// would need quoting, since a quoted argument is what does not survive.
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

    char abslog[PATHBUF];
    if (!AbsLogArg(abslog, sizeof(abslog))) abslog[0] = 0;

    char cmd[PATHBUF * 3];
    const char* args = OwnArgs();
    _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "\"%s\"" LOG_INI "%s%s%s%s",
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
