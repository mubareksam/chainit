// ===========================================================================
// ChainIt for Windows - setup program
//
// A real Windows installer with zero third-party tooling: the build script
// appends the signed ChainIt-Windows.exe payload plus a 52-byte trailer to this
// stub, so the single distributed file is both the installer and the payload.
//
// It installs to %LOCALAPPDATA%\Programs\ChainIt (no UAC prompt, per-user,
// in-place: re-running the setup overwrites the very same file), verifies the
// payload's SHA-256 before it is written, creates the Desktop and Start Menu
// chain-link shortcuts, registers Add/Remove Programs, and can launch the app.
//
//   ChainIt-Setup-<version>.exe            interactive
//   ChainIt-Setup-<version>.exe /S         silent
//   ChainIt-Setup-<version>.exe /UNINSTALL remove ChainIt
// ===========================================================================

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <wincrypt.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#ifndef CHAINIT_INSTALLER_VERSION
#define CHAINIT_INSTALLER_VERSION "1.3.0"
#endif

static const char kFooterMagic[8] = {'C','H','A','I','N','I','T','1'};
static const uint32_t kFooterSize = 52;

static HINSTANCE gInst = nullptr;
static HICON gIcon = nullptr;
static HWND gWnd = nullptr, gStatus = nullptr, gBar = nullptr, gLaunch = nullptr;
static HWND gInstallBtn = nullptr, gCancelBtn = nullptr;
static bool gSilent = false;
static volatile bool gCancelled = false;

static std::wstring wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}
static std::string narrow(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

static std::wstring selfPath() {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2 - 1);
    return (n > 0 && n < MAX_PATH * 2 - 1) ? std::wstring(buf, n) : std::wstring();
}

static std::wstring envPath(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2 - 1);
    return (n > 0 && n < MAX_PATH * 2 - 1) ? std::wstring(buf, n) : std::wstring();
}

static std::wstring installDir() {
    std::wstring local = envPath(L"LOCALAPPDATA");
    if (local.empty()) local = envPath(L"USERPROFILE") + L"\\AppData\\Local";
    if (local.empty()) return L"C:\\ChainIt";
    return local + L"\\Programs\\ChainIt";
}
static std::wstring installExe() { return installDir() + L"\\ChainIt-Windows.exe"; }

static std::wstring sha256Hex(const uint8_t* data, size_t len) {
    HCRYPTPROV prov = 0; HCRYPTHASH hash = 0;
    std::string out;
    if (CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash)) {
            if (CryptHashData(hash, data, (DWORD)len, 0)) {
                BYTE digest[32] = {}; DWORD dlen = 32;
                if (CryptGetHashParam(hash, HP_HASHVAL, digest, &dlen, 0)) {
                    char hex[65] = {};
                    for (DWORD i = 0; i < dlen && i < 32; ++i)
                        std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
                    out = hex;
                }
            }
            CryptDestroyHash(hash);
        }
        CryptReleaseContext(prov, 0);
    }
    return wide(out);
}

// --- footer / payload -------------------------------------------------------

struct PayloadInfo {
    bool ok = false;
    uint64_t offset = 0;
    uint64_t size = 0;
    std::wstring sha;
    std::string message;
};

