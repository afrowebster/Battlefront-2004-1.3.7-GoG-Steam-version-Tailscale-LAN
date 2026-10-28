// Battlefront (2004) Tailscale LAN proxy
// Copyright (C) 2026 afrowebster
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version. It is distributed WITHOUT ANY WARRANTY; see the LICENSE
// file or <https://www.gnu.org/licenses/>.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <dinput.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

using DirectInput8CreateFn = HRESULT(WINAPI *)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
using ConnectFn = int(WSAAPI *)(SOCKET, const sockaddr *, int);
using RecvFromFn = int(WSAAPI *)(SOCKET, char *, int, int, sockaddr *, int *);
using SendFn = int(WSAAPI *)(SOCKET, const char *, int, int);
using SendToFn = int(WSAAPI *)(SOCKET, const char *, int, int, const sockaddr *, int);

enum class ProxyMode {
    None,
    Client,
    Server
};

static HMODULE gSystemDinput8;
static DirectInput8CreateFn gDirectInput8Create;
static ConnectFn gConnect;
static RecvFromFn gRecvFrom;
static SendFn gSend;
static SendToFn gSendTo;
static ProxyMode gMode = ProxyMode::None;
static in_addr gServerAddress;
static SOCKET gDiscoverySocket = INVALID_SOCKET;
static CRITICAL_SECTION gPeerLock;
static sockaddr_in gPeers[32];
static DWORD gPeerSeen[32];
static volatile LONG gInitialized;

static CRITICAL_SECTION gLogLock;
static HANDLE gLogFile = INVALID_HANDLE_VALUE;
static LONG gLogBytes;
static const LONG kLogLimit = 4 * 1024 * 1024;

static void Log(const char *format, ...) {
    if (gLogFile == INVALID_HANDLE_VALUE) {
        return;
    }

    char line[512];
    SYSTEMTIME time;
    GetLocalTime(&time);
    int length = std::snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u ", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);

    va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(line + length, sizeof(line) - length - 2, format, arguments);
    va_end(arguments);
    if (written > 0) {
        length += (written < static_cast<int>(sizeof(line)) - length - 2) ? written : static_cast<int>(sizeof(line)) - length - 3;
    }
    line[length++] = '\r';
    line[length++] = '\n';

    EnterCriticalSection(&gLogLock);
    if (gLogBytes < kLogLimit) {
        DWORD ignored;
        WriteFile(gLogFile, line, length, &ignored, nullptr);
        gLogBytes += length;
    }
    LeaveCriticalSection(&gLogLock);
}

static const char *FormatAddress(const sockaddr *address, int length, char *output, size_t capacity) {
    if (address == nullptr || length < static_cast<int>(sizeof(sockaddr_in)) || address->sa_family != AF_INET) {
        std::snprintf(output, capacity, "<non-ipv4>");
        return output;
    }

    const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(address);
    const auto *bytes = reinterpret_cast<const unsigned char *>(&ipv4->sin_addr.s_addr);
    std::snprintf(output, capacity, "%u.%u.%u.%u:%u", bytes[0], bytes[1], bytes[2], bytes[3], ntohs(ipv4->sin_port));
    return output;
}

static bool IsDiscoveryAddress(const sockaddr *address, int length) {
    if (address == nullptr || length < static_cast<int>(sizeof(sockaddr_in)) || address->sa_family != AF_INET) {
        return false;
    }

    const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(address);
    return ipv4->sin_port == htons(3657) && ipv4->sin_addr.s_addr == INADDR_BROADCAST;
}

// Probe layout: 00 ff ff 00 00 | 4-byte per-search nonce | 04 10 00 00.
static bool IsDiscoveryProbe(const char *buffer, int length) {
    static const unsigned char head[] = { 0x00, 0xff, 0xff, 0x00, 0x00 };
    static const unsigned char tail[] = { 0x04, 0x10, 0x00, 0x00 };
    return length == 13 && std::memcmp(buffer, head, sizeof(head)) == 0 && std::memcmp(buffer + 9, tail, sizeof(tail)) == 0;
}

static bool IsTailscaleAddress(const sockaddr *address) {
    if (address == nullptr || address->sa_family != AF_INET) {
        return false;
    }
    const unsigned long host = ntohl(reinterpret_cast<const sockaddr_in *>(address)->sin_addr.s_addr);
    return (host & 0xffc00000UL) == 0x64400000UL;
}

