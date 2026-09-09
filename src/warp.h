#pragma once

#include "common.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>

// The Isle's matchmaker, api.warphosting.com.au.
//
// EOS gets the game logged in and fills the server browser, but The Isle also
// talks to a matchmaker of its own, and that is a separate wall. Most calls
// there carry a Steam ticket the matchmaker hands to Valve for validation, and
// no emulator can produce a ticket Valve will sign off on:
//
//   POST /v1/servers/status         {"ticketHex", "gameId"}
//     200 {"status": "ONLINE" | "OFFLINE" | "UPDATING" | ...}
//     401 {"detail": "Steam ticket invalid"}     <- the red line in the menu
//   POST /v1/match/request          {"steam_ticket", "ticket_hex", "server_id", ...}
//     202 {"session_id", "connection", "presence*", ...}
//     401 {"detail": "Steam ticket invalid"}     <- what Connect resolves through
//   POST /v1/servers/queue/join     {"serverId"}
//     200 {"ipAddress", "queuePort", "serverStatus"}   no ticket at all
//
// Every one of those URLs is a plain ASCII literal in the game executable, so
// this rewrites them in memory to a loopback address the proxy listens on and
// relays each request to the real matchmaker with its method, path, headers
// and body intact. The answer is passed straight back, so by default nothing
// about the game's behaviour changes - what this buys is a line in
// epic_proxy.log for every call, which is the only way to see what the client
// asks for and what it is told.
//
// Two calls are handled rather than relayed:
//
//   /v1/servers/status  a refused ticket is reported as ONLINE, so the menu
//                       stops printing the official network as offline. A real
//                       answer, including a real MAINTENANCE or UPDATING
//                       state, is passed through untouched.
//
// The official servers themselves stay out of reach: listing them, matchmaking
// into one and queueing all validate the same ticket, and there is no answer
// to those that can be invented locally.
//
// Set EOS_PROXY_NO_WARP=1 in the environment to leave the game's own requests
// alone.

#define WARP_HOST L"api.warphosting.com.au"

// Answer for a client whose ticket the matchmaker would not accept. Both
// fields of ServerStatusResponse are optional bar the status itself, and a
// version we would have to invent is worse than none.
#define WARP_ONLINE_PATH "/v1/servers/status"
#define WARP_ONLINE_BODY "{\"status\":\"ONLINE\"}"

#define WARP_REQ_MAX  (256u * 1024u)
#define WARP_RESP_MAX (4u * 1024u * 1024u)
#define WARP_LOG_BODY 400              // how much of an answer reaches the log

// Every matchmaker URL the client itself calls. The runtime/update and
// presence/server endpoints are in the executable too but only a dedicated
// server ever reaches them, so they are left out.
static const char *g_WarpUrls[] = {
    "https://api.warphosting.com.au/v1/servers/status",
    "https://api.warphosting.com.au/v1/servers/active",
    "https://api.warphosting.com.au/v1/match/request",
    "https://api.warphosting.com.au/v1/servers/password/check",
    "https://api.warphosting.com.au/v1/servers/queue/join",
    "https://api.warphosting.com.au/v1/presence/client",
};

// ------------------------------------------------------------ image patching

// Rewrites every copy of Find in the game executable's own image with Replace,
// which has to be the shorter of the two. Only the replacement and its
// terminator are written: the bytes after it belong to the next literal in the
// pool and are left as they are. Returns how many copies were rewritten.
static int Warp_PatchString(const char *find, const char *replace) {
    size_t findLen = strlen(find);
    size_t replLen = strlen(replace);
    if (replLen > findLen) return 0;

    HMODULE hExe = GetModuleHandleA(NULL);
    if (!hExe) return 0;

    BYTE *base = (BYTE *) hExe;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *) base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *) (base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    int patched = 0;

    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        // Initialised, readable, not code: where a string literal lands.
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_READ)) continue;
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        if (sec[i].Characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA) continue;

        DWORD size = sec[i].Misc.VirtualSize;
        if (size == 0 || size < findLen) continue;

        BYTE *start = base + sec[i].VirtualAddress;
        BYTE *end   = start + size - findLen;

        __try {
            for (BYTE *p = start; p <= end; p++) {
                if (*p != (BYTE) find[0]) continue;
                if (memcmp(p, find, findLen) != 0) continue;

                DWORD old;
                if (!VirtualProtect(p, replLen + 1, PAGE_READWRITE, &old)) {
                    LogText("Warp: VirtualProtect failed at 0x%p (error %lu)",
                            p, GetLastError());
                    continue;
                }
                memcpy(p, replace, replLen);
                p[replLen] = '\0';
                VirtualProtect(p, replLen + 1, old, &old);

                patched++;
                p += findLen - 1;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // A section that is mapped but not actually readable. Skip it and
            // keep going: the literals live in one of the others.
            LogText("Warp: skipped unreadable section %.8s", sec[i].Name);
        }
    }

    return patched;
}