static PayloadInfo readPayloadInfo() {
    PayloadInfo info;
    const std::wstring self = selfPath();
    if (self.empty()) { info.message = "Could not resolve the setup program's own path."; return info; }

    HANDLE h = CreateFileW(self.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { info.message = "Could not open the setup program for reading."; return info; }
    LARGE_INTEGER total{};
    if (!GetFileSizeEx(h, &total) || (uint64_t)total.QuadPart <= kFooterSize + 1024) {
        CloseHandle(h);
        info.message = "This setup file carries no ChainIt payload (it was truncated).";
        return info;
    }
    uint8_t footer[kFooterSize] = {};
    LARGE_INTEGER pos{}; pos.QuadPart = (LONGLONG)(total.QuadPart - kFooterSize);
    DWORD rd = 0;
    if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) || !ReadFile(h, footer, kFooterSize, &rd, nullptr) || rd != kFooterSize) {
        CloseHandle(h);
        info.message = "Could not read the setup trailer.";
        return info;
    }
    CloseHandle(h);

    if (memcmp(footer, kFooterMagic, 8) != 0) {
        info.message = "This file is not a ChainIt setup package (trailer magic missing).";
        return info;
    }
    uint64_t size = 0;
    memcpy(&size, footer + 8, 8);
    uint32_t declared = 0;
    memcpy(&declared, footer + 48, 4);
    if (declared != kFooterSize || size == 0 || size + kFooterSize >= (uint64_t)total.QuadPart) {
        info.message = "The setup trailer is inconsistent with this file's size.";
        return info;
    }
    wchar_t sha[33] = {};
    for (int i = 0; i < 32; ++i) swprintf(sha + i * 2, 3, L"%02x", footer[16 + i]);

    info.ok = true;
    info.size = size;
    info.offset = (uint64_t)total.QuadPart - kFooterSize - size;
    info.sha = sha;
    return info;
}

static const wchar_t* kTrayClass = L"ChainItTrayHiddenWindowClass";
static const wchar_t* kEngineMutex = L"Local\\ChainItSiblingEngineSingleInstance";

/**
 * Close a running ChainIt before its exe is overwritten.
 *
 * Windows keeps a running executable's image file locked, so writing the payload
 * over an active install fails outright. The previous attempt posted WM_CLOSE to
 * the hub window, which only *hides* it - ChainIt is a resident service, so the
 * engine kept running, kept the file locked, and every retry failed. Setup then
 * aborted while the OLD build stayed installed and kept launching a browser.
 *
 * The window that actually stops the engine is the tray window, and because it is
 * created with HWND_MESSAGE it is message-only: plain FindWindow() cannot see it,
 * only FindWindowEx(HWND_MESSAGE, ...) can. Closing it makes the old process
 * delete its tray icon and exit, releasing the file lock and the mutex.
 */
static bool stopRunningChainIt(int waitMs) {
    HANDLE mtx = OpenMutexW(SYNCHRONIZE, FALSE, kEngineMutex);
    HWND tray = FindWindowExW(HWND_MESSAGE, nullptr, kTrayClass, nullptr);
    if (!mtx && !tray) return true;
    if (tray) PostMessageW(tray, WM_CLOSE, 0, 0);
    for (int waited = 0; waited < waitMs; waited += 150) {
        Sleep(150);
        if (!FindWindowExW(HWND_MESSAGE, nullptr, kTrayClass, nullptr)) break;
    }
    bool gone = true;
    if (mtx) {
        // The window can be gone a beat before the process releases the mutex.
        gone = (WaitForSingleObject(mtx, 6000) == WAIT_OBJECT_0);
        CloseHandle(mtx);
    }
    return gone;
}