// The game asserts (int3 in FUN_005AEAF0) that every peer address is RFC 1918,
// so Tailscale's 100.64.0.0/10 is presented to it as 10.64.0.0/10 and mapped
// back on the way out: 100.A.B.C <-> 10.A.B.C.
static bool IsMappedAddress(unsigned long host) {
    return (host & 0xffc00000UL) == 0x0a400000UL;
}

static void ToGameAddress(sockaddr *address) {
    if (!IsTailscaleAddress(address)) {
        return;
    }
    auto *ipv4 = reinterpret_cast<sockaddr_in *>(address);
    ipv4->sin_addr.s_addr = htonl((ntohl(ipv4->sin_addr.s_addr) & 0x00ffffffUL) | 0x0a000000UL);
}

// Returns true and fills output when the destination is a mapped address.
static bool ToWireAddress(const sockaddr *address, int length, sockaddr_in &output) {
    if (address == nullptr || length < static_cast<int>(sizeof(sockaddr_in)) || address->sa_family != AF_INET) {
        return false;
    }
    output = *reinterpret_cast<const sockaddr_in *>(address);
    const unsigned long host = ntohl(output.sin_addr.s_addr);
    if (!IsMappedAddress(host)) {
        return false;
    }
    output.sin_addr.s_addr = htonl((host & 0x00ffffffUL) | 0x64000000UL);
    return true;
}

static void FormatHex(const char *buffer, int length, char *output, size_t capacity) {
    size_t used = 0;
    output[0] = '\0';
    for (int index = 0; index < length && index < 16 && used + 4 < capacity; ++index) {
        used += std::snprintf(output + used, capacity - used, "%02x ", static_cast<unsigned char>(buffer[index]));
    }
}

static bool IsDiscoveryReply(const char *buffer, int length) {
    return length > 13 && static_cast<unsigned char>(buffer[0]) == 0x01 && static_cast<unsigned char>(buffer[1]) == 0xff && static_cast<unsigned char>(buffer[2]) == 0xff;
}

static void RecordPeer(const sockaddr_in &source) {
    sockaddr_in peer = source;
    peer.sin_port = htons(3657);
    const DWORD now = GetTickCount();

    EnterCriticalSection(&gPeerLock);
    int slot = -1;
    int oldest = 0;
    for (int index = 0; index < 32; ++index) {
        if (gPeerSeen[index] != 0 && gPeers[index].sin_addr.s_addr == peer.sin_addr.s_addr) {
            slot = index;
            break;
        }
        if (gPeerSeen[index] == 0) {
            slot = index;
            break;
        }
        if (gPeerSeen[index] < gPeerSeen[oldest]) {
            oldest = index;
        }
    }

    if (slot < 0) {
        slot = oldest;
    }

    gPeers[slot] = peer;
    gPeerSeen[slot] = now;
    LeaveCriticalSection(&gPeerLock);
}

static int SnapshotPeers(sockaddr_in *peers, int capacity) {
    const DWORD now = GetTickCount();
    int count = 0;

    EnterCriticalSection(&gPeerLock);
    for (int index = 0; index < 32 && count < capacity; ++index) {
        if (gPeerSeen[index] == 0 || now - gPeerSeen[index] > 15000) {
            gPeerSeen[index] = 0;
            continue;
        }
        peers[count++] = gPeers[index];
    }
    LeaveCriticalSection(&gPeerLock);

    return count;
}

