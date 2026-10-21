#include "worker_agent/agent_config.h"
#include "worker_agent/agent_worker.h"
#include "worker_agent/autostart.h"
#include "worker_agent/bounded_log.h"

#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using monitor::worker_agent::AgentConfig;
using monitor::worker_agent::AgentWorkerController;
using monitor::worker_agent::AutostartRegistry;
using monitor::worker_agent::BoundedLog;
using monitor::worker_agent::ConfigLoadStatus;
using monitor::worker_agent::ConfigStore;
using monitor::worker_agent::WorkerState;

constexpr wchar_t kMainClass[] = L"WorkersTracking.WorkerAgent.Window";
constexpr wchar_t kSetupClass[] = L"WorkersTracking.WorkerAgent.Setup";
constexpr wchar_t kMutexName[] = L"Local\\WorkersTracking.WorkerAgent.SingleInstance";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kWorkerStateMessage = WM_APP + 2;
constexpr UINT kActivateMessage = WM_APP + 3;
constexpr UINT kTrayIdentifier = 1;
constexpr UINT kCommandSettings = 100;
constexpr UINT kCommandAutostart = 101;
constexpr UINT kCommandExit = 102;
constexpr int kNameEdit = 200;
constexpr int kAddressEdit = 201;
constexpr int kPortEdit = 202;
constexpr int kAutostartCheck = 203;
constexpr int kSaveButton = 204;
constexpr int kCancelButton = 205;

std::optional<std::string> narrow(const std::wstring_view value) {
    if (value.empty()) {
        return std::string{};
    }
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                             static_cast<int>(value.size()), nullptr, 0, nullptr,
                                             nullptr);
    if (required <= 0) {
        return std::nullopt;
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), required, nullptr,
                            nullptr) != required) {
        return std::nullopt;
    }
    return result;
}

std::wstring widen(const std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                             static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        return L"Недопустимый текст";
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), required) != required) {
        return L"Недопустимый текст";
    }
    return result;
}

std::wstring window_text(const HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring result(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(control, result.data(), length + 1);
    result.resize(static_cast<std::size_t>(copied));
    return result;
}

std::wstring executable_path() {
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size >= path.size()) {
        throw std::runtime_error("Could not determine executable path");
    }
    path.resize(size);
    return path;
}

struct SetupContext {
    AgentConfig working;
    std::optional<AgentConfig> result;
    bool done{};
};

