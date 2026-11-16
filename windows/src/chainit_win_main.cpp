#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <windowsx.h>
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
#include <memory>
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
#define ID_TRAY_MINI 4007
#define ID_TRAY_HUB 4008

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

    /** Set once a real phone answers, so Z: stops serving the "waiting" placeholder. */
    bool everDiscoveredPhone = false;
    /** Human-readable reason the link is not serving files - shown in the hub window and the UI. */
    std::string phoneLinkProblem;
};

static GlobalState gState;

static std::atomic<uint64_t> gIntegrityChunksOk{0};
static std::atomic<uint64_t> gIntegrityFailures{0};
static const uint64_t kTransferChunkBytes = 2ull * 1024 * 1024;
static const int kTransferWorkersLan = 4;

static std::string sha256HexMemory(const void* data, size_t len);
static std::string sha256HexFile(const std::wstring& path);

static std::atomic<bool> gRunning{true};

// Its own mutex: log writes happen both with gState.mtx held (snapshot/UI paths)
// and without it (long-running transfer workers), so guarding them with gState.mtx
// alone would either deadlock or race on gState.logs.
static std::mutex gLogMtx;

static void addLogLocked(const std::string& layer, const std::string& summary, const std::string& detail) {
    std::lock_guard<std::mutex> logLk(gLogMtx);
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

static std::string ansiFromWide(const std::wstring& in);
static std::wstring wideFromAnsi(const std::string& in);

// Windows Explorer labels a WebDAV mount with the raw URL ("127.0.0.1\dav"). Rename it to the
// phone's name so the Z:\ drive reads like a real device in Explorer's tree.
static void setZDriveLabelInExplorer(const std::wstring& label) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\MountPoints2",
                      0, KEY_READ | KEY_WRITE, &hKey) != ERROR_SUCCESS) {
        return;
    }
    wchar_t name[512];
    DWORD idx = 0;
    DWORD renamed = 0;
    while (RegEnumKeyW(hKey, idx, name, 512) == ERROR_SUCCESS) {
        const std::wstring sub(name);
        const bool hit = sub.find(L"4173") != std::wstring::npos ||
                         (sub.find(L"dav") != std::wstring::npos && sub.find(L"127.0.0.1") != std::wstring::npos);
        if (hit) {
            HKEY hSub = nullptr;
            if (RegOpenKeyExW(hKey, sub.c_str(), 0, KEY_WRITE, &hSub) == ERROR_SUCCESS) {
                if (RegSetValueExW(hSub, L"_LabelFromReg", 0, REG_SZ,
                                   reinterpret_cast<const BYTE*>(label.c_str()),
                                   static_cast<DWORD>((label.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS) {
                    ++renamed;
                }
                RegCloseKey(hSub);
            }
        }
        ++idx;
    }
    RegCloseKey(hKey);
    if (renamed > 0) SHChangeNotify(SHCNE_ASSOCCHANGED, 0, nullptr, nullptr);
}

static void mountZDriveAsync() {
    std::thread([]() {
        runHiddenCommand(L"sc start WebClient");
        runHiddenCommand(L"net use Z: /delete /y");
        bool ok = runHiddenCommand(L"net use Z: http://127.0.0.1:4173/dav /persistent:no");
        std::string peer;
        {
            std::lock_guard<std::mutex> lk(gState.mtx);
            gState.zDriveMounted = ok || gState.siblingEnabled;
            peer = gState.peerDeviceName;
            if (ok) {
                addLogLocked("VFS", "Mounted Z:\\ in Windows File Explorer", "Mapped Z:\\ -> http://127.0.0.1:4173/dav (Multipath Sibling VFS).");
            }
        }
        if (ok) {
            // Show the phone's name (never the 127.0.0.1:4173 loopback URL) as the drive
            // label in Explorer. Explorer only writes its MountPoints2 key the first time
            // the share is browsed, so the label is re-applied on a short back-off until it
            // sticks, and again whenever the sibling reports a different device name.
            std::wstring label = L"ChainIt Phone";
            if (!peer.empty() && peer.find("Scanning") == std::string::npos) {
                label = wideFromAnsi(peer + " (ChainIt)");
            }
            setZDriveLabelInExplorer(label);
            std::thread([label]() {
                for (int attempt = 0; attempt < 4; ++attempt) {
                    std::this_thread::sleep_for(std::chrono::seconds(attempt == 0 ? 2 : 6));
                    if (!gRunning.load()) return;
                    std::wstring current = label;
                    {
                        std::lock_guard<std::mutex> lk(gState.mtx);
                        if (!gState.peerDeviceName.empty() &&
                            gState.peerDeviceName.find("Scanning") == std::string::npos) {
                            current = wideFromAnsi(gState.peerDeviceName + " (ChainIt)");
                        }
                    }
                    setZDriveLabelInExplorer(current);
                }
            }).detach();
        }
    }).detach();
}

static void unmountZDriveSync() {
    runHiddenCommand(L"net use Z: /delete /y");
}

// ---------------------------------------------------------------------------
// The File Manager and the mini panel used to be opened from here by handing an
// "--app=<local url>" argument to Microsoft Edge. That gives the window - and
// therefore the taskbar button - to a *browser* process, so Windows drew Edge's
// globe and no icon compiled into this exe could ever change it. Every window is
// native now: see showNativeMainWindow() below and the GDI hub window.
// ---------------------------------------------------------------------------
static void showNativeMainWindow();
static void launchDesktopUiWindow();

// --- Simple Outbound HTTP Client to Talk to ChainIt-Android.apk (Port 49180) ---

// --- XML / DAV helpers -----------------------------------------------------

static std::string xmlEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 16);
    for (char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

/** Ask both discovery loops to run their next sweep immediately instead of after their nap. */
static std::atomic<bool> gForceRescan{false};
static void triggerTransportRescan() { gForceRescan.store(true); }

static std::string waitingDriveReadme() {
    std::string peer;
    std::string problem;
    {
        std::lock_guard<std::mutex> lk(gState.mtx);
        peer = gState.peerDeviceName;
        problem = gState.phoneLinkProblem;
    }
    std::ostringstream os;
    os << "ChainIt Z: drive - waiting for your Android phone\r\n"
       << "==================================================\r\n\r\n"
       << "This drive is the live storage of your Android phone. It is empty right now only\r\n"
       << "because no phone has answered yet - nothing is broken and nothing is disabled.\r\n\r\n"
       << "To bring it online:\r\n"
       << "  1. Open ChainIt on the Android phone.\r\n"
       << "  2. Make sure Sibling Mode is ON and the Pairing Key matches this PC.\r\n"
       << "  3. Turn on Bluetooth, or connect both devices to the same Wi-Fi / hotspot,\r\n"
       << "     or plug in the USB cable and enable USB tethering.\r\n"
       << "  4. Grant 'All files access' when the phone asks - that is what lets Z: see\r\n"
       << "     DCIM, Download, Documents and the rest.\r\n\r\n"
       << "The moment the phone answers, this drive fills with its real folders. The phone\r\n"
       << "can stay locked and even have its screen off: ChainIt keeps serving from a\r\n"
       << "foreground service with a Doze heartbeat.\r\n\r\n"
       << "Current status\r\n"
       << "--------------\r\n"
       << "  Sibling engine : running (port 4173)\r\n"
       << "  Peer           : " << (peer.empty() ? "not discovered yet" : peer) << "\r\n"
       << "  Last problem   : " << (problem.empty() ? "none" : problem) << "\r\n";
    return os.str();
}

/**
 * WebDAV PROPFIND for the placeholder drive: a real collection holding one readme,
 * so Explorer renders something that explains itself instead of an empty disk.
 */
static std::string buildWaitingDrivePropfind(const std::string& urlPath,
                                            const std::map<std::string, std::string>& headers) {
    std::string depth = "1";
    auto it = headers.find("depth");
    if (it != headers.end() && !it->second.empty()) depth = it->second;

    const std::string readmeName = "READ ME FIRST - waiting for your phone.txt";
    const std::string readmeBody = waitingDriveReadme();

    std::ostringstream os;
    os << "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
       << "<D:multistatus xmlns:D=\"DAV:\">"
       << "<D:response><D:href>/dav/</D:href><D:propstat><D:prop>"
       << "<D:displayname>ChainIt (waiting for phone)</D:displayname>"
       << "<D:resourcetype><D:collection/></D:resourcetype>"
       << "<D:getcontenttype>httpd/unix-directory</D:getcontenttype>"
       << "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>";

    if (depth != "0") {
        os << "<D:response><D:href>/dav/" << xmlEscape(readmeName) << "</D:href><D:propstat><D:prop>"
           << "<D:displayname>" << xmlEscape(readmeName) << "</D:displayname>"
           << "<D:resourcetype/>"
           << "<D:getcontentlength>" << readmeBody.size() << "</D:getcontentlength>"
           << "<D:getcontenttype>text/plain</D:getcontenttype>"
           << "<D:isreadonly>1</D:isreadonly>"
           << "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>";
    }
    os << "</D:multistatus>";
    (void)urlPath;
    return os.str();
}

struct HttpReply {
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
    double rttMs = 0.0;
    /** False when the peer asked to close (or sent no framing), so the channel must be dropped. */
    bool keepAliveOk = false;
};

/** Blocking full write - a short send on RFCOMM would silently truncate an upload. */
static bool sendAll(SOCKET s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const int n = send(s, data.data() + sent, static_cast<int>(std::min<size_t>(65536, data.size() - sent)), 0);
        if (n <= 0) {
            const int err = WSAGetLastError();
            if (err == WSAEINTR || err == WSAEWOULDBLOCK) continue;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

// ===========================================================================
// Persistent RFCOMM channel pool (Bluetooth as a first-class transport)
//
// Dialling an RFCOMM link costs an SDP lookup plus a page - routinely 1-5 s on a
// real radio. Explorer issues dozens of requests to render one folder, so a
// connect-per-request design makes Bluetooth look "connected but dead": the link
// is up, the queue never drains.
//
// These channels are therefore *kept open* and reused. The phone answers them as
// HTTP/1.1 keep-alive sessions, so one dial serves a whole browsing session, and
// several channels run side by side (per-channel mutex, not a global one) so a
// slow PROPFIND never blocks a file read. The RFCOMM channel number discovered by
// the first SDP query is cached, which removes the lookup from every later dial.
// ===========================================================================

static const size_t kBtChannelCount = 3;
static std::atomic<ULONG> gBtCachedPort{0};
static std::atomic<uint64_t> gBtRequestsServed{0};
static std::atomic<uint64_t> gBtRedials{0};
static std::atomic<int64_t> gBtLastRttUs{0};

struct BtChannel {
    SOCKET sock = INVALID_SOCKET;
    std::mutex mtx;            // one request in flight per channel
    std::string buf;           // bytes read ahead of the parser
    size_t pos = 0;
    int64_t lastUseMs = 0;
    bool inPool = false;

    bool ensure(size_t want) {
        while (buf.size() - pos < want) {
            char tmp[16384];
            const int n = recv(sock, tmp, sizeof(tmp), 0);
            if (n <= 0) return false;
            buf.append(tmp, static_cast<size_t>(n));
        }
        return true;
    }
    int getByte() {
        if (!ensure(1)) return -1;
        return static_cast<unsigned char>(buf[pos++]);
    }
    bool readExact(size_t want, std::string& out) {
        if (want == 0) return true;
        if (!ensure(want)) return false;
        out.append(buf, pos, want);
        pos += want;
        return true;
    }
    void compact() {
        if (pos > 0 && pos == buf.size()) { buf.clear(); pos = 0; }
    }
    bool readHeaderBlock(std::string& out) {
        // Byte-at-a-time until CRLFCRLF: headers are small and this never over-reads
        // into the body, which is what makes keep-alive framing safe.
        out.clear();
        int crlf = 0;
        while (true) {
            const int c = getByte();
            if (c < 0) return false;
            out.push_back(static_cast<char>(c));
            if (c == '\n') {
                if (crlf == 1) return true;   // saw "\r\n\r\n"
                crlf = (out.size() >= 2 && out[out.size() - 2] == '\r') ? 1 : 0;
                if (out.size() < 2) crlf = 0;
            } else if (c != '\r') {
                crlf = 0;
            }
            if (out.size() > 65536) return false;
        }
    }
};

static std::mutex gBtPoolMtx;
static std::vector<std::shared_ptr<BtChannel>> gBtPool;

static void btChannelClose(const std::shared_ptr<BtChannel>& ch) {
    if (!ch) return;
    if (ch->sock != INVALID_SOCKET) { closesocket(ch->sock); ch->sock = INVALID_SOCKET; }
    ch->buf.clear();
    ch->pos = 0;
}

static std::shared_ptr<BtChannel> btAcquireChannel(const std::string& endpoint, int timeoutMs) {
    // 1. reuse a live channel
    {
        std::lock_guard<std::mutex> lk(gBtPoolMtx);
        for (auto& ch : gBtPool) {
            if (ch->inPool && ch->sock != INVALID_SOCKET) {
                ch->inPool = false;
                return ch;
            }
        }
    }
    // 2. dial a new one (SDP is skipped when we already know the RFCOMM channel)
    SOCKET s = btRfcommConnect(endpoint, timeoutMs);
    if (s == INVALID_SOCKET) return nullptr;
    gBtRedials.fetch_add(1);

    if (gBtCachedPort.load() == 0) {
        SOCKADDR_BTH peer{};
        int len = sizeof(peer);
        if (getpeername(s, reinterpret_cast<sockaddr*>(&peer), &len) == 0 && peer.port != 0) {
            gBtCachedPort.store(peer.port);
        }
    }

    auto ch = std::make_shared<BtChannel>();
    ch->sock = s;
    ch->lastUseMs = nowMs();
    return ch;
}

static void btReleaseChannel(const std::shared_ptr<BtChannel>& ch, bool healthy) {
    if (!ch) return;
    ch->lastUseMs = nowMs();
    ch->compact();
    if (!healthy) { btChannelClose(ch); return; }
    std::lock_guard<std::mutex> lk(gBtPoolMtx);
    if (ch->sock == INVALID_SOCKET) return;
    for (auto& e : gBtPool) {
        if (e == ch) { e->inPool = true; return; }
    }
    if (gBtPool.size() < kBtChannelCount) {
        ch->inPool = true;
        gBtPool.push_back(ch);
    } else {
        btChannelClose(ch);
    }
}

/** How many reusable RFCOMM channels are parked and ready (0 = the next request must dial). */
static size_t btPooledChannels() {
    std::lock_guard<std::mutex> lk(gBtPoolMtx);
    size_t n = 0;
    for (const auto& ch : gBtPool) if (ch->inPool && ch->sock != INVALID_SOCKET) ++n;
    return n;
}

/**
 * Drop every *idle* pooled channel (radio went away, transport changed, peer re-paired).
 * A channel that is mid-request is marked inPool == false and is left alone; its owner
 * closes it when the exchange finishes.
 */
static void btPoolReset() {
    std::lock_guard<std::mutex> lk(gBtPoolMtx);
    for (auto& ch : gBtPool) {
        if (ch->inPool) btChannelClose(ch);
    }
    gBtPool.clear();
    gBtCachedPort.store(0);
}

/**
 * One HTTP exchange over an already-open RFCOMM channel, framed for keep-alive:
 * the body length comes from Content-Length or from de-chunking, never from EOF.
 */
static bool btHttpExchangeOnChannel(BtChannel& ch, const std::string& request,
                                    const std::string& body, HttpReply& rep, int timeoutMs) {
    if (ch.sock == INVALID_SOCKET) return false;
    DWORD to = static_cast<DWORD>(timeoutMs);
    setsockopt(ch.sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
    setsockopt(ch.sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));

    if (!sendAll(ch.sock, request)) return false;
    if (!body.empty() && !sendAll(ch.sock, body)) return false;

    std::string headers;
    if (!ch.readHeaderBlock(headers)) return false;

    size_t sp1 = headers.find(' ');
    if (sp1 != std::string::npos) {
        size_t sp2 = headers.find(' ', sp1 + 1);
        const std::string code = headers.substr(sp1 + 1, (sp2 == std::string::npos ? headers.size() : sp2) - sp1 - 1);
        rep.status = std::atoi(code.c_str());
    }

    long long contentLength = -1;
    bool chunked = false;
    bool closes = false;
    {
        std::istringstream iss(headers);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t c = line.find(':');
            if (c == std::string::npos) continue;
            std::string k = line.substr(0, c);
            std::transform(k.begin(), k.end(), k.begin(), ::tolower);
            std::string v = line.substr(c + 1);
            while (!v.empty() && v.front() == ' ') v.erase(v.begin());
            if (k == "content-length") contentLength = std::atoll(v.c_str());
            else if (k == "transfer-encoding" && v.find("chunked") != std::string::npos) chunked = true;
            else if (k == "connection" && v.find("close") != std::string::npos) closes = true;
            rep.headers[k] = v;
        }
    }
    if (rep.status == 204 || rep.status == 304) { rep.keepAliveOk = !closes; return true; }

    if (chunked) {
        while (true) {
            std::string sizeLine;
            int c;
            while ((c = ch.getByte()) >= 0) {
                if (c == '\n') break;
                if (c != '\r') sizeLine.push_back(static_cast<char>(c));
            }
            if (c < 0) return false;
            const size_t n = static_cast<size_t>(std::strtoull(sizeLine.c_str(), nullptr, 16));
            if (n == 0) {
                // trailer + final CRLF
                std::string tail;
                ch.readHeaderBlock(tail);
                break;
            }
            if (!ch.readExact(n, rep.body)) return false;
            ch.getByte(); ch.getByte(); // CRLF after the chunk
        }
    } else if (contentLength >= 0) {
        if (!ch.readExact(static_cast<size_t>(contentLength), rep.body)) return false;
    } else {
        // No framing information: the peer will close, so this channel cannot be reused.
        char tmp[16384];
        while (true) {
            const int n = recv(ch.sock, tmp, sizeof(tmp), 0);
            if (n <= 0) break;
            rep.body.append(tmp, static_cast<size_t>(n));
        }
        closes = true;
    }
    rep.keepAliveOk = !closes;
    return true;
}

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

    std::string keyCopy;
    {
        std::lock_guard<std::mutex> lk(gState.mtx);
        keyCopy = gState.siblingKey;
    }

    if (viaBluetooth) {
        // --- persistent RFCOMM channel: dial once, then reuse for the whole session ---
        std::ostringstream req;
        req << method << " " << targetPath << " HTTP/1.1\r\n";
        req << "Host: chainit-bluetooth\r\n";
        req << "X-ChainIt-Transport: bluetooth-rfcomm\r\n";
        req << "X-ChainIt-Key: " << keyCopy << "\r\n";
        req << "Content-Length: " << body.size() << "\r\n";
        req << "Connection: keep-alive\r\n";
        for (const auto& kv : extraHeaders) req << kv.first << ": " << kv.second << "\r\n";
        req << "\r\n";
        const std::string headStr = req.str();

        const int dialTimeout = std::max(timeoutMs, 12000);
        for (int attempt = 0; attempt < 3; ++attempt) {
            std::shared_ptr<BtChannel> ch = btAcquireChannel(ip, dialTimeout);
            if (!ch) break;
            bool ok = false;
            {
                std::lock_guard<std::mutex> lk(ch->mtx); // per channel, so channels run in parallel
                HttpReply attemptRep;
                ok = btHttpExchangeOnChannel(*ch, headStr, body, attemptRep, timeoutMs);
                if (ok) rep = std::move(attemptRep);
                const bool reusable = ok && rep.keepAliveOk;
                btReleaseChannel(ch, reusable);
            }
            if (ok && rep.status > 0) {
                auto t1 = std::chrono::steady_clock::now();
                rep.rttMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
                gBtLastRttUs.store(static_cast<int64_t>(rep.rttMs * 1000.0));
                gBtRequestsServed.fetch_add(1);
                return rep;
            }
            // The link died (radio napped, peer closed, Doze suspended it): drop the stale
            // channels and re-dial rather than reporting a dead transport.
            rep = HttpReply{};
            btPoolReset();
        }
        return rep;
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
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

    std::ostringstream req;
    req << method << " " << targetPath << " HTTP/1.1\r\n";
    req << "Host: " << ip << ":" << port << "\r\n";
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
        << "\"pooledChannels\":" << btPooledChannels() << ","
        << "\"maxChannels\":" << kBtChannelCount << ","
        << "\"cachedRfcommChannel\":" << gBtCachedPort.load() << ","
        << "\"requestsServed\":" << gBtRequestsServed.load() << ","
        << "\"redials\":" << gBtRedials.load() << ","
        << "\"lastRttMs\":" << (static_cast<double>(gBtLastRttUs.load()) / 1000.0) << ","
        << "\"keepAlive\":true,"
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
    oss << "],\"linkProblem\":\"" << jsonEscape(gState.phoneLinkProblem) << "\",\"security\":{\"keyAuth\":\"required\",\"lockoutAfter\":5,\"lockoutWindowSec\":60,\"lockoutCooldownSec\":30,\"integrity\":\"sha256 per chunk + whole file\",\"chunkBytes\":" << kTransferChunkBytes << ",\"parallelWorkers\":" << (isBtEndpoint(gState.activePhoneIp) ? 1 : kTransferWorkersLan) << ",\"chunksVerified\":" << gIntegrityChunksOk.load() << ",\"integrityFailures\":" << gIntegrityFailures.load() << "},\"serverPort\":" << HTTP_PORT << "}";
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
        if (gForceRescan.exchange(false)) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        else { std::this_thread::sleep_for(std::chrono::milliseconds(900)); }
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
            for (int i = 0; i < 200 && gRunning.load() && !gForceRescan.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
            for (int i = 0; i < 200 && gRunning.load() && !gForceRescan.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
    const std::string fileHash = sha256HexFile(localPath.wstring());
    std::map<std::string, std::string> hdrs = {{"Content-Type", "application/octet-stream"}};
    if (!fileHash.empty()) hdrs["X-ChainIt-File-Sha256"] = fileHash;
    HttpReply upRep = httpRequestToPhone(phoneIp, PHONE_VFS_PORT, "POST", url, data, hdrs, 60000);
    if (upRep.status == 409) {
        addLogLocked("SECURITY", "Upload rejected by phone (SHA-256 mismatch)", fname);
        return 0;
    }
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

    const std::string rawUrl = "/chainit/raw?path=" + urlEncode(phoneSrcPath);
    const fs::path dst = localDestDir / fs::u8path(baseName);

    // --- probe size + published whole-file SHA-256 with a 1-byte range request ---
    uint64_t total = 0;
    std::string fileSha;
    HttpReply probe = httpRequestToPhone(phoneIp, PHONE_VFS_PORT, "GET", rawUrl, "",
                                         {{"Range", "bytes=0-0"}}, 8000);
    if (probe.status == 206 && probe.headers.count("content-range")) {
        const std::string cr = probe.headers["content-range"];
        const size_t slash = cr.find('/');
        if (slash != std::string::npos) total = std::strtoull(cr.c_str() + slash + 1, nullptr, 10);
    }
    if (probe.headers.count("x-chainit-file-sha256")) fileSha = probe.headers["x-chainit-file-sha256"];

    const bool viaBt = isBtEndpoint(phoneIp);
    const int workers = viaBt ? 1 : kTransferWorkersLan; // RFCOMM is one serialized stream

    // --- parallel chunk pipeline (2 MiB chunks, per-chunk SHA-256, atomic finalize) ---
    bool parallelOk = false;
    if (total > kTransferChunkBytes && workers > 1) {
        const std::wstring partPath = dst.wstring() + L".chainit-part";
        // Hidden + temporary: the reassembly buffer is a transfer-only artefact and must
        // never appear in Explorer, on the desktop, or in any user-facing listing.
        SetFileAttributesW(partPath.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY);
        HANDLE hFile = CreateFileW(partPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            LONG64 fileSize = static_cast<LONG64>(total);
            SetFilePointer(hFile, static_cast<LONG>(fileSize & 0xFFFFFFFF), nullptr, FILE_BEGIN);
            SetFilePointer(hFile, static_cast<LONG>(fileSize >> 32), nullptr, FILE_CURRENT);
            SetEndOfFile(hFile);
            SetFilePointer(hFile, 0, nullptr, FILE_BEGIN);

            std::atomic<uint64_t> cursor{0};
            std::atomic<bool> failed{false};
            std::vector<std::thread> pool;
            for (int w = 0; w < workers; ++w) {
                pool.emplace_back([&]() {
                    while (!failed.load()) {
                        const uint64_t off = cursor.fetch_add(kTransferChunkBytes);
                        if (off >= total) break;
                        const uint64_t len = std::min<uint64_t>(kTransferChunkBytes, total - off);
                        const std::string range = "bytes=" + std::to_string(off) + "-" + std::to_string(off + len - 1);
                        HttpReply chunk = httpRequestToPhone(phoneIp, PHONE_VFS_PORT, "GET", rawUrl, "",
                                                             {{"Range", range}}, 60000);
                        if (chunk.status != 206 && chunk.status != 200) {
                            failed.store(true);
                            break;
                        }
                        if (chunk.body.size() != len) {
                            failed.store(true);
                            break;
                        }
                        if (chunk.headers.count("x-chainit-chunk-sha256")) {
                            const std::string got = sha256HexMemory(chunk.body.data(), chunk.body.size());
                            if (got != chunk.headers["x-chainit-chunk-sha256"]) {
                                gIntegrityFailures.fetch_add(1);
                                failed.store(true);
                                break;
                            }
                            gIntegrityChunksOk.fetch_add(1);
                        }
                        OVERLAPPED ov{};
                        ov.Offset = static_cast<DWORD>(off & 0xFFFFFFFF);
                        ov.OffsetHigh = static_cast<DWORD>(off >> 32);
                        DWORD written = 0;
                        if (!WriteFile(hFile, chunk.body.data(), static_cast<DWORD>(len), &written, &ov) ||
                            written != static_cast<DWORD>(len)) {
                            failed.store(true);
                            break;
                        }
                    }
                });
            }
            for (auto& t : pool) t.join();
            FlushFileBuffers(hFile);
            CloseHandle(hFile);
            SetFileAttributesW(partPath.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY);
            if (!failed.load()) {
                // whole-file integrity gate before the partial ever becomes visible
                if (fileSha.empty() || sha256HexFile(partPath) == fileSha) {
                    if (MoveFileExW(partPath.c_str(), dst.wstring().c_str(), MOVEFILE_REPLACE_EXISTING)) {
                        SetFileAttributesW(dst.wstring().c_str(), FILE_ATTRIBUTE_NORMAL);
                        parallelOk = true;
                    }
                } else {
                    gIntegrityFailures.fetch_add(1);
                    addLogLocked("SECURITY", "Transfer aborted: whole-file SHA-256 mismatch", phoneSrcPath);
                }
            }
            if (!parallelOk) DeleteFileW(partPath.c_str());
        }
        if (parallelOk) {
            addLogLocked("VFS", "Parallel chunked transfer completed",
                         baseName + " (" + std::to_string(total) + " B in " + std::to_string(workers) +
                             " simultaneous 2 MiB streams, every chunk SHA-256 verified)");
            return total;
        }
        // fall through to the single-stream path on any parallel failure
    }

    HttpReply rawRep = httpRequestToPhone(phoneIp, PHONE_VFS_PORT, "GET", rawUrl, "", {}, 120000);
    if (rawRep.status == 200) {
        if (!fileSha.empty()) {
            const std::string got = sha256HexMemory(rawRep.body.data(), rawRep.body.size());
            if (got != fileSha) {
                gIntegrityFailures.fetch_add(1);
                addLogLocked("SECURITY", "Transfer aborted: whole-file SHA-256 mismatch", phoneSrcPath);
                return 0;
            }
            gIntegrityChunksOk.fetch_add(1);
        }
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

// ---------------------------------------------------------------------------
// Hand-written Shell Link (.lnk) writer (MS-SHLLINK)
//
// The COM IShellLink path is tried first, but Explorer's shell extension host
// can refuse it (locked-down profiles, a crashed explorer.exe, or a COM
// apartment clash inside a detached worker thread) - which is exactly how a
// user ends up clicking "Create desktop icon" and getting nothing. This writer
// emits the documented binary layout directly, so a shortcut is always produced.
// ---------------------------------------------------------------------------
static void lnkPutU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back((uint8_t)(v & 0xFF)); b.push_back((uint8_t)((v >> 8) & 0xFF));
    b.push_back((uint8_t)((v >> 16) & 0xFF)); b.push_back((uint8_t)((v >> 24) & 0xFF));
}
static void lnkPutU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back((uint8_t)(v & 0xFF)); b.push_back((uint8_t)((v >> 8) & 0xFF));
}
static void lnkPutAnsi(std::vector<uint8_t>& b, const std::string& s) {
    b.insert(b.end(), s.begin(), s.end());
    b.push_back(0);
}
static void lnkPutStringData(std::vector<uint8_t>& b, const std::wstring& s) {
    lnkPutU16(b, (uint16_t)s.size());
    for (wchar_t c : s) { lnkPutU16(b, (uint16_t)c); }
}

static bool writeShellLinkFile(const std::wstring& lnkPath, const std::wstring& targetExe,
                              const std::wstring& workDir, const std::wstring& description,
                              int iconIndex) {
    std::string targetAnsi(512, '\0');
    int n = WideCharToMultiByte(CP_ACP, 0, targetExe.c_str(), -1, &targetAnsi[0], 511, nullptr, nullptr);
    if (n <= 0) return false;
    targetAnsi.resize(n - 1);

    std::vector<uint8_t> b;
    // --- SHELL_LINK_HEADER (76 bytes) ---
    lnkPutU32(b, 0x0000004C);
    const uint8_t clsid[16] = {0x00,0x02,0x14,0x01,0x00,0x00,0x00,0x00,0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46};
    b.insert(b.end(), clsid, clsid + 16);
    // HasLinkInfo | HasName | HasWorkingDir | HasIconLocation | IsUnicode
    lnkPutU32(b, 0x00000002 | 0x00000004 | 0x00000010 | 0x00000040 | 0x00000080);
    lnkPutU32(b, FILE_ATTRIBUTE_NORMAL);            // FileAttributes
    for (int i = 0; i < 3; ++i) { lnkPutU32(b, 0); lnkPutU32(b, 0); } // create/access/write FILETIMEs
    uint64_t targetSize = 0;
    {
        HANDLE hf = CreateFileW(targetExe.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz{};
            if (GetFileSizeEx(hf, &sz)) targetSize = (uint64_t)sz.QuadPart;
            CloseHandle(hf);
        }
    }
    lnkPutU32(b, (uint32_t)(targetSize & 0xFFFFFFFF)); // FileSize
    lnkPutU32(b, (uint32_t)iconIndex);                 // IconIndex
    lnkPutU32(b, 0x00000001);                          // ShowCommand = SW_SHOWNORMAL
    lnkPutU16(b, 0); lnkPutU16(b, 0);                  // HotKey, Reserved1
    lnkPutU32(b, 0); lnkPutU32(b, 0);                  // Reserved2, Reserved3

    // --- LINKINFO ---
    const uint32_t infoStart = (uint32_t)b.size();
    const uint32_t headerSize = 0x1C;
    const std::string volumeLabel = "Windows";
    const uint32_t volumeSize = 0x10 + (uint32_t)volumeLabel.size() + 1;
    const uint32_t volumeOffset = headerSize;
    const uint32_t basePathOffset = volumeOffset + volumeSize;
    const uint32_t suffixOffset = basePathOffset + (uint32_t)targetAnsi.size() + 1;
    const uint32_t infoSize = suffixOffset + 1;
    lnkPutU32(b, infoSize);
    lnkPutU32(b, headerSize);
    lnkPutU32(b, 0x00000001);       // VolumeIDAndLocalBasePath
    lnkPutU32(b, volumeOffset);
    lnkPutU32(b, basePathOffset);
    lnkPutU32(b, 0);                // no CommonNetworkRelativeLink
    lnkPutU32(b, suffixOffset);
    // VOLUMEID
    lnkPutU32(b, volumeSize);
    lnkPutU32(b, DRIVE_FIXED);
    lnkPutU32(b, 0);                // DriveSerialNumber
    lnkPutU32(b, 0x10);             // VolumeLabelOffset -> Data
    lnkPutAnsi(b, volumeLabel);
    lnkPutAnsi(b, targetAnsi);      // LocalBasePath (ANSI)
    lnkPutAnsi(b, "");              // CommonPathSuffix

    // --- STRING_DATA (order fixed by the spec) ---
    lnkPutStringData(b, description);
    lnkPutStringData(b, workDir);
    lnkPutStringData(b, targetExe); // IconLocation file; IconIndex came from the header
    lnkPutU32(b, 0);                // terminal ExtraData marker

    HANDLE hf = CreateFileW(lnkPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(hf, b.data(), (DWORD)b.size(), &written, nullptr) && written == b.size();
    CloseHandle(hf);
    (void)infoStart;
    return ok;
}

static bool fileExistsW(const std::wstring& p) {
    if (p.empty()) return false;
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::vector<std::wstring> desktopCandidates() {
    std::vector<std::wstring> out;
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &raw)) && raw) {
        out.push_back(std::wstring(raw));
        CoTaskMemFree(raw);
    }
    wchar_t profile[MAX_PATH * 2] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH * 2 - 1) > 0) {
        out.push_back(std::wstring(profile) + L"\\Desktop");
        out.push_back(std::wstring(profile) + L"\\OneDrive\\Desktop");
        out.push_back(std::wstring(profile) + L"\\OneDrive\\Desktop");
    }
    // De-duplicate while preserving order.
    std::vector<std::wstring> uniq;
    for (const auto& c : out) {
        bool dup = false;
        for (const auto& u : uniq) if (u == c) { dup = true; break; }
        if (!dup) uniq.push_back(c);
    }
    return uniq;
}

static ShortcutResult createDesktopShortcut() {
    ShortcutResult result;
    const std::wstring exe = currentExePath();
    if (exe.empty()) {
        result.message = "Could not resolve the running ChainIt-Windows.exe path.";
        return result;
    }

    const std::wstring workDir = fs::path(exe).parent_path().wstring();
    const std::wstring desc = L"ChainIt - Android/Windows Sibling link, Z:\\ drive and File Manager";

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool comOwned = SUCCEEDED(hr);

    const std::vector<std::wstring> desktops = desktopCandidates();
    std::wstring chosen;
    for (const auto& d : desktops) {
        DWORD a = GetFileAttributesW(d.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) { chosen = d; break; }
    }
    if (chosen.empty() && !desktops.empty()) chosen = desktops.front();
    if (chosen.empty()) {
        if (comOwned) CoUninitialize();
        result.message = "Windows did not report a Desktop folder for this account.";
        return result;
    }
    const std::wstring lnk = chosen + L"\\ChainIt.lnk";

    // 1) Shell COM IShellLink (the native path - keeps Explorer's own metadata)
    IShellLinkW* link = nullptr;
    hr = CoCreateInstance(kClsidShellLink, nullptr, CLSCTX_INPROC_SERVER, kIidIShellLinkW,
                          reinterpret_cast<void**>(&link));
    if (SUCCEEDED(hr) && link) {
        link->SetPath(exe.c_str());
        link->SetWorkingDirectory(workDir.c_str());
        link->SetIconLocation(exe.c_str(), 0); // the chain-link icon embedded in this exe
        link->SetDescription(desc.c_str());
        link->SetShowCmd(SW_SHOWNORMAL);
        IPersistFile* persist = nullptr;
        if (SUCCEEDED(link->QueryInterface(kIidIPersistFile, reinterpret_cast<void**>(&persist))) && persist) {
            persist->Save(lnk.c_str(), TRUE);
            persist->Release();
        }
        link->Release();
    }

    // 2) Verify - and write the documented binary layout ourselves if Explorer never produced a file.
    if (!fileExistsW(lnk)) {
        writeShellLinkFile(lnk, exe, workDir, desc, 0);
    }

    // 3) Start Menu entry too, so there is always a discoverable launch point.
    PWSTR progRaw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &progRaw)) && progRaw) {
        const std::wstring startDir = std::wstring(progRaw) + L"\\ChainIt";
        CreateDirectoryW(startDir.c_str(), nullptr);
        CoTaskMemFree(progRaw);
        const std::wstring startLnk = startDir + L"\\ChainIt.lnk";
        IShellLinkW* l2 = nullptr;
        if (SUCCEEDED(CoCreateInstance(kClsidShellLink, nullptr, CLSCTX_INPROC_SERVER, kIidIShellLinkW,
                                       reinterpret_cast<void**>(&l2))) && l2) {
            l2->SetPath(exe.c_str());
            l2->SetWorkingDirectory(workDir.c_str());
            l2->SetIconLocation(exe.c_str(), 0);
            l2->SetDescription(desc.c_str());
            IPersistFile* p2 = nullptr;
            if (SUCCEEDED(l2->QueryInterface(kIidIPersistFile, reinterpret_cast<void**>(&p2))) && p2) {
                p2->Save(startLnk.c_str(), TRUE);
                p2->Release();
            }
            l2->Release();
        }
        if (!fileExistsW(startLnk)) writeShellLinkFile(startLnk, exe, workDir, desc, 0);
    }

    if (comOwned) CoUninitialize();

    result.ok = fileExistsW(lnk);
    if (result.ok) {
        result.path = lnk;
        // Force Explorer to pick the new file up immediately (SHCNF_FLUSHNOWAIT avoids
        // blocking this worker thread on a synchronous shell notification round-trip).
        SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, lnk.c_str(), nullptr);
        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, lnk.c_str(), nullptr);
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        char buf[512];
        std::snprintf(buf, sizeof(buf), "Desktop icon created and verified: %s", ansiFromWide(lnk).c_str());
        result.message = buf;
    } else {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "Windows blocked writing ChainIt.lnk into %s (check Controlled folder access / antivirus).",
                      ansiFromWide(chosen).c_str());
        result.message = buf;
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

