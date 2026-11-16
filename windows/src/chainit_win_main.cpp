#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <ws2bth.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincrypt.h>
#include <wininet.h>
#include <urlmon.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "embedded_assets.h"

namespace fs = std::filesystem;

static const int HTTP_PORT = 4173;
static const int DISCOVERY_PORT = 49179;
static const int PHONE_VFS_PORT = 49180;

#define WM_TRAYICON (WM_USER + 101)
#define ID_TRAY_OPEN_UI 4001
#define ID_TRAY_OPEN_Z 4002
#define ID_TRAY_REMOUNT_Z 4003
#define ID_TRAY_EXIT 4004
#define ID_TRAY_UPDATE 4005
#define ID_TRAY_SHORTCUT 4006

// --- Native Bluetooth RFCOMM (ws2bth.h / AF_BTH) ---
// These service UUIDs MUST match android/.../net/BluetoothRfcommTransport.kt so the phone
// and this PC find each other with a real SDP lookup (no Wi-Fi, no Hotspot, no USB needed).
static const GUID kChainItPhoneUuid = {0xc4a1b170, 0x0001, 0x4000, {0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb}};
static const GUID kChainItPcUuid = {0xc4a1b170, 0x0002, 0x4000, {0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb}};

// An RFCOMM link is one serialized byte stream, so phone requests over Bluetooth are queued here.
static std::mutex gBtRfcommClientMtx;

// Live Bluetooth runtime status (radio presence, published PC receiver channel, paired device list)
static std::atomic<bool> gBtRadioPresent{false};
static std::atomic<bool> gBtServerListening{false};
static std::atomic<int> gBtServerChannel{0};
static std::mutex gBtPairedMtx;
static std::vector<std::string> gBtPairedDevices;

static std::string kChainItPhoneUuidStr = "c4a1b170-0001-4000-8000-00805f9b34fb";
static std::string kChainItPcUuidStr = "c4a1b170-0002-4000-8000-00805f9b34fb";

static bool isBtEndpoint(const std::string& ep) {
    return ep.rfind("bt://", 0) == 0;
}

static std::string btAddrToString(BTH_ADDR a) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                  static_cast<unsigned>((a >> 40) & 0xFF), static_cast<unsigned>((a >> 32) & 0xFF),
                  static_cast<unsigned>((a >> 24) & 0xFF), static_cast<unsigned>((a >> 16) & 0xFF),
                  static_cast<unsigned>((a >> 8) & 0xFF), static_cast<unsigned>(a & 0xFF));
    return std::string(buf);
}

// Accepts AA:BB:CC:DD:EE:FF, AA-BB-CC-DD-EE-FF or AABBCCDDEEFF
static bool parseBtMac(const std::string& in, BTH_ADDR* out) {
    std::string s;
    s.reserve(12);
    for (char c : in) {
        if (c == ':' || c == '-' || c == ' ') continue;
        s.push_back(c);
    }
    if (s.size() != 12) return false;
    BTH_ADDR v = 0;
    for (size_t i = 0; i < 12; i += 2) {
        char pair[3] = {s[i], s[i + 1], 0};
        char* endp = nullptr;
        unsigned long b = std::strtoul(pair, &endp, 16);
        if (endp != pair + 2) return false;
        v = (v << 8) | (static_cast<BTH_ADDR>(b) & 0xFF);
    }
    if (v == 0) return false;
    *out = v;
    return true;
}

// Parses "bt://AA:BB:CC:DD:EE:FF", "bt://AA:BB:CC:DD:EE:FF:12" or a bare MAC into addr + optional RFCOMM channel.
static bool parseBtEndpoint(const std::string& ep, BTH_ADDR* addr, ULONG* port) {
    std::string s = isBtEndpoint(ep) ? ep.substr(5) : ep;
    for (char& c : s) {
        if (c == '-' || c == '/') c = ':';
    }
    *addr = 0;
    *port = 0;
    int colons = 0;
    for (char c : s) {
        if (c == ':') colons++;
    }
    if (colons == 6) {
        size_t cut = s.find_last_of(':');
        std::string p = s.substr(cut + 1);
        if (p.empty() || p.find_first_not_of("0123456789") != std::string::npos) return false;
        unsigned long v = std::strtoul(p.c_str(), nullptr, 10);
        if (v < BT_PORT_MIN || v > BT_PORT_MAX) return false;
        *port = static_cast<ULONG>(v);
        s = s.substr(0, cut);
    } else if (colons != 5 && colons != 0) {
        return false;
    }
    return parseBtMac(s, addr);
}

// Turns "AA:BB:CC:DD:EE:FF" / "bt://aa-bb-cc-dd-ee-ff" into canonical "bt://AA:BB:CC:DD:EE:FF".
// Returns "" for plain IPv4 addresses so manual connect never confuses an IP with a MAC.
static std::string normalizeBtEndpoint(const std::string& in) {
    bool explicitBt = isBtEndpoint(in);
    std::string s = explicitBt ? in.substr(5) : in;
    while (!s.empty() && (s.front() == '/' || s.front() == ' ')) s.erase(s.begin());
    while (!s.empty() && (s.back() == '/' || s.back() == ' ')) s.pop_back();
    if (!explicitBt && s.find(':') == std::string::npos && s.find('-') == std::string::npos) return "";
    if (s.size() != 12 && !explicitBt && s.find_first_of("gGxX.") != std::string::npos && s.find(':') == std::string::npos) return "";
    BTH_ADDR addr = 0;
    ULONG port = 0;
    if (!parseBtEndpoint(s, &addr, &port)) return "";
    if (port != 0) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "bt://%s:%lu", btAddrToString(addr).c_str(), static_cast<unsigned long>(port));
        return std::string(buf);
    }
    return "bt://" + btAddrToString(addr);
}

// Opens a native RFCOMM socket to the phone's ChainIt service (SDP resolved by the Windows stack).
static SOCKET btRfcommConnect(const std::string& endpoint, int timeoutMs) {
    BTH_ADDR addr = 0;
    ULONG port = 0;
    if (!parseBtEndpoint(endpoint, &addr, &port)) return INVALID_SOCKET;

    SOCKADDR_BTH sa{};
    sa.addressFamily = AF_BTH;
    sa.btAddr = addr;
    if (port != 0) {
        sa.port = port;
    } else {
        sa.serviceClassId = kChainItPhoneUuid;
        sa.port = BT_PORT_ANY; // let the stack run the SDP query for kChainItPhoneUuid
    }

    SOCKET s = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    u_long nonBlock = 1;
    ioctlsocket(s, FIONBIO, &nonBlock);
    int cr = connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    if (cr == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            fd_set wfds, efds;
            FD_ZERO(&wfds);
            FD_SET(s, &wfds);
            FD_ZERO(&efds);
            FD_SET(s, &efds);
            timeval tv{};
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
            int sel = select(0, nullptr, &wfds, &efds, &tv);
            if (sel <= 0 || FD_ISSET(s, &efds)) {
                closesocket(s);
                return INVALID_SOCKET;
            }
        } else {
            closesocket(s);
            // Some Bluetooth stacks reject non-blocking connect: retry once in blocking mode.
            SOCKET s2 = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
            if (s2 == INVALID_SOCKET) return INVALID_SOCKET;
            if (connect(s2, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) == SOCKET_ERROR) {
                closesocket(s2);
                return INVALID_SOCKET;
            }
            s = s2;
        }
    }

    nonBlock = 0;
    ioctlsocket(s, FIONBIO, &nonBlock);
    DWORD sockTimeout = static_cast<DWORD>(timeoutMs);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&sockTimeout), sizeof(sockTimeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sockTimeout), sizeof(sockTimeout));
    return s;
}


// --- Minimal JSON String Escaping & Field Extraction Helpers ---

static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

static std::string jsonExtractString(const std::string& json, const std::string& key, const std::string& defVal = "") {
    std::string pat = "\"" + key + "\"";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) return defVal;
    pos = json.find(':', pos + pat.size());
    if (pos == std::string::npos) return defVal;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) pos++;
    if (pos >= json.size() || json[pos] != '"') return defVal;
    pos++;
    std::string out;
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '\\' && pos < json.size()) {
            char esc = json[pos++];
            switch (esc) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                default: out += esc; break;
            }
        } else if (c == '"') {
            break;
        } else {
            out += c;
        }
    }
    return out;
}

static bool jsonHasKey(const std::string& json, const std::string& key) {
    return json.find("\"" + key + "\"") != std::string::npos;
}

static bool jsonExtractBool(const std::string& json, const std::string& key, bool defVal) {
    std::string pat = "\"" + key + "\"";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) return defVal;
    pos = json.find(':', pos + pat.size());
    if (pos == std::string::npos) return defVal;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) pos++;
    if (json.compare(pos, 4, "true") == 0) return true;
    if (json.compare(pos, 5, "false") == 0) return false;
    return defVal;
}

static int64_t jsonExtractInt(const std::string& json, const std::string& key, int64_t defVal) {
    std::string pat = "\"" + key + "\"";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) return defVal;
    pos = json.find(':', pos + pat.size());
    if (pos == std::string::npos) return defVal;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) pos++;
    char* endPtr = nullptr;
    long long v = std::strtoll(json.c_str() + pos, &endPtr, 10);
    if (endPtr == json.c_str() + pos) return defVal;
    return static_cast<int64_t>(v);
}

static std::vector<std::string> jsonExtractStringArray(const std::string& json, const std::string& key) {
    std::vector<std::string> res;
    std::string pat = "\"" + key + "\"";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) return res;
    pos = json.find('[', pos + pat.size());
    if (pos == std::string::npos) return res;
    pos++;
    while (pos < json.size()) {
        while (pos < json.size() && json[pos] != '"' && json[pos] != ']') pos++;
        if (pos >= json.size() || json[pos] == ']') break;
        pos++;
        std::string item;
        while (pos < json.size()) {
            char c = json[pos++];
            if (c == '\\' && pos < json.size()) {
                char esc = json[pos++];
                if (esc == 'n') item += '\n';
                else item += esc;
            } else if (c == '"') {
                break;
            } else {
                item += c;
            }
        }
        res.push_back(item);
    }
    return res;
}

static std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            char hex[3] = {s[i + 1], s[i + 2], 0};
            out += static_cast<char>(std::strtol(hex, nullptr, 16));
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

static std::string urlEncode(const std::string& s) {
    std::ostringstream oss;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            oss << c;
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            oss << buf;
        }
    }
    return oss.str();
}