LRESULT CALLBACK setup_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* context =
        reinterpret_cast<SetupContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        context = static_cast<SetupContext*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
    }
    if (message == WM_CREATE && context != nullptr) {
        const HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        auto create_control = [&](const wchar_t* type, const wchar_t* text, DWORD style, int x,
                                  int y, int width, int height, int identifier) {
            HWND control = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, x, y,
                                           width, height, window,
                                           reinterpret_cast<HMENU>(
                                               static_cast<INT_PTR>(identifier)),
                                           GetModuleHandleW(nullptr), nullptr);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            return control;
        };
        create_control(L"STATIC", L"Имя сотрудника:", 0, 20, 20, 130, 22, 0);
        const HWND name = create_control(L"EDIT", widen(context->working.employee_name).c_str(),
                                         WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 160, 18, 260,
                                         24, kNameEdit);
        SendMessageW(name, EM_SETLIMITTEXT, 255, 0);
        create_control(L"STATIC", L"IPv4 адрес Observer:", 0, 20, 58, 130, 22, 0);
        const HWND address =
            create_control(L"EDIT", widen(context->working.observer_ipv4).c_str(),
                           WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 160, 56, 260, 24,
                           kAddressEdit);
        SendMessageW(address, EM_SETLIMITTEXT, 15, 0);
        create_control(L"STATIC", L"Порт:", 0, 20, 96, 130, 22, 0);
        const std::wstring port = std::to_wstring(context->working.port);
        const HWND port_edit =
            create_control(L"EDIT", port.c_str(),
                           WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 160, 94, 100, 24,
                           kPortEdit);
        SendMessageW(port_edit, EM_SETLIMITTEXT, 5, 0);
        const HWND check =
            create_control(L"BUTTON", L"Запускать вместе с Windows",
                           BS_AUTOCHECKBOX | WS_TABSTOP, 20, 132, 300, 24, kAutostartCheck);
        SendMessageW(check, BM_SETCHECK,
                     context->working.autostart ? BST_CHECKED : BST_UNCHECKED, 0);
        create_control(L"STATIC",
                       L"После сохранения мониторинг основного экрана будет активен.",
                       0, 20, 166, 400, 36, 0);
        create_control(L"BUTTON", L"Сохранить", BS_DEFPUSHBUTTON | WS_TABSTOP, 230, 210, 90, 28,
                       kSaveButton);
        create_control(L"BUTTON", L"Отмена", BS_PUSHBUTTON | WS_TABSTOP, 330, 210, 90, 28,
                       kCancelButton);
        SetFocus(name);
        return 0;
    }
    if (message == WM_COMMAND && context != nullptr) {
        const int command = LOWORD(wparam);
        if (command == kSaveButton) {
            AgentConfig candidate = context->working;
            const auto name = narrow(window_text(GetDlgItem(window, kNameEdit)));
            const auto address = narrow(window_text(GetDlgItem(window, kAddressEdit)));
            const std::wstring port_text = window_text(GetDlgItem(window, kPortEdit));
            wchar_t* end = nullptr;
            const unsigned long port = std::wcstoul(port_text.c_str(), &end, 10);
            if (!name || !address || end == port_text.c_str() || *end != L'\0' ||
                port > 65535) {
                MessageBoxW(window, L"Проверьте имя, IPv4 адрес и порт.",
                            L"Workers Tracking", MB_OK | MB_ICONWARNING);
                return 0;
            }
            candidate.employee_name = *name;
            candidate.observer_ipv4 = *address;
            candidate.port = static_cast<std::uint16_t>(port);
            candidate.autostart =
                SendMessageW(GetDlgItem(window, kAutostartCheck), BM_GETCHECK, 0, 0) ==
                BST_CHECKED;
            const std::string validation = monitor::worker_agent::validate_config(candidate);
            if (!validation.empty()) {
                MessageBoxW(window, widen(validation).c_str(), L"Ошибка проверки",
                            MB_OK | MB_ICONWARNING);
                return 0;
            }
            context->result = std::move(candidate);
            context->done = true;
            DestroyWindow(window);
            return 0;
        }
        if (command == kCancelButton) {
            context->done = true;
            DestroyWindow(window);
            return 0;
        }
    }
    if (message == WM_CLOSE && context != nullptr) {
        context->done = true;
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

std::optional<AgentConfig> show_setup(const HWND owner, const AgentConfig& initial) {
    SetupContext context{initial};
    HWND window = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kSetupClass, L"Настройка WorkerAgent",
        WS_CAPTION | WS_SYSMENU | WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT, 460, 290, owner,
        nullptr, GetModuleHandleW(nullptr), &context);
    if (window == nullptr) {
        throw std::runtime_error("Could not create setup window");
    }
    if (owner != nullptr) {
        EnableWindow(owner, FALSE);
    }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    MSG message{};
    while (!context.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (owner != nullptr) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    return context.result;
}

struct WorkerNotice {
    WorkerState state;
    std::string detail;
};

class TrayApplication {
public:
    TrayApplication(HINSTANCE instance, ConfigStore store)
        : instance_(instance),
          store_(std::move(store)),
          log_(store_.path().parent_path() / L"worker_agent.log"),
          worker_(monitor::worker_agent::run_agent_worker) {}

    ~TrayApplication() {
        shutdown();
    }

    void bind_window(const HWND window) noexcept {
        window_ = window;
    }

    int run() {
        window_ = CreateWindowExW(0, kMainClass, L"WorkerAgent", WS_OVERLAPPED, 0, 0, 0, 0,
                                  nullptr, nullptr, instance_, this);
        if (window_ == nullptr) {
            throw std::runtime_error("Could not create WorkerAgent window");
        }
        taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
        const auto loaded = store_.load();
        AgentConfig initial =
            loaded.status == ConfigLoadStatus::Loaded && loaded.config
                ? *loaded.config
                : AgentConfig{monitor::worker_agent::generate_agent_guid(), {}, "127.0.0.1",
                              45873, false};
        initial.autostart = autostart_.is_enabled(executable_path());
        if (loaded.status != ConfigLoadStatus::Loaded) {
            const auto configured = show_setup(window_, initial);
            if (!configured || !apply_configuration(*configured, false)) {
                shutdown();
                return 0;
            }
        } else {
            current_ = initial;
            static_cast<void>(store_.save(*current_));
        }
        add_tray_icon();
        start_worker();
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        shutdown();
        return static_cast<int>(message.wParam);
    }

    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == taskbar_created_) {
            add_tray_icon();
            return 0;
        }
        switch (message) {
            case kTrayMessage:
                on_tray(lparam);
                return 0;
            case kWorkerStateMessage: {
                std::unique_ptr<WorkerNotice> notice(
                    reinterpret_cast<WorkerNotice*>(lparam));
                if (notice) {
                    state_ = notice->state;
                    detail_ = std::move(notice->detail);
                    if (!detail_.empty()) {
                        log_.write(detail_);
                    }
                    update_tray_icon();
                }
                return 0;
            }
            case kActivateMessage:
                show_status();
                return 0;
            case WM_COMMAND:
                on_command(LOWORD(wparam));
                return 0;
            case WM_QUERYENDSESSION:
                worker_.stop();
                return TRUE;
            case WM_ENDSESSION:
                if (wparam != FALSE) {
                    shutdown();
                    PostQuitMessage(0);
                }
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            default:
                return DefWindowProcW(window_, message, wparam, lparam);
        }
    }