// ------------------------------------------------------------------ upstream

// Case insensitive search, for header names off the wire.
static const char *Warp_Find(const char *hay, int hayLen, const char *needle) {
    size_t n = strlen(needle);
    if ((int) n > hayLen) return NULL;
    for (int i = 0; i <= hayLen - (int) n; i++)
        if (_strnicmp(hay + i, needle, n) == 0) return hay + i;
    return NULL;
}

static int Warp_StartsWith(const char *line, const char *name) {
    return _strnicmp(line, name, strlen(name)) == 0;
}

// Headers WinHTTP owns, plus the hop by hop ones. Everything else the client
// sent is forwarded verbatim, which matters: the ticket and the presence
// signature travel in headers on some of these calls.
static int Warp_DropHeader(const char *line) {
    return Warp_StartsWith(line, "Host:")
        || Warp_StartsWith(line, "Content-Length:")
        || Warp_StartsWith(line, "Connection:")
        || Warp_StartsWith(line, "Proxy-Connection:")
        || Warp_StartsWith(line, "Expect:")
        || Warp_StartsWith(line, "Accept-Encoding:")   // keep the answer plain
        || Warp_StartsWith(line, "Transfer-Encoding:");
}

typedef struct {
    wchar_t method[16];
    wchar_t path[512];
    wchar_t headers[8192];
} WarpRequestHead;

// Splits the client's request line and headers into what WinHTTP needs.
static int Warp_ParseHead(const char *req, int headLen, WarpRequestHead *out) {
    const char *sp1 = memchr(req, ' ', headLen);
    if (!sp1) return 0;
    const char *sp2 = memchr(sp1 + 1, ' ', headLen - (int) (sp1 + 1 - req));
    if (!sp2) return 0;

    char method[16], path[512];
    int methodLen = (int) (sp1 - req);
    int pathLen   = (int) (sp2 - sp1 - 1);
    if (methodLen <= 0 || methodLen >= (int) sizeof(method)) return 0;
    if (pathLen   <= 0 || pathLen   >= (int) sizeof(path))   return 0;
    memcpy(method, req, methodLen); method[methodLen] = '\0';
    memcpy(path, sp1 + 1, pathLen); path[pathLen] = '\0';

    MultiByteToWideChar(CP_UTF8, 0, method, -1, out->method, 16);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, out->path, 512);

    // Rebuild the header block minus the ones WinHTTP sets itself.
    char keep[4096];
    int  keptLen = 0;
    const char *line = memchr(req, '\n', headLen);
    while (line) {
        line++;
        const char *nl = memchr(line, '\n', headLen - (int) (line - req));
        if (!nl) break;
        int len = (int) (nl - line);
        if (len > 0 && line[len - 1] == '\r') len--;
        if (len == 0) break;                        // end of the header block

        if (!Warp_DropHeader(line) && keptLen + len + 3 < (int) sizeof(keep)) {
            memcpy(keep + keptLen, line, len);
            keptLen += len;
            keep[keptLen++] = '\r';
            keep[keptLen++] = '\n';
        }
        line = nl;
    }
    keep[keptLen] = '\0';

    MultiByteToWideChar(CP_UTF8, 0, keep, -1, out->headers, 8192);
    return 1;
}