static bool extractPayload(const PayloadInfo& info, const std::wstring& dest, std::string* err) {
    const std::wstring self = selfPath();
    HANDLE h = CreateFileW(self.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { *err = "Could not open the setup package."; return false; }

    std::vector<uint8_t> buf((size_t)info.size);
    LARGE_INTEGER pos{}; pos.QuadPart = (LONGLONG)info.offset;
    if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) { CloseHandle(h); *err = "Payload seek failed."; return false; }

    size_t got = 0;
    while (got < buf.size()) {
        DWORD want = (DWORD)std::min<uint64_t>(1u << 20, buf.size() - got);
        DWORD rd = 0;
        if (!ReadFile(h, buf.data() + got, want, &rd, nullptr) || rd == 0) break;
        got += rd;
        if (gBar) {
            int pct = (int)((got * 100) / buf.size());
            SendMessageW(gBar, PBM_SETPOS, pct, 0);
        }
        if (gCancelled) { CloseHandle(h); *err = "Cancelled."; return false; }
    }
    CloseHandle(h);
    if (got != buf.size()) { *err = "Payload read stopped early - the download may be incomplete."; return false; }

    // Verify before anything touches the disk.
    const std::wstring actual = sha256Hex(buf.data(), buf.size());
    if (!info.sha.empty() && actual != info.sha) {
        *err = "SHA-256 mismatch: expected " + narrow(info.sha) + ", got " + narrow(actual);
        return false;
    }

    if (buf.size() < 2 || buf[0] != 'M' || buf[1] != 'Z') { *err = "Payload is not a Windows executable."; return false; }

    CreateDirectoryW(installDir().c_str(), nullptr);
    HANDLE out = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        // Still locked: close the engine through its message-only tray window and
        // keep retrying the write until the image lock is really gone.
        stopRunningChainIt(12000);
        for (int i = 0; i < 60; ++i) {
            out = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (out != INVALID_HANDLE_VALUE) break;
            Sleep(150);
        }
    }
    if (out == INVALID_HANDLE_VALUE) { *err = "Windows refused to write " + narrow(dest) + " (is ChainIt still running?)"; return false; }

    size_t wrote = 0;
    while (wrote < buf.size()) {
        DWORD want = (DWORD)std::min<uint64_t>(1u << 20, buf.size() - wrote);
        DWORD wr = 0;
        if (!WriteFile(out, buf.data() + wrote, want, &wr, nullptr) || wr == 0) break;
        wrote += wr;
    }
    FlushFileBuffers(out);
    CloseHandle(out);
    if (wrote != buf.size()) { DeleteFileW(dest.c_str()); *err = "Write stopped early."; return false; }
    return true;
}

// --- shortcuts + Add/Remove Programs ---------------------------------------

static bool makeShortcut(const std::wstring& lnkPath, const std::wstring& target, const std::wstring& workDir,
                        const std::wstring& desc) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellLinkW* link = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&link)) && link) {
        link->SetPath(target.c_str());
        link->SetWorkingDirectory(workDir.c_str());
        link->SetIconLocation(target.c_str(), 0);
        link->SetDescription(desc.c_str());
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&pf)) && pf) {
            ok = SUCCEEDED(pf->Save(lnkPath.c_str(), TRUE));
            pf->Release();
        }
        link->Release();
    }
    CoUninitialize();
    if (ok) SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, lnkPath.c_str(), nullptr);
    return ok;
}

static void writeUninstallEntry(const std::wstring& exe) {
    HKEY key = nullptr;
    const wchar_t* sub = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ChainIt";
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        auto setS = [&](const wchar_t* n, const std::wstring& v) {
            RegSetValueExW(key, n, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
        };
        auto setD = [&](const wchar_t* n, DWORD v) { RegSetValueExW(key, n, 0, REG_DWORD, (const BYTE*)&v, 4); };
        setS(L"DisplayName", L"ChainIt Sibling");
        setS(L"DisplayVersion", wide(CHAINIT_INSTALLER_VERSION));
        setS(L"Publisher", L"ChainIt");
        setS(L"DisplayIcon", exe);
        setS(L"InstallLocation", installDir());
        setS(L"UninstallString", L"\"" + exe + L"\" /uninstall");
        setS(L"QuietUninstallString", L"\"" + exe + L"\" /uninstall /S");
        setD(L"NoModify", 1);
        setD(L"NoRepair", 1);
        RegCloseKey(key);
    }
}

static void setStatus(const std::wstring& s) {
    if (gStatus) SetWindowTextW(gStatus, s.c_str());
    if (gSilent) std::wprintf(L"%ls\n", s.c_str());
}