static int WSAAPI HookConnect(SOCKET socket, const sockaddr *name, int nameLength) {
    if (gConnect == nullptr) {
        WSASetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }

    char text[32];
    if (IsDiscoveryAddress(name, nameLength)) {
        gDiscoverySocket = socket;
        if (gMode == ProxyMode::Client) {
            sockaddr_in target = *reinterpret_cast<const sockaddr_in *>(name);
            target.sin_addr = gServerAddress;
            const int result = gConnect(socket, reinterpret_cast<const sockaddr *>(&target), sizeof(target));
            Log("connect socket=%u broadcast:3657 redirected to %s result=%d error=%d", static_cast<unsigned>(socket), FormatAddress(reinterpret_cast<const sockaddr *>(&target), sizeof(target), text, sizeof(text)), result, result == SOCKET_ERROR ? WSAGetLastError() : 0);
            return result;
        }
        Log("connect socket=%u to broadcast:3657 recorded as discovery socket", static_cast<unsigned>(socket));
    }
    else {
        sockaddr_in wire;
        if (ToWireAddress(name, nameLength, wire)) {
            const int result = gConnect(socket, reinterpret_cast<const sockaddr *>(&wire), sizeof(wire));
            Log("connect socket=%u to %s (unmapped from 10.x) result=%d", static_cast<unsigned>(socket), FormatAddress(reinterpret_cast<const sockaddr *>(&wire), sizeof(wire), text, sizeof(text)), result);
            return result;
        }
        Log("connect socket=%u to %s (passthrough)", static_cast<unsigned>(socket), FormatAddress(name, nameLength, text, sizeof(text)));
    }

    return gConnect(socket, name, nameLength);
}

static int WSAAPI HookRecvFrom(SOCKET socket, char *buffer, int length, int flags, sockaddr *from, int *fromLength) {
    if (gRecvFrom == nullptr) {
        WSASetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }

    const int result = gRecvFrom(socket, buffer, length, flags, from, fromLength);
    if (result <= 0 || from == nullptr || fromLength == nullptr) {
        return result;
    }

    char text[32];
    if (IsDiscoveryProbe(buffer, result)) {
        Log("recvfrom socket=%u discovery probe from %s", static_cast<unsigned>(socket), FormatAddress(from, *fromLength, text, sizeof(text)));
        if (gMode == ProxyMode::Server && from->sa_family == AF_INET) {
            RecordPeer(*reinterpret_cast<const sockaddr_in *>(from));
        }
    }
    else if (IsDiscoveryReply(buffer, result)) {
        Log("recvfrom socket=%u discovery reply (%d bytes) from %s", static_cast<unsigned>(socket), result, FormatAddress(from, *fromLength, text, sizeof(text)));
    }
    else if (IsTailscaleAddress(from)) {
        static volatile LONG tailscaleLogged;
        if (InterlockedIncrement(&tailscaleLogged) <= 50) {
            char hex[64];
            FormatHex(buffer, result, hex, sizeof(hex));
            Log("recvfrom socket=%u other datagram (%d bytes) from %s: %s", static_cast<unsigned>(socket), result, FormatAddress(from, *fromLength, text, sizeof(text)), hex);
        }
    }

    if (*fromLength >= static_cast<int>(sizeof(sockaddr_in))) {
        ToGameAddress(from);
    }
    return result;
}

// Sends a discovery reply to every recently seen prober instead of broadcasting it.
static int UnicastReply(SOCKET socket, const char *buffer, int length, int flags, bool connected) {
    sockaddr_in peers[32];
    const int peerCount = SnapshotPeers(peers, 32);
    if (peerCount == 0) {
        Log("discovery reply on socket=%u: no recent probers, falling back to original send", static_cast<unsigned>(socket));
        return -2;
    }

    char text[32];
    int result = SOCKET_ERROR;
    for (int index = 0; index < peerCount; ++index) {
        const auto *peer = reinterpret_cast<const sockaddr *>(&peers[index]);
        int sent;
        if (connected) {
            if (gConnect(socket, peer, sizeof(peers[index])) == SOCKET_ERROR) {
                Log("discovery reply: connect to %s failed error=%d", FormatAddress(peer, sizeof(peers[index]), text, sizeof(text)), WSAGetLastError());
                continue;
            }
            sent = gSend(socket, buffer, length, flags);
        }
        else {
            sent = gSendTo(socket, buffer, length, flags, peer, sizeof(peers[index]));
        }
        Log("discovery reply (%d bytes) unicast to %s result=%d error=%d", length, FormatAddress(peer, sizeof(peers[index]), text, sizeof(text)), sent, sent == SOCKET_ERROR ? WSAGetLastError() : 0);
        if (sent != SOCKET_ERROR) {
            result = sent;
        }
    }

    return result;
}