static std::string getQueryParam(const std::string& query, const std::string& key, const std::string& defVal = "") {
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        std::string pair = (amp == std::string::npos) ? query.substr(pos) : query.substr(pos, amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            if (pair.substr(0, eq) == key) {
                return urlDecode(pair.substr(eq + 1));
            }
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return defVal;
}

static int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// --- Global Engine State ---

struct LogItem {
    std::string id;
    int64_t ts;
    std::string layer;
    std::string summary;
    std::string detail;
};

struct TransferItem {
    std::string id;
    std::string label;
    std::string op;
    std::string direction;
    std::string transportUsed;
    int64_t bytes;
    double speedMBps;
    int64_t ts;
};

struct DiscoveredEndpoint {
    std::string ip;
    std::string kind; // usb_rndis, wifi_lan, local_hotspot, bluetooth, online_p2p
    double rttMs = 3.0;
    int64_t lastOkMs = 0;
};

struct GlobalState {
    std::mutex mtx;
    std::string selectionMode = "auto";
    std::string manualTransportId = "wifi_lan";
    std::string activeTransportId = "wifi_lan";
    std::string siblingKey = "CHAINIT-SIBLING";

    // Discovered real phone endpoints
    std::map<std::string, DiscoveredEndpoint> phoneEndpoints; // keyed by kind
    std::string activePhoneIp;
    bool realPhoneConnected = false;

    // Sim / fallback toggles when testing without phone on LAN yet
    bool envUsb = false;
    bool envWifi = true;
    bool envHotspot = false;
    bool envBluetooth = true;
    bool envOnline = false;
    bool phoneScreenLocked = true;
    int phoneBatteryPct = 88;

    // Sibling configuration
    bool siblingEnabled = true;
    bool autoConnectOnRange = true;
    bool autoMountZDrive = true;
    bool zeroPromptTransfers = true;
    bool allowScreenOffVfs = true;
    bool zDriveMounted = false;
    std::string peerDeviceName = "Android Sibling (Scanning LAN/USB...)";
    std::string peerModel = "Auto-Discovery Active";
    std::string peerAndroidVersion = "Android";
    uint64_t phoneTotalBytes = 128ULL * 1024 * 1024 * 1024;
    uint64_t phoneUsedBytes = 42ULL * 1024 * 1024 * 1024;

    std::vector<TransferItem> transfers;
    std::vector<LogItem> logs;
    std::vector<SOCKET> wsClients;

    // Native Bluetooth (AF_BTH / RFCOMM) runtime info surfaced to the UI
    bool btRadioPresent = false;
    bool btReceiverListening = false;
    int btReceiverChannel = 0;
    std::vector<std::string> btPairedDevices;

    fs::path pcRoot;
    fs::path fallbackPhoneRoot;
};

static GlobalState gState;
static std::atomic<bool> gRunning{true};

static void addLogLocked(const std::string& layer, const std::string& summary, const std::string& detail) {
    char idBuf[16];
    std::snprintf(idBuf, sizeof(idBuf), "%08x", static_cast<unsigned>(nowMs() & 0xFFFFFFFF));
    gState.logs.insert(gState.logs.begin(), {idBuf, nowMs(), layer, summary, detail});
    if (gState.logs.size() > 50) gState.logs.resize(50);
}

// --- Silent Background Command Runner (for Windows net use Z:) ---

static bool runHiddenCommand(const std::wstring& cmdLine) {
    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    std::vector<wchar_t> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back(L'\0');
    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, 8000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return exitCode == 0;
}

static void mountZDriveAsync() {
    std::thread([]() {
        runHiddenCommand(L"sc start WebClient");
        runHiddenCommand(L"net use Z: /delete /y");
        bool ok = runHiddenCommand(L"net use Z: http://127.0.0.1:4173/dav /persistent:no");
        std::lock_guard<std::mutex> lk(gState.mtx);
        gState.zDriveMounted = ok || gState.siblingEnabled;
        if (ok) {
            addLogLocked("VFS", "Mounted Z:\\ in Windows File Explorer", "Mapped Z:\\ -> http://127.0.0.1:4173/dav (Multipath Sibling VFS).");
        }
    }).detach();
}

static void unmountZDriveSync() {
    runHiddenCommand(L"net use Z: /delete /y");
}

static void launchDesktopUiWindow() {
    const wchar_t* edgePaths[] = {
        L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
        L"C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
    };
    for (const wchar_t* edge : edgePaths) {
        if (GetFileAttributesW(edge) != INVALID_FILE_ATTRIBUTES) {
            std::wstring args = L"--app=http://127.0.0.1:4173 --window-size=1380,860";
            ShellExecuteW(nullptr, L"open", edge, args.c_str(), nullptr, SW_SHOWNORMAL);
            return;
        }
    }
    ShellExecuteW(nullptr, L"open", L"http://127.0.0.1:4173", nullptr, nullptr, SW_SHOWNORMAL);
}

// --- Simple Outbound HTTP Client to Talk to ChainIt-Android.apk (Port 49180) ---

struct HttpReply {
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
    double rttMs = 0.0;
};

static HttpReply httpRequestToPhone(
    const std::string& ip,
    int port,
    const std::string& method,
    const std::string& targetPath,
    const std::string& body = "",
    const std::map<std::string, std::string>& extraHeaders = {},
    int timeoutMs = 4000
) {
    HttpReply rep;
    auto t0 = std::chrono::steady_clock::now();

    // Native Bluetooth RFCOMM (AF_BTH) links are addressed as "bt://AA:BB:CC:DD:EE:FF" and carry
    // exactly the same HTTP/WebDAV protocol as the TCP transports, so Z:\ and the File Manager work
    // with Wi-Fi / Hotspot / USB all switched OFF.
    const bool viaBluetooth = isBtEndpoint(ip);
    std::unique_lock<std::mutex> btLk(gBtRfcommClientMtx, std::defer_lock);
    SOCKET s = INVALID_SOCKET;

    if (viaBluetooth) {
        btLk.lock(); // one serialized RFCOMM stream at a time
        s = btRfcommConnect(ip, timeoutMs);
        if (s == INVALID_SOCKET) return rep;
    } else {
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return rep;

        u_long nonBlock = 1;
        ioctlsocket(s, FIONBIO, &nonBlock);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(port));
        if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
            closesocket(s);
            return rep;
        }

        connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(s, &wfds);
        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        if (select(0, nullptr, &wfds, nullptr, &tv) <= 0) {
            closesocket(s);
            return rep;
        }

        nonBlock = 0;
        ioctlsocket(s, FIONBIO, &nonBlock);
        DWORD sockTimeout = static_cast<DWORD>(timeoutMs);
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&sockTimeout), sizeof(sockTimeout));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sockTimeout), sizeof(sockTimeout));
    }

    std::string keyCopy;
    {
        std::lock_guard<std::mutex> lk(gState.mtx);
        keyCopy = gState.siblingKey;
    }

    std::ostringstream req;
    req << method << " " << targetPath << " HTTP/1.1\r\n";
    if (viaBluetooth) {
        req << "Host: chainit-bluetooth\r\n";
        req << "X-ChainIt-Transport: bluetooth-rfcomm\r\n";
    } else {
        req << "Host: " << ip << ":" << port << "\r\n";
    }
    req << "X-ChainIt-Key: " << keyCopy << "\r\n";
    req << "Content-Length: " << body.size() << "\r\n";
    req << "Connection: close\r\n";
    for (const auto& kv : extraHeaders) {
        req << kv.first << ": " << kv.second << "\r\n";
    }
    req << "\r\n";
    std::string headStr = req.str();

    send(s, headStr.data(), static_cast<int>(headStr.size()), 0);
    if (!body.empty()) {
        size_t sent = 0;
        while (sent < body.size()) {
            int n = send(s, body.data() + sent, static_cast<int>(std::min<size_t>(65536, body.size() - sent)), 0);
            if (n <= 0) break;
            sent += static_cast<size_t>(n);
        }
    }

    std::string raw;
    char buf[16384];
    while (true) {
        int r = recv(s, buf, sizeof(buf), 0);
        if (r <= 0) break;
        raw.append(buf, static_cast<size_t>(r));
    }
    closesocket(s);

    auto t1 = std::chrono::steady_clock::now();
    rep.rttMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    size_t hdrEnd = raw.find("\r\n\r\n");
    if (hdrEnd == std::string::npos) return rep;
    std::string hdrBlock = raw.substr(0, hdrEnd);
    rep.body = raw.substr(hdrEnd + 4);

    std::istringstream iss(hdrBlock);
    std::string statusLine;
    std::getline(iss, statusLine);
    size_t sp1 = statusLine.find(' ');
    if (sp1 != std::string::npos) {
        rep.status = std::atoi(statusLine.c_str() + sp1 + 1);
    }
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t c = line.find(':');
        if (c != std::string::npos) {
            std::string k = line.substr(0, c);
            std::transform(k.begin(), k.end(), k.begin(), ::tolower);
            std::string v = line.substr(c + 1);
            while (!v.empty() && v.front() == ' ') v.erase(v.begin());
            rep.headers[k] = v;
        }
    }
    return rep;
}

// --- Local Filesystem VFS Helpers (for C:\ and Offline Demo Fallback) ---

static std::string guessMime(const std::string& filename) {
    std::string ext = fs::u8path(filename).extension().u8string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".webp") return "image/webp";
    if (ext == ".gif") return "image/gif";
    if (ext == ".md") return "text/markdown";
    if (ext == ".json") return "application/json";
    if (ext == ".csv") return "text/csv";
    if (ext == ".pdf") return "application/pdf";
    if (ext == ".txt" || ext == ".log" || ext == ".ts" || ext == ".rs" || ext == ".kt" || ext == ".ps1") return "text/plain";
    return "application/octet-stream";
}

static bool isEditableTextExt(const std::string& filename) {
    std::string ext = fs::u8path(filename).extension().u8string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".md" || ext == ".txt" || ext == ".json" || ext == ".csv" ||
           ext == ".svg" || ext == ".xml" || ext == ".log" || ext == ".ini" ||
           ext == ".ps1" || ext == ".ts" || ext == ".js" || ext == ".rs" || ext == ".kt";
}

static fs::path resolveLocalSafe(const fs::path& root, const std::string& virtualPath) {
    std::string clean = virtualPath;
    std::replace(clean.begin(), clean.end(), '\\', '/');
    while (!clean.empty() && clean.front() == '/') clean.erase(clean.begin());
    fs::path candidate = clean.empty() ? root : (root / fs::u8path(clean));
    std::error_code ec;
    fs::path normRoot = fs::weakly_canonical(root, ec);
    fs::path normCand = fs::weakly_canonical(candidate, ec);
    std::wstring rStr = normRoot.wstring();
    std::wstring cStr = normCand.wstring();
    if (cStr.size() < rStr.size() || cStr.compare(0, rStr.size(), rStr) != 0) {
        return normRoot;
    }
    return normCand;
}

static std::string toVirtualPosix(const fs::path& root, const fs::path& full) {
    std::error_code ec;
    fs::path rel = fs::relative(full, root, ec);
    if (ec || rel.empty() || rel == ".") return "/";
    std::string s = rel.generic_u8string();
    if (s.empty() || s == ".") return "/";
    return s.front() == '/' ? s : ("/" + s);
}

static std::string buildVfsItemJson(const fs::path& root, const fs::path& full) {
    std::error_code ec;
    bool isDir = fs::is_directory(full, ec);
    uint64_t sz = isDir ? 0 : fs::file_size(full, ec);
    if (ec) sz = 0;
    std::string name = full.filename().u8string();
    if (name.empty()) name = full.u8string();
    std::string vpath = toVirtualPosix(root, full);
    std::string ext = isDir ? "" : full.extension().u8string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    std::string mime = isDir ? "inode/directory" : guessMime(name);

    std::ostringstream oss;
    oss << "{\"name\":\"" << jsonEscape(name) << "\","
        << "\"path\":\"" << jsonEscape(vpath) << "\","
        << "\"kind\":\"" << (isDir ? "directory" : "file") << "\","
        << "\"sizeBytes\":" << sz << ","
        << "\"modifiedMs\":" << nowMs() << ","
        << "\"mimeType\":\"" << jsonEscape(mime) << "\","
        << "\"ext\":\"" << jsonEscape(ext) << "\"}";
    return oss.str();
}

static std::string listLocalDirJson(const fs::path& root, const std::string& dirPath) {
    fs::path full = resolveLocalSafe(root, dirPath);
    std::error_code ec;
    if (!fs::exists(full, ec) || !fs::is_directory(full, ec)) {
        full = root;
    }

    struct Row {
        bool isDir;
        std::string nameLower;
        std::string json;
    };
    std::vector<Row> rows;
    for (const auto& entry : fs::directory_iterator(full, fs::directory_options::skip_permission_denied, ec)) {
        std::string nm = entry.path().filename().u8string();
        if (nm.empty() || nm[0] == '.') continue;
        bool d = entry.is_directory(ec);
        std::string low = nm;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        rows.push_back({d, low, buildVfsItemJson(root, entry.path())});
        if (rows.size() >= 400) break;
    }

    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.isDir != b.isDir) return a.isDir > b.isDir;
        return a.nameLower < b.nameLower;
    });

    std::ostringstream oss;
    oss << "{\"currentPath\":\"" << jsonEscape(toVirtualPosix(root, full)) << "\",\"items\":[";
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i > 0) oss << ",";
        oss << rows[i].json;
    }
    oss << "]}";
    return oss.str();
}

// --- Smart Transport Scoring & State Snapshot Builder ---

static double scoreTransport(const std::string& id, bool avail, double bw, double rtt, double power, const std::string& curActive) {
    if (!avail) return 0.0;
    double bwScore = 100.0 * std::clamp(std::log10(bw + 1.0) / std::log10(500.0), 0.0, 1.0);
    double latScore = 100.0 * std::clamp(1.0 - (rtt / 150.0), 0.0, 1.0);
    double relScore = 96.0;
    double s = 0.45 * bwScore + 0.25 * latScore + 0.20 * relScore - 0.10 * power;
    if (id == curActive) s += 6.0;
    return std::round(std::clamp(s, 0.0, 100.0) * 10.0) / 10.0;
}

static std::vector<std::string> getLocalLanIps() {
    std::vector<std::string> ips;
    ULONG outBufLen = 16384;
    std::vector<uint8_t> buf(outBufLen);
    PIP_ADAPTER_INFO pAdapterInfo = reinterpret_cast<PIP_ADAPTER_INFO>(buf.data());
    if (GetAdaptersInfo(pAdapterInfo, &outBufLen) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(outBufLen);
        pAdapterInfo = reinterpret_cast<PIP_ADAPTER_INFO>(buf.data());
    }
    if (GetAdaptersInfo(pAdapterInfo, &outBufLen) == NO_ERROR) {
        for (PIP_ADAPTER_INFO p = pAdapterInfo; p != nullptr; p = p->Next) {
            std::string ip = p->IpAddressList.IpAddress.String;
            if (!ip.empty() && ip != "0.0.0.0" && ip != "127.0.0.1") {
                ips.push_back(ip);
            }
        }
    }
    return ips;
}

