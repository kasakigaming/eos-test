#pragma once

#include "common.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>

// The Isle's "Official Network Status" line.
//
// The client asks The Isle's own matchmaker, api.warphosting.com.au, for the
// state of the official server network, and every one of those calls carries a
// Steam ticket that the matchmaker hands to Valve for validation:
//
//   POST /v1/servers/status   {"ticketHex": "<hex>", "gameId": "the-isle"}
//     200 {"status": "ONLINE" | "OFFLINE" | "UPDATING" | ...}
//     401 {"detail": "Steam ticket invalid"}
//
// A Steam emulator cannot produce a ticket Valve will sign off on, so that call
// comes back 401 and the menu prints "Official Network Status: Offline" in red.
// The community browser is unaffected: /v1/servers/community answers without a
// ticket at all, which is why the server list fills in while the status line
// says the network is down.
//
// This turns the line back to Online. The status URL is a plain ASCII literal
// in the game executable, so it is rewritten in memory to a loopback address
// this DLL listens on. Requests are forwarded to the real matchmaker
// unchanged; its answer is passed straight through whenever it gives one, and
// only an authentication refusal is replaced with ONLINE. Nothing else is
// touched, so a real ticket - and any real MAINTENANCE or UPDATING state -
// still wins.
//
// What this does not do: the official servers themselves stay out of reach.
// Listing them (/v1/servers/active), matchmaking into one (/v1/match/request)
// and queueing (/v1/servers/queue/join) all validate the same ticket, and
// there is no answer to those that can be invented locally. This is the status
// line and nothing more.
//
// Set EOS_PROXY_NO_WARP=1 in the environment to leave the game's own request
// alone.

#define WARP_HOST        L"api.warphosting.com.au"
#define WARP_STATUS_PATH L"/v1/servers/status"
#define WARP_STATUS_URL  "https://api.warphosting.com.au/v1/servers/status"

// Answer for a client whose ticket the matchmaker would not accept. Both
// fields of ServerStatusResponse are optional bar the status itself, and a
// version we would have to invent is worse than none.
#define WARP_ONLINE_BODY "{\"status\":\"ONLINE\"}"

#define WARP_REQ_MAX  (64u * 1024u)
#define WARP_RESP_MAX (64u * 1024u)

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

                LogText("Warp: rewrote status URL at 0x%p -> %s", p, replace);
                patched++;
                p += findLen - 1;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // A section that is mapped but not actually readable. Skip it and
            // keep going: the literal lives in one of the others.
            LogText("Warp: skipped unreadable section %.8s", sec[i].Name);
        }
    }

    return patched;
}

// ------------------------------------------------------------------ upstream