static int WSAAPI HookSend(SOCKET socket, const char *buffer, int length, int flags) {
    if (gSend == nullptr) {
        WSASetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }

    if (IsDiscoveryProbe(buffer, length)) {
        Log("send socket=%u discovery probe", static_cast<unsigned>(socket));
    }

    if (gMode != ProxyMode::Server || !IsDiscoveryReply(buffer, length)) {
        return gSend(socket, buffer, length, flags);
    }

    if (socket != gDiscoverySocket) {
        Log("send socket=%u discovery reply on non-discovery socket (discovery=%u), passthrough", static_cast<unsigned>(socket), static_cast<unsigned>(gDiscoverySocket));
        return gSend(socket, buffer, length, flags);
    }

    const int result = UnicastReply(socket, buffer, length, flags, true);
    return result == -2 ? gSend(socket, buffer, length, flags) : result;
}

// Battlefront 1.3.7.4 (GOG) game-socket workaround. When the client's server
// browser is torn down after a join has already started, FUN_005E8320 closes
// the UDP 3658 socket and zeroes it, so every join packet goes to socket 0 and
// the join times out. Reopen it through the game's own FUN_005E85E0.
static const ULONG_PTR kImageBase = 0x00400000;
static const ULONG_PTR kOpenGameSocket = 0x005e85e0;   // socket()+bind(3658), stores DAT_00940690
static const ULONG_PTR kGameSocket = 0x00940690;       // UDP 3658 socket
static const ULONG_PTR kGameSocketOpen = 0x008c19e4;   // "3658 socket open" flag (holds the port)

static int gGameSocketFixState;   // 0 = not checked yet, 1 = available, -1 = unavailable

static void *GameAddress(ULONG_PTR address) {
    return reinterpret_cast<unsigned char *>(GetModuleHandleW(nullptr)) + (address - kImageBase);
}

// Checked lazily: the executable's code may not be in its final state while DllMain runs.
static bool GameSocketFixAvailable() {
    if (gGameSocketFixState != 0) {
        return gGameSocketFixState > 0;
    }

    // The executable is relocatable, so the store's operand holds the relocated
    // address of DAT_00940690 (mov [DAT_00940690], eax = a3 <address>).
    static const unsigned char prologue[] = { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x18 };
    const auto *code = static_cast<const unsigned char *>(GameAddress(kOpenGameSocket));
    const ULONG_PTR expectedStore = reinterpret_cast<ULONG_PTR>(GameAddress(kGameSocket));
    ULONG_PTR actualStore;
    std::memcpy(&actualStore, code + 0x1c, sizeof(actualStore));
    const bool matched = std::memcmp(code, prologue, sizeof(prologue)) == 0 && code[0x1b] == 0xa3 && actualStore == expectedStore;
    gGameSocketFixState = matched ? 1 : -1;

    char hex[64];
    FormatHex(reinterpret_cast<const char *>(code), 16, hex, sizeof(hex));
    char storeHex[32];
    FormatHex(reinterpret_cast<const char *>(code + 0x1b), 5, storeHex, sizeof(storeHex));
    Log("game socket fix %s: module base=%p code=%s| +1b=%s", matched ? "available" : "UNAVAILABLE", GetModuleHandleW(nullptr), hex, storeHex);
    return matched;
}

static SOCKET ReopenGameSocket() {
    if (!GameSocketFixAvailable()) {
        return 0;
    }
    auto *gameSocket = static_cast<volatile SOCKET *>(GameAddress(kGameSocket));
    if (*gameSocket != 0) {
        return *gameSocket;
    }

    using OpenGameSocketFn = int(__cdecl *)(int);
    const int port = reinterpret_cast<OpenGameSocketFn>(GameAddress(kOpenGameSocket))(0);
    *static_cast<volatile int *>(GameAddress(kGameSocketOpen)) = port;
    Log("game UDP 3658 socket was closed during join; reopened as socket=%u", static_cast<unsigned>(*gameSocket));
    return *gameSocket;
}