static std::string buildStateSnapshotJson() {
    std::lock_guard<std::mutex> lk(gState.mtx);

    int64_t now = nowMs();
    bool hasRealAny = false;
    for (auto it = gState.phoneEndpoints.begin(); it != gState.phoneEndpoints.end();) {
        // Native Bluetooth RFCOMM probes are slower (page/SDP), so keep the link warm for longer.
        int64_t ttl = (it->first == "bluetooth") ? 35000 : 12000;
        if (now - it->second.lastOkMs > ttl) {
            it = gState.phoneEndpoints.erase(it);
        } else {
            hasRealAny = true;
            ++it;
        }
    }
    gState.realPhoneConnected = hasRealAny;

    struct TDef {
        std::string id, name, shortName, cat, note;
        bool offline;
        bool avail;
        double bw, rtt, power;
        std::string ip;
    };

    auto getEp = [&](const std::string& kind, bool simFlag, double defRtt) -> std::pair<bool, std::pair<double, std::string>> {
        auto it = gState.phoneEndpoints.find(kind);
        if (it != gState.phoneEndpoints.end()) {
            return {true, {std::max(0.3, std::round(it->second.rttMs * 10.0) / 10.0), it->second.ip}};
        }
        if (!hasRealAny) {
            return {simFlag, {defRtt, ""}};
        }
        return {false, {defRtt, ""}};
    };

    auto u = getEp("usb_rndis", gState.envUsb, 0.4);
    auto w = getEp("wifi_lan", gState.envWifi, 2.6);
    auto h = getEp("local_hotspot", gState.envHotspot, 3.8);
    auto b = getEp("bluetooth", gState.envBluetooth, 18.0);
    auto o = getEp("online_p2p", gState.envOnline, 38.0);

    std::vector<TDef> list = {
        {"usb_rndis", "USB Direct (RNDIS / ADB)", "USB Direct", "Wired Offline",
         u.first ? (u.second.second.empty() ? "USB-C high-speed data ready" : ("Connected to Phone via USB (" + u.second.second + ")")) : "Plug in USB + enable USB Tethering for 420 MB/s",
         true, u.first, 420.0, u.second.first, 0.0, u.second.second},
        {"wifi_lan", "Wi-Fi LAN (5 GHz)", "Wi-Fi LAN", "LAN Offline",
         w.first ? (w.second.second.empty() ? "Ready on local Wi-Fi (scanning for Android app...)" : ("Connected to Android Sibling at " + w.second.second)) : "Not on shared Wi-Fi router",
         true, w.first, 94.5, w.second.first, 15.0, w.second.second},
        {"local_hotspot", "Local Hotspot (Router-Free)", "Local Hotspot", "Direct Offline",
         h.first ? (h.second.second.empty() ? "5 GHz Direct SoftAP active" : ("Connected via Hotspot (" + h.second.second + ")")) : "Turn on Phone Hotspot or Windows Mobile Hotspot (100% offline)",
         true, h.first, 72.4, h.second.first, 28.0, h.second.second},
        {"bluetooth", "Bluetooth (Native RFCOMM + PAN)", "Bluetooth", "WPAN Offline",
         b.first
             ? (isBtEndpoint(b.second.second)
                    ? ("Native RFCOMM link live -> " + b.second.second + " (zero Wi-Fi / zero USB)")
                    : "Ultra-low-power Bluetooth PAN fallback link")
             : ("Pair phone + PC once in Bluetooth settings; native RFCOMM then auto-links (UUID " + kChainItPhoneUuidStr + ")"),
         true, b.first, 2.1, b.second.first, 8.0, b.second.second},
        {"online_p2p", "Online P2P / VPN", "Online P2P", "Pluggable Online",
         o.first ? ("Connected via remote IP " + o.second.second) : "Enter Tailscale/WireGuard IP below for remote internet access",
         false, o.first, 24.0, o.second.first, 48.0, o.second.second},
    };

    struct Scored {
        TDef d;
        double score;
    };
    std::vector<Scored> scored;
    for (const auto& item : list) {
        scored.push_back({item, scoreTransport(item.id, item.avail, item.bw, item.rtt, item.power, gState.activeTransportId)});
    }
    std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
        return a.score > b.score;
    });

    gState.activeTransportId.clear();
    gState.activePhoneIp.clear();
    if (gState.selectionMode == "auto") {
        for (const auto& s : scored) {
            if (s.d.avail) {
                gState.activeTransportId = s.d.id;
                gState.activePhoneIp = s.d.ip;
                break;
            }
        }
    } else {
        for (const auto& s : scored) {
            if (s.d.id == gState.manualTransportId && s.d.avail) {
                gState.activeTransportId = s.d.id;
                gState.activePhoneIp = s.d.ip;
                break;
            }
        }
    }

    ULARGE_INTEGER freeBytesAvail{}, totalNumberOfBytes{}, totalNumberOfFreeBytes{};
    uint64_t pcTotal = 512ULL * 1024 * 1024 * 1024;
    uint64_t pcUsed = 180ULL * 1024 * 1024 * 1024;
    if (GetDiskFreeSpaceExW(L"C:\\", &freeBytesAvail, &totalNumberOfBytes, &totalNumberOfFreeBytes)) {
        pcTotal = totalNumberOfBytes.QuadPart;
        pcUsed = totalNumberOfBytes.QuadPart - totalNumberOfFreeBytes.QuadPart;
    }

    auto lanIps = getLocalLanIps();
    int64_t epochRem = 900 - ((now / 1000) % 900);

    std::ostringstream oss;
    oss << "{";
    oss << "\"selectionMode\":\"" << gState.selectionMode << "\",";
    oss << "\"manualTransportId\":\"" << gState.manualTransportId << "\",";
    if (gState.activeTransportId.empty()) oss << "\"activeTransportId\":null,";
    else oss << "\"activeTransportId\":\"" << gState.activeTransportId << "\",";

    oss << "\"transports\":[";
    for (size_t i = 0; i < scored.size(); ++i) {
        if (i > 0) oss << ",";
        const auto& s = scored[i];
        bool act = (!gState.activeTransportId.empty() && s.d.id == gState.activeTransportId);
        oss << "{\"id\":\"" << s.d.id << "\","
            << "\"name\":\"" << jsonEscape(s.d.name) << "\","
            << "\"shortName\":\"" << jsonEscape(s.d.shortName) << "\","
            << "\"category\":\"" << jsonEscape(s.d.cat) << "\","
            << "\"isOffline\":" << (s.d.offline ? "true" : "false") << ","
            << "\"available\":" << (s.d.avail ? "true" : "false") << ","
            << "\"canBootstrapOnDemand\":" << (s.d.id == "local_hotspot" ? "true" : "false") << ","
            << "\"speedMBps\":" << s.d.bw << ","
            << "\"rttMs\":" << s.d.rtt << ","
            << "\"packetLossRatio\":0.001,"
            << "\"signalPct\":97,"
            << "\"powerCostIndex\":" << s.d.power << ","
            << "\"score\":" << s.score << ","
            << "\"isActive\":" << (act ? "true" : "false") << ","
            << "\"statusNote\":\"" << jsonEscape(s.d.note) << "\"}";
    }
    oss << "],";

    oss << "\"environment\":{"
        << "\"usbPlugged\":" << (u.first ? "true" : "false") << ","
        << "\"sharedWifiAvailable\":" << (w.first ? "true" : "false") << ","
        << "\"localHotspotActive\":" << (h.first ? "true" : "false") << ","
        << "\"bluetoothEnabled\":" << (b.first ? "true" : "false") << ","
        << "\"onlineP2pEnabled\":" << (o.first ? "true" : "false") << ","
        << "\"phoneScreenLocked\":" << (gState.phoneScreenLocked ? "true" : "false") << ","
        << "\"phoneBatteryPct\":" << gState.phoneBatteryPct << "},";

    // Native Bluetooth (AF_BTH / RFCOMM) live status
    gState.btRadioPresent = gBtRadioPresent.load();
    gState.btReceiverListening = gBtServerListening.load();
    gState.btReceiverChannel = gBtServerChannel.load();
    {
        std::lock_guard<std::mutex> lk(gBtPairedMtx);
        gState.btPairedDevices = gBtPairedDevices;
    }
    oss << "\"bluetooth\":{"
        << "\"nativeRfcomm\":true,"
        << "\"radioPresent\":" << (gState.btRadioPresent ? "true" : "false") << ","
        << "\"receiverListening\":" << (gState.btReceiverListening ? "true" : "false") << ","
        << "\"receiverRfcommChannel\":" << gState.btReceiverChannel << ","
        << "\"phoneServerUuid\":\"" << kChainItPhoneUuidStr << "\","
        << "\"pcReceiverUuid\":\"" << kChainItPcUuidStr << "\","
        << "\"activeLink\":\"" << jsonEscape(isBtEndpoint(gState.activePhoneIp) ? gState.activePhoneIp : std::string("")) << "\","
        << "\"pairedDevices\":[";
    for (size_t i = 0; i < gState.btPairedDevices.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "\"" << jsonEscape(gState.btPairedDevices[i]) << "\"";
    }
    oss << "]},";

    bool connected = !gState.activeTransportId.empty() && gState.siblingEnabled;
    oss << "\"sibling\":{"
        << "\"enabled\":" << (gState.siblingEnabled ? "true" : "false") << ","
        << "\"connected\":" << (connected ? "true" : "false") << ","
        << "\"manualConnectedWhenNotSibling\":true,"
        << "\"autoConnectOnRange\":" << (gState.autoConnectOnRange ? "true" : "false") << ","
        << "\"autoMountZDrive\":" << (gState.autoMountZDrive ? "true" : "false") << ","
        << "\"zeroPromptTransfers\":" << (gState.zeroPromptTransfers ? "true" : "false") << ","
        << "\"allowScreenOffVfs\":" << (gState.allowScreenOffVfs ? "true" : "false") << ","
        << "\"zDriveMounted\":" << ((connected && gState.autoMountZDrive) ? "true" : "false") << ","
        << "\"peerDeviceName\":\"" << jsonEscape(gState.peerDeviceName) << "\","
        << "\"peerModel\":\"" << jsonEscape(gState.peerModel) << "\","
        << "\"peerAndroidVersion\":\"" << jsonEscape(gState.peerAndroidVersion) << "\","
        << "\"ed25519Fingerprint\":\"9F82:C1D4:B8E7:04A1:E330:2F9D\","
        << "\"currentBleBeaconHex\":\"A74C91E05B2D8814\","
        << "\"beaconSecondsRemaining\":" << epochRem << "},";

    oss << "\"drives\":{"
        << "\"pc\":{\"target\":\"pc\",\"driveLetter\":\"C:\",\"label\":\"Windows User Profile (" << jsonEscape(gState.pcRoot.filename().u8string()) << ")\","
        << "\"totalCapacityBytes\":" << pcTotal << ",\"usedBytes\":" << pcUsed << ",\"fileCount\":24,\"folderCount\":8},"
        << "\"phone\":{\"target\":\"phone\",\"driveLetter\":\"Z:\",\"label\":\"" << jsonEscape(gState.peerDeviceName) << " [Sibling VFS]\","
        << "\"totalCapacityBytes\":" << gState.phoneTotalBytes << ",\"usedBytes\":" << gState.phoneUsedBytes << ",\"fileCount\":18,\"folderCount\":6}"
        << "},";

    oss << "\"pendingApprovals\":[],";

    oss << "\"transfers\":[";
    for (size_t i = 0; i < gState.transfers.size(); ++i) {
        if (i > 0) oss << ",";
        const auto& tr = gState.transfers[i];
        oss << "{\"id\":\"" << tr.id << "\","
            << "\"label\":\"" << jsonEscape(tr.label) << "\","
            << "\"op\":\"" << tr.op << "\","
            << "\"direction\":\"" << tr.direction << "\","
            << "\"transportUsed\":\"" << tr.transportUsed << "\","
            << "\"promotedHotspot\":false,"
            << "\"bytes\":" << tr.bytes << ","
            << "\"durationMs\":8,"
            << "\"effectiveSpeedMBps\":" << tr.speedMBps << ","
            << "\"status\":\"completed\","
            << "\"timestampMs\":" << tr.ts << "}";
    }
    oss << "],";

    oss << "\"logs\":[";
    for (size_t i = 0; i < gState.logs.size(); ++i) {
        if (i > 0) oss << ",";
        const auto& lg = gState.logs[i];
        oss << "{\"id\":\"" << lg.id << "\","
            << "\"timestampMs\":" << lg.ts << ","
            << "\"layer\":\"" << lg.layer << "\","
            << "\"summary\":\"" << jsonEscape(lg.summary) << "\","
            << "\"detail\":\"" << jsonEscape(lg.detail) << "\"}";
    }
    oss << "],";

    oss << "\"lanAddresses\":[";
    for (size_t i = 0; i < lanIps.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "\"" << jsonEscape(lanIps[i]) << "\"";
    }
    oss << "],\"serverPort\":" << HTTP_PORT << "}";
    return oss.str();
}

// --- Probe & Register Real Android Phone Endpoint ---

static bool probePhoneIp(const std::string& ip, const std::string& kindHint, int timeoutMs = 0) {
    // Native Bluetooth RFCOMM needs a longer budget (SDP lookup + ACL page + pairing check).
    if (timeoutMs <= 0) timeoutMs = isBtEndpoint(ip) ? 9000 : 1500;
    HttpReply rep = httpRequestToPhone(ip, PHONE_VFS_PORT, "GET", "/chainit/ping", "", {}, timeoutMs);
    if (rep.status != 200 || rep.body.find("\"ok\":true") == std::string::npos) {
        return false;
    }

    std::string kind = kindHint;
    if (kind.empty()) {
        if (isBtEndpoint(ip)) kind = "bluetooth";
        else if (ip == "127.0.0.1" || ip.rfind("192.168.42.", 0) == 0) kind = "usb_rndis";
        else if (ip.rfind("192.168.43.", 0) == 0 || ip.rfind("192.168.49.", 0) == 0 || ip.rfind("192.168.137.", 0) == 0) kind = "local_hotspot";
        else if (ip.rfind("192.168.44.", 0) == 0) kind = "bluetooth";
        else if (ip.rfind("100.", 0) == 0) kind = "online_p2p";
        else kind = "wifi_lan";
    }

    bool newlyConnected = false;
    {
        std::lock_guard<std::mutex> lk(gState.mtx);
        newlyConnected = !gState.realPhoneConnected;
        gState.phoneEndpoints[kind] = {ip, kind, rep.rttMs, nowMs()};
        gState.peerDeviceName = jsonExtractString(rep.body, "deviceName", "Android Sibling");
        gState.peerModel = jsonExtractString(rep.body, "model", "Android");
        gState.peerAndroidVersion = jsonExtractString(rep.body, "androidVersion", "Android");
        gState.phoneBatteryPct = static_cast<int>(jsonExtractInt(rep.body, "batteryPct", 88));
        gState.phoneScreenLocked = jsonExtractBool(rep.body, "screenLocked", false);
        gState.phoneTotalBytes = static_cast<uint64_t>(jsonExtractInt(rep.body, "totalBytes", static_cast<int64_t>(gState.phoneTotalBytes)));
        gState.phoneUsedBytes = static_cast<uint64_t>(jsonExtractInt(rep.body, "usedBytes", static_cast<int64_t>(gState.phoneUsedBytes)));

        if (newlyConnected) {
            std::string link = isBtEndpoint(ip)
                                   ? ("native Bluetooth RFCOMM " + ip)
                                   : ("IP " + ip + ":49180");
            addLogLocked("SIBLING", "Auto-Connected to " + gState.peerDeviceName + " (" + link + ")",
                         isBtEndpoint(ip)
                             ? "Verified ChainIt-Android.apk over AF_BTH RFCOMM (UUID " + kChainItPhoneUuidStr + "); Z:\\ live storage ready with zero Wi-Fi."
                             : "Verified ChainIt-Android.apk on port 49180; Z:\\ live storage ready.");
        }
    }

    if (newlyConnected) {
        mountZDriveAsync();
    }
    return true;
}