// Replays the client's request against the real matchmaker over HTTPS.
// Returns the HTTP status it answered with, or 0 if it could not be reached.
static int Warp_Forward(const WarpRequestHead *head, const char *body, int bodyLen,
                        char *out, int outCap, int *outLen) {
    HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;
    int status = 0;

    *outLen = 0;

    // No agent of our own: the client sends its own User-Agent and it is
    // forwarded with the rest of the headers.
    hSession = WinHttpOpen(NULL, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) goto done;

    WinHttpSetTimeouts(hSession, 5000, 5000, 10000, 15000);

    hConnect = WinHttpConnect(hSession, WARP_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) goto done;

    hRequest = WinHttpOpenRequest(hConnect, head->method, head->path,
                                  NULL, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  WINHTTP_FLAG_SECURE);
    if (!hRequest) goto done;

    const wchar_t *extra = head->headers[0] ? head->headers : WINHTTP_NO_ADDITIONAL_HEADERS;
    DWORD extraLen = head->headers[0] ? (DWORD) -1 : 0;

    if (!WinHttpSendRequest(hRequest, extra, extraLen,
                            (LPVOID) body, (DWORD) bodyLen, (DWORD) bodyLen, 0))
        goto done;

    if (!WinHttpReceiveResponse(hRequest, NULL)) goto done;

    DWORD code = 0, codeSize = sizeof(code);
    if (!WinHttpQueryHeaders(hRequest,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &code, &codeSize,
                             WINHTTP_NO_HEADER_INDEX))
        goto done;
    status = (int) code;

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
        if (*outLen + (int) avail > outCap) {
            LogText("Warp: answer larger than %d bytes, truncating", outCap);
            avail = (DWORD) (outCap - *outLen);
        }
        if (avail == 0) break;

        DWORD read = 0;
        if (!WinHttpReadData(hRequest, out + *outLen, avail, &read) || read == 0) break;
        *outLen += (int) read;
    }

done:
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);
    return status;
}

// -------------------------------------------------------------- the listener

static void Warp_Send(SOCKET s, const char *data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(s, data + sent, len - sent, 0);
        if (n <= 0) return;
        sent += n;
    }
}

static void Warp_SendBody(SOCKET s, int status, const char *body, int bodyLen) {
    const char *reason = status == 200 ? "OK"
                       : status == 202 ? "Accepted"
                       : status == 401 ? "Unauthorized"
                       : status == 502 ? "Bad Gateway"
                                       : "Error";
    char head[256];
    int headLen = _snprintf_s(head, sizeof(head), _TRUNCATE,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n", status, reason, bodyLen);

    Warp_Send(s, head, headLen);
    Warp_Send(s, body, bodyLen);
}

// Answers a request one line at a time in epic_proxy.log. Response bodies are
// logged, request bodies are not: those carry the Steam ticket.
static void Warp_Serve(SOCKET client) {
    char *req = (char *) malloc(WARP_REQ_MAX);
    char *resp = (char *) malloc(WARP_RESP_MAX);
    WarpRequestHead *head = (WarpRequestHead *) malloc(sizeof(WarpRequestHead));
    if (!req || !resp || !head) goto cleanup;

    int have = 0, headerEnd = -1;

    // Headers first, then whatever Content-Length asks for. libcurl puts a
    // request of any size behind an Expect: 100-continue, so the go-ahead has
    // to be sent before the body ever arrives.
    while (have < (int) WARP_REQ_MAX - 1) {
        int n = recv(client, req + have, (int) WARP_REQ_MAX - 1 - have, 0);
        if (n <= 0) break;
        have += n;
        req[have] = '\0';

        const char *end = strstr(req, "\r\n\r\n");
        if (end) { headerEnd = (int) (end - req) + 4; break; }
    }
    if (headerEnd < 0) goto cleanup;

    if (Warp_Find(req, headerEnd, "Expect: 100-continue")) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        Warp_Send(client, cont, (int) sizeof(cont) - 1);
    }

    int contentLen = 0;
    const char *cl = Warp_Find(req, headerEnd, "Content-Length:");
    if (cl) contentLen = atoi(cl + 15);
    if (contentLen < 0 || contentLen > (int) WARP_REQ_MAX - headerEnd - 1)
        contentLen = 0;

    while (have - headerEnd < contentLen) {
        int n = recv(client, req + have, contentLen - (have - headerEnd), 0);
        if (n <= 0) break;
        have += n;
    }

    int bodyLen = have - headerEnd;
    if (bodyLen < 0) bodyLen = 0;

    if (!Warp_ParseHead(req, headerEnd, head)) {
        LogText("Warp: could not parse the request line, dropping it");
        goto cleanup;
    }

    int respLen = 0;
    int status = Warp_Forward(head, req + headerEnd, bodyLen, resp, WARP_RESP_MAX, &respLen);

    // %.*S prints a wide string through a narrow format. The answer is logged
    // short: a server list runs to hundreds of kilobytes.
    LogText("Warp: %.*S %.*S -> %d, %d bytes: %.*s%s",
            16, head->method, 120, head->path, status, respLen,
            respLen < WARP_LOG_BODY ? respLen : WARP_LOG_BODY, resp,
            respLen > WARP_LOG_BODY ? " ..." : "");

    int isStatusCall = wcsstr(head->path, L"/v1/servers/status") != NULL;

    if (status == 0) {
        LogText("Warp: matchmaker could not be reached");
        Warp_SendBody(client, 502, "{\"detail\":\"upstream unreachable\"}", 33);
    } else if (isStatusCall && status != 200) {
        LogText("Warp: status call refused, reporting ONLINE instead");
        Warp_SendBody(client, 200, WARP_ONLINE_BODY, (int) strlen(WARP_ONLINE_BODY));
    } else {
        Warp_SendBody(client, status, resp, respLen);
    }

cleanup:
    free(req);
    free(resp);
    free(head);
    shutdown(client, SD_BOTH);
    closesocket(client);
}