static int WSAAPI HookSendTo(SOCKET socket, const char *buffer, int length, int flags, const sockaddr *to, int toLength) {
    if (gSendTo == nullptr) {
        WSASetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }

    if (socket == 0 && gMode == ProxyMode::Client) {
        const SOCKET reopened = ReopenGameSocket();
        if (reopened != 0) {
            socket = reopened;
        }
    }

    if (!IsDiscoveryAddress(to, toLength)) {
        sockaddr_in wire;
        if (ToWireAddress(to, toLength, wire)) {
            static volatile LONG mappedLogged;
            const int result = gSendTo(socket, buffer, length, flags, reinterpret_cast<const sockaddr *>(&wire), sizeof(wire));
            if (InterlockedIncrement(&mappedLogged) <= 50) {
                char text[32];
                Log("sendto socket=%u (%d bytes) to %s (unmapped from 10.x) result=%d error=%d", static_cast<unsigned>(socket), length, FormatAddress(reinterpret_cast<const sockaddr *>(&wire), sizeof(wire), text, sizeof(text)), result, result == SOCKET_ERROR ? WSAGetLastError() : 0);
            }
            return result;
        }
        return gSendTo(socket, buffer, length, flags, to, toLength);
    }

    if (gMode == ProxyMode::Client) {
        sockaddr_in target = *reinterpret_cast<const sockaddr_in *>(to);
        target.sin_addr = gServerAddress;
        const int result = gSendTo(socket, buffer, length, flags, reinterpret_cast<const sockaddr *>(&target), sizeof(target));
        char text[32];
        Log("sendto socket=%u broadcast:3657 (%d bytes) redirected to %s result=%d", static_cast<unsigned>(socket), length, FormatAddress(reinterpret_cast<const sockaddr *>(&target), sizeof(target), text, sizeof(text)), result);
        return result;
    }

    if (gMode == ProxyMode::Server && IsDiscoveryReply(buffer, length)) {
        const int result = UnicastReply(socket, buffer, length, flags, false);
        if (result != -2) {
            return result;
        }
    }

    Log("sendto socket=%u broadcast:3657 (%d bytes) passthrough", static_cast<unsigned>(socket), length);
    return gSendTo(socket, buffer, length, flags, to, toLength);
}

static bool GetGameDirectory(char *output, size_t capacity) {
    if (GetModuleFileNameA(nullptr, output, static_cast<DWORD>(capacity)) == 0) {
        return false;
    }

    char *separator = std::strrchr(output, '\\');
    if (separator == nullptr) {
        return false;
    }
    *separator = '\0';
    return true;
}

static void OpenLog(const char *directory) {
    char logPath[MAX_PATH];
    std::snprintf(logPath, sizeof(logPath), "%s\\BattlefrontDirectIp.log", directory);
    gLogFile = CreateFileA(logPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

static bool Configure(const char *iniPath) {
    char mode[16];
    GetPrivateProfileStringA("BattlefrontDirectIp", "Mode", "", mode, sizeof(mode), iniPath);
    if (_stricmp(mode, "client") == 0) {
        char server[64];
        GetPrivateProfileStringA("BattlefrontDirectIp", "Server", "", server, sizeof(server), iniPath);
        const unsigned long address = inet_addr(server);
        if (address != INADDR_NONE && address != INADDR_ANY) {
            gServerAddress.s_addr = address;
            gMode = ProxyMode::Client;
            Log("mode=client server=%s", server);
            return true;
        }
        Log("mode=client but Server='%s' is not a valid IPv4 address", server);
        return false;
    }

    if (_stricmp(mode, "server") == 0) {
        gMode = ProxyMode::Server;
        Log("mode=server");
        return true;
    }

    Log("Mode='%s' not recognised (expected client or server); proxy inactive", mode);
    return false;
}

static void PatchImport(IMAGE_THUNK_DATA32 *entry, FARPROC replacement) {
    DWORD originalProtection;
    if (!VirtualProtect(&entry->u1.Function, sizeof(entry->u1.Function), PAGE_READWRITE, &originalProtection)) {
        Log("VirtualProtect failed error=%lu", GetLastError());
        return;
    }
    entry->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
    VirtualProtect(&entry->u1.Function, sizeof(entry->u1.Function), originalProtection, &originalProtection);
}

static bool PatchWinsockImports() {
    auto *base = reinterpret_cast<unsigned char *>(GetModuleHandleW(nullptr));
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }

    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    const auto &directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0) {
        return false;
    }

    auto *descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + directory.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor) {
        const char *moduleName = reinterpret_cast<const char *>(base + descriptor->Name);
        if (_stricmp(moduleName, "WS2_32.DLL") != 0) {
            continue;
        }

        // Battlefront.exe imports Winsock by ordinal, so match IAT entries by the
        // resolved export address rather than by import name.
        const HMODULE winsock = GetModuleHandleA("ws2_32.dll");
        if (winsock == nullptr) {
            Log("ws2_32.dll not loaded");
            return false;
        }
        const ULONG_PTR connectAddress = reinterpret_cast<ULONG_PTR>(GetProcAddress(winsock, "connect"));
        const ULONG_PTR recvFromAddress = reinterpret_cast<ULONG_PTR>(GetProcAddress(winsock, "recvfrom"));
        const ULONG_PTR sendAddress = reinterpret_cast<ULONG_PTR>(GetProcAddress(winsock, "send"));
        const ULONG_PTR sendToAddress = reinterpret_cast<ULONG_PTR>(GetProcAddress(winsock, "sendto"));

        auto *entries = reinterpret_cast<IMAGE_THUNK_DATA32 *>(base + descriptor->FirstThunk);
        for (; entries->u1.Function != 0; ++entries) {
            const ULONG_PTR function = entries->u1.Function;
            if (function == connectAddress) {
                gConnect = reinterpret_cast<ConnectFn>(function);
                PatchImport(entries, reinterpret_cast<FARPROC>(HookConnect));
            }
            else if (function == recvFromAddress) {
                gRecvFrom = reinterpret_cast<RecvFromFn>(function);
                PatchImport(entries, reinterpret_cast<FARPROC>(HookRecvFrom));
            }
            else if (function == sendAddress) {
                gSend = reinterpret_cast<SendFn>(function);
                PatchImport(entries, reinterpret_cast<FARPROC>(HookSend));
            }
            else if (function == sendToAddress) {
                gSendTo = reinterpret_cast<SendToFn>(function);
                PatchImport(entries, reinterpret_cast<FARPROC>(HookSendTo));
            }
        }

        Log("hooks: connect=%d recvfrom=%d send=%d sendto=%d", gConnect != nullptr, gRecvFrom != nullptr, gSend != nullptr, gSendTo != nullptr);
        return gConnect != nullptr && gRecvFrom != nullptr && gSend != nullptr;
    }

    Log("WS2_32.DLL import descriptor not found");
    return false;
}