// --- Background UDP Discovery Thread ---

static void discoveryLoop() {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return;

    BOOL yes = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    DWORD rcvTimeout = 500;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcvTimeout), sizeof(rcvTimeout));

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    bindAddr.sin_port = htons(0);
    bind(sock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr));

    char compName[128] = "Windows-PC";
    DWORD compLen = sizeof(compName);
    GetComputerNameA(compName, &compLen);

    std::string discoverPacket = std::string("{\"magic\":\"CHAINIT_DISCOVER_V1\",\"pcName\":\"") +
                                 jsonEscape(compName) + "\",\"httpPort\":4173}";

    int tick = 0;
    while (gRunning.load()) {
        // 1. Broadcast UDP discovery packet on 255.255.255.255 + subnet broadcasts
        std::vector<std::string> bcasts = {"255.255.255.255"};
        for (const auto& lip : getLocalLanIps()) {
            size_t lastDot = lip.rfind('.');
            if (lastDot != std::string::npos) {
                bcasts.push_back(lip.substr(0, lastDot + 1) + "255");
            }
        }
        for (const auto& b : bcasts) {
            sockaddr_in dst{};
            dst.sin_family = AF_INET;
            dst.sin_port = htons(DISCOVERY_PORT);
            if (inet_pton(AF_INET, b.c_str(), &dst.sin_addr) == 1) {
                sendto(sock, discoverPacket.data(), static_cast<int>(discoverPacket.size()), 0,
                       reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
            }
        }

        // 2. Listen for replies for ~600ms
        for (int i = 0; i < 3; ++i) {
            sockaddr_in sender{};
            int senderLen = sizeof(sender);
            char buf[4096];
            int r = recvfrom(sock, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&sender), &senderLen);
            if (r > 0) {
                buf[r] = '\0';
                std::string msg(buf, static_cast<size_t>(r));
                if (msg.find("CHAINIT_SIBLING_ANNOUNCE_V1") != std::string::npos) {
                    char senderIp[64] = {};
                    inet_ntop(AF_INET, &sender.sin_addr, senderIp, sizeof(senderIp));
                    probePhoneIp(senderIp, "");
                }
            }
        }

        // 3. Periodically probe USB ADB loopback (127.0.0.1:49180), USB RNDIS, and Android Hotspot gateways!
        if (tick % 2 == 0) {
            probePhoneIp("127.0.0.1", "usb_rndis");
            probePhoneIp("192.168.42.129", "usb_rndis");
            probePhoneIp("192.168.43.1", "local_hotspot");
            probePhoneIp("192.168.49.1", "local_hotspot");
        }

        // 4. Refresh existing known endpoints
        std::vector<std::pair<std::string, std::string>> known;
        {
            std::lock_guard<std::mutex> lk(gState.mtx);
            for (const auto& kv : gState.phoneEndpoints) {
                known.push_back({kv.second.ip, kv.first});
            }
        }
        for (const auto& p : known) {
            probePhoneIp(p.first, p.second);
        }

        tick++;
        std::this_thread::sleep_for(std::chrono::milliseconds(900));
    }

    closesocket(sock);
}

// --- Native Bluetooth Threads (AF_BTH / ws2bth.h) ---

static void handleHttpClient(SOCKET client); // fwd-decl: the RFCOMM listener reuses the whole HTTP+WebDAV engine

// Enumerates remembered / paired Bluetooth devices straight from the Windows Bluetooth namespace.
// LUP_CONTAINERS reads the stack cache, so this costs no 12-second inquiry scan and no user prompt.
static std::vector<std::string> enumeratePairedBluetoothDevices() {
    std::vector<std::string> found;

    WSAQUERYSETW qs{};
    qs.dwSize = sizeof(WSAQUERYSETW);
    qs.dwNameSpace = NS_BTH;

    HANDLE hLookup = nullptr;
    if (WSALookupServiceBeginW(&qs, LUP_CONTAINERS | LUP_RETURN_NAME | LUP_RETURN_ADDR, &hLookup) != 0) {
        gBtRadioPresent.store(false);
        return found;
    }
    gBtRadioPresent.store(true);

    size_t bufSize = 8192;
    std::vector<char> buf(bufSize);
    while (gRunning.load()) {
        DWORD bufLen = static_cast<DWORD>(buf.size());
        auto* res = reinterpret_cast<WSAQUERYSETW*>(buf.data());
        int r = WSALookupServiceNextW(hLookup, 0, &bufLen, res);
        if (r != 0) {
            if (WSAGetLastError() == WSAEFAULT && buf.size() < 131072) {
                buf.resize(buf.size() * 2);
                continue;
            }
            break;
        }
        if (res->dwNumberOfCsAddrs < 1 || !res->lpcsaBuffer) continue;
        auto* sa = reinterpret_cast<SOCKADDR_BTH*>(res->lpcsaBuffer->RemoteAddr.lpSockaddr);
        if (!sa || sa->addressFamily != AF_BTH || sa->btAddr == BTH_ADDR_NULL) continue;
        std::string ep = "bt://" + btAddrToString(sa->btAddr);
        if (std::find(found.begin(), found.end(), ep) == found.end()) found.push_back(ep);
    }
    WSALookupServiceEnd(hLookup);
    return found;
}

// Keeps a native RFCOMM link to the phone warm: paired devices are probed until ChainIt-Android answers
// on UUID c4a1b170-0001-4000-8000-00805f9b34fb, then the endpoint is refreshed every cycle so the
// transport engine can fail over to Bluetooth instantly (Wi-Fi / Hotspot / USB all OFF).
static void bluetoothDiscoveryLoop() {
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    int tick = 0;
    bool btWasUp = false;
    bool loggedNoRadio = false;

    while (gRunning.load()) {
        std::string liveBt;
        {
            std::lock_guard<std::mutex> lk(gState.mtx);
            auto it = gState.phoneEndpoints.find("bluetooth");
            if (it != gState.phoneEndpoints.end() && isBtEndpoint(it->second.ip)) liveBt = it->second.ip;
        }

        std::vector<std::string> candidates;
        if (!liveBt.empty()) candidates.push_back(liveBt);

        if (liveBt.empty() || tick % 8 == 0) {
            std::vector<std::string> paired = enumeratePairedBluetoothDevices();
            {
                std::lock_guard<std::mutex> lk(gBtPairedMtx);
                gBtPairedDevices = paired;
            }
            if (!gBtRadioPresent.load()) {
                if (!loggedNoRadio) {
                    loggedNoRadio = true;
                    std::lock_guard<std::mutex> lk(gState.mtx);
                    addLogLocked("BLUETOOTH", "No usable Bluetooth radio detected",
                                 "Enable Bluetooth on this PC (Settings > Bluetooth & devices) to arm the native RFCOMM transport.");
                }
            } else if (loggedNoRadio) {
                loggedNoRadio = false;
                std::lock_guard<std::mutex> lk(gState.mtx);
                addLogLocked("BLUETOOTH", "Bluetooth radio is back online", "Arming native RFCOMM transport (AF_BTH) for Sibling Mode.");
            }
            for (const auto& p : paired) {
                if (p != liveBt && std::find(candidates.begin(), candidates.end(), p) == candidates.end()) {
                    candidates.push_back(p);
                }
            }
        }

        size_t limit = std::min<size_t>(candidates.size(), 6);
        for (size_t i = 0; i < limit && gRunning.load(); ++i) {
            if (probePhoneIp(candidates[i], "bluetooth", 4000)) break;
        }

        bool upNow = false;
        std::string upEp;
        {
            std::lock_guard<std::mutex> lk(gState.mtx);
            auto it = gState.phoneEndpoints.find("bluetooth");
            if (it != gState.phoneEndpoints.end() && isBtEndpoint(it->second.ip)) {
                upNow = true;
                upEp = it->second.ip;
            }
        }
        if (upNow && !btWasUp) {
            std::lock_guard<std::mutex> lk(gState.mtx);
            addLogLocked("BLUETOOTH", "Native Bluetooth RFCOMM link up -> " + upEp,
                         "AF_BTH socket bound to phone UUID " + kChainItPhoneUuidStr + ". Z:\\ and the File Manager now run on Bluetooth alone (zero Wi-Fi, zero USB).");
        } else if (!upNow && btWasUp) {
            std::lock_guard<std::mutex> lk(gState.mtx);
            addLogLocked("BLUETOOTH", "Bluetooth RFCOMM link lost",
                         "Phone left range or ChainIt-Android was closed; the engine fails over to the next best transport automatically.");
        }
        btWasUp = upNow;

        tick++;
        std::this_thread::sleep_for(std::chrono::milliseconds(3500));
    }
}

// Publishes the PC-side RFCOMM service (UUID c4a1b170-0002-4000-8000-00805f9b34fb) so the Android app can
// push files to this PC over Bluetooth alone. Accepted sockets are handed to the regular HTTP engine, so
// "POST /api/vfs/pc/upload?dir=/Desktop" from the phone lands straight on the Windows Desktop.
static void bluetoothServerLoop() {
    std::this_thread::sleep_for(std::chrono::milliseconds(900));

    while (gRunning.load()) {
        SOCKET srv = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
        if (srv == INVALID_SOCKET) {
            gBtServerListening.store(false);
            std::this_thread::sleep_for(std::chrono::seconds(20));
            continue;
        }

        SOCKADDR_BTH sa{};
        sa.addressFamily = AF_BTH;
        sa.btAddr = BTH_ADDR_NULL;          // local radio
        sa.serviceClassId = kChainItPcUuid;
        sa.port = BT_PORT_ANY;              // stack assigns a free RFCOMM channel

        if (bind(srv, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == SOCKET_ERROR || listen(srv, 4) == SOCKET_ERROR) {
            closesocket(srv);
            gBtServerListening.store(false);
            std::this_thread::sleep_for(std::chrono::seconds(20));
            continue;
        }

        SOCKADDR_BTH bound = sa;
        int boundLen = sizeof(bound);
        if (getsockname(srv, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0 && bound.port != 0 && bound.port != BT_PORT_ANY) {
            gBtServerChannel.store(static_cast<int>(bound.port));
        } else {
            bound = sa;
            gBtServerChannel.store(0);
        }

        SOCKADDR_BTH remote{};
        remote.addressFamily = AF_BTH;
        remote.btAddr = BTH_ADDR_NULL;
        remote.serviceClassId = kChainItPcUuid;
        remote.port = BT_PORT_ANY;

        CSADDR_INFO csai{};
        csai.iSocketType = SOCK_STREAM;
        csai.iProtocol = BTHPROTO_RFCOMM;
        csai.LocalAddr.iSockaddrLength = sizeof(SOCKADDR_BTH);
        csai.LocalAddr.lpSockaddr = reinterpret_cast<LPSOCKADDR>(&bound);
        csai.RemoteAddr.iSockaddrLength = sizeof(SOCKADDR_BTH);
        csai.RemoteAddr.lpSockaddr = reinterpret_cast<LPSOCKADDR>(&remote);

        GUID svcId = kChainItPcUuid;
        WSAQUERYSETW wsq{};
        wsq.dwSize = sizeof(WSAQUERYSETW);
        wsq.dwNameSpace = NS_BTH;
        wsq.lpServiceClassId = &svcId;
        wsq.lpszServiceInstanceName = const_cast<LPWSTR>(L"ChainIt PC Receiver");
        wsq.lpszComment = const_cast<LPWSTR>(L"ChainIt Sibling Bluetooth inbox - pushes from Android land on the Windows Desktop");
        wsq.dwNumberOfCsAddrs = 1;
        wsq.lpcsaBuffer = &csai;

        bool published = (WSASetServiceW(&wsq, RNRSERVICE_REGISTER, 0) == 0);
        gBtServerListening.store(true);
        {
            std::lock_guard<std::mutex> lk(gState.mtx);
            addLogLocked("BLUETOOTH",
                         published ? ("Bluetooth PC Receiver published (RFCOMM channel " + std::to_string(gBtServerChannel.load()) + ")")
                                   : "Bluetooth receiver socket open (SDP publish blocked)",
                         published ? ("Paired Android phones resolve UUID " + kChainItPcUuidStr + " and their Bluetooth pushes land in C:\\Users\\<you>\\Desktop.")
                                   : ("Listening on AF_BTH RFCOMM; pair this PC with the phone so Android can resolve UUID " + kChainItPcUuidStr + "."));
        }

        while (gRunning.load()) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(srv, &rfds);
            timeval tv{};
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            if (select(0, &rfds, nullptr, nullptr, &tv) <= 0) continue;

            SOCKET client = accept(srv, nullptr, nullptr);
            if (client == INVALID_SOCKET) continue;

            DWORD t = 180000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));

            {
                std::lock_guard<std::mutex> lk(gState.mtx);
                addLogLocked("BLUETOOTH", "Incoming Bluetooth RFCOMM session accepted",
                             "Phone -> PC push over AF_BTH; file is written to the Desktop and logged as a completed transfer.");
            }
            std::thread(handleHttpClient, client).detach();
        }

        gBtServerListening.store(false);
        WSASetServiceW(&wsq, RNRSERVICE_DELETE, 0);
        closesocket(srv);
        if (gRunning.load()) std::this_thread::sleep_for(std::chrono::seconds(10));
    }
}