private:
    std::wstring state_text() const {
        switch (state_) {
            case WorkerState::Connected:
                return L"Подключено";
            case WorkerState::Connecting:
                return L"Подключение";
            case WorkerState::CaptureUnavailable:
                return L"Захват временно недоступен";
            case WorkerState::Stopped:
                return L"Остановлено";
            case WorkerState::Disconnected:
                return L"Нет соединения";
        }
        return L"Нет соединения";
    }

    HICON state_icon() const {
        const int identifier =
            state_ == WorkerState::Connected
                ? 32516
                : (state_ == WorkerState::Connecting ? 32512 : 32515);
        return LoadIconW(nullptr, MAKEINTRESOURCEW(identifier));
    }

    void add_tray_icon() {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = kTrayIdentifier;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        data.uCallbackMessage = kTrayMessage;
        data.hIcon = state_icon();
        const std::wstring tooltip =
            L"Workers Tracking — мониторинг экрана активен — " + state_text();
        wcsncpy_s(data.szTip, tooltip.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_ADD, &data);
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
        tray_added_ = true;
    }

    void update_tray_icon() {
        if (!tray_added_) {
            return;
        }
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = kTrayIdentifier;
        data.uFlags = NIF_ICON | NIF_TIP;
        data.hIcon = state_icon();
        const std::wstring tooltip =
            L"Workers Tracking — мониторинг экрана активен — " + state_text();
        wcsncpy_s(data.szTip, tooltip.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }

    void remove_tray_icon() {
        if (!tray_added_) {
            return;
        }
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = kTrayIdentifier;
        Shell_NotifyIconW(NIM_DELETE, &data);
        tray_added_ = false;
    }

    void on_tray(const LPARAM event) {
        if (LOWORD(event) == WM_LBUTTONUP || LOWORD(event) == WM_LBUTTONDBLCLK) {
            show_status();
        } else if (LOWORD(event) == WM_RBUTTONUP || LOWORD(event) == WM_CONTEXTMENU) {
            show_menu();
        }
    }

    void show_status() const {
        std::wstring text = L"Мониторинг основного экрана активен.\nСостояние: " + state_text();
        if (!detail_.empty()) {
            text += L"\nПоследняя ошибка: " + widen(detail_);
        }
        MessageBoxW(window_, text.c_str(), L"Workers Tracking", MB_OK | MB_ICONINFORMATION);
    }

    void show_menu() {
        POINT cursor{};
        GetCursorPos(&cursor);
        HMENU menu = CreatePopupMenu();
        const std::wstring summary = L"Мониторинг активен — " + state_text();
        AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, summary.c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCommandSettings, L"Настройки…");
        AppendMenuW(menu,
                    MF_STRING | (current_ && current_->autostart ? MF_CHECKED : MF_UNCHECKED),
                    kCommandAutostart, L"Запускать с Windows");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCommandExit, L"Выход");
        SetForegroundWindow(window_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, window_, nullptr);
        DestroyMenu(menu);
    }

    void on_command(const UINT command) {
        if (command == kCommandSettings && current_) {
            const auto configured = show_setup(window_, *current_);
            if (configured) {
                static_cast<void>(apply_configuration(*configured, true));
            }
        } else if (command == kCommandAutostart && current_) {
            AgentConfig updated = *current_;
            updated.autostart = !autostart_.is_enabled(executable_path());
            static_cast<void>(apply_configuration(updated, false));
        } else if (command == kCommandExit) {
            shutdown();
            DestroyWindow(window_);
        }
    }

    bool apply_configuration(AgentConfig configured, const bool restart) {
        if (current_ && configured.agent_guid != current_->agent_guid) {
            configured.agent_guid = current_->agent_guid;
        }
        const std::wstring executable = executable_path();
        const bool previous_autostart = autostart_.is_enabled(executable);
        const std::string registry_error =
            autostart_.set_enabled(configured.autostart, executable);
        if (!registry_error.empty()) {
            if (current_) {
                current_->autostart = autostart_.is_enabled(executable);
                static_cast<void>(store_.save(*current_));
            }
            MessageBoxW(window_, widen(registry_error).c_str(), L"Ошибка автозапуска",
                        MB_OK | MB_ICONERROR);
            return false;
        }
        configured.autostart = autostart_.is_enabled(executable);
        const std::string save_error = store_.save(configured);
        if (!save_error.empty()) {
            const std::string rollback_error =
                autostart_.set_enabled(previous_autostart, executable);
            if (!rollback_error.empty()) {
                log_.write(rollback_error);
            }
            if (current_) {
                current_->autostart = autostart_.is_enabled(executable);
                static_cast<void>(store_.save(*current_));
            }
            MessageBoxW(window_, widen(save_error).c_str(), L"Ошибка сохранения",
                        MB_OK | MB_ICONERROR);
            return false;
        }
        current_ = std::move(configured);
        if (restart) {
            worker_.apply(*current_);
        }
        return true;
    }

    void start_worker() {
        const HWND target = window_;
        worker_.start(*current_, [target](const WorkerState state, const std::string& detail) {
            auto notice = std::make_unique<WorkerNotice>(WorkerNotice{state, detail});
            if (!PostMessageW(target, kWorkerStateMessage, 0,
                              reinterpret_cast<LPARAM>(notice.get()))) {
                return;
            }
            static_cast<void>(notice.release());
        });
    }

    void shutdown() {
        if (shutting_down_) {
            return;
        }
        shutting_down_ = true;
        worker_.stop();
        remove_tray_icon();
    }

    HINSTANCE instance_{};
    HWND window_{};
    UINT taskbar_created_{};
    ConfigStore store_;
    AutostartRegistry autostart_;
    BoundedLog log_;
    AgentWorkerController worker_;
    std::optional<AgentConfig> current_;
    WorkerState state_{WorkerState::Connecting};
    std::string detail_;
    bool tray_added_{};
    bool shutting_down_{};
};

