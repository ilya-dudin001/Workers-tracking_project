#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include "common/winsock_socket.h"
#include "observer/agent_registry.h"
#include "observer/iocp_server.h"
#include "observer/name_store.h"
#include "observer/observer_window.h"

#include <cstdint>
#include <exception>
#include <string>

namespace {

monitor::observer::IocpServer::Config parse_config() {
    monitor::observer::IocpServer::Config config;
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == nullptr) return config;
    for (int index = 1; index + 1 < count; ++index) {
        const std::wstring option(arguments[index]);
        if (option == L"--port") {
            wchar_t* end = nullptr;
            const unsigned long value = wcstoul(arguments[index + 1], &end, 10);
            if (end != arguments[index + 1] && *end == L'\0' &&
                value > 0 && value <= 65535) {
                config.port = static_cast<std::uint16_t>(value);
            }
            ++index;
        } else if (option == L"--bind") {
            const int size = WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, arguments[index + 1], -1,
                nullptr, 0, nullptr, nullptr);
            if (size > 1) {
                std::string value(static_cast<std::size_t>(size), '\0');
                WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                    arguments[index + 1], -1, value.data(), size,
                    nullptr, nullptr);
                value.pop_back();
                config.bind_address = std::move(value);
            }
            ++index;
        }
    }
    LocalFree(arguments);
    return config;
}

}

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE,
                    _In_ PWSTR, _In_ int show_command) {
    static_cast<void>(SetProcessDpiAwarenessContext(
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct ComGuard {
        HRESULT result;
        ~ComGuard() {
            if (SUCCEEDED(result)) CoUninitialize();
        }
    };
    const ComGuard com_guard{com_result};
    static_cast<void>(com_guard);
    try {
        monitor::net::WinsockRuntime winsock;
        monitor::observer::AgentRegistry registry;
        auto names = monitor::observer::NameStore::local_app_data();
        for (const auto& [guid, name] : names.load()) {
            registry.set_local_name(guid, name);
        }
        monitor::observer::IocpServer server(registry, parse_config());
        if (!server.start()) {
            const std::string error = server.last_error();
            const std::wstring message =
                L"Observer TCP server could not start:\n" +
                std::wstring(error.begin(), error.end());
            MessageBoxW(nullptr, message.c_str(), L"Observer startup error",
                        MB_OK | MB_ICONERROR);
            return 1;
        }
        monitor::observer::ObserverWindow window(
            instance, registry, server, names);
        const int result = window.run(show_command);
        server.stop();
        return result;
    } catch (const std::exception& error) {
        const std::string text = error.what();
        const std::wstring message(text.begin(), text.end());
        MessageBoxW(nullptr, message.c_str(), L"Observer fatal error",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
}