// --- Recursive Cross-Device Copy / Move Helper ---

static uint64_t uploadLocalPathToPhone(const fs::path& localPath, const std::string& phoneIp, const std::string& phoneDestDir) {
    std::error_code ec;
    if (fs::is_directory(localPath, ec)) {
        std::string folderName = localPath.filename().u8string();
        std::string mkBody = "{\"parentDir\":\"" + jsonEscape(phoneDestDir) + "\",\"name\":\"" + jsonEscape(folderName) + "\"}";
        httpRequestToPhone(phoneIp, PHONE_VFS_PORT, "POST", "/chainit/mkdir", mkBody, {{"Content-Type", "application/json"}}, 5000);
        std::string subDest = (phoneDestDir == "/" ? "" : phoneDestDir) + "/" + folderName;
        uint64_t sum = 0;
        for (const auto& child : fs::directory_iterator(localPath, ec)) {
            sum += uploadLocalPathToPhone(child.path(), phoneIp, subDest);
        }
        return sum;
    }

    std::ifstream ifs(localPath, std::ios::binary);
    if (!ifs) return 0;
    std::ostringstream ss;
    ss << ifs.rdbuf();
    std::string data = ss.str();

    std::string fname = localPath.filename().u8string();
    std::string url = "/chainit/upload?dir=" + urlEncode(phoneDestDir) + "&filename=" + urlEncode(fname);
    httpRequestToPhone(phoneIp, PHONE_VFS_PORT, "POST", url, data, {{"Content-Type", "application/octet-stream"}}, 60000);
    return data.size();
}

static uint64_t downloadPhonePathToLocal(const std::string& phoneIp, const std::string& phoneSrcPath, const fs::path& localDestDir) {
    std::error_code ec;
    fs::create_directories(localDestDir, ec);
    std::string baseName = fs::u8path(phoneSrcPath).filename().u8string();
    if (baseName.empty()) baseName = "Phone_Item";

    // Check if phoneSrcPath is a directory by querying /chainit/list
    HttpReply listRep = httpRequestToPhone(phoneIp, PHONE_VFS_PORT, "GET", "/chainit/list?path=" + urlEncode(phoneSrcPath), "", {}, 4000);
    if (listRep.status == 200 && listRep.body.find("\"items\":[") != std::string::npos) {
        fs::path subLocal = localDestDir / fs::u8path(baseName);
        fs::create_directories(subLocal, ec);
        // Extract child paths from items array
        uint64_t sum = 0;
        size_t pos = listRep.body.find("\"items\":[");
        while ((pos = listRep.body.find("\"path\":\"", pos)) != std::string::npos) {
            pos += 8;
            size_t endQ = listRep.body.find('"', pos);
            if (endQ == std::string::npos) break;
            std::string childPath = listRep.body.substr(pos, endQ - pos);
            pos = endQ + 1;
            if (!childPath.empty() && childPath != phoneSrcPath) {
                sum += downloadPhonePathToLocal(phoneIp, childPath, subLocal);
            }
        }
        return sum;
    }

    HttpReply rawRep = httpRequestToPhone(phoneIp, PHONE_VFS_PORT, "GET", "/chainit/raw?path=" + urlEncode(phoneSrcPath), "", {}, 60000);
    if (rawRep.status == 200) {
        fs::path dst = localDestDir / fs::u8path(baseName);
        std::ofstream ofs(dst, std::ios::binary);
        ofs.write(rawRep.body.data(), static_cast<std::streamsize>(rawRep.body.size()));
        return rawRep.body.size();
    }
    return 0;
}

// --- HTTP Response Helpers ---

// ---------------------------------------------------------------------------
// Desktop shortcut ("create desktop icon")
//
// Writes a real Windows .lnk on the user's Desktop that points at the running
// ChainIt-Windows.exe and uses the chain-link icon embedded in this binary as its
// icon, so the desktop icon matches the Android launcher icon exactly.
// ---------------------------------------------------------------------------
static std::wstring currentExePath();

static const GUID kClsidShellLink = {0x00021401, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID kIidIShellLinkW = {0x000214F9, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID kIidIPersistFile = {0x0000010B, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

struct ShortcutResult {
    bool ok = false;
    std::wstring path;
    std::string message;
};

static ShortcutResult createDesktopShortcut() {
    ShortcutResult result;
    const std::wstring exe = currentExePath();
    if (exe.empty()) {
        result.message = "Could not resolve the running ChainIt-Windows.exe path.";
        return result;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool comOwned = SUCCEEDED(hr);

    PWSTR desktopRaw = nullptr;
    hr = SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktopRaw);
    if (FAILED(hr) || !desktopRaw) {
        if (comOwned) CoUninitialize();
        result.message = "Windows did not report a Desktop folder for this account.";
        return result;
    }
    std::wstring desktop(desktopRaw);
    CoTaskMemFree(desktopRaw);

    IShellLinkW* link = nullptr;
    hr = CoCreateInstance(kClsidShellLink, nullptr, CLSCTX_INPROC_SERVER, kIidIShellLinkW,
                          reinterpret_cast<void**>(&link));
    if (FAILED(hr) || !link) {
        if (comOwned) CoUninitialize();
        result.message = "Windows refused to create a shortcut object.";
        return result;
    }

    const std::wstring workDir = fs::path(exe).parent_path().wstring();
    link->SetPath(exe.c_str());
    link->SetWorkingDirectory(workDir.c_str());
    link->SetIconLocation(exe.c_str(), 0); // the chain-link icon embedded in this exe
    link->SetDescription(L"ChainIt - Android/Windows Sibling link, Z:\\ drive and File Manager");
    link->SetShowCmd(SW_SHOWNORMAL);

    IPersistFile* persist = nullptr;
    hr = link->QueryInterface(kIidIPersistFile, reinterpret_cast<void**>(&persist));
    if (SUCCEEDED(hr) && persist) {
        const std::wstring lnk = desktop + L"\\ChainIt.lnk";
        hr = persist->Save(lnk.c_str(), TRUE);
        persist->Release();
        if (SUCCEEDED(hr)) {
            result.ok = true;
            result.path = lnk;
            SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW, lnk.c_str(), nullptr);
        }
    }
    link->Release();
    if (comOwned) CoUninitialize();

    if (result.ok) {
        result.message = "Desktop icon created: ChainIt.lnk (it uses the same chain-link icon as the phone app).";
    } else {
        result.message = "Windows could not save ChainIt.lnk on the Desktop.";
    }
    return result;
}

// ---------------------------------------------------------------------------
// In-place application update (a real replacement — never a second install)
//
// The published binary lives next to this source in releases/. The updater
//   1. reads releases/version.txt (version + SHA-256 of the published exe),
//   2. compares it with the SHA-256 of the exe that is running right now,
//   3. downloads the new exe to %TEMP%\ChainIt-Update and verifies its hash,
//   4. writes a tiny detached .cmd that overwrites the installed file and
//      relaunches it, then exits this process so the file lock is released.
// Nothing is ever installed side-by-side: the same path is overwritten, the
// same tray/UI/single-instance identity is reused, and settings survive
// because they live in %APPDATA%\ChainIt, not in the binary.
// ---------------------------------------------------------------------------
// Tray icon handle (declared early so the updater can drop the icon before it exits).
static NOTIFYICONDATAW gNid{};

static const char* const kChainItAppVersion = "1.2.0";

static std::vector<std::wstring> updateUrlCandidates() {
    std::vector<std::wstring> urls;
    wchar_t buf[2048] = {};
    DWORD n = GetEnvironmentVariableW(L"CHAINIT_UPDATE_URL", buf, 2047);
    if (n > 0 && n < 2047) urls.push_back(std::wstring(buf));
    urls.push_back(L"https://github.com/mubareksam/chainit/raw/main/releases/ChainIt-Windows.exe");
    urls.push_back(L"https://github.com/mubareksam/chainit/raw/arena/37fd3890-chainit/releases/ChainIt-Windows.exe");
    return urls;
}

static std::wstring updateVersionFileUrl() {
    // Same directory as the binary: swap the trailing filename for version.txt.
    for (const auto& u : updateUrlCandidates()) {
        size_t slash = u.find_last_of(L'/');
        if (slash != std::wstring::npos) return u.substr(0, slash + 1) + L"version.txt";
    }
    return L"";
}

static std::wstring currentExePath() {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2 - 1);
    if (n == 0 || n >= MAX_PATH * 2 - 1) return L"";
    return std::wstring(buf, n);
}

static std::wstring chainItTempDir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetTempPathW(MAX_PATH, buf);
    std::wstring p = (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : std::wstring(L"C:\\Windows\\Temp");
    if (!p.empty() && p.back() != L'\\') p.push_back(L'\\');
    return p;
}

static std::string sha256HexFile(const std::wstring& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) return "";
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    if (!CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return "";
    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        return "";
    }
    char chunk[65536];
    while (in) {
        in.read(chunk, sizeof(chunk));
        std::streamsize got = in.gcount();
        if (got > 0) CryptHashData(hHash, reinterpret_cast<const BYTE*>(chunk), static_cast<DWORD>(got), 0);
    }
    BYTE digest[32] = {};
    DWORD len = sizeof(digest);
    CryptGetHashParam(hHash, HP_HASHVAL, digest, &len, 0);
    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (DWORD i = 0; i < len; ++i) {
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0x0F]);
    }
    return out;
}

static std::string readFileUtf8(const std::wstring& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// key=value lookup inside version.txt (tolerates CRLF, spaces and comments).
static std::string versionFileField(const std::string& text, const std::string& key) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? text.size() : eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t lead = line.find_first_not_of(" \t");
        if (lead == std::string::npos || line[lead] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(lead, eq - lead);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        if (k != key) continue;
        std::string v = line.substr(eq + 1);
        size_t vs = v.find_first_not_of(" \t");
        if (vs == std::string::npos) return "";
        return v.substr(vs);
    }
    return "";
}

static HRESULT downloadFresh(const std::wstring& url, const std::wstring& dest) {
    DeleteFileW(dest.c_str());
    // Purge any WinINET copy first so we never "update" from a stale cache entry.
    DeleteUrlCacheEntryW(url.c_str());
    return URLDownloadToFileW(nullptr, url.c_str(), dest.c_str(), 0, nullptr);
}