// Runs from DllMain: the executable's imports are already bound by then, and a
// /norender dedicated server may never call DirectInput8Create at all.
static void InitializeProxy() {
    if (InterlockedCompareExchange(&gInitialized, 1, 0) != 0) {
        return;
    }

    InitializeCriticalSection(&gPeerLock);
    InitializeCriticalSection(&gLogLock);

    char directory[MAX_PATH];
    if (!GetGameDirectory(directory, sizeof(directory))) {
        return;
    }
    OpenLog(directory);
    Log("BattlefrontDirectIp proxy loaded in %s", directory);

    char iniPath[MAX_PATH];
    std::snprintf(iniPath, sizeof(iniPath), "%s\\BattlefrontDirectIp.ini", directory);
    if (!Configure(iniPath)) {
        return;
    }

    if (!PatchWinsockImports()) {
        Log("Winsock import patch failed; proxy inactive");
        gMode = ProxyMode::None;
        return;
    }
    Log("proxy active");
}

static bool LoadSystemDirectInput() {
    if (gDirectInput8Create != nullptr) {
        return true;
    }

    char path[MAX_PATH];
    const UINT length = GetSystemDirectoryA(path, MAX_PATH);
    if (length == 0 || length + 13 >= MAX_PATH) {
        return false;
    }
    std::strcat(path, "\\dinput8.dll");

    gSystemDinput8 = LoadLibraryA(path);
    if (gSystemDinput8 == nullptr) {
        return false;
    }

    gDirectInput8Create = reinterpret_cast<DirectInput8CreateFn>(GetProcAddress(gSystemDinput8, "DirectInput8Create"));
    return gDirectInput8Create != nullptr;
}

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version, REFIID interfaceId, LPVOID *output, LPUNKNOWN outer) {
    if (!LoadSystemDirectInput()) {
        Log("DirectInput8Create: failed to load system dinput8.dll");
        return E_FAIL;
    }

    const HRESULT result = gDirectInput8Create(instance, version, interfaceId, output, outer);
    Log("DirectInput8Create forwarded hr=0x%08lx", static_cast<unsigned long>(result));
    return result;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        InitializeProxy();
    }
    return TRUE;
}