static DWORD WINAPI Warp_ClientThread(LPVOID param) {
    Warp_Serve((SOCKET) (UINT_PTR) param);
    return 0;
}

static DWORD WINAPI Warp_Thread(LPVOID param) {
    (void) param;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LogText("Warp: WSAStartup failed (error %d)", WSAGetLastError());
        return 0;
    }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        LogText("Warp: socket failed (error %d)", WSAGetLastError());
        return 0;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = 0;                          // any free port
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(listener, (struct sockaddr *) &addr, sizeof(addr)) == SOCKET_ERROR ||
        listen(listener, 16) == SOCKET_ERROR) {
        LogText("Warp: bind/listen failed (error %d)", WSAGetLastError());
        closesocket(listener);
        return 0;
    }

    int addrLen = sizeof(addr);
    if (getsockname(listener, (struct sockaddr *) &addr, &addrLen) == SOCKET_ERROR) {
        LogText("Warp: getsockname failed (error %d)", WSAGetLastError());
        closesocket(listener);
        return 0;
    }
    unsigned port = ntohs(addr.sin_port);

    // Every replacement is shorter than the literal it overwrites, because the
    // loopback host is shorter than api.warphosting.com.au. That is what makes
    // the rewrite possible at all.
    int total = 0;
    for (int i = 0; i < (int) (sizeof(g_WarpUrls) / sizeof(g_WarpUrls[0])); i++) {
        const char *url = g_WarpUrls[i];
        const char *slash = strstr(url, "/v1/");
        if (!slash) continue;

        // Copied out before the patch runs. Only the game executable is
        // scanned, so this array is never a target, but reading a literal back
        // after a rewrite that could have touched it is a trap worth closing.
        char path[128], replacement[160];
        strncpy_s(path, sizeof(path), slash, _TRUNCATE);
        _snprintf_s(replacement, sizeof(replacement), _TRUNCATE,
                    "http://127.0.0.1:%u%s", port, path);

        int n = Warp_PatchString(url, replacement);
        LogText("Warp: %s -> %s (%d copies)", path, n ? replacement : "not found", n);
        total += n;
    }

    if (total == 0) {
        LogText("Warp: no matchmaker URL found in the game image, leaving it alone. "
                "The official network status line will read Offline.");
        closesocket(listener);
        return 0;
    }
    LogText("Warp: matchmaker relay listening on 127.0.0.1:%u (%d URLs rewritten)",
            port, total);

    for (;;) {
        SOCKET client = accept(listener, NULL, NULL);
        if (client == INVALID_SOCKET) break;

        // One thread per call: the game fires several of these at once and a
        // slow upstream must not hold the others up.
        HANDLE h = CreateThread(NULL, 0, Warp_ClientThread,
                                (LPVOID) (UINT_PTR) client, 0, NULL);
        if (h) CloseHandle(h);
        else   Warp_Serve(client);
    }

    closesocket(listener);
    return 0;
}

// Called from DllMain, so it does no more than start a thread: the socket and
// WinHTTP work all happens off the loader lock.
static void Warp_Start(void) {
    char off[8];
    if (GetEnvironmentVariableA("EOS_PROXY_NO_WARP", off, sizeof(off)) > 0) {
        LogText("Warp: EOS_PROXY_NO_WARP is set, leaving the matchmaker alone");
        return;
    }

    HANDLE h = CreateThread(NULL, 0, Warp_Thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else LogText("Warp: CreateThread failed (error %lu)", GetLastError());
}