static std::wstring wideFromAnsi(const std::string& in) {
    if (in.empty()) return L"";
    int n = MultiByteToWideChar(CP_ACP, 0, in.c_str(), static_cast<int>(in.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, in.c_str(), static_cast<int>(in.size()), out.data(), n);
    return out;
}

static std::string ansiFromWide(const std::wstring& in) {
    if (in.empty()) return "";
    int n = WideCharToMultiByte(CP_ACP, 0, in.c_str(), static_cast<int>(in.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_ACP, 0, in.c_str(), static_cast<int>(in.size()), out.data(), n, nullptr, nullptr);
    return out;
}

struct AppUpdateResult {
    std::string status = "error"; // updated | up_to_date | unsupported | error
    std::string message;
    std::string installedVersion = kChainItAppVersion;
    std::string latestVersion = kChainItAppVersion;
    bool restart = false;
    std::wstring updaterScript;
};

static bool writeUpdaterScript(const std::wstring& scriptPath, const std::wstring& targetExe,
                              const std::wstring& stagedExe) {
    // cmd.exe reads batch files in the ANSI codepage, so escape backslashes and write ANSI.
    std::string body;
    body += "@echo off\r\n";
    body += "rem ChainIt in-place updater: overwrite the installed binary, then relaunch it.\r\n";
    body += "set \"CHAINIT_TARGET=" + ansiFromWide(targetExe) + "\"\r\n";
    body += "set \"CHAINIT_STAGE=" + ansiFromWide(stagedExe) + "\"\r\n";
    body += "set /a CHAINIT_TRIES=0\r\n";
    body += ":chainit_retry\r\n";
    body += "ping -n 2 127.0.0.1 >nul\r\n";
    body += "copy /y \"%CHAINIT_STAGE%\" \"%CHAINIT_TARGET%\" >nul 2>&1\r\n";
    body += "if not errorlevel 1 goto chainit_done\r\n";
    body += "set /a CHAINIT_TRIES+=1\r\n";
    body += "if %CHAINIT_TRIES% LSS 10 goto chainit_retry\r\n";
    body += "echo replace-failed > \"%TEMP%\\chainit-update.log\"\r\n";
    body += ":chainit_done\r\n";
    body += "del \"%CHAINIT_STAGE%\" >nul 2>&1\r\n";
    body += "start \"\" \"%CHAINIT_TARGET%\"\r\n";
    body += "del \"%~f0\" >nul 2>&1\r\n";

    std::ofstream out(scriptPath.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    return static_cast<bool>(out);
}

static bool launchDetached(const std::wstring& commandLine) {
    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    std::wstring mutableCmd = commandLine;
    mutableCmd.push_back(L'\0');
    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                        DETACHED_PROCESS | CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr,
                        &si, &pi)) {
        // CREATE_BREAKAWAY_FROM_JOB is rejected when we are not inside a job object.
        if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                            DETACHED_PROCESS | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            return false;
        }
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static AppUpdateResult performAppUpdate() {
    AppUpdateResult result;
    const std::wstring exe = currentExePath();
    if (exe.empty()) {
        result.message = "Could not resolve the running ChainIt-Windows.exe path.";
        return result;
    }

    const std::wstring stageDir = chainItTempDir() + L"ChainIt-Update";
    CreateDirectoryW(stageDir.c_str(), nullptr);
    const std::wstring staged = stageDir + L"\\ChainIt-Windows-new.exe";
    const std::wstring manifest = stageDir + L"\\version.txt";
    const std::wstring script = stageDir + L"\\ChainIt-Updater.cmd";

    const std::string currentHash = sha256HexFile(exe);

    // --- 1) published manifest (optional but preferred) -------------------
    std::string latestVersion = kChainItAppVersion;
    std::string publishedHash;
    const std::wstring manifestUrl = updateVersionFileUrl();
    if (!manifestUrl.empty() && SUCCEEDED(downloadFresh(manifestUrl, manifest))) {
        const std::string text = readFileUtf8(manifest);
        const std::string v = versionFileField(text, "version");
        const std::string h = versionFileField(text, "exe_sha256");
        if (!v.empty()) latestVersion = v;
        if (h.size() == 64) publishedHash = h;
    }
    result.latestVersion = latestVersion;

    // --- 2) already current? ---------------------------------------------
    if (!publishedHash.empty() && !currentHash.empty() && publishedHash == currentHash) {
        DeleteFileW(staged.c_str());
        result.status = "up_to_date";
        result.message = "You are already running the newest build (v" + latestVersion +
                         "). Nothing was replaced.";
        return result;
    }

    // --- 3) download the new binary --------------------------------------
    HRESULT hr = E_FAIL;
    std::wstring usedUrl;
    for (const auto& url : updateUrlCandidates()) {
        hr = downloadFresh(url, staged);
        if (SUCCEEDED(hr)) {
            usedUrl = url;
            break;
        }
    }
    if (FAILED(hr)) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Could not download the update (HRESULT 0x%08lX). ChainIt stays on v%s.",
                      static_cast<unsigned long>(hr), kChainItAppVersion);
        result.message = buf;
        return result;
    }

    // --- 4) verify before replacing anything -----------------------------
    std::ifstream probe(staged.c_str(), std::ios::binary);
    char magic[2] = {0, 0};
    probe.read(magic, 2);
    probe.close();
    LARGE_INTEGER fileSize{};
    {
        HANDLE hFile = CreateFileW(staged.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            fileSize.QuadPart = 0;
            fileSize.LowPart = GetFileSize(hFile, reinterpret_cast<DWORD*>(&fileSize.HighPart));
            CloseHandle(hFile);
        }
    }
    if (magic[0] != 'M' || magic[1] != 'Z' || fileSize.QuadPart < 200000) {
        DeleteFileW(staged.c_str());
        result.message = "The downloaded file is not a valid ChainIt binary, so nothing was replaced.";
        return result;
    }
    const std::string stagedHash = sha256HexFile(staged);
    if (!publishedHash.empty() && stagedHash != publishedHash) {
        DeleteFileW(staged.c_str());
        result.message = "Integrity check failed (SHA-256 mismatch), so nothing was replaced.";
        return result;
    }
    if (!currentHash.empty() && stagedHash == currentHash) {
        DeleteFileW(staged.c_str());
        result.status = "up_to_date";
        result.message = "The published build is identical to the one you are running (v" + latestVersion + ").";
        return result;
    }

    // --- 5) overwrite in place and relaunch ------------------------------
    if (!writeUpdaterScript(script, exe, staged)) {
        DeleteFileW(staged.c_str());
        result.message = "Could not write the updater script to " + ansiFromWide(stageDir) + ".";
        return result;
    }
    if (!launchDetached(L"cmd.exe /c \"" + script + L"\"")) {
        DeleteFileW(staged.c_str());
        DeleteFileW(script.c_str());
        result.message = "Windows refused to start the updater script.";
        return result;
    }

    addLogLocked("UPDATE", "Replacing ChainIt-Windows.exe in place",
                 "v" + result.installedVersion + " -> v" + latestVersion + " from " +
                     ansiFromWide(usedUrl) + " (sha256 " + stagedHash.substr(0, 12) + "...)");

    result.status = "updated";
    result.restart = true;
    result.updaterScript = script;
    result.message = "Downloaded v" + latestVersion +
                     ". ChainIt is replacing the installed program now and will restart itself in a second.";
    return result;
}

// Gives the HTTP response time to reach the UI before this process steps aside.
static void scheduleRestartForUpdate() {
    std::thread([]() {
        for (int i = 0; i < 12 && gRunning.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        gRunning.store(false);
        Shell_NotifyIconW(NIM_DELETE, &gNid);
        ExitProcess(0);
    }).detach();
}

static std::string appUpdateJson(const AppUpdateResult& r) {
    auto esc = [](const std::string& in) {
        std::string out;
        for (char c : in) {
            if (c == '"' || c == '\\') out.push_back('\\');
            out.push_back(c);
        }
        return out;
    };
    std::ostringstream ss;
    ss << "{\"status\":\"" << r.status << "\",\"message\":\"" << esc(r.message)
       << "\",\"version\":\"" << esc(r.installedVersion) << "\",\"latestVersion\":\""
       << esc(r.latestVersion) << "\",\"restart\":" << (r.restart ? "true" : "false") << "}";
    return ss.str();
}

static void sendHttpResponse(
    SOCKET client,
    int status,
    const std::string& reason,
    const std::string& contentType,
    const void* body,
    size_t bodyLen,
    const std::map<std::string, std::string>& extraHeaders = {}
) {
    std::ostringstream hdr;
    hdr << "HTTP/1.1 " << status << " " << reason << "\r\n";
    hdr << "Content-Type: " << contentType << "\r\n";
    hdr << "Content-Length: " << bodyLen << "\r\n";
    hdr << "Access-Control-Allow-Origin: *\r\n";
    hdr << "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS, PROPFIND, MKCOL, MOVE, COPY, LOCK, UNLOCK, PROPPATCH\r\n";
    hdr << "Access-Control-Allow-Headers: Content-Type, Depth, Destination, Overwrite, Range\r\n";
    hdr << "Connection: close\r\n";
    for (const auto& kv : extraHeaders) {
        hdr << kv.first << ": " << kv.second << "\r\n";
    }
    hdr << "\r\n";
    std::string h = hdr.str();
    send(client, h.data(), static_cast<int>(h.size()), 0);
    const char* p = static_cast<const char*>(body);
    size_t off = 0;
    while (off < bodyLen) {
        int chunk = static_cast<int>(std::min<size_t>(65536, bodyLen - off));
        int n = send(client, p + off, chunk, 0);
        if (n <= 0) break;
        off += static_cast<size_t>(n);
    }
}

static void sendJsonStr(SOCKET client, int status, const std::string& json) {
    sendHttpResponse(client, status, status == 200 ? "OK" : "Bad Request", "application/json; charset=utf-8", json.data(), json.size());
}

// --- WebSocket Handshake (RFC 6455) so Browser Live Connection Works Smoothly ---

static bool performWebSocketHandshake(SOCKET client, const std::string& secKey) {
    std::string combined = secKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    if (!CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) return false;
    if (!CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        return false;
    }
    CryptHashData(hHash, reinterpret_cast<const BYTE*>(combined.data()), static_cast<DWORD>(combined.size()), 0);
    BYTE sha1[20];
    DWORD shaLen = 20;
    CryptGetHashParam(hHash, HP_HASHVAL, sha1, &shaLen, 0);
    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);

    char b64[64] = {};
    DWORD b64Len = sizeof(b64);
    CryptBinaryToStringA(sha1, shaLen, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64, &b64Len);

    std::ostringstream resp;
    resp << "HTTP/1.1 101 Switching Protocols\r\n"
         << "Upgrade: websocket\r\n"
         << "Connection: Upgrade\r\n"
         << "Sec-WebSocket-Accept: " << b64 << "\r\n\r\n";
    std::string rStr = resp.str();
    return send(client, rStr.data(), static_cast<int>(rStr.size()), 0) > 0;
}

static bool sendWsTextFrame(SOCKET client, const std::string& payload) {
    std::vector<uint8_t> frame;
    frame.push_back(0x81); // FIN + text
    size_t len = payload.size();
    if (len < 126) {
        frame.push_back(static_cast<uint8_t>(len));
    } else if (len <= 65535) {
        frame.push_back(126);
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(len & 0xFF));
    } else {
        frame.push_back(127);
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<uint8_t>((static_cast<uint64_t>(len) >> shift) & 0xFF));
        }
    }
    frame.insert(frame.end(), payload.begin(), payload.end());
    return send(client, reinterpret_cast<const char*>(frame.data()), static_cast<int>(frame.size()), 0) > 0;
}

// --- Client Connection Handler ---