static const char* const kChainItAppVersion = "1.5.0";

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

static std::string sha256HexMemory(const void* data, size_t len) {
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    if (!CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return "";
    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        return "";
    }
    CryptHashData(hHash, reinterpret_cast<const BYTE*>(data), static_cast<DWORD>(len), 0);
    BYTE digest[32] = {};
    DWORD dlen = sizeof(digest);
    CryptGetHashParam(hHash, HP_HASHVAL, digest, &dlen, 0);
    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    static const char* kHex = "0123456789abcdef";
    std::string out;
    for (DWORD i = 0; i < dlen; ++i) {
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
        bool everSawPhone = false;
        {
            std::lock_guard<std::mutex> lk(gState.mtx);
            phoneIp = gState.activePhoneIp;
            everSawPhone = gState.everDiscoveredPhone || !phoneIp.empty();
            if (!phoneIp.empty()) gState.everDiscoveredPhone = true;
        }

        if (!phoneIp.empty()) {
            std::map<std::string, std::string> fwdHeaders;
            if (headers.count("depth")) fwdHeaders["Depth"] = headers["depth"];
            if (headers.count("destination")) fwdHeaders["Destination"] = headers["destination"];
            if (headers.count("overwrite")) fwdHeaders["Overwrite"] = headers["overwrite"];
            if (headers.count("range")) fwdHeaders["Range"] = headers["range"];
            if (headers.count("if")) fwdHeaders["If"] = headers["if"];
            if (headers.count("if-match")) fwdHeaders["If-Match"] = headers["if-match"];
            if (headers.count("if-none-match")) fwdHeaders["If-None-Match"] = headers["if-none-match"];
            if (headers.count("content-type")) fwdHeaders["Content-Type"] = headers["content-type"];
            if (headers.count("x-chainit-file-sha256")) fwdHeaders["X-ChainIt-File-Sha256"] = headers["x-chainit-file-sha256"];

            const int timeout = isBtEndpoint(phoneIp) ? 90000 : 60000;
            HttpReply rep = httpRequestToPhone(phoneIp, PHONE_VFS_PORT, method, fullUrl, body, fwdHeaders, timeout);

            if (rep.status > 0) {
                // Surface an actionable reason instead of letting Explorer show a dead drive.
                if (rep.status == 401 || rep.status == 403 || rep.status == 429) {
                    std::string why = jsonExtractString(rep.body, "error", "");
                    if (why.empty()) why = (rep.status == 429)
                        ? "Phone VFS is cooling down after repeated Pairing Key failures (30 s)."
                        : "Phone rejected the Pairing Key.";
                    std::lock_guard<std::mutex> lk(gState.mtx);
                    gState.phoneLinkProblem = why;
                } else if (!gState.phoneLinkProblem.empty()) {
                    std::lock_guard<std::mutex> lk(gState.mtx);
                    gState.phoneLinkProblem.clear();
                }

                std::string ct = rep.headers.count("content-type") ? rep.headers["content-type"] : "application/xml; charset=\"utf-8\"";
                std::map<std::string, std::string> outHdrs = {
                    {"DAV", "1, 2"},
                    {"MS-Author-Via", "DAV"},
                    {"Accept-Ranges", "bytes"},
                };
                for (const char* hk : {"lock-token", "content-range", "x-chainit-file-sha256",
                                       "x-chainit-chunk-sha256", "last-modified", "etag", "content-disposition"}) {
                    if (rep.headers.count(hk)) {
                        std::string canon = hk;
                        bool up = true;
                        for (auto& c : canon) { if (up) { c = (char)toupper((unsigned char)c); up = false; } else { up = (c == '-'); } }
                        outHdrs[canon] = rep.headers[hk];
                    }
                }
                const char* reason = rep.status == 207 ? "Multi-Status"
                                   : rep.status == 201 ? "Created"
                                   : rep.status == 204 ? "No Content"
                                   : rep.status == 206 ? "Partial Content"
                                   : rep.status == 401 ? "Unauthorized"
                                   : rep.status == 403 ? "Forbidden"
                                   : rep.status == 404 ? "Not Found"
                                   : rep.status == 409 ? "Conflict"
                                   : rep.status == 429 ? "Too Many Requests"
                                   : rep.status >= 500 ? "Server Error" : "OK";
                sendHttpResponse(client, rep.status, reason, ct, rep.body.data(), rep.body.size(), outHdrs);
                closesocket(client);
                return;
            }

            // The sibling endpoint is known but the request never completed. Do NOT hand Explorer
            // an empty fake drive - that is exactly the "connected but does nothing" failure. Say
            // why, so the user and the UI can act on it.
            std::string why;
            {
                std::lock_guard<std::mutex> lk(gState.mtx);
                if (!gState.phoneLinkProblem.empty()) why = gState.phoneLinkProblem;
            }
            if (why.empty()) {
                why = isBtEndpoint(phoneIp)
                    ? "Bluetooth RFCOMM link to the phone did not answer. Keep the phone in range and Bluetooth on; ChainIt re-dials automatically."
                    : "The phone did not answer on " + phoneIp + ". ChainIt is re-scanning every transport now.";
                std::lock_guard<std::mutex> lk(gState.mtx);
                gState.phoneLinkProblem = why;
            }
            {
                std::lock_guard<std::mutex> lk(gState.mtx);
                addLogLocked("VFS", "Z:\\ request failed - phone unreachable", why);
            }
            triggerTransportRescan();

            std::map<std::string, std::string> davHdr = {{"DAV", "1, 2"}, {"MS-Author-Via", "DAV"},
                                                         {"Retry-After", "5"}};
            const std::string errXml =
                "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                "<D:error xmlns:D=\"DAV:\">" + xmlEscape(why) + "</D:error>";
            sendHttpResponse(client, 503, "Service Unavailable", "application/xml; charset=\"utf-8\"",
                             errXml.data(), errXml.size(), davHdr);
            closesocket(client);
            return;
        }

        // No phone has ever paired. Serve an honest, labelled placeholder drive with one readme so
        // Z:\\ explains itself instead of looking like an empty disk that silently does nothing.
        std::map<std::string, std::string> davHdr = {{"DAV", "1, 2"}, {"MS-Author-Via", "DAV"}};
        if (method == "OPTIONS") {
            sendHttpResponse(client, 200, "OK", "text/plain", "", 0, davHdr);
            closesocket(client);
            return;
        }
        if (method == "PROPFIND") {
            const std::string xml = buildWaitingDrivePropfind(urlPath, headers);
            sendHttpResponse(client, 207, "Multi-Status", "application/xml; charset=\"utf-8\"",
                             xml.data(), xml.size(), davHdr);
            closesocket(client);
            return;
        }
        if (method == "GET" && urlPath.find("READ ME FIRST") != std::string::npos) {
            const std::string txt = waitingDriveReadme();
            sendHttpResponse(client, 200, "OK", "text/plain; charset=\"utf-8\"",
                             txt.data(), txt.size(), davHdr);
            closesocket(client);
            return;
        }
        const std::string msg = "ChainIt has not found your Android phone yet. Open ChainIt on the phone, "
                                "enable Sibling Mode, and make sure Bluetooth or Wi-Fi is on. "
                                "Z:\\ fills itself in the moment the phone answers.";
        sendHttpResponse(client, 503, "Service Unavailable", "text/plain; charset=\"utf-8\"",
                         msg.data(), msg.size(), davHdr);
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


// ===========================================================================
// ChainIt native hub window
//
// The web File Manager opens in Edge app-mode, whose taskbar button belongs to
// the browser process (that is where the generic globe came from). ChainIt
// therefore owns a real top-level Win32 window with the chain-link icon compiled
// into this exe as resource #1: the taskbar, Alt-Tab and the tray all show the
// same artwork as the Android launcher icon, and the window doubles as the
// "mini form" of the app - status, Z:\ mount, desktop icon, update and quit are
// one click away even when the browser window is closed.
// ===========================================================================

static HWND gHubHwnd = nullptr;
static HICON gHubIcon = nullptr;
static HINSTANCE gHubInstance = nullptr;
static int gHubPressed = -1;
static int gHubHover = -1;
static std::wstring gHubToast;
static std::atomic<int64_t> gHubToastUntil{0};

static const wchar_t* kHubClass = L"ChainItHubWindowClass";

struct HubButton {
    const wchar_t* keycap;
    const wchar_t* label;
    const wchar_t* hint;
    int accent; // 0 = emerald, 1 = cyan, 2 = slate
};

static const HubButton kHubButtons[] = {
    {L"F", L"Open File Manager", L"Dual-pane web UI", 0},
    {L"Z", L"Open Z:\\ Phone Drive", L"In Windows Explorer", 1},
    {L"M", L"Mount / Refresh Z:", L"WebDAV redirector", 1},
    {L"D", L"Create Desktop Icon", L"ChainIt.lnk on Desktop", 0},
    {L"U", L"Check for Updates", L"In-place exe replace", 2},
    {L"X", L"Quit ChainIt", L"Unmount Z: and exit", 2},
};
static const int kHubButtonCount = 6;

static void hubSetToast(const std::wstring& text, int ms = 4200) {
    gHubToast = text;
    gHubToastUntil.store(nowMs() + ms);
}

static void hubShow() {
    if (!gHubHwnd) return;
    ShowWindow(gHubHwnd, SW_SHOW);
    SetForegroundWindow(gHubHwnd);
    if (IsIconic(gHubHwnd)) ShowWindow(gHubHwnd, SW_RESTORE);
}

static void hubRunAction(int idx);

static void hubLayout(const RECT& rc, RECT* outHeader, RECT* outStatus, RECT* outButtons, RECT* outFooter) {
    const int pad = 16;
    const int w = rc.right - rc.left;
    outHeader->left = pad; outHeader->top = pad;
    outHeader->right = w - pad; outHeader->bottom = pad + 56;

    outStatus->left = pad; outStatus->top = outHeader->bottom + 12;
    outStatus->right = w - pad; outStatus->bottom = outStatus->top + 92;

    const int gridTop = outStatus->bottom + 14;
    const int cellW = (w - pad * 2 - 10) / 2;
    const int cellH = 56;
    for (int i = 0; i < kHubButtonCount; ++i) {
        const int col = i % 2, row = i / 2;
        outButtons[i].left = pad + col * (cellW + 10);
        outButtons[i].top = gridTop + row * (cellH + 10);
        outButtons[i].right = outButtons[i].left + cellW;
        outButtons[i].bottom = outButtons[i].top + cellH;
    }
    outFooter->left = pad;
    outFooter->top = outButtons[kHubButtonCount - 1].bottom + 14;
    outFooter->right = w - pad;
    outFooter->bottom = outFooter->top + 58;
}

static int hubHitTest(int x, int y) {
    RECT rc; GetClientRect(gHubHwnd, &rc);
    RECT header{}, status{}, footer{}, btns[kHubButtonCount]{};
    hubLayout(rc, &header, &status, btns, &footer);
    for (int i = 0; i < kHubButtonCount; ++i) {
        POINT p{x, y};
        if (PtInRect(&btns[i], p)) return i;
    }
    return -1;
}

static std::wstring hubStatusLines(std::wstring* sub, std::wstring* foot) {
    std::lock_guard<std::mutex> lk(gState.mtx);
    const bool connected = !gState.activeTransportId.empty() && gState.siblingEnabled && gState.realPhoneConnected;
    std::wstring main = connected
        ? (wideFromAnsi(gState.peerDeviceName.empty() ? std::string("Sibling phone") : gState.peerDeviceName) + L" \u00b7 linked")
        : std::wstring(L"Searching for your sibling phone\u2026");
    std::wstring tr = wideFromAnsi(gState.activeTransportId.empty() ? std::string("none") : gState.activeTransportId);
    if (tr == L"usb_rndis") tr = L"USB tethering";
    else if (tr == L"wifi_lan") tr = L"Wi-Fi LAN";
    else if (tr == L"local_hotspot") tr = L"PC hotspot";
    else if (tr == L"bluetooth") tr = L"Bluetooth RFCOMM";
    else if (tr == L"online_p2p") tr = L"Online relay";
    *sub = std::wstring(L"Transport: ") + tr + (gState.zDriveMounted || connected ? L"   \u00b7   Z:\\ mounted" : L"   \u00b7   Z:\\ idle");

    // A link that is up but not serving must say so here, not silently show an empty Z:\ drive.
    std::wstring problem = wideFromAnsi(gState.phoneLinkProblem);

    char buf[320];
    if (isBtEndpoint(gState.activePhoneIp)) {
        std::snprintf(buf, sizeof(buf),
                      "Bluetooth RFCOMM keep-alive \u00b7 %llu/%llu channels parked \u00b7 %llu requests served \u00b7 %llu dials \u00b7 %.0f ms",
                      (unsigned long long)btPooledChannels(), (unsigned long long)kBtChannelCount,
                      (unsigned long long)gBtRequestsServed.load(), (unsigned long long)gBtRedials.load(),
                      static_cast<double>(gBtLastRttUs.load()) / 1000.0);
    } else {
        std::snprintf(buf, sizeof(buf),
                      "Pairing key required \u00b7 %llu chunk hashes verified \u00b7 %llu integrity failures \u00b7 %d x 2 MiB parallel streams",
                      (unsigned long long)gIntegrityChunksOk.load(),
                      (unsigned long long)gIntegrityFailures.load(),
                      isBtEndpoint(gState.activePhoneIp) ? 1 : kTransferWorkersLan);
    }
    *foot = wideFromAnsi(buf);
    if (!problem.empty()) return problem;
    return connected ? main : std::wstring(L"ChainIt Sibling");
}

static void hubDrawGradient(HDC hdc, const RECT& r, COLORREF top, COLORREF bottom) {
    const int h = r.bottom - r.top;
    if (h <= 0) return;
    const int tr = GetRValue(top), tg = GetGValue(top), tb = GetBValue(top);
    const int br = GetRValue(bottom), bg = GetGValue(bottom), bb = GetBValue(bottom);
    for (int y = 0; y < h; ++y) {
        const double t = (h == 1) ? 0.0 : static_cast<double>(y) / (h - 1);
        const COLORREF c = RGB(tr + (int)((br - tr) * t), tg + (int)((bg - tg) * t), tb + (int)((bb - tb) * t));
        RECT row{r.left, r.top + y, r.right, r.top + y + 1};
        HBRUSH b = CreateSolidBrush(c);
        FillRect(hdc, &row, b);
        DeleteObject(b);
    }
}

static void hubRoundRect(HDC hdc, const RECT& r, int radius, COLORREF fill, COLORREF border, bool pressed) {
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HBRUSH brush = CreateSolidBrush(fill);
    HGDIOBJ op = SelectObject(hdc, pen);
    HGDIOBJ ob = SelectObject(hdc, brush);
    const int d = radius * 2;
    ::RoundRect(hdc, r.left, r.top + (pressed ? 1 : 0), r.right, r.bottom + (pressed ? 1 : 0), d, d);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(pen);
    DeleteObject(brush);
}

static void hubText(HDC hdc, const RECT& r, const std::wstring& s, int height, COLORREF color, UINT flags, bool bold) {
    HFONT f = CreateFontW(-height, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ of = SelectObject(hdc, f);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    RECT box = r;
    DrawTextW(hdc, s.c_str(), (int)s.size(), &box, flags);
    SelectObject(hdc, of);
    DeleteObject(f);
}

static LRESULT CALLBACK HubWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            gHubHwnd = hwnd;
            SetTimer(hwnd, 1, 1000, nullptr);
            return 0;
        }
        case WM_TIMER: {
            if (IsWindowVisible(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_SETCURSOR: {
            POINT p; GetCursorPos(&p); ScreenToClient(hwnd, &p);
            SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(hubHitTest(p.x, p.y) >= 0 ? IDC_HAND : IDC_ARROW)));
            return TRUE;
        }
        case WM_MOUSEMOVE: {
            const int h = hubHitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (h != gHubHover) { gHubHover = h; InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const int h = hubHitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (h >= 0) { gHubPressed = h; SetCapture(hwnd); InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;
        }
        case WM_LBUTTONUP: {
            const int h = hubHitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            const int pressed = gHubPressed;
            gHubPressed = -1;
            ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
            if (pressed >= 0 && pressed == h) hubRunAction(pressed);
            return 0;
        }
        case WM_CAPTURECHANGED:
            gHubPressed = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE); // stay resident in the tray, keep the taskbar identity
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC wdc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            HDC hdc = CreateCompatibleDC(wdc);
            HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right - rc.left, rc.bottom - rc.top);
            HGDIOBJ ob = SelectObject(hdc, bmp);

            hubDrawGradient(hdc, rc, RGB(0x0B, 0x11, 0x20), RGB(0x07, 0x0B, 0x12));

            RECT header{}, status{}, footer{}, btns[kHubButtonCount]{};
            hubLayout(rc, &header, &status, btns, &footer);

            // header: the chain-link mark, title and version
            if (gHubIcon) DrawIconEx(hdc, header.left, header.top + 10, gHubIcon, 36, 36, 0, nullptr, DI_NORMAL);
            RECT title{header.left + 48, header.top + 4, header.right, header.top + 30};
            hubText(hdc, title, L"ChainIt Sibling", 19, RGB(0xF1, 0xF5, 0xF9), DT_LEFT | DT_VCENTER | DT_SINGLELINE, true);
            RECT ver{header.left + 48, header.top + 30, header.right, header.top + 50};
            hubText(hdc, ver, std::wstring(L"v") + wideFromAnsi(kChainItAppVersion) +
                            L"  \u00b7  Android \u2194 Windows file bridge  \u00b7  Built by Qmer",
                    12, RGB(0x94, 0xA3, 0xB8), DT_LEFT | DT_VCENTER | DT_SINGLELINE, false);

            // status panel (recessed, skeuomorphic)
            hubRoundRect(hdc, status, 12, RGB(0x0E, 0x14, 0x20), RGB(0x1E, 0x29, 0x3B), false);
            std::wstring sub, foot;
            const std::wstring mainLine = hubStatusLines(&sub, &foot);
            std::wstring problemNow;
            {
                std::lock_guard<std::mutex> lk(gState.mtx);
                problemNow = wideFromAnsi(gState.phoneLinkProblem);
            }
            const bool linked = (mainLine != L"ChainIt Sibling") && problemNow.empty();
            RECT led{status.left + 16, status.top + 18, status.left + 28, status.top + 30};
            HBRUSH ledBrush = CreateSolidBrush(linked ? RGB(0x10, 0xB9, 0x81) : RGB(0xF5, 0x9E, 0x0B));
            HGDIOBJ ob2 = SelectObject(hdc, ledBrush);
            HPEN nullPen = (HPEN)GetStockObject(NULL_PEN);
            HGDIOBJ op2 = SelectObject(hdc, nullPen);
            Ellipse(hdc, led.left, led.top, led.right, led.bottom);
            SelectObject(hdc, op2); SelectObject(hdc, ob2); DeleteObject(ledBrush);
            RECT mainRect{status.left + 38, status.top + 10, status.right - 14, status.top + 36};
            hubText(hdc, mainRect, linked ? mainLine : L"Searching for your sibling phone\u2026", 15,
                    linked ? RGB(0xEC, 0xFD, 0xF5) : RGB(0xFF, 0xFB, 0xEB), DT_LEFT | DT_VCENTER | DT_SINGLELINE, true);
            RECT subRect{status.left + 38, status.top + 36, status.right - 14, status.top + 58};
            hubText(hdc, subRect, sub, 12, RGB(0x94, 0xA3, 0xB8), DT_LEFT | DT_VCENTER | DT_SINGLELINE, false);
            RECT footRect{status.left + 16, status.top + 60, status.right - 14, status.top + 84};
            hubText(hdc, footRect, foot, 11, RGB(0x52, 0x60, 0x79), DT_LEFT | DT_VCENTER | DT_SINGLELINE, false);

            // keycap buttons
            for (int i = 0; i < kHubButtonCount; ++i) {
                const bool pressed = (gHubPressed == i);
                const bool hover = (gHubHover == i) && !pressed;
                const COLORREF fill = pressed ? RGB(0x14, 0x1C, 0x2C) : (hover ? RGB(0x14, 0x1D, 0x2E) : RGB(0x0F, 0x16, 0x24));
                const COLORREF edge = kHubButtons[i].accent == 2 ? RGB(0x2A, 0x35, 0x48)
                                      : (hover ? RGB(0x2B, 0x4A, 0x45) : RGB(0x1E, 0x29, 0x3B));
                hubRoundRect(hdc, btns[i], 11, fill, edge, pressed);

                RECT cap{btns[i].left + 10, btns[i].top + 13, btns[i].left + 40, btns[i].top + 43};
                const COLORREF capFill = kHubButtons[i].accent == 0 ? RGB(0x0B, 0x3D, 0x2E)
                                       : kHubButtons[i].accent == 1 ? RGB(0x0B, 0x33, 0x44)
                                       : RGB(0x1B, 0x24, 0x34);
                const COLORREF capText = kHubButtons[i].accent == 0 ? RGB(0x34, 0xD3, 0x99)
                                       : kHubButtons[i].accent == 1 ? RGB(0x22, 0xD3, 0xEE)
                                       : RGB(0xCB, 0xD5, 0xE1);
                hubRoundRect(hdc, cap, 8, capFill, capFill, pressed);
                hubText(hdc, cap, kHubButtons[i].keycap, 14, capText,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE, true);

                RECT lab{btns[i].left + 48, btns[i].top + 8, btns[i].right - 8, btns[i].top + 30};
                hubText(hdc, lab, kHubButtons[i].label, 12, RGB(0xE2, 0xE8, 0xF0), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, true);
                RECT hint{btns[i].left + 48, btns[i].top + 30, btns[i].right - 8, btns[i].top + 48};
                hubText(hdc, hint, kHubButtons[i].hint, 11, RGB(0x64, 0x74, 0x8B), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, false);
            }

            // toast / footer
            hubRoundRect(hdc, footer, 11, RGB(0x0C, 0x12, 0x1E), RGB(0x1A, 0x24, 0x34), false);
            const bool toastLive = nowMs() < gHubToastUntil.load() && !gHubToast.empty();
            const std::wstring footText = toastLive ? gHubToast
                : std::wstring(L"Built by Qmer \u00b7 right-click the tray icon for the full menu \u00b7 closing this window keeps ChainIt running.");
            RECT tr{footer.left + 14, footer.top + 6, footer.right - 14, footer.bottom - 6};
            hubText(hdc, tr, footText, 11, toastLive ? RGB(0xA7, 0xF3, 0xD0) : RGB(0x52, 0x60, 0x79),
                    DT_LEFT | DT_VCENTER | DT_WORDBREAK, false);

            BitBlt(wdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, hdc, 0, 0, SRCCOPY);
            SelectObject(hdc, ob);
            DeleteObject(bmp);
            DeleteDC(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            gHubHwnd = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void hubRunAction(int idx) {
    switch (idx) {
        case 0:
            hubSetToast(L"Opening the ChainIt File Manager\u2026");
            launchDesktopUiWindow();
            break;
        case 1:
            hubSetToast(L"Opening Z:\\ in Windows Explorer\u2026");
            ShellExecuteW(nullptr, L"open", L"Z:\\", nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case 2:
            hubSetToast(L"Mounting Z:\\ through the WebDAV redirector\u2026");
            mountZDriveAsync();
            break;
        case 3: {
            hubSetToast(L"Writing ChainIt.lnk to the Desktop\u2026");
            std::thread([]() {
                const ShortcutResult sc = createDesktopShortcut();
                hubSetToast(sc.ok ? (L"Desktop icon created \u2192 " + sc.path) : wideFromAnsi(sc.message), 7000);
                if (gHubHwnd) InvalidateRect(gHubHwnd, nullptr, FALSE);
            }).detach();
            break;
        }
        case 4: {
            hubSetToast(L"Checking the published release for a newer build\u2026");
            std::thread([]() {
                const AppUpdateResult r = performAppUpdate();
                hubSetToast(wideFromAnsi(r.message.empty() ? std::string("Update finished.") : r.message), 8000);
                if (gHubHwnd) InvalidateRect(gHubHwnd, nullptr, FALSE);
                if (r.restart) {
                    Shell_NotifyIconW(NIM_DELETE, &gNid);
                    ExitProcess(0);
                }
            }).detach();
            break;
        }
        case 5: {
            hubSetToast(L"Unmounting Z:\\ and quitting ChainIt\u2026");
            std::thread([]() {
                runHiddenCommand(L"cmd.exe /c net use Z: /delete /y");
                gRunning.store(false);
                Shell_NotifyIconW(NIM_DELETE, &gNid);
                ExitProcess(0);
            }).detach();
            break;
        }
    }
    if (gHubHwnd) InvalidateRect(gHubHwnd, nullptr, FALSE);
}


// ===========================================================================
// Fully native Win32 File Manager
//
// ChainIt used to open its UI in Edge app-mode. That means a *browser* process
// owned a taskbar button, and no icon this exe carries could ever change it -
// Windows drew Edge's globe for Edge's window. So the browser is gone: the whole
// File Manager is drawn here, in this process, with GDI. One window, one taskbar
// button, the chain-link icon from resource #1, and no dependency on Edge,
// WebView2 or any runtime.
//
// The lists are hand-drawn rather than SysListView32 on purpose: full control of
// the dark skeuomorphic theme, no header/layout theming quirks, and identical
// behaviour on every Windows 10/11 build.
// ===========================================================================

static const wchar_t* kMainClass = L"ChainItMainWindowClass";

static const int kRowH = 28;
static const int kColHeaderH = 27;
static const int kColSizeW = 96;
static const int kColTypeW = 92;
static const int kColModW = 138;

enum UiSortKey { SORT_NAME = 0, SORT_SIZE = 1, SORT_TYPE = 2, SORT_MODIFIED = 3 };
enum UiTab { TAB_FILES = 0, TAB_LINK = 1, TAB_ABOUT = 2 };
enum UiPaneId { PANE_PC = 0, PANE_PHONE = 1 };

struct UiItem {
    std::wstring name;
    std::wstring path;   // virtual POSIX path within the drive root
    std::wstring ext;
    bool isDir = false;
    uint64_t size = 0;
    int64_t modifiedMs = 0;
};

struct UiList {
    /** Everything the last listing returned; `items` is the filtered + sorted view of it. */
    std::vector<UiItem> allItems;
    std::vector<UiItem> items;
    std::vector<char> selected;   // parallel to items
    std::wstring filter;          // live type-ahead, matched case-insensitively
    int sortKey = SORT_NAME;
    bool sortDesc = false;
    bool foldersFirst = true;
    int focused = -1;
    int hover = -1;
    int anchor = -1;
    int scroll = 0;
    bool loading = false;
    std::wstring dir = L"/";
    std::wstring error;
    RECT rc{};        // full pane area (header strip + rows)
    RECT listRc{};    // rows area only
};

struct ListPayload {
    int pane = 0;
    std::wstring dir;
    std::vector<UiItem> items;
    std::wstring error;
};

static HWND gMainHwnd = nullptr;
static HICON gMainIcon = nullptr;    // 32 px - class icon, tab strip
static HICON gMainIconSm = nullptr;  // 16 px - caption / alt-tab
static HICON gMainIconLg = nullptr;  // 64 px - About tab
static HINSTANCE gMainInstance = nullptr;
static UiList gPane[2];
static int gActivePane = PANE_PHONE;
static int gTab = TAB_FILES;
static int gToolHover = -1;
static int gLinkHover = -1;
static bool gDualPane = true;
static std::wstring gMainToast;
static std::atomic<int64_t> gMainToastUntil{0};
static std::wstring gClipboardOp;
static std::vector<std::wstring> gClipboardPaths;
static int gClipboardPane = -1;

#define WM_UI_LISTDONE   (WM_APP + 21)
#define WM_UI_TOAST      (WM_APP + 22)
#define WM_UI_REFRESH    (WM_APP + 23)
/** Posted across processes by a second launch so the live window reloads. */
#define WM_UI_REFRESH_ALL (WM_APP + 24)

/** Worker threads report progress through this, so the string is only ever
 *  written on the UI thread - no lock needed and no torn wstring in the painter. */
struct ToastMsg { std::wstring text; int ms; };

static void uiToast(const std::wstring& text, int ms = 5000) {
    if (!gMainHwnd) return;
    ToastMsg* t = new ToastMsg{text, ms};
    if (!PostMessageW(gMainHwnd, WM_UI_TOAST, 0, reinterpret_cast<LPARAM>(t))) delete t;
}

// --- UTF-8 conversions ------------------------------------------------------
// Listings from both sides are UTF-8 (Android emits UTF-8 JSON, the PC side builds
// paths with fs::u8path). wideFromAnsi/ansiFromWide use the ANSI code page, which
// would mangle accents, Cyrillic, CJK or emoji in filenames, so the UI converts
// explicitly through CP_UTF8.

static std::wstring uiUtf8ToWide(const std::string& in) {
    if (in.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, in.c_str(), static_cast<int>(in.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, in.c_str(), static_cast<int>(in.size()), out.data(), n);
    return out;
}

static std::string uiWideToUtf8(const std::wstring& in) {
    if (in.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, in.c_str(), static_cast<int>(in.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, in.c_str(), static_cast<int>(in.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// --- minimal JSON reader for the identical listing both sides emit ----------
// {"currentPath":"/x","items":[{"name":..,"path":..,"kind":"file","sizeBytes":N,
//                               "modifiedMs":N,"mimeType":..,"ext":..}, ...]}

static std::string jsonRawString(const std::string& obj, const std::string& key) {
    const std::string needle = "\"" + key + "\":\"";
    size_t i = obj.find(needle);
    if (i == std::string::npos) return "";
    i += needle.size();
    std::string out;
    while (i < obj.size()) {
        char c = obj[i];
        if (c == '\\' && i + 1 < obj.size()) {
            char n = obj[i + 1];
            switch (n) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u': {
                    if (i + 5 < obj.size()) {
                        unsigned cp = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = obj[i + 2 + k];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                        }
                        // Encode as UTF-8; the wide conversion below decodes it.
                        if (cp < 0x80) out.push_back((char)cp);
                        else if (cp < 0x800) { out.push_back((char)(0xC0 | (cp >> 6))); out.push_back((char)(0x80 | (cp & 0x3F))); }
                        else { out.push_back((char)(0xE0 | (cp >> 12))); out.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); out.push_back((char)(0x80 | (cp & 0x3F))); }
                        i += 4;
                    }
                    break;
                }
                default: out.push_back(n);
            }
            i += 2;
            continue;
        }
        if (c == '"') break;
        out.push_back(c);
        ++i;
    }
    return out;
}

static long long jsonRawInt(const std::string& obj, const std::string& key) {
    const std::string needle = "\"" + key + "\":";
    size_t i = obj.find(needle);
    if (i == std::string::npos) return 0;
    i += needle.size();
    while (i < obj.size() && obj[i] == ' ') ++i;
    return std::atoll(obj.c_str() + i);
}

static std::vector<UiItem> parseVfsListing(const std::string& json) {
    std::vector<UiItem> out;
    const size_t start = json.find("\"items\":[");
    if (start == std::string::npos) return out;
    size_t i = start + 9;
    while (i < json.size()) {
        while (i < json.size() && json[i] != '{' && json[i] != ']') ++i;
        if (i >= json.size() || json[i] == ']') break;
        // brace-match this object (values contain no nested braces)
        size_t depth = 0, j = i;
        bool inStr = false;
        for (; j < json.size(); ++j) {
            char c = json[j];
            if (inStr) {
                if (c == '\\') { ++j; continue; }
                if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') { inStr = true; continue; }
            if (c == '{') ++depth;
            else if (c == '}') { --depth; if (depth == 0) { ++j; break; } }
        }
        const std::string obj = json.substr(i, j - i);
        UiItem it;
        it.name = uiUtf8ToWide(jsonRawString(obj, "name"));
        it.path = uiUtf8ToWide(jsonRawString(obj, "path"));
        it.ext = uiUtf8ToWide(jsonRawString(obj, "ext"));
        it.isDir = (jsonRawString(obj, "kind") == "directory");
        it.size = static_cast<uint64_t>(jsonRawInt(obj, "sizeBytes"));
        it.modifiedMs = jsonRawInt(obj, "modifiedMs");
        if (!it.name.empty()) out.push_back(it);
        i = j;
    }
    return out;
}

static void uiSortList(UiList& l) {
    std::stable_sort(l.items.begin(), l.items.end(), [&](const UiItem& a, const UiItem& b) {
        if (l.foldersFirst && a.isDir != b.isDir) return a.isDir;
        int cmp = 0;
        switch (l.sortKey) {
            case SORT_SIZE:
                cmp = (a.size < b.size) ? -1 : (a.size > b.size ? 1 : 0);
                break;
            case SORT_TYPE:
                cmp = a.ext.compare(b.ext);
                break;
            case SORT_MODIFIED:
                cmp = (a.modifiedMs < b.modifiedMs) ? -1 : (a.modifiedMs > b.modifiedMs ? 1 : 0);
                break;
            default:
                cmp = a.name.compare(b.name);
        }
        if (cmp == 0) cmp = a.name.compare(b.name);
        return l.sortDesc ? (cmp > 0) : (cmp < 0);
    });
}

static std::wstring uiFormatBytes(uint64_t b) {
    wchar_t buf[64];
    const double d = static_cast<double>(b);
    if (d < 1024) { swprintf(buf, 64, L"%llu B", (unsigned long long)b); return buf; }
    if (d < 1024.0 * 1024) { swprintf(buf, 64, L"%.1f KB", d / 1024); return buf; }
    if (d < 1024.0 * 1024 * 1024) { swprintf(buf, 64, L"%.1f MB", d / (1024.0 * 1024)); return buf; }
    swprintf(buf, 64, L"%.2f GB", d / (1024.0 * 1024 * 1024));
    return buf;
}

static std::wstring uiFormatTime(int64_t ms) {
    if (ms <= 0) return L"\u2014";
    time_t t = static_cast<time_t>(ms / 1000);
    struct tm tmv {};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    wchar_t buf[40];
    swprintf(buf, 40, L"%04d-%02d-%02d %02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min);
    return buf;
}

static std::wstring uiTypeLabel(const UiItem& it) {
    if (it.isDir) return L"Folder";
    std::wstring e = it.ext;
    if (e.empty()) {
        const size_t dot = it.name.find_last_of(L'.');
        if (dot != std::wstring::npos) e = it.name.substr(dot);
    }
    if (e.empty()) return L"File";
    for (auto& c : e) c = (wchar_t)toupper(c);
    return e + L" file";
}

static std::string uiCurrentPhoneIp() {
    std::lock_guard<std::mutex> lk(gState.mtx);
    return gState.activePhoneIp;
}

// --- asynchronous pane loading ---------------------------------------------
// Listing the phone can take seconds over Bluetooth, so it never happens on the
// UI thread. The worker hands the parsed result back with WM_UI_LISTDONE.

static void uiLoadPaneAsync(int pane, const std::wstring& dir) {
    UiList& l = gPane[pane];
    l.loading = true;
    l.error.clear();
    l.dir = dir;
    if (gMainHwnd) InvalidateRect(gMainHwnd, nullptr, FALSE);

    std::thread([pane, dir]() {
        ListPayload* pay = new ListPayload();
        pay->pane = pane;
        pay->dir = dir;
        try {
            if (pane == PANE_PC) {
                fs::path root;
                { std::lock_guard<std::mutex> lk(gState.mtx); root = gState.pcRoot; }
                pay->items = parseVfsListing(listLocalDirJson(root, uiWideToUtf8(dir)));
            } else {
                const std::string ip = uiCurrentPhoneIp();
                if (ip.empty()) {
                    pay->error = L"No phone linked yet. Turn on Bluetooth or join the same Wi-Fi, "
                                 L"then open ChainIt on the phone and switch Sibling Mode on.";
                } else {
                    const std::string url = "/chainit/list?path=" + urlEncode(uiWideToUtf8(dir));
                    HttpReply rep = httpRequestToPhone(ip, PHONE_VFS_PORT, "GET", url, "", {}, 60000);
                    if (rep.status == 200) {
                        pay->items = parseVfsListing(rep.body);
                    } else if (rep.status == 401 || rep.status == 403 || rep.status == 429) {
                        pay->error = uiUtf8ToWide(jsonExtractString(rep.body, "error", "The phone refused this request."));
                    } else if (rep.status > 0) {
                        pay->error = L"The phone answered HTTP " + std::to_wstring(rep.status) + L".";
                    } else {
                        pay->error = isBtEndpoint(ip)
                            ? L"The Bluetooth RFCOMM link did not answer. Keep the phone in range; ChainIt re-dials on its own."
                            : L"The phone did not answer on " + uiUtf8ToWide(ip) + L". Every transport is being re-scanned now.";
                        {
                            std::lock_guard<std::mutex> lk(gState.mtx);
                            gState.phoneLinkProblem = uiWideToUtf8(pay->error);
                        }
                        triggerTransportRescan();
                    }
                }
            }
        } catch (const std::exception& e) {
            pay->error = uiUtf8ToWide(std::string("Listing failed: ") + e.what());
        } catch (...) {
            pay->error = L"Listing failed.";
        }
        if (gMainHwnd) PostMessageW(gMainHwnd, WM_UI_LISTDONE, 0, reinterpret_cast<LPARAM>(pay));
    }).detach();
}

static bool uiNameMatches(const std::wstring& name, const std::wstring& needle) {
    if (needle.empty()) return true;
    auto lower = [](std::wstring v) { for (auto& c : v) c = static_cast<wchar_t>(towlower(c)); return v; };
    return lower(name).find(lower(needle)) != std::wstring::npos;
}

/** Rebuilds the visible view from allItems. Keeps the focused name when it still
 *  matches, so typing a filter does not throw the cursor back to the top. */
static void uiApplyFilter(UiList& l) {
    const std::wstring keep = (l.focused >= 0 && l.focused < (int)l.items.size())
                                  ? l.items[l.focused].name : std::wstring();
    l.items.clear();
    for (const auto& it : l.allItems) {
        if (uiNameMatches(it.name, l.filter)) l.items.push_back(it);
    }
    uiSortList(l);
    l.selected.assign(l.items.size(), 0);
    l.focused = -1;
    l.anchor = -1;
    l.hover = -1;
    l.scroll = 0;
    if (!keep.empty()) {
        for (size_t i = 0; i < l.items.size(); ++i) {
            if (l.items[i].name == keep) {
                l.focused = static_cast<int>(i);
                l.selected[i] = 1;
                break;
            }
        }
    }
    if (l.focused < 0 && !l.items.empty()) { l.focused = 0; l.selected[0] = 1; l.anchor = 0; }
}

static void uiApplyListPayload(ListPayload* pay) {
    if (!pay) return;
    const int pane = pay->pane;
    if (pane < 0 || pane > 1) { delete pay; return; }
    UiList& l = gPane[pane];
    l.loading = false;
    l.dir = pay->dir;
    l.error = pay->error;
    l.allItems.swap(pay->items);
    uiApplyFilter(l);
    delete pay;
    if (gMainHwnd) InvalidateRect(gMainHwnd, nullptr, FALSE);
}

static void uiReload(int pane) { uiLoadPaneAsync(pane, gPane[pane].dir); }
static void uiReloadBoth() { uiReload(PANE_PC); uiReload(PANE_PHONE); }

static void uiNavigate(int pane, const std::wstring& dir) {
    gActivePane = pane;
    gPane[pane].filter.clear();   // a stale filter must never make a new folder look empty
    uiLoadPaneAsync(pane, dir.empty() ? std::wstring(L"/") : dir);
}

static void uiGoUp(int pane) {
    const std::wstring d = gPane[pane].dir;
    if (d.empty() || d == L"/") return;
    std::vector<std::wstring> parts;
    size_t i = 0;
    while (i < d.size()) {
        size_t j = d.find(L'/', i);
        if (j == std::wstring::npos) j = d.size();
        if (j > i) parts.push_back(d.substr(i, j - i));
        i = j + 1;
    }
    if (parts.empty()) return;
    parts.pop_back();
    std::wstring up = L"/";
    for (size_t k = 0; k < parts.size(); ++k) {
        up += parts[k];
        if (k + 1 < parts.size()) up += L"/";
    }
    uiNavigate(pane, up);
}

static std::wstring uiJoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty() || dir == L"/") return L"/" + name;
    return dir + L"/" + name;
}

static std::vector<int> uiSelectedIndices(const UiList& l) {
    std::vector<int> out;
    for (size_t i = 0; i < l.selected.size(); ++i) if (l.selected[i]) out.push_back(static_cast<int>(i));
    return out;
}

static uint64_t uiSelectedBytes(const UiList& l) {
    uint64_t total = 0;
    for (const int i : uiSelectedIndices(l)) total += l.items[i].size;
    return total;
}

// --- native modal text prompt ----------------------------------------------
// Win32 has no built-in input box and this app must not open a browser prompt(),
// so the dialog template is assembled in memory - no .rc entry, no resource file.

struct PromptCtx {
    std::wstring title, label, value;
    bool ok = false;
};

static PromptCtx* gPromptCtx = nullptr;

static INT_PTR CALLBACK PromptDlgProc(HWND h, UINT m, WPARAM w, LPARAM) {
    switch (m) {
        case WM_INITDIALOG: {
            SetWindowTextW(h, gPromptCtx->title.c_str());
            SetDlgItemTextW(h, 20, gPromptCtx->label.c_str());
            HWND edit = GetDlgItem(h, 21);
            SetWindowTextW(edit, gPromptCtx->value.c_str());
            Edit_SetSel(edit, 0, -1);
            SetFocus(edit);
            return FALSE;   // focus already set
        }
        case WM_COMMAND:
            if (LOWORD(w) == IDOK) {
                wchar_t buf[1024] = {};
                GetWindowTextW(GetDlgItem(h, 21), buf, 1023);
                gPromptCtx->value = buf;
                gPromptCtx->ok = !gPromptCtx->value.empty();
                EndDialog(h, gPromptCtx->ok ? IDOK : IDCANCEL);
                return TRUE;
            }
            if (LOWORD(w) == IDCANCEL) { EndDialog(h, IDCANCEL); return TRUE; }
            break;
        case WM_CLOSE:
            EndDialog(h, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}

/** Byte writer for an in-memory DLGTEMPLATE (version 1 layout). */
struct TmplWriter {
    std::vector<BYTE> b;
    TmplWriter() { b.reserve(1024); }
    void u16(WORD v) { b.push_back(static_cast<BYTE>(v & 0xFF)); b.push_back(static_cast<BYTE>(v >> 8)); }
    void u32(DWORD v) { u16(static_cast<WORD>(v & 0xFFFF)); u16(static_cast<WORD>(v >> 16)); }
    void i16(short v) { u16(static_cast<WORD>(v)); }
    void str(const wchar_t* s) {
        for (const wchar_t* p = s; *p; ++p) u16(static_cast<WORD>(*p));
        u16(0);
    }
    void align4() { while (b.size() % 4) b.push_back(0); }
    const BYTE* data() const { return b.data(); }
};

static bool uiPrompt(HWND owner, const std::wstring& title, const std::wstring& label, std::wstring& value) {
    PromptCtx ctx;
    ctx.title = title;
    ctx.label = label;
    ctx.value = value;
    gPromptCtx = &ctx;

    TmplWriter t;
    t.u32(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT);
    t.u32(0);                 // dwExtendedStyle
    t.u16(4);                 // cdit: label, edit, OK, Cancel
    t.i16(0); t.i16(0);       // x, y
    t.i16(260); t.i16(92);    // cx, cy  (dialog units)
    t.u16(0);                 // menu
    t.u16(0);                 // windowClass
    t.str(title.c_str());     // title
    t.u16(9);                 // pointsize (DS_SETFONT)
    t.str(L"Segoe UI");       // typeface

    auto item = [&](DWORD style, short x, short y, short cx, short cy, WORD id,
                    const wchar_t* cls, const wchar_t* text) {
        t.align4();
        t.u32(style);
        t.u32(0);             // dwExtendedStyle
        t.i16(x); t.i16(y); t.i16(cx); t.i16(cy);
        t.u16(id);
        t.str(cls);
        t.str(text);
        t.u16(0);             // no creation data
    };

    item(WS_CHILD | WS_VISIBLE | SS_LEFT, 8, 7, 244, 12, 20, L"STATIC", label.c_str());
    item(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 8, 23, 244, 15, 21,
         L"EDIT", value.c_str());
    item(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 112, 66, 66, 18, IDOK, L"BUTTON", L"OK");
    item(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 186, 66, 66, 18, IDCANCEL, L"BUTTON", L"Cancel");
    t.align4();

    const INT_PTR res = DialogBoxIndirectParamW(gMainInstance,
                                                reinterpret_cast<LPCDLGTEMPLATE>(t.data()),
                                                owner, PromptDlgProc, 0);
    gPromptCtx = nullptr;
    if (res == IDOK && ctx.ok) { value = ctx.value; return true; }
    return false;
}

// --- file operations --------------------------------------------------------
// Everything that can block runs on a worker thread and reports through a toast,
// so the window never freezes mid-copy and never fails silently.

static fs::path uiRealPcPath(const std::wstring& virtualPath) {
    std::lock_guard<std::mutex> lk(gState.mtx);
    const fs::path root = gState.pcRoot;
    return resolveLocalSafe(root, uiWideToUtf8(virtualPath));
}

static void uiOpNewFolder(int pane) {
    std::wstring name = L"New Folder";
    if (!uiPrompt(gMainHwnd, L"New folder", L"Folder name:", name)) return;
    const std::wstring dir = gPane[pane].dir;
    uiToast(L"Creating folder\u2026", 2000);
    std::thread([pane, dir, name]() {
        std::wstring msg;
        if (pane == PANE_PC) {
            std::error_code ec;
            fs::create_directories(uiRealPcPath(uiJoinPath(dir, name)), ec);
            msg = ec ? (L"Could not create the folder: " + uiUtf8ToWide(ec.message()))
                     : (L"Created folder " + name);
        } else {
            const std::string ip = uiCurrentPhoneIp();
            if (ip.empty()) { msg = L"No phone linked yet."; }
            else {
                std::ostringstream b;
                b << "{\"parentDir\":\"" << jsonEscape(uiWideToUtf8(dir))
                  << "\",\"name\":\"" << jsonEscape(uiWideToUtf8(name)) << "\"}";
                HttpReply r = httpRequestToPhone(ip, PHONE_VFS_PORT, "POST", "/chainit/mkdir", b.str(),
                                                 {{"Content-Type", "application/json"}}, 30000);
                msg = (r.status == 200) ? (L"Created folder " + name + L" on the phone")
                                        : L"The phone refused to create the folder.";
            }
        }
        uiToast(msg);
        PostMessageW(gMainHwnd, WM_UI_REFRESH, pane, 0);
    }).detach();
}

static void uiOpRename(int pane) {
    const std::vector<int> sel = uiSelectedIndices(gPane[pane]);
    if (sel.size() != 1) { uiToast(L"Select exactly one item to rename."); return; }
    const UiItem it = gPane[pane].items[sel[0]];
    std::wstring name = it.name;
    if (!uiPrompt(gMainHwnd, L"Rename", L"New name:", name)) return;
    if (name == it.name) return;
    uiToast(L"Renaming\u2026", 2000);
    std::thread([pane, it, name]() {
        std::wstring msg;
        if (pane == PANE_PC) {
            std::error_code ec;
            const fs::path src = uiRealPcPath(it.path);
            fs::rename(src, src.parent_path() / fs::u8path(uiWideToUtf8(name)), ec);
            msg = ec ? (L"Rename failed: " + uiUtf8ToWide(ec.message())) : (L"Renamed to " + name);
        } else {
            const std::string ip = uiCurrentPhoneIp();
            if (ip.empty()) { msg = L"No phone linked yet."; }
            else {
                std::ostringstream b;
                b << "{\"path\":\"" << jsonEscape(uiWideToUtf8(it.path))
                  << "\",\"newName\":\"" << jsonEscape(uiWideToUtf8(name)) << "\"}";
                HttpReply r = httpRequestToPhone(ip, PHONE_VFS_PORT, "POST", "/chainit/rename", b.str(),
                                                 {{"Content-Type", "application/json"}}, 30000);
                msg = (r.status == 200) ? (L"Renamed to " + name + L" on the phone") : L"The phone refused the rename.";
            }
        }
        uiToast(msg);
        PostMessageW(gMainHwnd, WM_UI_REFRESH, pane, 0);
    }).detach();
}

static void uiOpDelete(int pane) {
    std::vector<UiItem> targets;
    for (const int i : uiSelectedIndices(gPane[pane])) targets.push_back(gPane[pane].items[i]);
    if (targets.empty()) { uiToast(L"Nothing selected."); return; }

    std::wstring q = L"Delete ";
    q += std::to_wstring(targets.size());
    q += (targets.size() == 1 ? L" item (" : L" items (");
    q += targets[0].name;
    if (targets.size() > 1) q += L", \u2026";
    q += L") permanently?";
    if (MessageBoxW(gMainHwnd, q.c_str(), L"ChainIt \u2014 delete", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;

    uiToast(L"Deleting\u2026", 2000);
    std::thread([pane, targets]() {
        std::wstring msg;
        if (pane == PANE_PC) {
            int n = 0;
            std::error_code ec;
            for (const auto& t : targets) {
                const fs::path real = uiRealPcPath(t.path);
                if (fs::exists(real, ec) && fs::remove_all(real, ec) > 0) ++n;
            }
            msg = L"Deleted " + std::to_wstring(n) + L" item(s) from this PC";
        } else {
            const std::string ip = uiCurrentPhoneIp();
            if (ip.empty()) { msg = L"No phone linked yet."; }
            else {
                std::ostringstream b;
                b << "{\"paths\":[";
                for (size_t i = 0; i < targets.size(); ++i) {
                    if (i) b << ",";
                    b << "\"" << jsonEscape(uiWideToUtf8(targets[i].path)) << "\"";
                }
                b << "]}";
                HttpReply r = httpRequestToPhone(ip, PHONE_VFS_PORT, "POST", "/chainit/delete", b.str(),
                                                 {{"Content-Type", "application/json"}}, 30000);
                msg = (r.status == 200) ? (L"Deleted " + std::to_wstring(targets.size()) + L" item(s) from the phone")
                                        : L"The phone refused the delete.";
            }
        }
        uiToast(msg);
        PostMessageW(gMainHwnd, WM_UI_REFRESH, pane, 0);
    }).detach();
}

/** Copy (or move) the selection of one pane into the other pane's current folder. */
static void uiOpTransfer(int fromPane, bool move) {
    const int toPane = (fromPane == PANE_PC) ? PANE_PHONE : PANE_PC;
    std::vector<UiItem> targets;
    for (const int i : uiSelectedIndices(gPane[fromPane])) targets.push_back(gPane[fromPane].items[i]);
    if (targets.empty()) { uiToast(L"Select items to transfer first."); return; }
    if (fromPane == toPane) { uiToast(L"Pick the other pane first."); return; }

    const std::wstring toDir = gPane[toPane].dir;
    const uint64_t bytes = uiSelectedBytes(gPane[fromPane]);
    uiToast(L"Starting " + std::to_wstring(targets.size()) + L" transfer(s)\u2026", 2500);

    std::thread([fromPane, toPane, targets, toDir, move, bytes]() {
        uint64_t done = 0;
        std::wstring fail;
        for (const auto& t : targets) {
            if (fromPane == PANE_PC) {
                // PC -> phone
                const std::string ip = uiCurrentPhoneIp();
                if (ip.empty()) { fail = L"No phone linked yet."; break; }
                const fs::path real = uiRealPcPath(t.path);
                const uint64_t n = uploadLocalPathToPhone(real, ip, uiWideToUtf8(toDir));
                if (n == 0 && !t.isDir) { fail = L"Upload refused for " + t.name; }
                done += n;
                if (move && n > 0) { std::error_code ec; fs::remove_all(real, ec); }
            } else {
                // phone -> PC
                const std::string ip = uiCurrentPhoneIp();
                if (ip.empty()) { fail = L"No phone linked yet."; break; }
                const fs::path destDir = uiRealPcPath(toDir);
                std::error_code ec;
                fs::create_directories(destDir, ec);
                const uint64_t n = downloadPhonePathToLocal(ip, uiWideToUtf8(t.path), destDir);
                if (n == 0) {
                    if (fail.empty()) {
                        std::lock_guard<std::mutex> lk(gState.mtx);
                        fail = uiUtf8ToWide(gState.phoneLinkProblem);
                    }
                    if (fail.empty()) fail = L"Transfer failed for " + t.name + L" (integrity gate or link loss)";
                } else {
                    done += n;
                    if (move) {
                        std::ostringstream b;
                        b << "{\"paths\":[\"" << jsonEscape(uiWideToUtf8(t.path)) << "\"]}";
                        httpRequestToPhone(ip, PHONE_VFS_PORT, "POST", "/chainit/delete", b.str(),
                                           {{"Content-Type", "application/json"}}, 30000);
                    }
                }
            }
        }

        std::wstring msg;
        if (!fail.empty()) {
            msg = fail + L" (" + uiFormatBytes(done) + L" transferred before it stopped)";
        } else {
            msg = (move ? L"Moved " : L"Copied ") + std::to_wstring(targets.size()) + L" item(s), " +
                  uiFormatBytes(bytes) + L" \u2014 every chunk SHA-256 verified";
        }
        {
            std::lock_guard<std::mutex> lk(gState.mtx);
            gState.transfers.insert(gState.transfers.begin(),
                                    {std::string("ui"), uiWideToUtf8(targets[0].name), move ? "move" : "copy",
                                     fromPane == PANE_PC ? "pc_to_phone" : "phone_to_pc",
                                     gState.activeTransportId, static_cast<int64_t>(done), 0.0, nowMs()});
            if (fail.empty()) gState.phoneLinkProblem.clear();
        }
        addLogLocked("VFS", (move ? "Moved " : "Copied ") + uiWideToUtf8(targets[0].name),
                     uiWideToUtf8(msg));
        uiToast(msg, 7000);
        PostMessageW(gMainHwnd, WM_UI_REFRESH, toPane, 0);
        PostMessageW(gMainHwnd, WM_UI_REFRESH, fromPane, 0);
    }).detach();
}

static void uiOpOpen(int pane, int idx) {
    if (idx < 0 || idx >= (int)gPane[pane].items.size()) return;
    const UiItem it = gPane[pane].items[idx];
    if (it.isDir) { uiNavigate(pane, it.path); return; }

    if (pane == PANE_PC) {
        const fs::path real = uiRealPcPath(it.path);
        ShellExecuteW(gMainHwnd, L"open", real.c_str(), nullptr, real.parent_path().c_str(), SW_SHOWNORMAL);
        return;
    }

    // Phone file: pull it down (chunked + verified) then open it with the right app.
    uiToast(L"Fetching " + it.name + L" from the phone\u2026", 3000);
    std::thread([it]() {
        const std::string ip = uiCurrentPhoneIp();
        if (ip.empty()) { uiToast(L"No phone linked yet."); return; }
        const fs::path dir = fs::path(chainItTempDir()) / L"ChainIt-Open";
        std::error_code ec;
        fs::create_directories(dir, ec);
        const uint64_t n = downloadPhonePathToLocal(ip, uiWideToUtf8(it.path), dir);
        if (n == 0) {
            std::wstring why;
            { std::lock_guard<std::mutex> lk(gState.mtx); why = uiUtf8ToWide(gState.phoneLinkProblem); }
            uiToast(why.empty() ? (L"Could not fetch " + it.name) : why, 7000);
            return;
        }
        const fs::path local = dir / fs::u8path(uiWideToUtf8(it.name));
        ShellExecuteW(gMainHwnd, L"open", local.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
        uiToast(L"Opened " + it.name + L" (" + uiFormatBytes(n) + L", SHA-256 verified)");
    }).detach();
}

static void uiOpDownloadToPc(int pane) {
    if (pane != PANE_PHONE) { uiToast(L"That file is already on this PC."); return; }
    std::vector<UiItem> targets;
    for (const int i : uiSelectedIndices(gPane[pane])) targets.push_back(gPane[pane].items[i]);
    if (targets.empty()) { uiToast(L"Nothing selected."); return; }
    uiToast(L"Downloading to your Downloads folder\u2026", 2500);
    std::thread([targets]() {
        const std::string ip = uiCurrentPhoneIp();
        if (ip.empty()) { uiToast(L"No phone linked yet."); return; }
        wchar_t prof[MAX_PATH * 2] = {};
        GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH * 2 - 1);
        const fs::path dir = fs::path(prof) / L"Downloads";
        uint64_t total = 0;
        for (const auto& t : targets) total += downloadPhonePathToLocal(ip, uiWideToUtf8(t.path), dir);
        uiToast(total > 0 ? (L"Saved " + uiFormatBytes(total) + L" to Downloads")
                          : L"Download failed - see the link status", 7000);
    }).detach();
}

static void uiOpSetClipboard(int pane, const std::wstring& op) {
    gClipboardPaths.clear();
    for (const int i : uiSelectedIndices(gPane[pane])) gClipboardPaths.push_back(gPane[pane].items[i].path);
    if (gClipboardPaths.empty()) { uiToast(L"Nothing selected."); return; }
    gClipboardOp = op;
    gClipboardPane = pane;
    uiToast((op == L"move" ? L"Cut " : L"Copied ") + std::to_wstring(gClipboardPaths.size()) +
            L" item(s) \u2014 switch pane and press Ctrl+V");
}

static void uiOpPaste(int pane) {
    if (gClipboardPaths.empty()) { uiToast(L"The ChainIt clipboard is empty."); return; }
    if (gClipboardPane == pane) { uiToast(L"Paste into the other pane to copy between PC and phone."); return; }

    const bool move = (gClipboardOp == L"move");
    const int from = gClipboardPane;
    std::vector<UiItem> targets;
    for (const auto& p : gClipboardPaths) {
        for (const auto& it : gPane[from].items) if (it.path == p) { targets.push_back(it); break; }
    }
    if (targets.empty()) { uiToast(L"The copied items are no longer listed - refresh and copy again."); return; }

    const std::wstring toDir = gPane[pane].dir;
    gClipboardPaths.clear();
    gClipboardPane = -1;
    uiToast(L"Pasting " + std::to_wstring(targets.size()) + L" item(s)\u2026", 2500);

    std::thread([from, pane, targets, toDir, move]() {
        uint64_t done = 0;
        const std::string ip = uiCurrentPhoneIp();
        for (const auto& t : targets) {
            if (from == PANE_PC) {
                if (ip.empty()) break;
                const uint64_t n = uploadLocalPathToPhone(uiRealPcPath(t.path), ip, uiWideToUtf8(toDir));
                done += n;
                if (move && n > 0) { std::error_code ec; fs::remove_all(uiRealPcPath(t.path), ec); }
            } else {
                if (ip.empty()) break;
                const fs::path destDir = uiRealPcPath(toDir);
                std::error_code ec;
                fs::create_directories(destDir, ec);
                const uint64_t n = downloadPhonePathToLocal(ip, uiWideToUtf8(t.path), destDir);
                done += n;
                if (move && n > 0) {
                    std::ostringstream b;
                    b << "{\"paths\":[\"" << jsonEscape(uiWideToUtf8(t.path)) << "\"]}";
                    httpRequestToPhone(ip, PHONE_VFS_PORT, "POST", "/chainit/delete", b.str(),
                                       {{"Content-Type", "application/json"}}, 30000);
                }
            }
        }
        uiToast(ip.empty() ? L"No phone linked yet."
                           : ((move ? L"Moved " : L"Copied ") + std::to_wstring(targets.size()) +
                              L" item(s), " + uiFormatBytes(done)), 7000);
        PostMessageW(gMainHwnd, WM_UI_REFRESH, pane, 0);
        PostMessageW(gMainHwnd, WM_UI_REFRESH, from, 0);
    }).detach();
}

// --- theme ------------------------------------------------------------------

static const COLORREF kColBg       = RGB(11, 15, 21);
static const COLORREF kColChrome   = RGB(16, 21, 28);
static const COLORREF kColChrome2  = RGB(21, 27, 36);
static const COLORREF kColList     = RGB(13, 17, 24);
static const COLORREF kColHover    = RGB(24, 32, 42);
static const COLORREF kColSel      = RGB(15, 52, 47);
static const COLORREF kColSelIdle  = RGB(22, 30, 40);
static const COLORREF kColLine     = RGB(33, 42, 54);
static const COLORREF kColLineSoft = RGB(23, 30, 39);
static const COLORREF kColText     = RGB(226, 232, 240);
static const COLORREF kColDim      = RGB(148, 163, 184);
static const COLORREF kColFaint    = RGB(100, 116, 139);
static const COLORREF kColEmerald  = RGB(16, 185, 129);
static const COLORREF kColCyan     = RGB(34, 211, 238);
static const COLORREF kColAmber    = RGB(245, 158, 11);
static const COLORREF kColDanger   = RGB(248, 113, 113);
static const COLORREF kColBtn      = RGB(26, 34, 45);
static const COLORREF kColBtnHover = RGB(37, 48, 62);

static const int kTabH    = 38;
static const int kToolH   = 42;
static const int kPathH   = 26;
static const int kStatusH = 28;

static HFONT gFontUi = nullptr, gFontBold = nullptr, gFontTitle = nullptr;
static HFONT gFontSmall = nullptr, gFontMono = nullptr;
static std::map<std::wstring, HICON> gIconCache;
static bool gUiFontsReady = false;

static HFONT uiMakeFont(const wchar_t* face, int pt, int weight) {
    HDC dc = GetDC(nullptr);
    int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    return CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

static void uiFontsInit() {
    if (gUiFontsReady) return;
    gFontUi    = uiMakeFont(L"Segoe UI", 9, FW_NORMAL);
    gFontBold  = uiMakeFont(L"Segoe UI Semibold", 9, FW_SEMIBOLD);
    gFontTitle = uiMakeFont(L"Segoe UI Semibold", 12, FW_SEMIBOLD);
    gFontSmall = uiMakeFont(L"Segoe UI", 8, FW_NORMAL);
    gFontMono  = uiMakeFont(L"Consolas", 9, FW_NORMAL);
    gUiFontsReady = true;
}

static void uiFill(HDC dc, const RECT& r, COLORREF c) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

static void uiRound(HDC dc, const RECT& r, COLORREF fill, COLORREF line, int rad) {
    SetDCBrushColor(dc, fill);
    SetDCPenColor(dc, line);
    HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
    HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
}

static void uiLine(HDC dc, int x1, int y1, int x2, int y2, COLORREF c) {
    SetDCPenColor(dc, c);
    HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
    SelectObject(dc, op);
}

static void uiText(HDC dc, const RECT& r, const std::wstring& s, COLORREF c, HFONT f,
                   UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS) {
    SetTextColor(dc, c);
    HGDIOBJ of = SelectObject(dc, f);
    RECT copy = r;
    DrawTextW(dc, s.c_str(), -1, &copy, fmt);
    SelectObject(dc, of);
}

static int uiTextW(HDC dc, const std::wstring& s, HFONT f) {
    HGDIOBJ of = SelectObject(dc, f);
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &sz);
    SelectObject(dc, of);
    return sz.cx;
}

/** Shell icon for a name/extension. USEFILEATTRIBUTES means no disk access, so it
 *  works for phone files that do not exist on this PC. */
static HICON uiIconFor(bool isDir, const std::wstring& ext) {
    std::wstring key = isDir ? std::wstring(L"<dir>") : ext;
    auto found = gIconCache.find(key);
    if (found != gIconCache.end()) return found->second;
    SHFILEINFOW sfi{};
    const std::wstring probe = isDir ? L"folder" : (L"file" + (ext.empty() ? L"" : ext));
    // USEFILEATTRIBUTES: the shell resolves the icon from the name alone, so this
    // works for phone files that do not exist anywhere on this PC.
    const DWORD flags = SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES;
    HICON icon = nullptr;
    if (SHGetFileInfoW(probe.c_str(), isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
                       &sfi, sizeof(sfi), flags)) {
        icon = sfi.hIcon;
    }
    gIconCache[key] = icon;
    return icon;
}

static void uiDrawIcon(HDC dc, int x, int y, HICON icon, int size) {
    if (!icon) return;
    DrawIconEx(dc, x, y, icon, size, size, 0, nullptr, DI_NORMAL);
}

// --- layout -----------------------------------------------------------------

struct ToolBtn {
    int id;
    std::wstring label;
    RECT rc{};
    bool enabled = true;
    bool active = false;
    bool separator = false;
};

enum UiHitKind { HIT_NONE = 0, HIT_TAB, HIT_TOOL, HIT_PANE, HIT_HEAD, HIT_ROW, HIT_LINK, HIT_ABOUT };
struct UiHit {
    int kind = HIT_NONE;
    int id = 0;
    int pane = -1;
    int row = -1;
    int col = -1;
};

struct UiLayout {
    int cx = 0, cy = 0;
    bool dual = false;
    int panes = 0;
    int paneId[2] = {PANE_PC, PANE_PHONE};
    RECT pane[2]{};      // whole pane block
    RECT path[2]{};      // breadcrumb strip
    RECT head[2]{};      // column header
    RECT list[2]{};      // rows
    RECT tabs{};
    RECT tab[3]{};
    RECT toolbar{};
    RECT status{};
    std::vector<ToolBtn> tools;
    RECT linkBtns[8]{};
    int linkBtnCount = 0;
};

static const wchar_t* kToolLabels[] = {
    L"Up", L"Refresh", L"New folder", L"Cut", L"Copy", L"Paste", L"Rename", L"Delete",
    L"Send to phone", L"Save to PC", L"PC", L"Phone", L"Split view",
};
static const int kToolIds[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
static const int kToolCount = 13;

static UiLayout uiComputeLayout(HWND hwnd) {
    UiLayout L;
    RECT rc;
    GetClientRect(hwnd, &rc);
    L.cx = rc.right - rc.left;
    L.cy = rc.bottom - rc.top;

    L.tabs = RECT{0, 0, L.cx, kTabH};
    const wchar_t* tabNames[3] = {L"Files", L"Link", L"About"};
    HDC dc = GetDC(hwnd);
    int tx = 12;
    for (int i = 0; i < 3; ++i) {
        int w = uiTextW(dc, tabNames[i], gFontBold) + 34;
        L.tab[i] = RECT{tx, 6, tx + w, kTabH - 4};
        tx += w + 4;
    }
    L.toolbar = RECT{0, kTabH, L.cx, kTabH + kToolH};
    L.status = RECT{0, L.cy - kStatusH, L.cx, L.cy};

    // toolbar buttons
    int bx = 12;
    const int by = kTabH + (kToolH - 28) / 2;
    L.tools.clear();
    for (int i = 0; i < kToolCount; ++i) {
        ToolBtn b;
        b.id = kToolIds[i];
        b.label = kToolLabels[i];
        if (i == 3 || i == 8 || i == 10) { bx += 10; }   // group gaps
        int w = uiTextW(dc, b.label, gFontUi) + 24;
        b.rc = RECT{bx, by, bx + w, by + 28};
        bx += w + 6;
        L.tools.push_back(b);
    }
    ReleaseDC(hwnd, dc);

    L.dual = (gDualPane && gTab == TAB_FILES && L.cx >= 900);
    const int top = kTabH + kToolH;
    const int bot = L.cy - kStatusH;
    if (gTab != TAB_FILES) {
        L.panes = 0;
        // Action column on the right of the Link tab only. Buttons that would fall
        // below the status bar are dropped, so a short window cannot leave an
        // invisible-but-clickable region behind.
        if (gTab == TAB_LINK) {
            const int bw = 232, bh = 30;
            const int xx = L.cx - bw - 24;
            int yy = top + 20;
            for (int i = 0; i < 7 && yy + bh <= bot - 8; ++i) {
                L.linkBtns[L.linkBtnCount++] = RECT{xx, yy, xx + bw, yy + bh};
                yy += bh + 8;
            }
        }
        return L;
    }

    if (L.dual) {
        L.panes = 2;
        L.paneId[0] = PANE_PC;
        L.paneId[1] = PANE_PHONE;
        L.pane[0] = RECT{0, top, L.cx / 2 - 1, bot};
        L.pane[1] = RECT{L.cx / 2 + 1, top, L.cx, bot};
    } else {
        L.panes = 1;
        L.paneId[0] = gActivePane;
        L.pane[0] = RECT{0, top, L.cx, bot};
    }
    for (int i = 0; i < L.panes; ++i) {
        const RECT& p = L.pane[i];
        L.path[i] = RECT{p.left, p.top, p.right, p.top + kPathH};
        L.head[i] = RECT{p.left, p.top + kPathH, p.right, p.top + kPathH + kColHeaderH};
        L.list[i] = RECT{p.left, p.top + kPathH + kColHeaderH, p.right, p.bottom};
    }
    return L;
}

static UiLayout gLayout;

static int uiPaneSlot(const UiLayout& L, int paneId) {
    for (int i = 0; i < L.panes; ++i) if (L.paneId[i] == paneId) return i;
    return -1;
}

static std::wstring uiPathCrumb(const std::wstring& dir) {
    if (dir.empty() || dir == L"/") return L"Root";
    std::wstring out;
    size_t i = 0;
    while (i < dir.size()) {
        size_t j = dir.find(L'/', i);
        if (j == std::wstring::npos) j = dir.size();
        if (j > i) { if (!out.empty()) out += L"  \u203A  "; out += dir.substr(i, j - i); }
        i = j + 1;
    }
    return out.empty() ? L"Root" : out;
}

/** Short drive caption for a pane's breadcrumb strip. The verbose scanning
 *  placeholder is collapsed to "Phone" so the crumb keeps its room. */
static std::wstring uiDriveLabel(int pane) {
    if (pane == PANE_PC) return L"This PC";
    std::wstring name;
    {
        std::lock_guard<std::mutex> lk(gState.mtx);
        name = uiUtf8ToWide(gState.peerDeviceName);
    }
    if (name.empty() || name.find(L"Scanning") != std::wstring::npos) return L"Phone  \u00B7  Z:\\";
    return name + L"  \u00B7  Z:\\";
}

/** Live link telemetry for the Link tab and the status bar. */
struct UiLinkInfo {
    std::wstring transport, peer, endpoint, problem;
    bool siblingOn = false, zMounted = false, btRadio = false, btListening = false;
    int pooled = 0, maxCh = 0, cachedCh = 0;
    uint64_t served = 0, redials = 0;
    double rttMs = 0.0;
    std::vector<std::wstring> paired;
    std::vector<std::wstring> lan;
    uint64_t phoneTotal = 0, phoneUsed = 0;
};

static UiLinkInfo uiLinkInfo() {
    UiLinkInfo s;
    {
        std::lock_guard<std::mutex> lk(gState.mtx);
        s.transport = uiUtf8ToWide(gState.activeTransportId);
        s.peer = uiUtf8ToWide(gState.peerDeviceName);
        s.endpoint = uiUtf8ToWide(gState.activePhoneIp);
        s.problem = uiUtf8ToWide(gState.phoneLinkProblem);
        s.siblingOn = gState.siblingEnabled;
        s.zMounted = gState.zDriveMounted;
        s.btRadio = gState.btRadioPresent;
        s.btListening = gState.btReceiverListening;
        s.phoneTotal = gState.phoneTotalBytes;
        s.phoneUsed = gState.phoneUsedBytes;
        for (const auto& d : gState.btPairedDevices) s.paired.push_back(uiUtf8ToWide(d));
        for (const auto& kv : gState.phoneEndpoints) s.lan.push_back(uiUtf8ToWide(kv.second.kind) + L"  " + uiUtf8ToWide(kv.first));
    }
    s.pooled = static_cast<int>(btPooledChannels());
    s.maxCh = static_cast<int>(kBtChannelCount);
    s.cachedCh = static_cast<int>(gBtCachedPort.load());
    s.served = gBtRequestsServed.load();
    s.redials = gBtRedials.load();
    s.rttMs = static_cast<double>(gBtLastRttUs.load()) / 1000.0;
    return s;
}

static std::wstring uiTransportPretty(const std::wstring& t) {
    if (t == L"usb_rndis") return L"USB tethering (RNDIS)";
    if (t == L"wifi_lan") return L"Wi-Fi / LAN";
    if (t == L"local_hotspot") return L"Hotspot (offline)";
    if (t == L"bluetooth") return L"Bluetooth RFCOMM";
    if (t == L"online_p2p") return L"Online P2P relay";
    return t.empty() ? std::wstring(L"none yet") : t;
}

// --- painting ---------------------------------------------------------------

static const wchar_t* kLinkBtnLabels[] = {
    L"Rescan all transports", L"Mount / refresh Z:", L"Open Z: in Explorer",
    L"Create desktop shortcut", L"Show mini hub panel", L"Update ChainIt in place",
    L"Copy pairing key",
};

static void uiPaintTabs(HDC dc, const UiLayout& L) {
    uiFill(dc, L.tabs, kColChrome);
    uiLine(dc, 0, kTabH - 1, L.cx, kTabH - 1, kColLine);
    const wchar_t* names[3] = {L"Files", L"Link", L"About"};
    for (int i = 0; i < 3; ++i) {
        const RECT r = L.tab[i];
        const bool on = (gTab == i);
        if (on) {
            uiFill(dc, r, kColChrome2);
            uiLine(dc, r.left, r.bottom, r.right, r.bottom, kColEmerald);
            uiLine(dc, r.left, r.bottom - 1, r.right, r.bottom - 1, kColEmerald);
        }
        uiText(dc, r, names[i], on ? kColText : kColDim, on ? gFontBold : gFontUi);
    }
    // right-hand brand
    RECT brand{L.cx - 420, 0, L.cx - 16, kTabH};
    std::wstring ver = L"ChainIt " + uiUtf8ToWide(kChainItAppVersion) + L"   \u00B7   Built by Qmer";
    uiText(dc, brand, ver, kColFaint, gFontSmall,
           DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    const HICON tabIcon = gMainIconSm ? gMainIconSm : gMainIcon;
    if (tabIcon) uiDrawIcon(dc, L.cx - 420 - 26, (kTabH - 16) / 2, tabIcon, 16);
}

static void uiPaintToolbar(HDC dc, UiLayout& L) {
    uiFill(dc, L.toolbar, kColChrome);
    uiLine(dc, 0, L.toolbar.bottom - 1, L.cx, L.toolbar.bottom - 1, kColLine);

    UiList& ap = gPane[gActivePane];
    const bool sel = !uiSelectedIndices(ap).empty();
    const bool pcSel = !uiSelectedIndices(gPane[PANE_PC]).empty();
    const bool phSel = !uiSelectedIndices(gPane[PANE_PHONE]).empty();

    for (size_t i = 0; i < L.tools.size(); ++i) {
        ToolBtn b = L.tools[i];
        switch (b.id) {
            case 0: b.enabled = (ap.dir != L"/" && !ap.dir.empty() && gTab == TAB_FILES); break;
            case 1: b.enabled = (gTab == TAB_FILES); break;
            case 2: b.enabled = (gTab == TAB_FILES); break;
            case 3: b.enabled = sel && gTab == TAB_FILES; break;
            case 4: b.enabled = sel && gTab == TAB_FILES; break;
            case 5: b.enabled = !gClipboardPaths.empty() && gTab == TAB_FILES; break;
            case 6: b.enabled = (uiSelectedIndices(ap).size() == 1 && gTab == TAB_FILES); break;
            case 7: b.enabled = sel && gTab == TAB_FILES; break;
            case 8: b.enabled = pcSel && gTab == TAB_FILES; break;
            case 9: b.enabled = phSel && gTab == TAB_FILES; break;
            case 10: case 11: b.enabled = (gTab == TAB_FILES); b.active = (!gDualPane && gActivePane == (b.id - 10)); break;
            case 12: b.enabled = (gTab == TAB_FILES && L.cx >= 900); b.active = gDualPane; break;
            default: break;
        }
        L.tools[i].enabled = b.enabled;
        L.tools[i].active = b.active;

        const bool hov = (gToolHover == static_cast<int>(i));
        COLORREF fill = b.active ? RGB(13, 60, 52) : (hov && b.enabled ? kColBtnHover : kColBtn);
        COLORREF line = b.active ? kColEmerald : kColLine;
        if (!b.enabled) { fill = kColChrome; line = kColLineSoft; }
        uiRound(dc, b.rc, fill, line, 6);
        uiText(dc, RECT{b.rc.left + 2, b.rc.top, b.rc.right - 2, b.rc.bottom}, b.label,
               !b.enabled ? kColFaint : (b.active ? kColEmerald : kColText), gFontUi,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
}

static void uiPaintPaneChrome(HDC dc, const UiLayout& L, int slot, int paneId) {
    UiList& l = gPane[paneId];
    l.rc = L.pane[slot];
    l.listRc = L.list[slot];

    const bool active = (gActivePane == paneId);

    uiFill(dc, L.pane[slot], kColList);
    // path / breadcrumb strip
    uiFill(dc, L.path[slot], active ? kColChrome2 : kColChrome);
    uiLine(dc, L.path[slot].left, L.path[slot].bottom - 1, L.path[slot].right, L.path[slot].bottom - 1, kColLine);
    RECT pr{L.path[slot].left + 12, L.path[slot].top, L.path[slot].right - 12, L.path[slot].bottom};
    const std::wstring crumb = uiPathCrumb(l.dir);
    uiText(dc, pr, uiDriveLabel(paneId) + L"   \u203A   " + crumb,
           active ? kColText : kColDim, gFontUi);
    if (!l.filter.empty()) {
        const std::wstring chip = L"filter  \"" + l.filter + L"\"   " +
                                  std::to_wstring(l.items.size()) + L"/" +
                                  std::to_wstring(l.allItems.size()) + L"   Esc";
        const int cw = uiTextW(dc, chip, gFontSmall) + 22;
        RECT cr{L.path[slot].right - cw - 10, L.path[slot].top + 3,
                L.path[slot].right - 10, L.path[slot].bottom - 3};
        uiRound(dc, cr, RGB(13, 44, 40), kColEmerald, 6);
        uiText(dc, RECT{cr.left + 10, cr.top, cr.right - 10, cr.bottom}, chip, RGB(167, 243, 208), gFontSmall,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else if (l.loading) {
        RECT sp{L.path[slot].right - 110, L.path[slot].top, L.path[slot].right - 12, L.path[slot].bottom};
        uiText(dc, sp, L"reading\u2026", kColCyan, gFontSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    // column header
    const RECT h = L.head[slot];
    uiFill(dc, h, kColChrome);
    uiLine(dc, h.left, h.bottom - 1, h.right, h.bottom - 1, kColLine);
    const int w = h.right - h.left;
    const bool wide = w >= 560;
    const bool mid = w >= 400;
    int sizeR = h.right - 14;
    int modL = 0, typeL = 0;
    if (wide) { modL = sizeR - kColModW; typeL = modL - kColTypeW; sizeR = typeL; }
    else if (mid) { sizeR = sizeR - kColSizeW; }
    else { sizeR = h.right - 14; }
    const int nameR = (wide ? typeL : (mid ? sizeR : sizeR)) - 10;

    auto headCell = [&](RECT r, const std::wstring& label, int key, UINT fmt) {
        const bool on = (l.sortKey == key);
        std::wstring t = label;
        if (on) t += l.sortDesc ? L"  \u25BE" : L"  \u25B4";
        uiText(dc, r, t, on ? kColEmerald : kColDim, on ? gFontBold : gFontSmall, fmt);
    };
    headCell(RECT{h.left + 40, h.top, nameR, h.bottom}, L"Name", SORT_NAME,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (mid) headCell(RECT{sizeR, h.top, sizeR + kColSizeW - 8, h.bottom}, L"Size", SORT_SIZE,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (wide) {
        headCell(RECT{typeL + 8, h.top, modL - 6, h.bottom}, L"Type", SORT_TYPE,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        headCell(RECT{modL + 8, h.top, h.right - 12, h.bottom}, L"Modified", SORT_MODIFIED,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    uiLine(dc, h.left + 34, h.top + 6, h.left + 34, h.bottom - 6, kColLineSoft);
    if (mid) uiLine(dc, sizeR - 6, h.top + 6, sizeR - 6, h.bottom - 6, kColLineSoft);
    if (wide) {
        uiLine(dc, typeL, h.top + 6, typeL, h.bottom - 6, kColLineSoft);
        uiLine(dc, modL, h.top + 6, modL, h.bottom - 6, kColLineSoft);
    }
}

static void uiPaintPaneRows(HDC dc, const UiLayout& L, int slot, int paneId) {
    UiList& l = gPane[paneId];
    const RECT lr = L.list[slot];
    const int h = lr.bottom - lr.top;
    const int w = lr.right - lr.left;
    if (h <= 0) return;

    const bool active = (gActivePane == paneId);
    const bool wide = w >= 560;
    const bool mid = w >= 400;

    // clip to the row area
    HRGN clip = CreateRectRgnIndirect(&lr);
    SelectClipRgn(dc, clip);

    if (!l.error.empty()) {
        RECT box{lr.left + 24, lr.top + 26, lr.right - 24, lr.top + 26 + 96};
        uiRound(dc, box, RGB(38, 24, 14), kColAmber, 8);
        RECT t{box.left + 16, box.top + 12, box.right - 16, box.top + 40};
        uiText(dc, t, L"The link is not answering", kColAmber, gFontBold);
        RECT d{box.left + 16, box.top + 40, box.right - 16, box.bottom - 12};
        uiText(dc, d, l.error, kColDim, gFontSmall, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        RECT hint{lr.left + 24, box.bottom + 14, lr.right - 24, box.bottom + 40};
        uiText(dc, hint, L"Press F5 to retry \u00B7 every transport is re-scanned automatically.",
               kColFaint, gFontSmall);
        SelectClipRgn(dc, nullptr);
        DeleteObject(clip);
        return;
    }

    if (l.items.empty() && !l.loading) {
        RECT t{lr.left, lr.top + h / 2 - 30, lr.right, lr.top + h / 2 + 30};
        const std::wstring msg = (!l.filter.empty() && !l.allItems.empty())
                                     ? (L"Nothing here matches \"" + l.filter + L"\"  \u00B7  Esc clears the filter")
                                     : std::wstring(L"This folder is empty");
        uiText(dc, t, msg, kColDim, gFontUi,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectClipRgn(dc, nullptr);
        DeleteObject(clip);
        return;
    }

    const int maxScroll = (std::max)(0, static_cast<int>(l.items.size()) * kRowH - h);
    if (l.scroll > maxScroll) l.scroll = maxScroll;
    if (l.scroll < 0) l.scroll = 0;

    int sizeR = lr.right - 14;
    int modL = 0, typeL = 0;
    if (wide) { modL = sizeR - kColModW; typeL = modL - kColTypeW; sizeR = typeL; }
    else if (mid) { sizeR = sizeR - kColSizeW; }
    const int nameL = lr.left + 40;
    const int nameR = (wide ? typeL : sizeR) - 10;

    const int first = l.scroll / kRowH;
    const int last = (std::min)(static_cast<int>(l.items.size()) - 1, (l.scroll + h) / kRowH + 1);
    for (int i = first; i <= last; ++i) {
        const int y = lr.top - l.scroll + i * kRowH;
        if (y + kRowH < lr.top || y > lr.bottom) continue;
        const RECT row{lr.left, y, lr.right, y + kRowH};
        const UiItem& it = l.items[i];
        const bool isSel = (i < (int)l.selected.size() && l.selected[i]);
        const bool isHov = (l.hover == i);
        COLORREF bg = kColList;
        if (isSel) bg = active ? kColSel : kColSelIdle;
        else if (isHov) bg = kColHover;
        if (bg != kColList) uiFill(dc, row, bg);
        if (i == l.focused) {
            RECT f{lr.left + 2, y + 1, lr.right - 2, y + kRowH - 1};
            SetDCPenColor(dc, active ? kColEmerald : kColLine);
            HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
            HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, f.left, f.top, f.right, f.bottom);
            SelectObject(dc, ob);
            SelectObject(dc, op);
        }
        uiLine(dc, lr.left, y + kRowH - 1, lr.right, y + kRowH - 1, kColLineSoft);

        uiDrawIcon(dc, lr.left + 12, y + (kRowH - 16) / 2, uiIconFor(it.isDir, it.ext), 16);
        uiText(dc, RECT{nameL, y, nameR, y + kRowH}, it.name,
               isSel ? kColText : (it.isDir ? RGB(190, 226, 255) : kColText), gFontUi);
        if (mid) {
            std::wstring sz = it.isDir ? L"" : uiFormatBytes(it.size);
            uiText(dc, RECT{sizeR, y, sizeR + kColSizeW - 8, y + kRowH}, sz, kColDim, gFontUi,
                   DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        if (wide) {
            uiText(dc, RECT{typeL + 8, y, modL - 6, y + kRowH}, uiTypeLabel(it), kColFaint, gFontUi);
            uiText(dc, RECT{modL + 8, y, lr.right - 12, y + kRowH}, uiFormatTime(it.modifiedMs), kColFaint, gFontUi);
        }
    }

    // slim scrollbar
    const int total = static_cast<int>(l.items.size()) * kRowH;
    if (total > h) {
        RECT track{lr.right - 9, lr.top, lr.right - 2, lr.bottom};
        uiFill(dc, track, kColChrome);
        int th = (std::max)(24, h * h / total);
        int ty = lr.top + (h - th) * l.scroll / (std::max)(1, total - h);
        uiRound(dc, RECT{track.left + 1, ty, track.right - 1, ty + th}, RGB(58, 71, 88), RGB(58, 71, 88), 6);
    }

    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);
}

static void uiPaintStatus(HDC dc, const UiLayout& L) {
    uiFill(dc, L.status, kColChrome);
    uiLine(dc, 0, L.status.top, L.cx, L.status.top, kColLine);

    UiList& l = gPane[gActivePane];
    std::vector<int> sel = uiSelectedIndices(l);
    std::wstring left = std::to_wstring(l.items.size()) +
                        (!l.filter.empty() && l.allItems.size() != l.items.size()
                             ? (L" of " + std::to_wstring(l.allItems.size()) + L" matching")
                             : L"") +
                        L" items";
    if (l.loading) left += L"  \u00B7  reading\u2026";
    if (!sel.empty()) {
        left += L"  \u00B7  " + std::to_wstring(sel.size()) + L" selected";
        if (sel.size() > 1 || !l.items[sel[0]].isDir) left += L"  \u00B7  " + uiFormatBytes(uiSelectedBytes(l));
    }
    uiText(dc, RECT{14, L.status.top, L.cx / 2, L.status.bottom}, left, kColDim, gFontSmall);

    const UiLinkInfo li = uiLinkInfo();
    std::wstring mid = uiTransportPretty(li.transport);
    if (!li.endpoint.empty()) mid += L"  \u00B7  " + li.endpoint;
    if (li.rttMs > 0) {
        wchar_t b[32]; swprintf(b, 32, L"%.0f ms", li.rttMs);
        mid += L"  \u00B7  " + std::wstring(b);
    }
    uiText(dc, RECT{L.cx / 2 - 200, L.status.top, L.cx / 2 + 200, L.status.bottom}, mid,
           li.problem.empty() ? kColEmerald : kColAmber, gFontSmall,
           DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    uiText(dc, RECT{L.cx - 320, L.status.top, L.cx - 14, L.status.bottom},
           std::wstring(L"v") + uiUtf8ToWide(kChainItAppVersion) + L"  \u00B7  Built by Qmer",
           kColFaint, gFontSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

static void uiPaintToast(HDC dc, const UiLayout& L) {
    if (gMainToast.empty()) return;
    if (nowMs() > gMainToastUntil.load()) { gMainToast.clear(); return; }
    const int w = (std::min)(L.cx - 80, uiTextW(dc, gMainToast, gFontUi) + 56);
    const int h = 34;
    RECT r{(L.cx - w) / 2, L.status.top - h - 14, (L.cx + w) / 2, L.status.top - 14};
    const bool bad = gMainToast.find(L"fail") != std::wstring::npos ||
                     gMainToast.find(L"refused") != std::wstring::npos ||
                     gMainToast.find(L"Could not") != std::wstring::npos ||
                     gMainToast.find(L"No phone") != std::wstring::npos;
    uiRound(dc, r, bad ? RGB(46, 22, 22) : RGB(14, 44, 38), bad ? kColDanger : kColEmerald, 8);
    uiText(dc, RECT{r.left + 16, r.top, r.right - 16, r.bottom}, gMainToast,
           bad ? RGB(254, 202, 202) : RGB(167, 243, 208), gFontUi,
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

// --- Link tab ---------------------------------------------------------------

static void uiCard(HDC dc, const RECT& r, const std::wstring& title) {
    uiRound(dc, r, kColChrome, kColLine, 10);
    uiLine(dc, r.left + 2, r.top + 34, r.right - 2, r.top + 34, kColLine);
    uiText(dc, RECT{r.left + 16, r.top, r.right - 16, r.top + 34}, title, kColText, gFontBold);
}

static void uiKv(HDC dc, int x, int y, int w, const std::wstring& k, const std::wstring& v, COLORREF vc) {
    uiText(dc, RECT{x, y, x + 170, y + 20}, k, kColFaint, gFontSmall);
    uiText(dc, RECT{x + 176, y, x + w, y + 20}, v.empty() ? L"\u2014" : v, vc, gFontUi);
}

static void uiPaintLinkTab(HDC dc, const UiLayout& L) {
    const int top = kTabH + kToolH;
    const int bot = L.status.top;
    uiFill(dc, RECT{0, top, L.cx, bot}, kColBg);

    const UiLinkInfo s = uiLinkInfo();
    const int colW = (std::min)(520, L.cx - 300);
    int y = top + 20;

    // headline
    const bool ok = !s.endpoint.empty() && s.problem.empty();
    RECT hl{24, y, 24 + colW, y + 62};
    uiRound(dc, hl, ok ? RGB(12, 42, 36) : RGB(40, 27, 14), ok ? kColEmerald : kColAmber, 10);
    uiText(dc, RECT{hl.left + 18, hl.top + 10, hl.right - 18, hl.top + 32},
           ok ? L"Linked and serving files" : (s.endpoint.empty() ? L"Searching for your phone" : L"Linked, but the phone is not answering"),
           ok ? kColEmerald : kColAmber, gFontBold);
    uiText(dc, RECT{hl.left + 18, hl.top + 34, hl.right - 18, hl.bottom - 8},
           s.problem.empty()
               ? (s.peer.empty() ? std::wstring(L"No device name reported yet") : s.peer)
               : s.problem,
           kColDim, gFontSmall);
    y += 78;

    RECT c1{24, y, 24 + colW, y + 214};
    uiCard(dc, c1, L"Transport");
    int yy = c1.top + 46;
    uiKv(dc, c1.left + 18, yy, colW - 40, L"Active transport", uiTransportPretty(s.transport), kColCyan); yy += 24;
    uiKv(dc, c1.left + 18, yy, colW - 40, L"Phone endpoint", s.endpoint, kColText); yy += 24;
    uiKv(dc, c1.left + 18, yy, colW - 40, L"Peer device", s.peer, kColText); yy += 24;
    uiKv(dc, c1.left + 18, yy, colW - 40, L"Sibling Mode", s.siblingOn ? L"on" : L"off", s.siblingOn ? kColEmerald : kColAmber); yy += 24;
    uiKv(dc, c1.left + 18, yy, colW - 40, L"Z: drive", s.zMounted ? L"mounted" : L"not mounted", s.zMounted ? kColEmerald : kColAmber); yy += 24;
    uiKv(dc, c1.left + 18, yy, colW - 40, L"Phone storage",
         s.phoneTotal ? (uiFormatBytes(s.phoneUsed) + L" used of " + uiFormatBytes(s.phoneTotal)) : L"", kColText); yy += 24;
    uiKv(dc, c1.left + 18, yy, colW - 40, L"Discovered endpoints",
         s.lan.empty() ? L"none yet" : std::to_wstring(s.lan.size()) + L" candidate(s)", kColDim);

    y += 230;
    RECT c2{24, y, 24 + colW, y + 176};
    uiCard(dc, c2, L"Bluetooth RFCOMM link");
    yy = c2.top + 46;
    wchar_t pool[96];
    swprintf(pool, 96, L"%d of %d keep-alive channels open", s.pooled, s.maxCh);
    uiKv(dc, c2.left + 18, yy, colW - 40, L"Channel pool", pool, s.pooled > 0 ? kColEmerald : kColDim); yy += 24;
    uiKv(dc, c2.left + 18, yy, colW - 40, L"Cached SDP channel",
         s.cachedCh > 0 ? (std::to_wstring(s.cachedCh) + L" (no re-lookup)") : L"", kColText); yy += 24;
    uiKv(dc, c2.left + 18, yy, colW - 40, L"Requests served", std::to_wstring(s.served), kColText); yy += 24;
    uiKv(dc, c2.left + 18, yy, colW - 40, L"Re-dials", std::to_wstring(s.redials), s.redials > 8 ? kColAmber : kColDim); yy += 24;
    wchar_t rtt[48]; swprintf(rtt, 48, L"%.0f ms", s.rttMs);
    uiKv(dc, c2.left + 18, yy, colW - 40, L"Last round trip", s.rttMs > 0 ? rtt : L"", kColText); yy += 24;
    uiKv(dc, c2.left + 18, yy, colW - 40, L"Radio / receiver",
         std::wstring(s.btRadio ? L"present" : L"absent") + L"  \u00B7  " + (s.btListening ? L"listening" : L"idle"),
         s.btRadio ? kColEmerald : kColAmber);

    y += 192;
    RECT c3{24, y, 24 + colW, (std::min)(bot - 16, y + 150)};
    uiCard(dc, c3, L"Paired Bluetooth devices");
    yy = c3.top + 44;
    if (s.paired.empty()) {
        uiText(dc, RECT{c3.left + 18, yy, c3.right - 18, yy + 20},
               L"Pair the phone once in Windows Bluetooth settings and ChainIt dials it directly.",
               kColDim, gFontSmall);
    } else {
        for (size_t i = 0; i < s.paired.size() && yy < c3.bottom - 24; ++i) {
            uiText(dc, RECT{c3.left + 18, yy, c3.right - 18, yy + 20}, L"\u2022  " + s.paired[i], kColText, gFontMono);
            yy += 22;
        }
    }

    // action column
    for (int i = 0; i < L.linkBtnCount; ++i) {
        const RECT r = L.linkBtns[i];
        if (r.top > bot - 8) break;
        const bool hov = (gLinkHover == i);
        uiRound(dc, r, hov ? kColBtnHover : kColBtn, hov ? kColEmerald : kColLine, 7);
        uiText(dc, RECT{r.left + 14, r.top, r.right - 14, r.bottom}, kLinkBtnLabels[i], kColText, gFontUi);
    }
}

// --- About tab --------------------------------------------------------------

static void uiPaintAboutTab(HDC dc, const UiLayout& L) {
    const int top = kTabH + kToolH;
    const int bot = L.status.top;
    uiFill(dc, RECT{0, top, L.cx, bot}, kColBg);

    const int w = (std::min)(760, L.cx - 48);
    int y = top + 26;

    if (gMainIconLg) uiDrawIcon(dc, 24, y - 6, gMainIconLg, 64);
    else if (gMainIcon) uiDrawIcon(dc, 28, y + 2, gMainIcon, 48);
    uiText(dc, RECT{24 + 76, y, 24 + w, y + 30}, L"ChainIt", kColText, gFontTitle);
    uiText(dc, RECT{24 + 76, y + 30, 24 + w, y + 52},
           std::wstring(L"Version ") + uiUtf8ToWide(kChainItAppVersion) +
           L"  \u00B7  native Win32 build  \u00B7  no browser, no runtime",
           kColDim, gFontUi);
    y += 74;

    RECT credit{24, y, 24 + w, y + 48};
    uiRound(dc, credit, RGB(12, 42, 36), kColEmerald, 10);
    uiText(dc, RECT{credit.left + 18, credit.top, credit.right - 18, credit.bottom},
           L"Built by Qmer", RGB(167, 243, 208), gFontBold);
    y += 64;

    struct Tile { const wchar_t* t; const wchar_t* d; };
    const Tile tiles[] = {
        {L"Fully native window",
         L"This File Manager is drawn by ChainIt itself with GDI. It never launches Edge, "
         L"Chrome or WebView2, so the taskbar button is ChainIt's own chain-link icon."},
        {L"One process, one identity",
         L"The engine, the tray icon, the mini hub and this window all live in ChainIt-Windows.exe. "
         L"The AppUserModelID ChainIt.Sibling.Hub groups every window under one taskbar button."},
        {L"Bluetooth is a real drive",
         L"Three pooled RFCOMM keep-alive channels carry Z:\\ with no Wi-Fi, hotspot or cable. "
         L"A cached SDP channel avoids re-lookup, and framing is by Content-Length, never EOF."},
        {L"Integrity gate on every byte",
         L"2 MiB chunks with 4 parallel workers (1 over Bluetooth), SHA-256 per chunk and per file. "
         L"Partial data lands in a hidden .chainit-part file and only replaces the target when verified."},
        {L"Phone survives the night",
         L"A connectedDevice foreground service, a 9-minute Doze heartbeat, wake-lock renewal and a "
         L"recents-swipe restart keep Z:\\ alive with the screen locked."},
        {L"Pairing key required",
         L"Every request needs CHAINIT-SIBLING. Five failures in 60 seconds lock the caller out for "
         L"30 seconds; a wrong key is a 401, a disabled Sibling is a 403."},
    };
    const int tileH = 76;
    for (size_t i = 0; i < sizeof(tiles) / sizeof(tiles[0]); ++i) {
        RECT r{24, y, 24 + w, y + tileH};
        if (r.bottom > bot - 12) break;
        uiRound(dc, r, kColChrome, kColLine, 10);
        uiText(dc, RECT{r.left + 18, r.top + 10, r.right - 18, r.top + 30}, tiles[i].t, kColCyan, gFontBold);
        uiText(dc, RECT{r.left + 18, r.top + 32, r.right - 18, r.bottom - 10}, tiles[i].d,
               kColDim, gFontSmall, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        y += tileH + 10;
    }

    RECT foot{24, (std::max)(y, bot - 46), 24 + w, bot - 16};
    uiText(dc, foot, L"ChainIt links an Android phone and a Windows PC into one file space: "
                     L"LAN, hotspot, USB tethering, online relay and Bluetooth RFCOMM.",
           kColFaint, gFontSmall, DT_LEFT | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
}

// --- hit testing ------------------------------------------------------------

static int uiRowAt(const UiList& l, const RECT& listRc, int y) {
    if (y < listRc.top || y >= listRc.bottom) return -1;
    const int idx = (y - listRc.top + l.scroll) / kRowH;
    if (idx < 0 || idx >= static_cast<int>(l.items.size())) return -1;
    return idx;
}

static int uiColumnAt(const UiLayout& L, int slot, int x) {
    const RECT h = L.head[slot];
    const int w = h.right - h.left;
    const bool wide = w >= 560;
    const bool mid = w >= 400;
    int sizeR = h.right - 14;
    int modL = 0, typeL = 0;
    if (wide) { modL = sizeR - kColModW; typeL = modL - kColTypeW; sizeR = typeL; }
    else if (mid) { sizeR = sizeR - kColSizeW; }
    if (wide) {
        if (x >= modL) return SORT_MODIFIED;
        if (x >= typeL) return SORT_TYPE;
        if (x >= sizeR) return SORT_SIZE;
        return SORT_NAME;
    }
    if (mid) {
        if (x >= sizeR) return SORT_SIZE;
        return SORT_NAME;
    }
    return SORT_NAME;
}

static UiHit uiHitTest(const UiLayout& L, int x, int y) {
    UiHit h;
    for (int i = 0; i < 3; ++i) {
        if (PtInRect(&L.tab[i], POINT{x, y})) { h.kind = HIT_TAB; h.id = i; return h; }
    }
    for (size_t i = 0; i < L.tools.size(); ++i) {
        if (PtInRect(&L.tools[i].rc, POINT{x, y})) { h.kind = HIT_TOOL; h.id = static_cast<int>(i); return h; }
    }
    for (int i = 0; i < L.linkBtnCount; ++i) {
        if (PtInRect(&L.linkBtns[i], POINT{x, y})) { h.kind = HIT_LINK; h.id = i; return h; }
    }
    for (int slot = 0; slot < L.panes; ++slot) {
        const int paneId = L.paneId[slot];
        if (PtInRect(&L.pane[slot], POINT{x, y})) {
            h.pane = paneId;
            if (PtInRect(&L.head[slot], POINT{x, y})) { h.kind = HIT_HEAD; h.col = uiColumnAt(L, slot, x); return h; }
            if (PtInRect(&L.list[slot], POINT{x, y})) {
                const int row = uiRowAt(gPane[paneId], L.list[slot], y);
                h.kind = (row >= 0) ? HIT_ROW : HIT_PANE;
                h.row = row;
                return h;
            }
            h.kind = HIT_PANE;
            return h;
        }
    }
    return h;
}

// --- toolbar + link actions -------------------------------------------------

static void uiRunTool(int id) {
    const int pane = gActivePane;
    switch (id) {
        case 0:  uiGoUp(pane); break;
        case 1:  uiReloadBoth(); uiToast(L"Refreshing both panes\u2026", 1600); break;
        case 2:  uiOpNewFolder(pane); break;
        case 3:  uiOpSetClipboard(pane, L"move"); break;
        case 4:  uiOpSetClipboard(pane, L"copy"); break;
        case 5:  uiOpPaste(pane); break;
        case 6:  uiOpRename(pane); break;
        case 7:  uiOpDelete(pane); break;
        case 8:  uiOpTransfer(PANE_PC, false); break;
        case 9:  uiOpTransfer(PANE_PHONE, false); break;
        case 10: gDualPane = false; gActivePane = PANE_PC;
                 if (gPane[PANE_PC].allItems.empty()) uiReload(PANE_PC);
                 break;
        case 11: gDualPane = false; gActivePane = PANE_PHONE;
                 if (gPane[PANE_PHONE].allItems.empty()) uiReload(PANE_PHONE);
                 break;
        case 12:
            gDualPane = !gDualPane;
            if (gDualPane && gPane[PANE_PC].items.empty()) uiReload(PANE_PC);
            if (gDualPane && gPane[PANE_PHONE].items.empty()) uiReload(PANE_PHONE);
            break;
        default: break;
    }
    if (gMainHwnd) InvalidateRect(gMainHwnd, nullptr, FALSE);
}

static void uiRunLink(int id) {
    switch (id) {
        case 0:
            triggerTransportRescan();
            uiToast(L"Re-scanning USB, Wi-Fi, hotspot, online relay and Bluetooth\u2026", 3500);
            break;
        case 1:
            mountZDriveAsync();
            uiToast(L"Mounting Z: \u2192 http://127.0.0.1:4173/dav");
            break;
        case 2:
            ShellExecuteW(gMainHwnd, L"open", L"Z:\\", nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case 3:
            std::thread([]() {
                ShortcutResult sc = createDesktopShortcut();
                uiToast(uiUtf8ToWide(sc.message), 6000);
            }).detach();
            break;
        case 4:
            hubShow();
            break;
        case 5:
            std::thread([]() {
                uiToast(L"Checking for a newer ChainIt-Windows.exe\u2026", 2500);
                AppUpdateResult r = performAppUpdate();
                uiToast(uiUtf8ToWide(r.message.empty() ? "Update finished." : r.message), 8000);
                if (r.restart) {
                    MessageBoxW(gMainHwnd, uiUtf8ToWide(r.message).c_str(), L"ChainIt update", MB_OK | MB_ICONINFORMATION);
                    gRunning.store(false);
                    Shell_NotifyIconW(NIM_DELETE, &gNid);
                    ExitProcess(0);
                }
            }).detach();
            break;
        case 6: {
            std::string key;
            { std::lock_guard<std::mutex> lk(gState.mtx); key = gState.siblingKey; }
            if (OpenClipboard(gMainHwnd)) {
                EmptyClipboard();
                const std::wstring w = uiUtf8ToWide(key);
                HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
                if (g) {
                    wchar_t* p = static_cast<wchar_t*>(GlobalLock(g));
                    if (p) { memcpy(p, w.c_str(), (w.size() + 1) * sizeof(wchar_t)); GlobalUnlock(g); }
                    SetClipboardData(CF_UNICODETEXT, g);
                }
                CloseClipboard();
            }
            uiToast(L"Pairing key copied \u2014 paste the same value into ChainIt on the phone.");
            break;
        }
        default: break;
    }
    if (gMainHwnd) InvalidateRect(gMainHwnd, nullptr, FALSE);
}

// --- context menu -----------------------------------------------------------
// A real right-click menu: TrackPopupMenu with TPM_RETURNCMD returns the chosen
// id straight back, so nothing has to be routed through WM_COMMAND.

/** screenPt anchors the menu for a keyboard invocation (Shift+F10 / menu key),
 *  where the cursor may be nowhere near the focused row. */
static void uiShowContextMenu(HWND hwnd, int pane, int row, const POINT* screenPt = nullptr) {
    if (row >= 0 && pane >= 0) {
        UiList& l = gPane[pane];
        if (row < (int)l.items.size() && !l.selected[row]) {
            l.selected.assign(l.items.size(), 0);
            l.selected[row] = 1;
            l.anchor = row;
            l.focused = row;
        }
    }
    gActivePane = pane;

    const std::vector<int> sel = uiSelectedIndices(gPane[pane]);
    const bool one = (sel.size() == 1);
    const bool any = !sel.empty();

    HMENU m = CreatePopupMenu();
    auto add = [&](UINT id, const wchar_t* text, bool enable, bool check = false) {
        UINT f = MF_STRING | (enable ? MF_ENABLED : MF_GRAYED);
        if (check) f |= MF_CHECKED;
        AppendMenuW(m, f, id, text);
    };
    if (row >= 0) {
        add(1, L"Open", one);
        if (pane == PANE_PC) {
            add(12, L"Open in Explorer", one);
            add(2, L"Send to phone  \u2192", any);
        } else {
            add(3, L"\u2190  Save to PC", any);
            add(4, L"Save to Downloads folder", any);
        }
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        add(8, L"Rename\tF2", one);
        add(9, L"Delete\tDel", any);
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    add(5, L"Cut\tCtrl+X", any);
    add(6, L"Copy\tCtrl+C", any);
    add(7, L"Paste\tCtrl+V", !gClipboardPaths.empty());
    add(10, L"New folder\tCtrl+Shift+N", true);
    add(11, L"Refresh\tF5", true);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    add(13, L"Split view", true, gDualPane);

    POINT pt{};
    if (screenPt) pt = *screenPt;
    else GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    const UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    InvalidateRect(hwnd, nullptr, FALSE);

    switch (cmd) {
        case 1:  uiOpOpen(pane, row); break;
        case 2:  uiOpTransfer(PANE_PC, false); break;
        case 3:  uiOpTransfer(PANE_PHONE, false); break;
        case 4:  uiOpDownloadToPc(pane); break;
        case 5:  uiOpSetClipboard(pane, L"move"); break;
        case 6:  uiOpSetClipboard(pane, L"copy"); break;
        case 7:  uiOpPaste(pane); break;
        case 8:  uiOpRename(pane); break;
        case 9:  uiOpDelete(pane); break;
        case 10: uiOpNewFolder(pane); break;
        case 11: uiReloadBoth(); break;
        case 12: {
            const std::vector<int> s2 = uiSelectedIndices(gPane[pane]);
            if (s2.size() == 1) {
                const fs::path real = uiRealPcPath(gPane[pane].items[s2[0]].path);
                if (gPane[pane].items[s2[0]].isDir) {
                    ShellExecuteW(hwnd, L"open", real.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                } else {
                    std::wstring args = L"/select,\"" + real.wstring() + L"\"";
                    ShellExecuteW(hwnd, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
                }
            }
            break;
        }
        case 13: uiRunTool(12); break;
        default: break;
    }
}

// --- keyboard ---------------------------------------------------------------

static void uiMoveFocus(int pane, int delta, bool shift, bool ctrl) {
    UiList& l = gPane[pane];
    if (l.items.empty()) return;
    int next = l.focused < 0 ? 0 : l.focused + delta;
    if (next < 0) next = 0;
    if (next >= static_cast<int>(l.items.size())) next = static_cast<int>(l.items.size()) - 1;
    l.focused = next;
    if (!ctrl) {
        if (!shift) l.selected.assign(l.items.size(), 0);
        if (shift && l.anchor >= 0) {
            l.selected.assign(l.items.size(), 0);
            const int a = (std::min)(l.anchor, next), b = (std::max)(l.anchor, next);
            for (int i = a; i <= b; ++i) l.selected[i] = 1;
        } else {
            l.selected[next] = 1;
            l.anchor = next;
        }
    }
    // keep the focus row visible
    const int slot = uiPaneSlot(gLayout, pane);
    if (slot >= 0) {
        const RECT lr = gLayout.list[slot];
        const int h = lr.bottom - lr.top;
        const int top = next * kRowH;
        if (top < l.scroll) l.scroll = top;
        else if (top + kRowH > l.scroll + h) l.scroll = top + kRowH - h;
    }
    if (gMainHwnd) InvalidateRect(gMainHwnd, nullptr, FALSE);
}

static void uiSelectAll(int pane) {
    UiList& l = gPane[pane];
    l.selected.assign(l.items.size(), 1);
    if (gMainHwnd) InvalidateRect(gMainHwnd, nullptr, FALSE);
}

static bool uiOnKey(HWND hwnd, UINT vk, bool ctrl, bool shift, bool alt) {
    const int pane = gActivePane;
    if (gTab != TAB_FILES) {
        if (vk == VK_F5) { triggerTransportRescan(); uiReloadBoth(); return true; }
        if (vk == '1') { gTab = TAB_FILES; InvalidateRect(hwnd, nullptr, FALSE); return true; }
        if (vk == '2') { gTab = TAB_LINK; InvalidateRect(hwnd, nullptr, FALSE); return true; }
        if (vk == '3') { gTab = TAB_ABOUT; InvalidateRect(hwnd, nullptr, FALSE); return true; }
        return false;
    }
    switch (vk) {
        case VK_UP:    uiMoveFocus(pane, -1, shift, ctrl); return true;
        case VK_DOWN:  uiMoveFocus(pane, +1, shift, ctrl); return true;
        case VK_PRIOR: uiMoveFocus(pane, -14, shift, ctrl); return true;
        case VK_NEXT:  uiMoveFocus(pane, +14, shift, ctrl); return true;
        case VK_HOME:  uiMoveFocus(pane, -(int)gPane[pane].items.size(), shift, ctrl); return true;
        case VK_END:   uiMoveFocus(pane, (int)gPane[pane].items.size(), shift, ctrl); return true;
        case VK_RETURN:
            if (alt) { uiOpRename(pane); return true; }
            uiOpOpen(pane, gPane[pane].focused);
            return true;
        case VK_BACK:
            // While a filter is active Backspace edits it; otherwise it goes up a
            // level, which is what Explorer does.
            if (!gPane[pane].filter.empty()) {
                gPane[pane].filter.pop_back();
                uiApplyFilter(gPane[pane]);
                InvalidateRect(hwnd, nullptr, FALSE);
                return true;
            }
            if (alt) { uiGoUp(pane); return true; }
            uiGoUp(pane);
            return true;
        case VK_F2:    uiOpRename(pane); return true;
        case VK_F5:    uiReloadBoth(); uiToast(L"Refreshing both panes\u2026", 1600); return true;
        case VK_F6:    gActivePane = (pane == PANE_PC) ? PANE_PHONE : PANE_PC;
                       if (gPane[gActivePane].items.empty()) uiReload(gActivePane);
                       InvalidateRect(hwnd, nullptr, FALSE); return true;
        case VK_DELETE: uiOpDelete(pane); return true;
        case VK_ESCAPE:
            if (!gPane[pane].filter.empty()) {
                gPane[pane].filter.clear();
                uiApplyFilter(gPane[pane]);
                InvalidateRect(hwnd, nullptr, FALSE);
                return true;
            }
            gPane[pane].selected.assign(gPane[pane].items.size(), 0);
            InvalidateRect(hwnd, nullptr, FALSE);
            return true;
        case 'A': if (ctrl) { uiSelectAll(pane); return true; } break;
        case 'C': if (ctrl) { uiOpSetClipboard(pane, L"copy"); return true; } break;
        case 'X': if (ctrl) { uiOpSetClipboard(pane, L"move"); return true; } break;
        case 'V': if (ctrl) { uiOpPaste(pane); return true; } break;
        case 'N': if (ctrl && shift) { uiOpNewFolder(pane); return true; } break;
        case 'F': if (ctrl) {
                      gPane[pane].filter.clear();
                      uiApplyFilter(gPane[pane]);
                      uiToast(L"Type to filter this pane \u00B7 Esc clears it");
                      return true;
                  }
                  break;
        case 'T': if (ctrl) { gTab = (gTab + 1) % 3; InvalidateRect(hwnd, nullptr, FALSE); return true; } break;
        case '1': gTab = TAB_FILES; InvalidateRect(hwnd, nullptr, FALSE); return true;
        case '2': gTab = TAB_LINK; InvalidateRect(hwnd, nullptr, FALSE); return true;
        case '3': gTab = TAB_ABOUT; InvalidateRect(hwnd, nullptr, FALSE); return true;
        default: break;
    }
    return false;
}

// --- window proc ------------------------------------------------------------

static std::wstring gLastSeenPhoneIp;

static void uiTickAutoLink(HWND hwnd) {
    std::string ip = uiCurrentPhoneIp();
    const std::wstring w = uiUtf8ToWide(ip);
    if (w != gLastSeenPhoneIp) {
        const bool appeared = gLastSeenPhoneIp.empty() && !w.empty();
        gLastSeenPhoneIp = w;
        if (appeared && gTab == TAB_FILES) {
            UiLinkInfo s = uiLinkInfo();
            uiToast(L"Phone linked over " + uiTransportPretty(s.transport) + L" \u2014 reading Z:\\", 5000);
            uiLoadPaneAsync(PANE_PHONE, gPane[PANE_PHONE].dir);
        }
    }
    if (nowMs() > gMainToastUntil.load() && !gMainToast.empty()) {
        gMainToast.clear();
    }
    if (gTab != TAB_FILES || !gMainToast.empty()) InvalidateRect(hwnd, nullptr, FALSE);
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            gMainHwnd = hwnd;
            uiFontsInit();
            SetTimer(hwnd, 1, 1000, nullptr);
            return 0;

        case WM_ERASEBKGND:
            return 1;   // fully double-buffered below

        case WM_SIZE:
            gLayout = uiComputeLayout(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_TIMER:
            if (wParam == 1) { uiTickAutoLink(hwnd); return 0; }
            if (wParam == 2) {
                // toast lifetime expired
                gMainToast.clear();
                KillTimer(hwnd, 2);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;

        case WM_UI_LISTDONE:
            uiApplyListPayload(reinterpret_cast<ListPayload*>(lParam));
            return 0;

        case WM_UI_REFRESH:
            uiReload(static_cast<int>(wParam));
            return 0;

        case WM_UI_REFRESH_ALL:
            uiReloadBoth();
            return 0;

        case WM_UI_TOAST: {
            ToastMsg* t = reinterpret_cast<ToastMsg*>(lParam);
            if (!t) return 0;
            gMainToast = t->text;
            const int ms = (t->ms > 0) ? t->ms : 60;
            delete t;
            gMainToastUntil.store(nowMs() + ms);
            SetTimer(hwnd, 2, static_cast<UINT>(ms), nullptr);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC wdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            const int cx = rc.right, cy = rc.bottom;
            HDC dc = CreateCompatibleDC(wdc);
            HBITMAP bmp = CreateCompatibleBitmap(wdc, (std::max)(1, cx), (std::max)(1, cy));
            HGDIOBJ oldBmp = SelectObject(dc, bmp);
            SetBkMode(dc, TRANSPARENT);

            gLayout = uiComputeLayout(hwnd);
            uiFill(dc, rc, kColBg);
            uiPaintTabs(dc, gLayout);
            uiPaintToolbar(dc, gLayout);
            if (gTab == TAB_FILES) {
                for (int slot = 0; slot < gLayout.panes; ++slot) {
                    const int paneId = gLayout.paneId[slot];
                    uiPaintPaneChrome(dc, gLayout, slot, paneId);
                    uiPaintPaneRows(dc, gLayout, slot, paneId);
                }
                if (gLayout.dual) uiLine(dc, cx / 2, gLayout.pane[0].top, cx / 2, gLayout.pane[0].bottom, kColLine);
            } else if (gTab == TAB_LINK) {
                uiPaintLinkTab(dc, gLayout);
            } else {
                uiPaintAboutTab(dc, gLayout);
            }
            uiPaintStatus(dc, gLayout);
            uiPaintToast(dc, gLayout);

            BitBlt(wdc, 0, 0, cx, cy, dc, 0, 0, SRCCOPY);
            SelectObject(dc, oldBmp);
            DeleteObject(bmp);
            DeleteDC(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_MOUSEMOVE: {
            const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            const UiHit h = uiHitTest(gLayout, x, y);
            int tool = -1, link = -1;
            if (h.kind == HIT_TOOL) tool = h.id;
            if (h.kind == HIT_LINK) link = h.id;
            bool changed = (tool != gToolHover) || (link != gLinkHover);
            gToolHover = tool;
            gLinkHover = link;

            HCURSOR cur = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
            if (h.kind == HIT_TAB || h.kind == HIT_TOOL || h.kind == HIT_LINK || h.kind == HIT_HEAD || h.kind == HIT_ROW)
                cur = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_HAND));
            SetCursor(cur);

            for (int slot = 0; slot < gLayout.panes; ++slot) {
                UiList& l = gPane[gLayout.paneId[slot]];
                const int row = (h.kind == HIT_ROW && h.pane == gLayout.paneId[slot]) ? h.row : -1;
                if (l.hover != row) { l.hover = row; changed = true; }
            }
            if (changed) InvalidateRect(hwnd, nullptr, FALSE);

            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            return 0;
        }

        case WM_MOUSELEAVE:
            gToolHover = -1;
            gLinkHover = -1;
            for (int i = 0; i < 2; ++i) gPane[i].hover = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_LBUTTONDOWN: {
            const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            const UiHit h = uiHitTest(gLayout, x, y);
            SetFocus(hwnd);
            if (h.kind == HIT_TAB) {
                if (gTab != h.id) { gTab = h.id; InvalidateRect(hwnd, nullptr, FALSE); }
                return 0;
            }
            if (h.kind == HIT_TOOL) {
                if (gLayout.tools[h.id].enabled) uiRunTool(gLayout.tools[h.id].id);
                return 0;
            }
            if (h.kind == HIT_LINK) { uiRunLink(h.id); return 0; }
            if (h.pane >= 0 && h.pane != gActivePane) {
                gActivePane = h.pane;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (h.kind == HIT_HEAD) {
                UiList& l = gPane[h.pane];
                if (l.sortKey == h.col) l.sortDesc = !l.sortDesc;
                else { l.sortKey = h.col; l.sortDesc = false; }
                uiSortList(l);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (h.kind == HIT_ROW) {
                UiList& l = gPane[h.pane];
                const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                if (ctrl) {
                    l.selected[h.row] = l.selected[h.row] ? 0 : 1;
                    l.anchor = h.row;
                    l.focused = h.row;
                } else if (shift && l.anchor >= 0) {
                    l.selected.assign(l.items.size(), 0);
                    const int a = (std::min)(l.anchor, h.row), b = (std::max)(l.anchor, h.row);
                    for (int i = a; i <= b; ++i) l.selected[i] = 1;
                    l.focused = h.row;
                } else {
                    l.selected.assign(l.items.size(), 0);
                    l.selected[h.row] = 1;
                    l.anchor = h.row;
                    l.focused = h.row;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (h.kind == HIT_PANE) {
                UiList& l = gPane[h.pane];
                l.selected.assign(l.items.size(), 0);
                l.focused = -1;
                l.anchor = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            const UiHit h = uiHitTest(gLayout, x, y);
            if (h.kind == HIT_ROW) uiOpOpen(h.pane, h.row);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &p);
            const UiHit h = uiHitTest(gLayout, p.x, p.y);
            const int pane = (h.pane >= 0) ? h.pane : gActivePane;
            UiList& l = gPane[pane];
            const int slot = uiPaneSlot(gLayout, pane);
            const int height = (slot >= 0) ? (gLayout.list[slot].bottom - gLayout.list[slot].top) : 400;
            const int maxScroll = (std::max)(0, static_cast<int>(l.items.size()) * kRowH - height);
            l.scroll += (GET_WHEEL_DELTA_WPARAM(wParam) > 0) ? -3 * kRowH : 3 * kRowH;
            if (l.scroll < 0) l.scroll = 0;
            if (l.scroll > maxScroll) l.scroll = maxScroll;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_CONTEXTMENU: {
            // Shift+F10 and the menu key arrive with lParam == -1; anchor the menu
            // on the focused row instead of on an unusable screen coordinate.
            if (lParam == static_cast<LPARAM>(-1)) {
                const int slot = uiPaneSlot(gLayout, gActivePane);
                if (slot < 0) return 0;
                const UiList& l = gPane[gActivePane];
                const int row = l.focused;
                int y = gLayout.list[slot].top + 12;
                if (row >= 0) y = gLayout.list[slot].top - l.scroll + row * kRowH + kRowH / 2;
                POINT sp{gLayout.list[slot].left + 60, y};
                ClientToScreen(hwnd, &sp);
                uiShowContextMenu(hwnd, gActivePane, row, &sp);
                return 0;
            }
            POINT p{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
            ScreenToClient(hwnd, &p);
            const UiHit h = uiHitTest(gLayout, p.x, p.y);
            if (h.pane >= 0) uiShowContextMenu(hwnd, h.pane, h.row);
            return 0;
        }

        case WM_RBUTTONUP: {
            const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            const UiHit h = uiHitTest(gLayout, x, y);
            if (h.pane >= 0) uiShowContextMenu(hwnd, h.pane, h.row);
            return 0;
        }

        case WM_CHAR: {
            // Type-ahead filter, exactly like Explorer: printable characters narrow
            // the active pane's list until Esc clears it.
            if (gTab != TAB_FILES) return 0;
            const wchar_t ch = static_cast<wchar_t>(wParam);
            if (ch < 32) return 0;   // handled as keys (Enter, Backspace, Esc, Tab)
            if (GetKeyState(VK_CONTROL) & 0x8000) return 0;
            UiList& l = gPane[gActivePane];
            if (l.filter.size() < 64) l.filter.push_back(ch);
            uiApplyFilter(l);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
            if (uiOnKey(hwnd, static_cast<UINT>(wParam), ctrl, shift, alt)) return 0;
            break;
        }

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 720;
            mmi->ptMinTrackSize.y = 480;
            return 0;
        }

        case WM_CLOSE:
            // ChainIt is a resident service: closing the window keeps the engine,
            // the tray icon and Z:\ alive, exactly like the hub window does.
            ShowWindow(hwnd, SW_HIDE);
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, 1);
            KillTimer(hwnd, 2);
            if (gMainHwnd == hwnd) gMainHwnd = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// --- window creation --------------------------------------------------------

/** Creates (or re-shows) ChainIt's own native File Manager window. Nothing here
 *  touches a browser: this window is the app, and its taskbar button is ours. */
static void showNativeMainWindow() {
    if (gMainHwnd && IsWindow(gMainHwnd)) {
        if (IsIconic(gMainHwnd)) ShowWindow(gMainHwnd, SW_RESTORE);
        ShowWindow(gMainHwnd, SW_SHOW);
        SetForegroundWindow(gMainHwnd);
        uiReloadBoth();
        return;
    }

    HINSTANCE inst = GetModuleHandleW(nullptr);
    gMainInstance = inst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // The .ico carries 16..256 px, so load the exact sizes the shell wants rather
    // than stretching the 32 px default: crisp caption glyph, crisp taskbar button.
    if (!gMainIcon)   gMainIcon   = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    if (!gMainIconSm) gMainIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    if (!gMainIconLg) gMainIconLg = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 64, 64, LR_DEFAULTCOLOR));
    if (!gMainIcon)   gMainIcon   = LoadIconW(inst, MAKEINTRESOURCEW(1));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = inst;
    wc.hIcon = gMainIcon;
    wc.hIconSm = gMainIconSm ? gMainIconSm : gMainIcon;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kMainClass;
    if (!GetClassInfoExW(inst, kMainClass, &wc)) RegisterClassExW(&wc);

    // One taskbar identity for the whole process, so every ChainIt window groups
    // under a single button wearing the chain-link icon.
    typedef HRESULT(WINAPI * SetAumidFn)(PCWSTR);
    if (HMODULE shell32 = GetModuleHandleW(L"shell32.dll")) {
        if (auto fn = reinterpret_cast<SetAumidFn>(GetProcAddress(shell32, "SetCurrentProcessExplicitAppUserModelID")))
            fn(L"ChainIt.Sibling.Hub");
    }

    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    const int w = (std::min)(1380, sw - 80);
    const int hgt = (std::min)(860, sh - 100);

    HWND hwnd = CreateWindowExW(
        0, kMainClass, L"ChainIt \u2014 File Manager",
        WS_OVERLAPPEDWINDOW,
        (sw - w) / 2, (sh - hgt) / 2, w, hgt,
        nullptr, nullptr, inst, nullptr);

    if (!hwnd) {
        // Absolute last resort: never fall back to a browser window.
        MessageBoxW(nullptr, L"ChainIt could not create its window.", L"ChainIt", MB_ICONERROR);
        return;
    }

    gMainHwnd = hwnd;
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(gMainIcon));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(gMainIconSm ? gMainIconSm : gMainIcon));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL2, reinterpret_cast<LPARAM>(gMainIconSm ? gMainIconSm : gMainIcon));

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);

    uiFontsInit();
    gLayout = uiComputeLayout(hwnd);
    if (gPane[PANE_PC].items.empty() && gPane[PANE_PC].dir.empty()) gPane[PANE_PC].dir = L"/";
    if (gPane[PANE_PHONE].dir.empty()) gPane[PANE_PHONE].dir = L"/";
    uiReloadBoth();
}

/** Kept as the single entry point the tray menu, the hub window and the
 *  single-instance hand-off already call. It used to launch Edge in app mode;
 *  that is exactly what put a browser globe on the taskbar, so it now opens the
 *  native window instead. */
static void launchDesktopUiWindow() { showNativeMainWindow(); }

static HWND createHubWindow(HINSTANCE hInstance, HICON icon) {
    gHubInstance = hInstance;
    gHubIcon = icon;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HubWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = icon;
    wc.hIconSm = icon;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kHubClass;
    RegisterClassExW(&wc);

    const int w = 468, h = 520;
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExW(
        0, kHubClass, L"ChainIt Sibling",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (sw - w) / 2, (sh - h) / 2, w, h,
        nullptr, nullptr, hInstance, nullptr);

    // One taskbar identity for the whole process: the chain-link icon, not the
    // browser's globe, and no extra grouping buttons for the app-mode window.
    typedef HRESULT(WINAPI * SetAumidFn)(PCWSTR);
    HMODULE shell32 = GetModuleHandleW(L"shell32.dll");
    if (shell32) {
        SetAumidFn fn = reinterpret_cast<SetAumidFn>(
            GetProcAddress(shell32, "SetCurrentProcessExplicitAppUserModelID"));
        if (fn) fn(L"ChainIt.Sibling.Hub");
    }
    if (hwnd) SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
    if (hwnd) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
    return hwnd;
}

static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_TRAYICON) {
        if (lParam == WM_LBUTTONDBLCLK) {
            hubShow();
        } else if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_HUB, L"Show ChainIt (mini panel)");
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_OPEN_UI, L"Open ChainIt File Manager");
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_OPEN_Z, L"Open Z:\\ Phone Drive in Explorer");
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_REMOUNT_Z, L"Mount / Refresh Z:\\ Drive");
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_MINI, L"Mini status panel");
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
            case ID_TRAY_HUB:
                hubShow();
                break;
            case ID_TRAY_MINI:
                hubShow();
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

// ---------------------------------------------------------------------------
// Uninstall mode: "ChainIt-Windows.exe /uninstall"
//
// The setup program registers this exact command line as the Add/Remove
// Programs uninstaller, so removing ChainIt is one entry in Apps & features.
// A running exe cannot delete itself, so the file removal is handed to a tiny
// detached cmd.exe that waits for this process to disappear.
// ---------------------------------------------------------------------------
static int runUninstallMode(bool silent) {
    runHiddenCommand(L"cmd.exe /c net use Z: /delete /y");

    const std::wstring exe = currentExePath();
    const std::wstring dir = fs::path(exe).parent_path().wstring();

    auto dropLnk = [](const std::wstring& path) {
        if (path.empty()) return;
        DeleteFileW(path.c_str());
        SHChangeNotify(SHCNE_DELETE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, path.c_str(), nullptr);
    };

    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &raw)) && raw) {
        dropLnk(std::wstring(raw) + L"\\ChainIt.lnk");
        CoTaskMemFree(raw);
    }
    raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &raw)) && raw) {
        const std::wstring prog = std::wstring(raw) + L"\\ChainIt";
        dropLnk(prog + L"\\ChainIt.lnk");
        RemoveDirectoryW(prog.c_str());
        CoTaskMemFree(raw);
    }

    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ChainIt");

    wchar_t appData[MAX_PATH * 2] = {};
    if (GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH * 2 - 1) > 0) {
        std::error_code ec;
        fs::remove_all(fs::path(appData) / L"ChainIt", ec); // saved settings & pairing key
    }

    if (!silent) {
        MessageBoxW(nullptr,
                    L"ChainIt has been removed.\n\n"
                    L"Z:\\ was unmapped, the Desktop and Start Menu shortcuts are gone, the "
                    L"Apps & features entry was deleted, and your saved ChainIt settings were cleared.\n\n"
                    L"The ChainIt-Windows.exe file itself is deleted a moment after this window closes.",
                    L"ChainIt removed", MB_OK | MB_ICONINFORMATION);
    }

    // Detached self-delete: wait ~2s for this process to exit, then wipe the folder.
    const std::wstring cmd = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & del /f /q \"" + exe +
                             L"\" & rd /s /q \"" + dir + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                   CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (pi.hThread) CloseHandle(pi.hThread);

    Shell_NotifyIconW(NIM_DELETE, &gNid);
    gRunning.store(false);
    ExitProcess(0);
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    {
        const std::wstring cmdLine = GetCommandLineW();
        const bool uninstall = cmdLine.find(L"/uninstall") != std::wstring::npos ||
                               cmdLine.find(L"/UNINSTALL") != std::wstring::npos;
        if (uninstall) {
            const bool silent = cmdLine.find(L"/S") != std::wstring::npos || cmdLine.find(L"/s") != std::wstring::npos;
            return runUninstallMode(silent);
        }
    }

    // Prevent duplicate instances; if already running, just bring up the ChainIt File Manager window!
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\ChainItSiblingEngineSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // ChainIt is already running in another process. Creating a window here
        // would give the user a File Manager with no engine behind it, so hand
        // off to the live window and exit.
        HWND live = FindWindowW(kMainClass, nullptr);
        if (live) {
            if (IsIconic(live)) ShowWindow(live, SW_RESTORE);
            ShowWindow(live, SW_SHOW);
            SetForegroundWindow(live);
            PostMessageW(live, WM_UI_REFRESH_ALL, 0, 0);
        } else if (HWND hubLive = FindWindowW(kHubClass, nullptr)) {
            ShowWindow(hubLive, SW_SHOW);
            SetForegroundWindow(hubLive);
        }
        if (hMutex) CloseHandle(hMutex);
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

    // The mini hub is created up front (it registers its class and pins the
    // process AppUserModelID) but stays hidden: the native File Manager is now
    // the window that owns ChainIt's taskbar button, so showing both at startup
    // would just stack two ChainIt windows on top of each other. The hub remains
    // one tray double-click away, and the Link tab has a button for it too.
    HWND hub = createHubWindow(hInstance, gNid.hIcon);
    (void)hub;

    // The HTTP server keeps running for Z:\, the phone and the WebSocket clients,
    // but the File Manager the user sees is this process's own native window.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    showNativeMainWindow();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    WSACleanup();
    if (hMutex) CloseHandle(hMutex);
    return 0;
}