// Replays the game's own request against the real matchmaker over HTTPS.
// Returns the HTTP status it answered with, or 0 if it could not be reached.
static int Warp_Forward(const char *body, int bodyLen, char *out, int outCap, int *outLen) {
    HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;
    int status = 0;

    *outLen = 0;

    hSession = WinHttpOpen(L"TheIsleClient/1.0",
                           WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) goto done;

    WinHttpSetTimeouts(hSession, 5000, 5000, 8000, 8000);

    hConnect = WinHttpConnect(hSession, WARP_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) goto done;

    hRequest = WinHttpOpenRequest(hConnect, L"POST", WARP_STATUS_PATH,
                                  NULL, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  WINHTTP_FLAG_SECURE);
    if (!hRequest) goto done;

    if (!WinHttpSendRequest(hRequest,
                            L"Content-Type: application/json\r\n", (DWORD) -1,
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
        if (*outLen + (int) avail > outCap) avail = (DWORD) (outCap - *outLen);
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

static void Warp_SendJson(SOCKET s, int status, const char *body) {
    char head[256];
    int headLen = _snprintf_s(head, sizeof(head), _TRUNCATE,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status == 200 ? "OK" : "Bad Gateway", (int) strlen(body));

    Warp_Send(s, head, headLen);
    Warp_Send(s, body, (int) strlen(body));
}

// Case insensitive search, for header names off the wire.
static const char *Warp_FindHeader(const char *hay, int hayLen, const char *needle) {
    size_t n = strlen(needle);
    if ((int) n > hayLen) return NULL;
    for (int i = 0; i <= hayLen - (int) n; i++)
        if (_strnicmp(hay + i, needle, n) == 0) return hay + i;
    return NULL;
}

static void Warp_Serve(SOCKET client) {
    char *req = (char *) malloc(WARP_REQ_MAX);
    char *resp = (char *) malloc(WARP_RESP_MAX);
    if (!req || !resp) goto cleanup;

    int have = 0, headerEnd = -1;

    // Headers first, then whatever Content-Length asks for. libcurl puts a
    // request this size behind an Expect: 100-continue, so the go-ahead has to
    // be sent before the body ever arrives.
    while (have < (int) WARP_REQ_MAX - 1) {
        int n = recv(client, req + have, (int) WARP_REQ_MAX - 1 - have, 0);
        if (n <= 0) break;
        have += n;
        req[have] = '\0';

        const char *end = strstr(req, "\r\n\r\n");
        if (end) { headerEnd = (int) (end - req) + 4; break; }
    }
    if (headerEnd < 0) goto cleanup;

    if (Warp_FindHeader(req, headerEnd, "Expect: 100-continue")) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        Warp_Send(client, cont, (int) sizeof(cont) - 1);
    }

    int contentLen = 0;
    const char *cl = Warp_FindHeader(req, headerEnd, "Content-Length:");
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

    int respLen = 0;
    int status = Warp_Forward(req + headerEnd, bodyLen, resp, WARP_RESP_MAX, &respLen);

    if (status == 200) {
        // The matchmaker answered properly. Whatever it says is the truth.
        char head[256];
        int headLen = _snprintf_s(head, sizeof(head), _TRUNCATE,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n", respLen);
        Warp_Send(client, head, headLen);
        Warp_Send(client, resp, respLen);
        LogText("Warp: matchmaker answered 200, passed through (%d bytes)", respLen);
    } else {
        LogText("Warp: matchmaker answered %d, reporting ONLINE instead", status);
        Warp_SendJson(client, 200, WARP_ONLINE_BODY);
    }

cleanup:
    free(req);
    free(resp);
    shutdown(client, SD_BOTH);
    closesocket(client);
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
        listen(listener, 4) == SOCKET_ERROR) {
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

    // Shorter than the https:// literal it replaces, which is what makes the
    // rewrite possible at all.
    char replacement[64];
    _snprintf_s(replacement, sizeof(replacement), _TRUNCATE,
                "http://127.0.0.1:%u/v1/servers/status", port);

    int patched = Warp_PatchString(WARP_STATUS_URL, replacement);
    if (patched == 0) {
        LogText("Warp: status URL not found in the game image, leaving it alone. "
                "The official network status line will read Offline.");
        closesocket(listener);
        return 0;
    }
    LogText("Warp: official network status shim listening on 127.0.0.1:%u (%d URL copies)",
            port, patched);

    for (;;) {
        SOCKET client = accept(listener, NULL, NULL);
        if (client == INVALID_SOCKET) break;
        Warp_Serve(client);
    }

    closesocket(listener);
    return 0;
}

// Called from DllMain, so it does no more than start a thread: the socket and
// WinHTTP work all happens off the loader lock.
static void Warp_Start(void) {
    char off[8];
    if (GetEnvironmentVariableA("EOS_PROXY_NO_WARP", off, sizeof(off)) > 0) {
        LogText("Warp: EOS_PROXY_NO_WARP is set, leaving the status request alone");
        return;
    }

    HANDLE h = CreateThread(NULL, 0, Warp_Thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else LogText("Warp: CreateThread failed (error %lu)", GetLastError());
}