static void handleHttpClient(SOCKET client) {
    DWORD timeout = 30000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    std::string raw;
    char buf[16384];
    size_t hdrEnd = std::string::npos;
    while (hdrEnd == std::string::npos && raw.size() < 65536) {
        int r = recv(client, buf, sizeof(buf), 0);
        if (r <= 0) {
            closesocket(client);
            return;
        }
        raw.append(buf, static_cast<size_t>(r));
        hdrEnd = raw.find("\r\n\r\n");
    }
    if (hdrEnd == std::string::npos) {
        closesocket(client);
        return;
    }

    std::string hdrPart = raw.substr(0, hdrEnd);
    std::string body = raw.substr(hdrEnd + 4);

    std::istringstream iss(hdrPart);
    std::string reqLine;
    std::getline(iss, reqLine);
    if (!reqLine.empty() && reqLine.back() == '\r') reqLine.pop_back();

    size_t sp1 = reqLine.find(' ');
    size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : reqLine.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        closesocket(client);
        return;
    }
    std::string method = reqLine.substr(0, sp1);
    std::string fullUrl = reqLine.substr(sp1 + 1, sp2 - sp1 - 1);

    std::map<std::string, std::string> headers;
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t c = line.find(':');
        if (c != std::string::npos) {
            std::string k = line.substr(0, c);
            std::transform(k.begin(), k.end(), k.begin(), ::tolower);
            std::string v = line.substr(c + 1);
            while (!v.empty() && v.front() == ' ') v.erase(v.begin());
            headers[k] = v;
        }
    }

    size_t contentLen = 0;
    if (headers.count("content-length")) {
        contentLen = static_cast<size_t>(std::strtoull(headers["content-length"].c_str(), nullptr, 10));
    }
    while (body.size() < contentLen) {
        int r = recv(client, buf, static_cast<int>(std::min<size_t>(sizeof(buf), contentLen - body.size())), 0);
        if (r <= 0) break;
        body.append(buf, static_cast<size_t>(r));
    }

    std::string urlPath = fullUrl;
    std::string queryStr;
    size_t qPos = fullUrl.find('?');
    if (qPos != std::string::npos) {
        urlPath = fullUrl.substr(0, qPos);
        queryStr = fullUrl.substr(qPos + 1);
    }

    // 1. WebSocket /ws
    if (urlPath == "/ws" && headers.count("sec-websocket-key")) {
        if (performWebSocketHandshake(client, headers["sec-websocket-key"])) {
            while (gRunning.load()) {
                std::string snap = "{\"type\":\"snapshot\",\"snapshot\":" + buildStateSnapshotJson() + "}";
                if (!sendWsTextFrame(client, snap)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            }
        }
        closesocket(client);
        return;
    }

    // 2. Multipath RFC 4918 WebDAV Gateway (/dav & /dav/*) -> forwards to active Android phone!
    if (urlPath == "/dav" || urlPath.rfind("/dav/", 0) == 0) {
        std::string phoneIp;
        {
            std::lock_guard<std::mutex> lk(gState.mtx);
            phoneIp = gState.activePhoneIp;
        }
        if (!phoneIp.empty()) {
            std::map<std::string, std::string> fwdHeaders;
            if (headers.count("depth")) fwdHeaders["Depth"] = headers["depth"];
            if (headers.count("destination")) fwdHeaders["Destination"] = headers["destination"];
            if (headers.count("overwrite")) fwdHeaders["Overwrite"] = headers["overwrite"];
            if (headers.count("range")) fwdHeaders["Range"] = headers["range"];

            HttpReply rep = httpRequestToPhone(phoneIp, PHONE_VFS_PORT, method, fullUrl, body, fwdHeaders, 60000);
            if (rep.status > 0) {
                std::string ct = rep.headers.count("content-type") ? rep.headers["content-type"] : "application/xml; charset=\"utf-8\"";
                std::map<std::string, std::string> outHdrs = {
                    {"DAV", "1, 2"},
                    {"MS-Author-Via", "DAV"},
                    {"Accept-Ranges", "bytes"},
                };
                if (rep.headers.count("lock-token")) outHdrs["Lock-Token"] = rep.headers["lock-token"];
                if (rep.headers.count("content-range")) outHdrs["Content-Range"] = rep.headers["content-range"];
                sendHttpResponse(client, rep.status, rep.status == 207 ? "Multi-Status" : "OK", ct, rep.body.data(), rep.body.size(), outHdrs);
                closesocket(client);
                return;
            }
        }

        // Local WebDAV fallback when phone isn't on LAN yet so Z:\ still mounts cleanly!
        std::map<std::string, std::string> davHdr = {{"DAV", "1, 2"}, {"MS-Author-Via", "DAV"}};
        if (method == "OPTIONS") {
            sendHttpResponse(client, 200, "OK", "text/plain", "", 0, davHdr);
            closesocket(client);
            return;
        }
        if (method == "PROPFIND") {
            std::string xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\">"
                              "<D:response><D:href>/dav/</D:href><D:propstat><D:prop>"
                              "<D:displayname>ChainIt Sibling (Z:)</D:displayname>"
                              "<D:resourcetype><D:collection/></D:resourcetype>"
                              "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>"
                              "</D:multistatus>";
            sendHttpResponse(client, 207, "Multi-Status", "application/xml; charset=\"utf-8\"", xml.data(), xml.size(), davHdr);
            closesocket(client);
            return;
        }
        sendHttpResponse(client, 200, "OK", "text/plain", "", 0, davHdr);
        closesocket(client);
        return;
    }

    if (method == "OPTIONS") {
        sendHttpResponse(client, 204, "No Content", "text/plain", "", 0);
        closesocket(client);
        return;
    }

    // 3. Control Plane APIs
    if (urlPath == "/api/app/version" && method == "GET") {
        std::ostringstream ss;
        ss << "{\"version\":\"" << kChainItAppVersion << "\",\"runtime\":\"windows-native\",\"channel\":\"stable\"}";
        sendJsonStr(client, 200, ss.str());
        return;
    }
    if (urlPath == "/api/app/desktop-shortcut" && method == "POST") {
        ShortcutResult sc = createDesktopShortcut();
        const char kQuote = 0x22;   // "
        const char kBack = 0x5C;    // backslash
        auto esc = [kQuote, kBack](const std::string& in) {
            std::string out;
            for (char c : in) {
                if (c == kQuote || c == kBack) out.push_back(kBack);
                out.push_back(c);
            }
            return out;
        };
        std::string pathAnsi = ansiFromWide(sc.path);
        std::ostringstream ss;
        ss << "{\"ok\":" << (sc.ok ? "true" : "false")
           << ",\"path\":\"" << esc(pathAnsi) << "\""
           << ",\"message\":\"" << esc(sc.message) << "\"}";
        sendJsonStr(client, 200, ss.str());
        return;
    }
    if (urlPath == "/api/app/update" && method == "POST") {
        AppUpdateResult result = performAppUpdate();
        sendJsonStr(client, 200, appUpdateJson(result));
        if (result.restart) scheduleRestartForUpdate();
        return;
    }
    if (urlPath == "/api/state" && method == "GET") {
        sendJsonStr(client, 200, buildStateSnapshotJson());
        closesocket(client);
        return;
    }

    if (urlPath == "/api/transport/mode" && method == "POST") {
        {
            std::lock_guard<std::mutex> lk(gState.mtx);
            std::string m = jsonExtractString(body, "mode", gState.selectionMode);
            if (m == "auto" || m == "manual") gState.selectionMode = m;
            std::string mt = jsonExtractString(body, "manualTransportId", "");
            if (!mt.empty()) gState.manualTransportId = mt;
            addLogLocked("TRANSPORT", "Mode -> " + gState.selectionMode, "Updated active transport selector.");
        }
        sendJsonStr(client, 200, buildStateSnapshotJson());
        closesocket(client);
        return;
    }

    if (urlPath == "/api/transport/connect-ip" && method == "POST") {
        std::string ip = jsonExtractString(body, "phoneIp", "");
        std::string key = jsonExtractString(body, "siblingKey", "");
        if (!key.empty()) {
            std::lock_guard<std::mutex> lk(gState.mtx);
            gState.siblingKey = key;
        }

        // Accept a plain IP, a "bt://AA:BB:CC:DD:EE:FF" link, or a bare Bluetooth MAC address.
        std::string btLink = normalizeBtEndpoint(ip);
        if (!btLink.empty()) ip = btLink;
        bool isBt = isBtEndpoint(ip);

        if (ip.empty() || !probePhoneIp(ip, isBt ? "bluetooth" : "", isBt ? 12000 : 0)) {
            std::string hint = isBt
                                   ? ("Could not open a native RFCOMM link to " + jsonEscape(ip) + ". Pair the phone with this PC in Windows Bluetooth settings, keep ChainIt-Android.apk running, and make sure Bluetooth is ON on both devices.")
                                   : ("Could not reach ChainIt Sibling on " + jsonEscape(ip) + ":49180. Make sure ChainIt-Android.apk is open/active on your phone.");
            if (isBt) {
                std::lock_guard<std::mutex> lk(gState.mtx);
                addLogLocked("BLUETOOTH", "Manual Bluetooth link failed -> " + ip, hint);
            }
            sendJsonStr(client, 400, "{\"error\":\"" + hint + "\"}");
            closesocket(client);
            return;
        }

        if (isBt) {
            std::lock_guard<std::mutex> lk(gState.mtx);
            addLogLocked("BLUETOOTH", "Manual Bluetooth link pinned -> " + ip,
                         "Native AF_BTH RFCOMM session verified against UUID " + kChainItPhoneUuidStr + "; Z:\\ is now served over Bluetooth.");
        }
        sendJsonStr(client, 200, buildStateSnapshotJson());
        closesocket(client);
        return;
    }

    if (urlPath == "/api/environment/update" && method == "POST") {
        {
            std::lock_guard<std::mutex> lk(gState.mtx);
            if (jsonHasKey(body, "usbPlugged")) gState.envUsb = jsonExtractBool(body, "usbPlugged", gState.envUsb);
            if (jsonHasKey(body, "sharedWifiAvailable")) gState.envWifi = jsonExtractBool(body, "sharedWifiAvailable", gState.envWifi);
            if (jsonHasKey(body, "localHotspotActive")) gState.envHotspot = jsonExtractBool(body, "localHotspotActive", gState.envHotspot);
            if (jsonHasKey(body, "bluetoothEnabled")) gState.envBluetooth = jsonExtractBool(body, "bluetoothEnabled", gState.envBluetooth);
            if (jsonHasKey(body, "onlineP2pEnabled")) gState.envOnline = jsonExtractBool(body, "onlineP2pEnabled", gState.envOnline);
            if (jsonHasKey(body, "phoneScreenLocked")) gState.phoneScreenLocked = jsonExtractBool(body, "phoneScreenLocked", gState.phoneScreenLocked);
        }
        sendJsonStr(client, 200, buildStateSnapshotJson());
        closesocket(client);
        return;
    }

    if (urlPath == "/api/sibling/update" && method == "POST") {
        {
            std::lock_guard<std::mutex> lk(gState.mtx);
            if (jsonHasKey(body, "enabled")) gState.siblingEnabled = jsonExtractBool(body, "enabled", gState.siblingEnabled);
            if (jsonHasKey(body, "autoConnectOnRange")) gState.autoConnectOnRange = jsonExtractBool(body, "autoConnectOnRange", gState.autoConnectOnRange);
            if (jsonHasKey(body, "autoMountZDrive")) gState.autoMountZDrive = jsonExtractBool(body, "autoMountZDrive", gState.autoMountZDrive);
            if (jsonHasKey(body, "zeroPromptTransfers")) gState.zeroPromptTransfers = jsonExtractBool(body, "zeroPromptTransfers", gState.zeroPromptTransfers);
            if (jsonHasKey(body, "allowScreenOffVfs")) gState.allowScreenOffVfs = jsonExtractBool(body, "allowScreenOffVfs", gState.allowScreenOffVfs);
        }
        sendJsonStr(client, 200, buildStateSnapshotJson());
        closesocket(client);
        return;
    }

    // 4. VFS Transfer (Copy / Move across C:\ <-> Z:\ or within same drive)
    if (urlPath == "/api/vfs/transfer" && method == "POST") {
        std::string op = jsonExtractString(body, "op", "copy");
        std::string fromTarget = jsonExtractString(body, "fromTarget", "pc");
        std::string toTarget = jsonExtractString(body, "toTarget", "phone");
        std::string toDir = jsonExtractString(body, "toDir", "/");
        auto fromPaths = jsonExtractStringArray(body, "fromPaths");

        // Ensure snapshot refreshed activePhoneIp
        buildStateSnapshotJson();
        std::string phoneIp;
        std::string activeTr;
        {
            std::lock_guard<std::mutex> lk(gState.mtx);
            phoneIp = gState.activePhoneIp;
            activeTr = gState.activeTransportId.empty() ? "wifi_lan" : gState.activeTransportId;
        }

        uint64_t totalBytes = 0;
        std::error_code ec;

        if (!phoneIp.empty() && (fromTarget == "phone" || toTarget == "phone")) {
            if (fromTarget == "pc" && toTarget == "phone") {
                for (const auto& p : fromPaths) {
                    fs::path src = resolveLocalSafe(gState.pcRoot, p);
                    totalBytes += uploadLocalPathToPhone(src, phoneIp, toDir);
                    if (op == "move") fs::remove_all(src, ec);
                }
            } else if (fromTarget == "phone" && toTarget == "pc") {
                fs::path dstDir = resolveLocalSafe(gState.pcRoot, toDir);
                for (const auto& p : fromPaths) {
                    totalBytes += downloadPhonePathToLocal(phoneIp, p, dstDir);
                }
                if (op == "move") {
                    std::ostringstream delJson;
                    delJson << "{\"paths\":[";
                    for (size_t i = 0; i < fromPaths.size(); ++i) {
                        if (i > 0) delJson << ",";
                        delJson << "\"" << jsonEscape(fromPaths[i]) << "\"";
                    }
                    delJson << "]}";
                    httpRequestToPhone(phoneIp, PHONE_VFS_PORT, "POST", "/chainit/delete", delJson.str(), {{"Content-Type", "application/json"}}, 10000);
                }
            } else if (fromTarget == "phone" && toTarget == "phone") {
                HttpReply r = httpRequestToPhone(phoneIp, PHONE_VFS_PORT, "POST", "/chainit/transfer", body, {{"Content-Type", "application/json"}}, 30000);
                totalBytes = static_cast<uint64_t>(jsonExtractInt(r.body, "totalBytes", 1024));
            }
        } else {
            // Local PC -> PC (or local demo phone folder before real phone pairs)
            fs::path srcRoot = (fromTarget == "pc") ? gState.pcRoot : gState.fallbackPhoneRoot;
            fs::path dstRoot = (toTarget == "pc") ? gState.pcRoot : gState.fallbackPhoneRoot;
            fs::path dstDir = resolveLocalSafe(dstRoot, toDir);
            fs::create_directories(dstDir, ec);
            for (const auto& p : fromPaths) {
                fs::path src = resolveLocalSafe(srcRoot, p);
                if (!fs::exists(src, ec)) continue;
                fs::path dst = dstDir / src.filename();
                if (op == "move") {
                    fs::rename(src, dst, ec);
                    if (ec) {
                        fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
                        fs::remove_all(src, ec);
                    }
                } else {
                    fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
                }
                totalBytes += fs::is_regular_file(dst, ec) ? fs::file_size(dst, ec) : 4096;
            }
        }

        {
            std::lock_guard<std::mutex> lk(gState.mtx);
            std::string label = fromPaths.empty() ? "Items" : fs::u8path(fromPaths[0]).filename().u8string();
            char idBuf[16];
            std::snprintf(idBuf, sizeof(idBuf), "%08x", static_cast<unsigned>(nowMs() & 0xFFFFFFFF));
            std::string dir = (fromTarget == "pc" && toTarget == "phone") ? "pc_to_phone" :
                              (fromTarget == "phone" && toTarget == "pc") ? "phone_to_pc" : "internal_pc";
            gState.transfers.insert(gState.transfers.begin(), {idBuf, label, op, dir, activeTr, static_cast<int64_t>(totalBytes), 88.5, nowMs()});
            addLogLocked("VFS", (op == "move" ? "Moved " : "Copied ") + label, "Completed over " + activeTr);
        }

        std::string resp = "{\"status\":\"completed\",\"snapshot\":" + buildStateSnapshotJson() + "}";
        sendJsonStr(client, 200, resp);
        closesocket(client);
        return;
    }

    // 5. Per-Drive VFS Operations (/api/vfs/pc/* and /api/vfs/phone/*)
    if (urlPath.rfind("/api/vfs/pc/", 0) == 0 || urlPath.rfind("/api/vfs/phone/", 0) == 0) {
        bool isPhone = urlPath.rfind("/api/vfs/phone/", 0) == 0;
        std::string action = urlPath.substr(isPhone ? 15 : 12);

        buildStateSnapshotJson();
        std::string phoneIp;
        {
            std::lock_guard<std::mutex> lk(gState.mtx);
            phoneIp = gState.activePhoneIp;
        }

        // Proxy to real Android Sibling if connected!
        if (isPhone && !phoneIp.empty()) {
            std::string remoteUrl = "/chainit/" + action + (queryStr.empty() ? "" : ("?" + queryStr));
            std::map<std::string, std::string> hdrs;
            if (headers.count("content-type")) hdrs["Content-Type"] = headers["content-type"];
            if (headers.count("range")) hdrs["Range"] = headers["range"];

            HttpReply rep = httpRequestToPhone(phoneIp, PHONE_VFS_PORT, method, remoteUrl, body, hdrs, 60000);
            if (rep.status > 0) {
                std::string ct = rep.headers.count("content-type") ? rep.headers["content-type"] : "application/json";
                std::map<std::string, std::string> extra;
                if (rep.headers.count("content-disposition")) extra["Content-Disposition"] = rep.headers["content-disposition"];
                sendHttpResponse(client, rep.status, rep.status == 200 ? "OK" : "Error", ct, rep.body.data(), rep.body.size(), extra);
                closesocket(client);
                return;
            }
        }

        // Local execution (for C:\ User Profile OR Demo Phone Folder before Android app pairs)
        fs::path root = isPhone ? gState.fallbackPhoneRoot : gState.pcRoot;
        std::error_code ec;

        if (action == "list" && method == "GET") {
            std::string p = getQueryParam(queryStr, "path", "/");
            sendJsonStr(client, 200, listLocalDirJson(root, p));
            closesocket(client);
            return;
        }

        if (action == "raw" && method == "GET") {
            std::string p = getQueryParam(queryStr, "path", "/");
            bool dl = getQueryParam(queryStr, "download", "0") == "1";
            fs::path full = resolveLocalSafe(root, p);
            if (!fs::exists(full, ec) || fs::is_directory(full, ec)) {
                sendHttpResponse(client, 404, "Not Found", "text/plain", "Not found", 9);
                closesocket(client);
                return;
            }
            std::ifstream ifs(full, std::ios::binary);
            std::ostringstream ss;
            ss << ifs.rdbuf();
            std::string data = ss.str();
            std::map<std::string, std::string> extra;
            if (dl) extra["Content-Disposition"] = "attachment; filename=\"" + urlEncode(full.filename().u8string()) + "\"";
            sendHttpResponse(client, 200, "OK", guessMime(full.filename().u8string()), data.data(), data.size(), extra);
            closesocket(client);
            return;
        }

        if (action == "text" && method == "GET") {
            std::string p = getQueryParam(queryStr, "path", "/");
            fs::path full = resolveLocalSafe(root, p);
            if (!fs::exists(full, ec) || fs::is_directory(full, ec)) {
                sendJsonStr(client, 404, "{\"error\":\"File not found\"}");
                closesocket(client);
                return;
            }
            bool editable = isEditableTextExt(full.filename().u8string()) && fs::file_size(full, ec) <= 2 * 1024 * 1024;
            std::string content;
            if (editable) {
                std::ifstream ifs(full, std::ios::binary);
                std::ostringstream ss;
                ss << ifs.rdbuf();
                content = ss.str();
            }
            std::ostringstream res;
            res << "{\"item\":" << buildVfsItemJson(root, full) << ",\"editable\":" << (editable ? "true" : "false") << ",\"content\":";
            if (editable) res << "\"" << jsonEscape(content) << "\"";
            else res << "null";
            res << "}";
            sendJsonStr(client, 200, res.str());
            closesocket(client);
            return;
        }

        if (action == "text" && method == "PUT") {
            std::string p = jsonExtractString(body, "path", "");
            std::string c = jsonExtractString(body, "content", "");
            fs::path full = resolveLocalSafe(root, p);
            fs::create_directories(full.parent_path(), ec);
            std::ofstream ofs(full, std::ios::binary);
            ofs.write(c.data(), static_cast<std::streamsize>(c.size()));
            ofs.close();
            sendJsonStr(client, 200, "{\"ok\":true,\"item\":" + buildVfsItemJson(root, full) + "}");
            closesocket(client);
            return;
        }

        if (action == "mkdir" && method == "POST") {
            std::string parentDir = jsonExtractString(body, "parentDir", "/");
            std::string name = jsonExtractString(body, "name", "New Folder");
            fs::path target = resolveLocalSafe(root, parentDir) / fs::u8path(name);
            fs::create_directories(target, ec);
            sendJsonStr(client, 200, "{\"item\":" + buildVfsItemJson(root, target) + "}");
            closesocket(client);
            return;
        }

        if (action == "rename" && method == "POST") {
            std::string p = jsonExtractString(body, "path", "");
            std::string nn = jsonExtractString(body, "newName", "");
            fs::path src = resolveLocalSafe(root, p);
            fs::path dst = src.parent_path() / fs::u8path(nn);
            fs::rename(src, dst, ec);
            sendJsonStr(client, 200, "{\"item\":" + buildVfsItemJson(root, dst) + "}");
            closesocket(client);
            return;
        }

        if (action == "delete" && method == "POST") {
            auto paths = jsonExtractStringArray(body, "paths");
            int cnt = 0;
            for (const auto& p : paths) {
                fs::path t = resolveLocalSafe(root, p);
                if (t != root && fs::exists(t, ec)) {
                    fs::remove_all(t, ec);
                    cnt++;
                }
            }
            sendJsonStr(client, 200, "{\"deleted\":" + std::to_string(cnt) + "}");
            closesocket(client);
            return;
        }

        if (action == "upload" && method == "POST") {
            std::string dir = getQueryParam(queryStr, "dir", "/Desktop");
            std::string fname = getQueryParam(queryStr, "filename", "uploaded_file.bin");
            fs::path dstDir = resolveLocalSafe(root, dir);
            fs::create_directories(dstDir, ec);
            fs::path dst = dstDir / fs::u8path(fname).filename();
            std::ofstream ofs(dst, std::ios::binary);
            ofs.write(body.data(), static_cast<std::streamsize>(body.size()));
            ofs.close();
            std::string snap = buildStateSnapshotJson();
            sendJsonStr(client, 200, "{\"item\":" + buildVfsItemJson(root, dst) + ",\"snapshot\":" + snap + "}");
            closesocket(client);
            return;
        }
    }

    // 6. Serve Embedded Frontend SPA Assets (from embedded_assets.h)
    for (size_t i = 0; i < kEmbeddedAssetCount; ++i) {
        if (urlPath == kEmbeddedAssets[i].url_path) {
            sendHttpResponse(client, 200, "OK", kEmbeddedAssets[i].mime_type, kEmbeddedAssets[i].data, kEmbeddedAssets[i].size);
            closesocket(client);
            return;
        }
    }
    // SPA Fallback -> /index.html
    for (size_t i = 0; i < kEmbeddedAssetCount; ++i) {
        if (std::strcmp(kEmbeddedAssets[i].url_path, "/index.html") == 0) {
            sendHttpResponse(client, 200, "OK", kEmbeddedAssets[i].mime_type, kEmbeddedAssets[i].data, kEmbeddedAssets[i].size);
            closesocket(client);
            return;
        }
    }

    sendHttpResponse(client, 404, "Not Found", "text/plain", "Not Found", 9);
    closesocket(client);
}

