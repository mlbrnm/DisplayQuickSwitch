#include "backend.h"
#include "config.h"
#include "resource.h"
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <algorithm>
#include <fstream>
#include <thread>

namespace {
constexpr UINT TrayMessage = WM_APP + 1, WorkDone = WM_APP + 2;
constexpr UINT Manage = 200, Restore = 201, Diagnose = 202, Startup = 203, Quit = 204, PresetBase = 10000;
HINSTANCE instance{};
std::wstring widen(const char* s) {
    auto n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (!n) return L"Unknown error";
    std::wstring text(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, text.data(), n);
    text.pop_back(); return text;
}
void error(HWND owner, const std::exception& e) {
    MessageBoxW(owner, widen(e.what()).c_str(), L"Nvidia Quick Switch", MB_OK | MB_ICONERROR);
    try {
        auto path = qs::configPath().parent_path();
        std::filesystem::create_directories(path);
        auto log = path / L"errors.log";
        // Bounded error logging; no idle activity and no settings/profile dumps.
        if (std::filesystem::exists(log) && std::filesystem::file_size(log) > 256 * 1024) return;
        std::ofstream file(log, std::ios::app);
        SYSTEMTIME now{}; GetLocalTime(&now);
        file << now.wYear << '-' << now.wMonth << '-' << now.wDay << ' ' << now.wHour << ':' << now.wMinute << ':' << now.wSecond
             << " " << e.what() << "\n";
    } catch (...) {}
}
std::wstring text(HWND dialog, int id) {
    HWND control = GetDlgItem(dialog, id);
    auto length = GetWindowTextLengthW(control);
    std::wstring s(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, s.data(), length + 1); s.resize(length);
    return qs::trim(s);
}
void setText(HWND d, int id, const std::wstring& value) { SetDlgItemTextW(d, id, value.c_str()); }
void centerPrimary(HWND window) {
    RECT work{}, rect{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0); GetWindowRect(window, &rect);
    SetWindowPos(window, HWND_TOP, work.left + ((work.right - work.left) - (rect.right - rect.left)) / 2,
                 work.top + ((work.bottom - work.top) - (rect.bottom - rect.top)) / 2, 0, 0, SWP_NOSIZE);
}
void fillCombo(HWND d, int id, const std::vector<std::pair<const wchar_t*, int>>& items, int selected) {
    auto h = GetDlgItem(d, id); SendMessageW(h, CB_RESETCONTENT, 0, 0);
    for (auto [label, value] : items) {
        auto index = SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        SendMessageW(h, CB_SETITEMDATA, index, value);
        if (value == selected) SendMessageW(h, CB_SETCURSEL, index, 0);
    }
}
int comboValue(HWND d, int id) {
    auto h = GetDlgItem(d, id); auto i = SendMessageW(h, CB_GETCURSEL, 0, 0);
    if (i == CB_ERR) throw std::runtime_error("Select a value from each list.");
    return static_cast<int>(SendMessageW(h, CB_GETITEMDATA, i, 0));
}
struct Editor { qs::Preset preset; std::vector<qs::Monitor> monitors; };
void showFields(HWND d, const qs::Preset& p) {
    setText(d, IDC_NAME, p.name);
    CheckDlgButton(d, IDC_PRIMARY, p.primary ? BST_CHECKED : BST_UNCHECKED);
    setText(d, IDC_WIDTH, p.width ? std::to_wstring(p.width) : L"");
    setText(d, IDC_HEIGHT, p.height ? std::to_wstring(p.height) : L"");
    setText(d, IDC_REFRESH, qs::formatRefresh(p.refreshMilliHz));
    setText(d, IDC_FPS, p.maxFps >= 0 ? std::to_wstring(p.maxFps) : L"");
    fillCombo(d, IDC_SCALING, {{L"Leave unchanged", 0}, {L"No scaling (GPU)", 3}, {L"Aspect ratio (GPU)", 5},
                             {L"Full-screen / stretch (GPU)", 2}, {L"Integer scaling (GPU)", 8}}, static_cast<int>(p.scaling));
    fillCombo(d, IDC_GSYNC, {{L"Leave unchanged", -1}, {L"Off", 0}, {L"On — full-screen only", 1},
                           {L"On — full-screen and windowed", 2}}, static_cast<int>(p.gsync));
    fillCombo(d, IDC_VSYNC, {{L"Leave unchanged", -1}, {L"Application controlled", 0}, {L"Off", 1}, {L"On", 2}}, static_cast<int>(p.vsync));
}
INT_PTR CALLBACK editorProc(HWND d, UINT msg, WPARAM w, LPARAM l) {
    auto e = reinterpret_cast<Editor*>(GetWindowLongPtrW(d, DWLP_USER));
    if (msg == WM_INITDIALOG) {
        e = reinterpret_cast<Editor*>(l); SetWindowLongPtrW(d, DWLP_USER, l);
        for (size_t i = 0; i < e->monitors.size(); ++i) {
            const auto& m = e->monitors[i];
            auto label = m.name + L" (" + m.gdiName + L")" + (m.primary ? L" — primary" : L"");
            SendDlgItemMessageW(d, IDC_MONITOR, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            if (_wcsicmp(m.key.c_str(), e->preset.monitor.c_str()) == 0)
                SendDlgItemMessageW(d, IDC_MONITOR, CB_SETCURSEL, i, 0);
        }
        SendDlgItemMessageW(d, IDC_NAME, EM_SETLIMITTEXT, 100, 0);
        SendDlgItemMessageW(d, IDC_WIDTH, EM_SETLIMITTEXT, 5, 0);
        SendDlgItemMessageW(d, IDC_HEIGHT, EM_SETLIMITTEXT, 5, 0);
        SendDlgItemMessageW(d, IDC_REFRESH, EM_SETLIMITTEXT, 8, 0);
        SendDlgItemMessageW(d, IDC_FPS, EM_SETLIMITTEXT, 4, 0);
        showFields(d, e->preset); centerPrimary(d); return TRUE;
    }
    if (msg == WM_COMMAND && e) {
        auto id = LOWORD(w);
        if (id == IDCANCEL) { EndDialog(d, IDCANCEL); return TRUE; }
        if (id == IDOK || id == IDC_CAPTURE) {
            try {
                auto index = SendDlgItemMessageW(d, IDC_MONITOR, CB_GETCURSEL, 0, 0);
                if (index < 0 || static_cast<size_t>(index) >= e->monitors.size())
                    throw std::runtime_error("Select an active target monitor. Reopen the editor if displays have changed.");
                if (id == IDC_CAPTURE) {
                    auto p = qs::capture(e->monitors[index]);
                    p.name = text(d, IDC_NAME); p.primary = IsDlgButtonChecked(d, IDC_PRIMARY) == BST_CHECKED;
                    showFields(d, p); return TRUE;
                }
                auto p = e->preset;
                p.name = text(d, IDC_NAME); p.monitor = e->monitors[index].key;
                p.primary = IsDlgButtonChecked(d, IDC_PRIMARY) == BST_CHECKED;
                auto width = text(d, IDC_WIDTH), height = text(d, IDC_HEIGHT), fps = text(d, IDC_FPS);
                p.width = width.empty() ? 0 : qs::parseUnsigned(width, 32768);
                p.height = height.empty() ? 0 : qs::parseUnsigned(height, 32768);
                p.refreshMilliHz = qs::parseRefresh(text(d, IDC_REFRESH));
                p.scaling = static_cast<qs::Scaling>(comboValue(d, IDC_SCALING));
                p.gsync = static_cast<qs::Gsync>(comboValue(d, IDC_GSYNC));
                p.vsync = static_cast<qs::Vsync>(comboValue(d, IDC_VSYNC));
                p.maxFps = fps.empty() ? -1 : static_cast<int>(qs::parseUnsigned(fps, 1023));
                qs::validate(p); e->preset = std::move(p); EndDialog(d, IDOK);
            } catch (const std::exception& ex) { error(d, ex); }
            return TRUE;
        }
    }
    return FALSE;
}
bool editPreset(HWND owner, qs::Preset& p) {
    Editor e{p, qs::monitors()};
    if (e.monitors.empty()) throw std::runtime_error("No active monitors found.");
    if (p.monitor.empty()) {
        auto m = std::find_if(e.monitors.begin(), e.monitors.end(), [](auto& x) { return x.primary; });
        if (m == e.monitors.end()) m = e.monitors.begin();
        e.preset.monitor = m->key;
        e.preset.name = L"New preset";
    }
    auto result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_EDITOR), owner, editorProc, reinterpret_cast<LPARAM>(&e));
    if (result == -1) throw std::runtime_error("Cannot create preset editor.");
    if (result != IDOK) return false;
    p = std::move(e.preset); return true;
}
struct Manager { std::vector<qs::Preset>* presets; std::filesystem::path file; };
void refreshList(HWND d, const Manager& m, size_t selected = 0) {
    auto list = GetDlgItem(d, IDC_PRESETS); SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (auto& p : *m.presets) SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.name.c_str()));
    if (!m.presets->empty()) SendMessageW(list, LB_SETCURSEL, std::min(selected, m.presets->size() - 1), 0);
    EnableWindow(GetDlgItem(d, IDC_EDIT), !m.presets->empty()); EnableWindow(GetDlgItem(d, IDC_DELETE), !m.presets->empty());
}
INT_PTR CALLBACK managerProc(HWND d, UINT msg, WPARAM w, LPARAM l) {
    auto m = reinterpret_cast<Manager*>(GetWindowLongPtrW(d, DWLP_USER));
    if (msg == WM_INITDIALOG) { SetWindowLongPtrW(d, DWLP_USER, l); refreshList(d, *reinterpret_cast<Manager*>(l)); centerPrimary(d); return TRUE; }
    if (msg == WM_COMMAND && m) {
        auto id = LOWORD(w);
        if (id == IDCANCEL) { EndDialog(d, IDCANCEL); return TRUE; }
        if (id == IDC_PRESETS && HIWORD(w) == LBN_DBLCLK) id = IDC_EDIT;
        if (id == IDC_NEW || id == IDC_EDIT || id == IDC_DELETE) {
            try {
                auto next = *m->presets;
                auto index = SendDlgItemMessageW(d, IDC_PRESETS, LB_GETCURSEL, 0, 0);
                if (id != IDC_NEW && (index < 0 || static_cast<size_t>(index) >= next.size())) return TRUE;
                if (id == IDC_DELETE) {
                    if (MessageBoxW(d, L"Delete this preset?", L"Nvidia Quick Switch", MB_YESNO | MB_ICONQUESTION) != IDYES) return TRUE;
                    next.erase(next.begin() + index);
                } else {
                    auto p = id == IDC_NEW ? qs::Preset{} : next[index];
                    if (!editPreset(d, p)) return TRUE;
                    for (size_t i = 0; i < next.size(); ++i)
                        if ((id == IDC_NEW || i != static_cast<size_t>(index)) && _wcsicmp(next[i].name.c_str(), p.name.c_str()) == 0)
                            throw std::runtime_error("Preset names must be unique.");
                    if (id == IDC_NEW) { index = static_cast<LRESULT>(next.size()); next.push_back(std::move(p)); }
                    else next[index] = std::move(p);
                }
                qs::savePresets(m->file, next); *m->presets = std::move(next);
                refreshList(d, *m, static_cast<size_t>(std::max<LRESULT>(0, index)));
            } catch (const std::exception& ex) { error(d, ex); }
            return TRUE;
        }
    }
    return FALSE;
}
INT_PTR CALLBACK diagnosticsProc(HWND d, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_INITDIALOG) {
        SendDlgItemMessageW(d, IDC_TEXT, EM_SETLIMITTEXT, 1024 * 1024, 0);
        SetDlgItemTextW(d, IDC_TEXT, reinterpret_cast<const wchar_t*>(l)); centerPrimary(d); return TRUE;
    }
    if (msg == WM_COMMAND && LOWORD(w) == IDCANCEL) { EndDialog(d, IDCANCEL); return TRUE; }
    return FALSE;
}
std::wstring executable() {
    std::wstring result(32768, L'\0');
    auto size = GetModuleFileNameW(nullptr, result.data(), static_cast<DWORD>(result.size()));
    if (!size || size >= result.size()) throw std::runtime_error("Cannot locate the executable.");
    result.resize(size); return result;
}
bool startupEnabled() {
    wchar_t value[32768]{}; DWORD size = sizeof(value);
    auto result = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"NvidiaQuickSwitch",
                              RRF_RT_REG_SZ, nullptr, value, &size);
    return result == ERROR_SUCCESS && std::wstring(value) == L"\"" + executable() + L"\"";
}
void toggleStartup() {
    bool enabled = startupEnabled();
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot edit the current user's startup entry.");
    auto value = L"\"" + executable() + L"\"";
    auto result = enabled ? RegDeleteValueW(key, L"NvidiaQuickSwitch") :
        RegSetValueExW(key, L"NvidiaQuickSwitch", 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * 2));
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) throw std::runtime_error("Cannot update the startup entry.");
}
struct Result { std::unique_ptr<qs::Transaction> transaction; std::string error; bool restoring = false; std::wstring label; };
struct App {
    HWND window{}, confirm{};
    NOTIFYICONDATAW icon{};
    std::vector<qs::Preset> presets;
    std::filesystem::path file;
    std::unique_ptr<qs::Transaction> undo;
    std::thread worker;
    bool busy = false, managing = false;
    ULONGLONG deadline = 0;
    std::wstring last;
    ~App() { if (worker.joinable()) worker.join(); }
    void tooltip(const std::wstring& s) {
        wcsncpy_s(icon.szTip, s.c_str(), _TRUNCATE); icon.uFlags = NIF_TIP;
        Shell_NotifyIconW(NIM_MODIFY, &icon);
    }
    void addIcon() {
        icon = {}; icon.cbSize = sizeof(icon); icon.hWnd = window; icon.uID = 1;
        icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; icon.uCallbackMessage = TrayMessage;
        icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(icon.szTip, L"Nvidia Quick Switch");
        if (!Shell_NotifyIconW(NIM_ADD, &icon)) throw std::runtime_error("Cannot add the tray icon.");
        icon.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &icon);
    }
    void menu() {
        auto menu = CreatePopupMenu();
        const bool blocked = busy || managing;
        if (presets.empty()) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No presets — choose Manage presets");
        for (size_t i = 0; i < presets.size(); ++i) {
            auto label = presets[i].name;
            // Escape menu accelerator markers in user-supplied names.
            for (size_t p = 0; (p = label.find(L'&', p)) != std::wstring::npos; p += 2) label.insert(p, 1, L'&');
            AppendMenuW(menu, MF_STRING | (blocked ? MF_GRAYED : 0), PresetBase + i, label.c_str());
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (blocked || !undo ? MF_GRAYED : 0), Restore, L"Restore previous settings");
        AppendMenuW(menu, MF_STRING | (blocked ? MF_GRAYED : 0), Manage, L"Manage presets…");
        AppendMenuW(menu, MF_STRING | (blocked ? MF_GRAYED : 0), Diagnose, L"Read-only diagnostics…");
        AppendMenuW(menu, MF_STRING | (startupEnabled() ? MF_CHECKED : 0) | (blocked ? MF_GRAYED : 0), Startup, L"Start with Windows");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (blocked ? MF_GRAYED : 0), Quit, L"Exit");
        POINT point{}; GetCursorPos(&point); SetForegroundWindow(window);
        auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr);
        DestroyMenu(menu); PostMessageW(window, WM_NULL, 0, 0);
        if (command) PostMessageW(window, WM_COMMAND, command, 0);
    }
    void apply(size_t index) {
        if (busy || managing || index >= presets.size()) return;
        auto p = presets[index]; busy = true; tooltip(L"Nvidia Quick Switch — applying…");
        worker = std::thread([this, p] {
            auto r = std::make_unique<Result>(); r->transaction = std::make_unique<qs::Transaction>(); r->label = p.name;
            try { r->transaction->apply(p); } catch (const std::exception& e) { r->error = e.what(); }
            PostMessageW(window, WorkDone, 0, reinterpret_cast<LPARAM>(r.release()));
        });
    }
    void restore() {
        if (!undo || worker.joinable()) return;
        if (confirm) { DestroyWindow(confirm); confirm = nullptr; }
        busy = true; tooltip(L"Nvidia Quick Switch — restoring…");
        worker = std::thread([this, tx = std::move(undo)]() mutable {
            auto r = std::make_unique<Result>(); r->transaction = std::move(tx); r->restoring = true;
            try { r->transaction->restore(); } catch (const std::exception& e) { r->error = e.what(); }
            PostMessageW(window, WorkDone, 0, reinterpret_cast<LPARAM>(r.release()));
        });
    }
};
INT_PTR CALLBACK confirmProc(HWND d, UINT msg, WPARAM w, LPARAM l) {
    auto a = reinterpret_cast<App*>(GetWindowLongPtrW(d, DWLP_USER));
    if (msg == WM_INITDIALOG) {
        a = reinterpret_cast<App*>(l); SetWindowLongPtrW(d, DWLP_USER, l);
        a->confirm = d; a->deadline = GetTickCount64() + 20000;
        if (!SetTimer(d, 1, 200, nullptr)) PostMessageW(d, WM_COMMAND, IDCANCEL, 0);
        centerPrimary(d); SetForegroundWindow(d); PostMessageW(d, WM_TIMER, 1, 0); return TRUE;
    }
    if (!a) return FALSE;
    if (msg == WM_TIMER) {
        auto now = GetTickCount64();
        if (now >= a->deadline) { KillTimer(d, 1); a->restore(); return TRUE; }
        auto remaining = (a->deadline - now + 999) / 1000;
        setText(d, IDC_STATUS, L"Applied: " + a->last + L"\r\nReverting in " + std::to_wstring(remaining) + L" seconds unless you choose Keep.");
        return TRUE;
    }
    if (msg == WM_CLOSE || (msg == WM_COMMAND && LOWORD(w) == IDCANCEL)) { KillTimer(d, 1); a->restore(); return TRUE; }
    if (msg == WM_COMMAND && LOWORD(w) == IDOK) {
        KillTimer(d, 1); a->busy = false; a->confirm = nullptr; DestroyWindow(d);
        a->tooltip(L"Nvidia Quick Switch — last applied: " + a->last); return TRUE;
    }
    return FALSE;
}
LRESULT CALLBACK windowProc(HWND window, UINT msg, WPARAM w, LPARAM l) {
    auto a = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        a = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(a)); a->window = window;
    }
    if (!a) return DefWindowProcW(window, msg, w, l);
    try {
        static const UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        if (msg == taskbarCreated) { a->addIcon(); return 0; }
        if (msg == TrayMessage) {
            auto event = LOWORD(l);
            if (event == WM_CONTEXTMENU || event == NIN_SELECT || event == NIN_KEYSELECT) a->menu();
            return 0;
        }
        if (msg == WorkDone) {
            std::unique_ptr<Result> r(reinterpret_cast<Result*>(l));
            if (a->worker.joinable()) a->worker.join();
            if (!r->error.empty()) {
                if (r->transaction->needsRestore()) a->undo = std::move(r->transaction);
                a->busy = false; a->tooltip(L"Nvidia Quick Switch — operation failed");
                error(window, std::runtime_error(r->error)); return 0;
            }
            if (r->restoring) { a->busy = false; a->last.clear(); a->tooltip(L"Nvidia Quick Switch — previous settings restored"); return 0; }
            a->undo = std::move(r->transaction); a->last = r->label;
            if (!CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_CONFIRM), window, confirmProc, reinterpret_cast<LPARAM>(a))) {
                a->restore(); throw std::runtime_error("Cannot create the confirmation dialog; restoring previous settings.");
            }
            ShowWindow(a->confirm, SW_SHOW); return 0;
        }
        if (msg == WM_COMMAND && !a->busy && !a->managing) {
            auto command = LOWORD(w);
            if (command >= PresetBase && command < PresetBase + a->presets.size()) { a->apply(command - PresetBase); return 0; }
            if (command == Restore) { a->restore(); return 0; }
            if (command == Manage) {
                a->managing = true;
                Manager manager{&a->presets, a->file};
                DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_MANAGER), window, managerProc, reinterpret_cast<LPARAM>(&manager));
                a->managing = false; return 0;
            }
            if (command == Diagnose) {
                a->managing = true;
                try {
                    auto report = qs::diagnostics();
                    DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_DIAGNOSTICS), window, diagnosticsProc, reinterpret_cast<LPARAM>(report.c_str()));
                } catch (...) { a->managing = false; throw; }
                a->managing = false; return 0;
            }
            if (command == Startup) { toggleStartup(); return 0; }
            if (command == Quit) { DestroyWindow(window); return 0; }
        }
        if (msg == WM_CLOSE) { if (!a->busy && !a->managing) DestroyWindow(window); return 0; }
        if (msg == WM_QUERYENDSESSION) return TRUE; // Shutdown cannot guarantee driver rollback; see README.
        if (msg == WM_DESTROY) { Shell_NotifyIconW(NIM_DELETE, &a->icon); PostQuitMessage(0); return 0; }
    } catch (const std::exception& ex) { error(window, ex); }
    return DefWindowProcW(window, msg, w, l);
}
}
int WINAPI wWinMain(HINSTANCE h, HINSTANCE, PWSTR, int) {
    instance = h;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\NvidiaQuickSwitch.Tray.v1");
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Nvidia Quick Switch is already running. Look for it in the tray overflow menu.", L"Nvidia Quick Switch", MB_OK);
        CloseHandle(mutex); return 0;
    }
    int exitCode = 0;
    try {
        App app; app.file = qs::configPath(); app.presets = qs::loadPresets(app.file);
        WNDCLASSEXW cls{}; cls.cbSize = sizeof(cls); cls.hInstance = h; cls.lpfnWndProc = windowProc;
        cls.lpszClassName = L"NvidiaQuickSwitch.Tray"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&cls)) throw std::runtime_error("Cannot register the tray window.");
        // Hidden top-level window, not a message-only window: receives Explorer restart broadcasts.
        auto window = CreateWindowExW(0, cls.lpszClassName, L"Nvidia Quick Switch", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, h, &app);
        if (!window) throw std::runtime_error("Cannot create the tray window.");
        app.addIcon();
        // Startup never changes any monitor/driver state or automatically applies a preset.
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (app.confirm && IsDialogMessageW(app.confirm, &message)) continue;
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    } catch (const std::exception& ex) { error(nullptr, ex); exitCode = 1; }
    CloseHandle(mutex); return exitCode;
}