static int doInstall() {
    setStatus(L"Reading the ChainIt payload\u2026");
    PayloadInfo info = readPayloadInfo();
    if (!info.ok) {
        setStatus(wide(info.message));
        if (!gSilent) MessageBoxW(gWnd, wide(info.message).c_str(), L"ChainIt Setup", MB_OK | MB_ICONERROR);
        return 1;
    }

    setStatus(L"Closing the running ChainIt so its file can be replaced\u2026");
    if (!stopRunningChainIt(12000)) {
        const std::wstring warn =
            L"ChainIt is still running and Windows will not let setup replace its file.\n\n"
            L"Right-click the ChainIt icon in the system tray, choose\n"
            L"\"Unmount Z:\\ && Exit ChainIt\", then run this setup again.";
        setStatus(warn);
        if (!gSilent) MessageBoxW(gWnd, warn.c_str(), L"ChainIt Setup", MB_OK | MB_ICONWARNING);
    }

    setStatus(L"Unpacking ChainIt-Windows.exe (" + std::to_wstring(info.size / 1024) + L" KB)\u2026");
    std::string err;
    const std::wstring exe = installExe();
    if (!extractPayload(info, exe, &err)) {
        setStatus(wide(err));
        if (!gSilent) MessageBoxW(gWnd, wide(err).c_str(), L"ChainIt Setup", MB_OK | MB_ICONERROR);
        return 2;
    }

    setStatus(L"Verifying SHA-256 integrity\u2026");
    if (gBar) SendMessageW(gBar, PBM_SETPOS, 100, 0);

    setStatus(L"Creating Desktop and Start Menu shortcuts\u2026");
    const std::wstring desc = L"ChainIt - Android/Windows Sibling link, Z:\\ drive and File Manager";
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &raw)) && raw) {
        makeShortcut(std::wstring(raw) + L"\\ChainIt.lnk", exe, installDir(), desc);
        CoTaskMemFree(raw);
    }
    raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &raw)) && raw) {
        const std::wstring dir = std::wstring(raw) + L"\\ChainIt";
        CreateDirectoryW(dir.c_str(), nullptr);
        makeShortcut(dir + L"\\ChainIt.lnk", exe, installDir(), desc);
        CoTaskMemFree(raw);
    }

    setStatus(L"Registering ChainIt in Apps & features\u2026");
    writeUninstallEntry(exe);

    setStatus(L"ChainIt " + wide(CHAINIT_INSTALLER_VERSION) + L" is installed.");
    return 0;
}