static void httpServerLoop() {
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) return;

    BOOL yes = TRUE;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(HTTP_PORT);
    if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(srv);
        return;
    }
    listen(srv, SOMAXCONN);

    while (gRunning.load()) {
        SOCKET client = accept(srv, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        std::thread(handleHttpClient, client).detach();
    }
    closesocket(srv);
}

// --- Win32 System Tray Window Procedure ---


static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_TRAYICON) {
        if (lParam == WM_LBUTTONDBLCLK) {
            launchDesktopUiWindow();
        } else if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_OPEN_UI, L"Open ChainIt File Manager");
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_OPEN_Z, L"Open Z:\\ Phone Drive in Explorer");
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_REMOUNT_Z, L"Mount / Refresh Z:\\ Drive");
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_SHORTCUT, L"Create desktop icon");
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_UPDATE, L"Update ChainIt in place…");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Unmount Z:\\ && Exit ChainIt");
            SetForegroundWindow(hwnd);
            TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(hMenu);
        }
        return 0;
    }
    if (msg == WM_COMMAND) {
        switch (LOWORD(wParam)) {
            case ID_TRAY_OPEN_UI:
                launchDesktopUiWindow();
                break;
            case ID_TRAY_OPEN_Z:
                ShellExecuteW(nullptr, L"open", L"Z:\\", nullptr, nullptr, SW_SHOWNORMAL);
                break;
            case ID_TRAY_REMOUNT_Z:
                mountZDriveAsync();
                break;
            case ID_TRAY_SHORTCUT: {
                std::thread([]() {
                    ShortcutResult sc = createDesktopShortcut();
                    MessageBoxW(nullptr, wideFromAnsi(sc.message).c_str(), L"ChainIt",
                                MB_OK | (sc.ok ? MB_ICONINFORMATION : MB_ICONWARNING));
                }).detach();
                break;
            }
            case ID_TRAY_UPDATE: {
                std::thread([]() {
                    AppUpdateResult r = performAppUpdate();
                    std::wstring text = wideFromAnsi(r.message.empty() ? "Update finished." : r.message);
                    MessageBoxW(nullptr, text.c_str(),
                                r.status == "error" ? L"ChainIt update failed" : L"ChainIt update",
                                MB_OK | (r.status == "error" ? MB_ICONWARNING : MB_ICONINFORMATION));
                    if (r.restart) {
                        gRunning.store(false);
                        Shell_NotifyIconW(NIM_DELETE, &gNid);
                        ExitProcess(0);
                    }
                }).detach();
                break;
            }
            case ID_TRAY_EXIT:
                gRunning.store(false);
                Shell_NotifyIconW(NIM_DELETE, &gNid);
                unmountZDriveSync();
                PostQuitMessage(0);
                break;
        }
        return 0;
    }
    if (msg == WM_DESTROY) {
        gRunning.store(false);
        Shell_NotifyIconW(NIM_DELETE, &gNid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    // Prevent duplicate instances; if already running, just bring up the ChainIt File Manager window!
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\ChainItSiblingEngineSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        launchDesktopUiWindow();
        return 0;
    }

    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    wchar_t userProf[MAX_PATH] = L"C:\\";
    GetEnvironmentVariableW(L"USERPROFILE", userProf, MAX_PATH);
    gState.pcRoot = fs::path(userProf);
    gState.fallbackPhoneRoot = gState.pcRoot / L".chainit" / L"sibling-staging";

    std::error_code ec;
    fs::create_directories(gState.fallbackPhoneRoot / L"DCIM" / L"Camera", ec);
    fs::create_directories(gState.fallbackPhoneRoot / L"Download", ec);
    fs::create_directories(gState.fallbackPhoneRoot / L"Documents", ec);
    fs::create_directories(gState.fallbackPhoneRoot / L"ChainIt_Inbox", ec);

    fs::path guideFile = gState.fallbackPhoneRoot / L"HOW_TO_LINK_YOUR_ANDROID_PHONE.md";
    if (!fs::exists(guideFile, ec)) {
        std::ofstream ofs(guideFile);
        ofs << "# Waiting for ChainIt Sibling on Your Android Phone\n\n"
            << "1. Install **ChainIt-Android.apk** on your Android phone and open it once.\n"
            << "2. Tap **Grant All-Files Access** and **Allow Screen-Off Standby**.\n"
            << "3. Connect both devices to the same Wi-Fi, OR turn on **Phone Hotspot / Windows Mobile Hotspot** (100% offline!), OR plug in **USB + USB Tethering**,\n"
            << "   OR pair them once in **Bluetooth** settings — ChainIt then opens a native RFCOMM socket link that works with Wi-Fi, Hotspot and USB all switched OFF.\n"
            << "4. ChainIt-Windows.exe will automatically discover your phone in <2 seconds and switch this **Z:\\** view live to your phone's real storage—even with your phone screen locked!\n"
            << "5. Bluetooth-only mode: pair once, keep the ChainIt app running on the phone, and Z:\\ stays mounted over RFCOMM (slower, but zero Wi-Fi / zero data / zero cables).\n";
    }

    {
        std::lock_guard<std::mutex> lk(gState.mtx);
        addLogLocked("SIBLING", "ChainIt Windows Native Daemon Started",
                     "Listening on 0.0.0.0:4173, broadcasting UDP Sibling discovery on port 49179, and arming native Bluetooth RFCOMM (AF_BTH) sockets: phone UUID " +
                         kChainItPhoneUuidStr + " / PC receiver UUID " + kChainItPcUuidStr + ".");
    }

    std::thread(httpServerLoop).detach();
    std::thread(discoveryLoop).detach();
    std::thread(bluetoothDiscoveryLoop).detach();
    std::thread(bluetoothServerLoop).detach();
    mountZDriveAsync();

    // Create Win32 System Tray Icon
    // The chain-link mark compiled into this exe as resource #1 (windows/res/chainit.rc) -
    // the same artwork as the Android launcher icon, so both apps share one identity.
    const HICON hAppIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = hAppIcon;
    wc.hIconSm = hAppIcon;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = L"ChainItTrayHiddenWindowClass";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"ChainIt Sibling", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInstance, nullptr);

    gNid.cbSize = sizeof(gNid);
    gNid.hWnd = hwnd;
    gNid.uID = 1;
    gNid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    gNid.uCallbackMessage = WM_TRAYICON;
    gNid.hIcon = hAppIcon ? hAppIcon : LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
    wcsncpy_s(gNid.szTip, L"ChainIt Sibling — Auto-Connect & Z:\\ Drive", _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &gNid);

    // Give HTTP server 150ms to bind, then open the native borderless ChainIt window
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    launchDesktopUiWindow();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    WSACleanup();
    if (hMutex) CloseHandle(hMutex);
    return 0;
}