LRESULT CALLBACK main_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* application =
        reinterpret_cast<TrayApplication*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        application = static_cast<TrayApplication*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(application));
        application->bind_window(window);
    }
    return application != nullptr ? application->handle(message, wparam, lparam)
                                  : DefWindowProcW(window, message, wparam, lparam);
}

void register_window_classes(const HINSTANCE instance) {
    WNDCLASSEXW main_class{sizeof(main_class)};
    main_class.lpfnWndProc = main_window_proc;
    main_class.hInstance = instance;
    main_class.lpszClassName = kMainClass;
    main_class.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    main_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    if (RegisterClassExW(&main_class) == 0) {
        throw std::runtime_error("Could not register WorkerAgent window");
    }
    WNDCLASSEXW setup_class{sizeof(setup_class)};
    setup_class.lpfnWndProc = setup_window_proc;
    setup_class.hInstance = instance;
    setup_class.lpszClassName = kSetupClass;
    setup_class.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    setup_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    setup_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (RegisterClassExW(&setup_class) == 0) {
        throw std::runtime_error("Could not register setup window");
    }
}

}  // namespace

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex == nullptr) {
        MessageBoxW(nullptr, L"Не удалось проверить запущенный экземпляр.",
                    L"Workers Tracking", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        const HWND existing = FindWindowW(kMainClass, nullptr);
        if (existing != nullptr) {
            PostMessageW(existing, kActivateMessage, 0, 0);
        } else {
            MessageBoxW(nullptr, L"WorkerAgent уже запущен.", L"Workers Tracking",
                        MB_OK | MB_ICONINFORMATION);
        }
        CloseHandle(mutex);
        return 0;
    }
    int result = 1;
    try {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);
        register_window_classes(instance);
        TrayApplication application(instance, ConfigStore::for_current_user());
        result = application.run();
    } catch (const std::exception& error) {
        MessageBoxW(nullptr, widen(error.what()).c_str(), L"Workers Tracking",
                    MB_OK | MB_ICONERROR);
    }
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return result;
}