// --- UI ---------------------------------------------------------------------

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wParam) == 1002) { // Cancel / Close
                if (IsWindowEnabled(gInstallBtn)) { DestroyWindow(hwnd); }
                else { gCancelled = true; SetWindowTextW(gCancelBtn, L"Cancelling\u2026"); EnableWindow(gCancelBtn, FALSE); }
            } else if (LOWORD(wParam) == 1001) { // Install, then Finish
                if (GetWindowLongPtrW(gInstallBtn, GWLP_USERDATA) == 1) { DestroyWindow(hwnd); return 0; }
                EnableWindow(gInstallBtn, FALSE);
                EnableWindow(gLaunch, FALSE);
                SetWindowTextW(gCancelBtn, L"Cancel");
                EnableWindow(gCancelBtn, TRUE);
                int rc = doInstall();
                if (rc == 0) {
                    if (gLaunch && IsDlgButtonChecked(gLaunch, BST_CHECKED)) {
                        ShellExecuteW(nullptr, L"open", installExe().c_str(), nullptr, installDir().c_str(), SW_SHOWNORMAL);
                    }
                    if (gSilent) { PostQuitMessage(0); return 0; }
                    SetWindowTextW(gInstallBtn, L"Finish");
                    SetWindowTextW(gCancelBtn, L"Close");
                    EnableWindow(gInstallBtn, TRUE);
                    EnableWindow(gCancelBtn, TRUE);
                    EnableWindow(gLaunch, TRUE);
                    SetWindowLongPtrW(gInstallBtn, GWLP_USERDATA, 1); // 1001 now means Finish
                } else {
                    SetWindowTextW(gInstallBtn, L"Retry");
                    SetWindowTextW(gCancelBtn, L"Close");
                    EnableWindow(gInstallBtn, TRUE);
                    EnableWindow(gCancelBtn, TRUE);
                }
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    gInst = hInstance;
    const std::wstring cmd = GetCommandLineW();
    gSilent = (cmd.find(L"/S") != std::wstring::npos) || (cmd.find(L"/s") != std::wstring::npos);

    if (cmd.find(L"/UNINSTALL") != std::wstring::npos || cmd.find(L"/uninstall") != std::wstring::npos) {
        const std::wstring exe = installExe();
        ShellExecuteW(nullptr, L"open", exe.c_str(), L"/uninstall", installDir().c_str(), SW_SHOWNORMAL);
        return 0;
    }

    // Only one setup at a time.
    HANDLE one = CreateMutexW(nullptr, TRUE, L"Local\\ChainItSetupSingleInstance");
    if (one && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"ChainIt Setup is already running.", L"ChainIt Setup", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    gIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = gIcon;
    wc.hIconSm = gIcon;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"ChainItSetupWindowClass";
    RegisterClassExW(&wc);

    if (gSilent) {
        return doInstall();
    }

    const int W = 470, H = 300;
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    gWnd = CreateWindowExW(0, wc.lpszClassName, L"ChainIt Setup",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                           (sx - W) / 2, (sy - H) / 2, W, H, nullptr, nullptr, hInstance, nullptr);

    HFONT font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT bold = CreateFontW(-19, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT small = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    HWND iconWnd = CreateWindowExW(0, L"STATIC", nullptr, WS_CHILD | WS_VISIBLE | SS_ICON, 20, 18, 40, 40, gWnd, nullptr, gInst, nullptr);
    SendMessageW(iconWnd, STM_SETICON, (WPARAM)gIcon, 0);

    HWND title = CreateWindowExW(0, L"STATIC", L"ChainIt for Windows", WS_CHILD | WS_VISIBLE, 72, 14, 360, 26, gWnd, nullptr, gInst, nullptr);
    SendMessageW(title, WM_SETFONT, (WPARAM)bold, TRUE);

    HWND sub = CreateWindowExW(0, L"STATIC",
        (std::wstring(L"Version ") + wide(CHAINIT_INSTALLER_VERSION) + L"  \u00b7  installs to " + installDir()).c_str(),
        WS_CHILD | WS_VISIBLE, 72, 42, 380, 18, gWnd, nullptr, gInst, nullptr);
    SendMessageW(sub, WM_SETFONT, (WPARAM)small, TRUE);

    HWND note = CreateWindowExW(0, L"STATIC",
        L"ChainIt links your Android phone to Windows as a real Z:\\ drive, with a dual-pane File Manager,\n"
        L"USB / Wi-Fi / hotspot / Bluetooth failover, parallel chunked transfers and SHA-256 integrity checks.\n"
        L"Re-running this setup updates the same file in place - your settings are kept.",
        WS_CHILD | WS_VISIBLE, 20, 78, 430, 62, gWnd, nullptr, gInst, nullptr);
    SendMessageW(note, WM_SETFONT, (WPARAM)font, TRUE);

    gBar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE, 20, 152, 430, 16, gWnd, nullptr, gInst, nullptr);
    SendMessageW(gBar, PBM_SETRANGE32, 0, 100);

    gStatus = CreateWindowExW(0, L"STATIC", L"Ready to install.", WS_CHILD | WS_VISIBLE, 20, 176, 430, 36, gWnd, nullptr, gInst, nullptr);
    SendMessageW(gStatus, WM_SETFONT, (WPARAM)font, TRUE);

    gLaunch = CreateWindowExW(0, L"BUTTON", L"Launch ChainIt when setup finishes", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                              20, 214, 320, 20, gWnd, (HMENU)1003, gInst, nullptr);
    SendMessageW(gLaunch, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(gLaunch, BM_SETCHECK, BST_CHECKED, 0);

    gInstallBtn = CreateWindowExW(0, L"BUTTON", L"Install ChainIt", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                  240, 240, 100, 28, gWnd, (HMENU)1001, gInst, nullptr);
    SendMessageW(gInstallBtn, WM_SETFONT, (WPARAM)font, TRUE);
    gCancelBtn = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE, 350, 240, 100, 28, gWnd, (HMENU)1002, gInst, nullptr);
    SendMessageW(gCancelBtn, WM_SETFONT, (WPARAM)font, TRUE);

    ShowWindow(gWnd, SW_SHOWNORMAL);
    UpdateWindow(gWnd);

    MSG m{};
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (one) CloseHandle(one);
    return 0;
}
