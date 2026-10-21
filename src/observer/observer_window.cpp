#include "observer/observer_window.h"

#define NOMINMAX
#include <d2d1.h>
#include <dwrite.h>
#include <windowsx.h>
#include <wrl/client.h>

#include "observer/jpeg_decoder.h"
#include "observer/layout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace monitor::observer {
namespace {

using Microsoft::WRL::ComPtr;

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    static_cast<void>(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), result.data(), size));
    return result;
}

std::optional<std::string> narrow(const std::wstring& text) {
    if (text.empty()) return std::nullopt;
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0 || size > 255) return std::nullopt;
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), size, nullptr, nullptr) <= 0) {
        return std::nullopt;
    }
    return result;
}

class RenamePrompt {
public:
    static std::optional<std::wstring> show(HWND parent,
                                            const std::wstring& current) {
        static bool registered = false;
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        if (!registered) {
            WNDCLASSW window_class{};
            window_class.lpfnWndProc = window_proc;
            window_class.hInstance = instance;
            window_class.hCursor =
                LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
            window_class.hbrBackground =
                reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            window_class.lpszClassName = L"WorkersTrackingRenamePrompt";
            if (RegisterClassW(&window_class) == 0 &&
                GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                return std::nullopt;
            }
            registered = true;
        }
        RenamePrompt prompt;
        prompt.parent_ = parent;
        prompt.initial_ = current;
        RECT parent_rect{};
        GetWindowRect(parent, &parent_rect);
        const int x = parent_rect.left + static_cast<int>(
            std::max(0L, (parent_rect.right - parent_rect.left - 380L) / 2L));
        const int y = parent_rect.top + static_cast<int>(
            std::max(0L, (parent_rect.bottom - parent_rect.top - 145L) / 2L));
        prompt.window_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME, L"WorkersTrackingRenamePrompt",
            L"Rename employee", WS_CAPTION | WS_SYSMENU | WS_POPUP,
            x, y, 380, 145, parent, nullptr, instance, &prompt);
        if (prompt.window_ == nullptr) return std::nullopt;
        EnableWindow(parent, FALSE);
        ShowWindow(prompt.window_, SW_SHOW);
        MSG message{};
        while (!prompt.done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(prompt.window_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        EnableWindow(parent, TRUE);
        SetForegroundWindow(parent);
        return prompt.result_;
    }

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                        WPARAM wparam, LPARAM lparam) {
        RenamePrompt* self = reinterpret_cast<RenamePrompt*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create =
                reinterpret_cast<const CREATESTRUCTW*>(lparam);
            self = static_cast<RenamePrompt*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        }
        if (self == nullptr) return DefWindowProcW(window, message, wparam, lparam);
        switch (message) {
            case WM_CREATE:
                self->edit_ = CreateWindowExW(
                    WS_EX_CLIENTEDGE, L"EDIT", self->initial_.c_str(),
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                    12, 14, 340, 25, window,
                    reinterpret_cast<HMENU>(100), GetModuleHandleW(nullptr), nullptr);
                static_cast<void>(SendMessageW(self->edit_, EM_SETLIMITTEXT, 255, 0));
                CreateWindowW(L"BUTTON", L"OK",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                    188, 55, 78, 28, window, reinterpret_cast<HMENU>(IDOK),
                    GetModuleHandleW(nullptr), nullptr);
                CreateWindowW(L"BUTTON", L"Cancel",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    274, 55, 78, 28, window, reinterpret_cast<HMENU>(IDCANCEL),
                    GetModuleHandleW(nullptr), nullptr);
                SetFocus(self->edit_);
                static_cast<void>(SendMessageW(self->edit_, EM_SETSEL, 0, -1));
                return 0;
            case WM_COMMAND:
                if (LOWORD(wparam) == IDOK) {
                    const int length = GetWindowTextLengthW(self->edit_);
                    std::wstring value(static_cast<std::size_t>(length) + 1U, L'\0');
                    GetWindowTextW(self->edit_, value.data(), length + 1);
                    value.resize(static_cast<std::size_t>(length));
                    if (!value.empty()) {
                        self->result_ = std::move(value);
                        DestroyWindow(window);
                    } else {
                        MessageBeep(MB_ICONWARNING);
                    }
                    return 0;
                }
                if (LOWORD(wparam) == IDCANCEL) {
                    DestroyWindow(window);
                    return 0;
                }
                break;
            case WM_CLOSE:
                DestroyWindow(window);
                return 0;
            case WM_DESTROY:
                self->done_ = true;
                return 0;
            default:
                break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    HWND parent_ = nullptr;
    HWND window_ = nullptr;
    HWND edit_ = nullptr;
    std::wstring initial_;
    std::optional<std::wstring> result_;
    bool done_ = false;
};

D2D1_RECT_F to_d2d(const Rect& rect) {
    return D2D1::RectF(static_cast<float>(rect.left),
                       static_cast<float>(rect.top),
                       static_cast<float>(rect.right),
                       static_cast<float>(rect.bottom));
}

}

class ObserverWindow::Impl {
public:
    Impl(HINSTANCE instance, AgentRegistry& registry, ProfileSink& sink,
         NameStore& names)
        : instance_(instance), registry_(registry), router_(sink), names_(names) {}

    int run(const int show_command) {
        WNDCLASSEXW window_class{sizeof(window_class)};
        window_class.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = instance_;
        window_class.hCursor =
            LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = L"WorkersTrackingObserver";
        if (RegisterClassExW(&window_class) == 0 &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return 1;
        }
        window_ = CreateWindowExW(
            0, window_class.lpszClassName, L"Workers Tracking Observer",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
            nullptr, nullptr, instance_, this);
        if (window_ == nullptr) return 1;
        ShowWindow(window_, show_command);
        UpdateWindow(window_);
        SetTimer(window_, 1, 33, nullptr);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

private:
    struct CachedBitmap {
        std::uint64_t generation = 0;
        std::uint64_t sequence = 0;
        ComPtr<ID2D1Bitmap> bitmap;
    };

    static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                        WPARAM wparam, LPARAM lparam) {
        Impl* self = reinterpret_cast<Impl*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create =
                reinterpret_cast<const CREATESTRUCTW*>(lparam);
            self = static_cast<Impl*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        }
        return self == nullptr ? DefWindowProcW(window, message, wparam, lparam)
                               : self->handle_message(message, wparam, lparam);
    }

    LRESULT handle_message(const UINT message, const WPARAM wparam,
                           const LPARAM lparam) {
        switch (message) {
            case WM_TIMER:
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            case WM_PAINT:
                paint();
                return 0;
            case WM_SIZE:
                if (target_) {
                    static_cast<void>(target_->Resize(
                        D2D1::SizeU(LOWORD(lparam), HIWORD(lparam))));
                }
                return 0;
            case WM_DPICHANGED: {
                const auto* suggested = reinterpret_cast<const RECT*>(lparam);
                SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                    suggested->right - suggested->left,
                    suggested->bottom - suggested->top,
                    SWP_NOACTIVATE | SWP_NOZORDER);
                return 0;
            }
            case WM_LBUTTONDBLCLK:
                toggle_focus(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
                return 0;
            case WM_RBUTTONUP:
                show_context_menu(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
                return 0;
            case WM_KEYDOWN:
                if (wparam == VK_F11) {
                    toggle_fullscreen();
                    return 0;
                }
                if (wparam == VK_ESCAPE) {
                    if (router_.selection()) {
                        router_.clear();
                    } else if (fullscreen_) {
                        toggle_fullscreen();
                    }
                    return 0;
                }
                break;
            case WM_DESTROY:
                KillTimer(window_, 1);
                PostQuitMessage(0);
                return 0;
            default:
                break;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }

    bool create_device_resources() {
        if (!factory_ && FAILED(D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf()))) {
            return false;
        }
        if (!write_factory_ && FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(write_factory_.GetAddressOf())))) {
            return false;
        }
        if (!text_format_ && FAILED(write_factory_->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            14.0F, L"", &text_format_))) {
            return false;
        }
        if (!target_) {
            RECT client{};
            GetClientRect(window_, &client);
            const D2D1_RENDER_TARGET_PROPERTIES properties =
                D2D1::RenderTargetProperties(
                    D2D1_RENDER_TARGET_TYPE_DEFAULT,
                    D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN,
                                      D2D1_ALPHA_MODE_UNKNOWN),
                    96.0F, 96.0F);
            if (FAILED(factory_->CreateHwndRenderTarget(
                properties,
                D2D1::HwndRenderTargetProperties(
                    window_, D2D1::SizeU(
                        static_cast<UINT32>(std::max(0L, client.right)),
                        static_cast<UINT32>(std::max(0L, client.bottom)))),
                target_.GetAddressOf()))) {
                return false;
            }
            if (FAILED(target_->CreateSolidColorBrush(
                D2D1::ColorF(0xE8E8EA), &text_brush_)) ||
                FAILED(target_->CreateSolidColorBrush(
                    D2D1::ColorF(0x27AE60), &online_brush_)) ||
                FAILED(target_->CreateSolidColorBrush(
                    D2D1::ColorF(0xF2C94C), &stale_brush_)) ||
                FAILED(target_->CreateSolidColorBrush(
                    D2D1::ColorF(0xEB5757), &offline_brush_))) {
                return false;
            }
        }
        return true;
    }

    void discard_device_resources() {
        cache_.clear();
        offline_brush_.Reset();
        stale_brush_.Reset();
        online_brush_.Reset();
        text_brush_.Reset();
        target_.Reset();
    }

    void paint() {
        PAINTSTRUCT paint_structure{};
        BeginPaint(window_, &paint_structure);
        if (!create_device_resources()) {
            EndPaint(window_, &paint_structure);
            return;
        }
        displayed_ = registry_.snapshot(AgentRegistry::Clock::now());
        std::vector<AgentSnapshot> visible;
        if (const auto selected = router_.selection()) {
            const auto found = std::find_if(displayed_.begin(), displayed_.end(),
                [&selected](const AgentSnapshot& item) {
                    return item.guid == *selected;
                });
            if (found != displayed_.end()) visible.push_back(*found);
        } else {
            visible = displayed_;
        }
        RECT client{};
        GetClientRect(window_, &client);
        layouts_ = LayoutEngine::compute(visible.size(),
            {client.left, client.top, client.right, client.bottom});
        target_->BeginDraw();
        target_->Clear(D2D1::ColorF(0x101317));
        std::set<protocol::AgentGuid> live;
        for (std::size_t index = 0; index < visible.size(); ++index) {
            live.insert(visible[index].guid);
            draw_tile(visible[index], layouts_[index]);
        }
        for (auto iterator = cache_.begin(); iterator != cache_.end();) {
            if (!live.contains(iterator->first)) iterator = cache_.erase(iterator);
            else ++iterator;
        }
        const HRESULT result = target_->EndDraw();
        if (result == D2DERR_RECREATE_TARGET) {
            discard_device_resources();
        }
        EndPaint(window_, &paint_structure);
    }

    void draw_tile(const AgentSnapshot& agent, const TileLayout& layout) {
        const auto bounds = to_d2d(layout.bounds);
        target_->FillRectangle(bounds, offline_brush_.Get());
        const int inset = 2;
        const Rect image_area{layout.content.left + inset,
            layout.content.top + inset, layout.content.right - inset,
            layout.content.bottom - inset};
        ID2D1Bitmap* bitmap = bitmap_for(agent);
        if (bitmap != nullptr) {
            const D2D1_SIZE_F source = bitmap->GetSize();
            const float available_width =
                static_cast<float>(std::max(0, image_area.right - image_area.left));
            const float available_height =
                static_cast<float>(std::max(0, image_area.bottom - image_area.top));
            const float scale = std::min(available_width / source.width,
                                         available_height / source.height);
            const float width = source.width * scale;
            const float height = source.height * scale;
            const float left = static_cast<float>(image_area.left) +
                               (available_width - width) / 2.0F;
            const float top = static_cast<float>(image_area.top) +
                              (available_height - height) / 2.0F;
            target_->DrawBitmap(bitmap,
                D2D1::RectF(left, top, left + width, top + height), 1.0F,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
        ID2D1SolidColorBrush* status = agent.status == AgentStatus::Online
            ? online_brush_.Get()
            : agent.status == AgentStatus::Stale
                ? stale_brush_.Get() : offline_brush_.Get();
        target_->FillEllipse(D2D1::Ellipse(
            D2D1::Point2F(static_cast<float>(layout.label.left + 10),
                          static_cast<float>((layout.label.top +
                                              layout.label.bottom) / 2)),
            5.0F, 5.0F), status);
        std::array<wchar_t, 512> label{};
        const std::wstring name = widen(agent.employee_name);
        const std::wstring host = widen(agent.hostname);
        static_cast<void>(_snwprintf_s(label.data(), label.size(), _TRUNCATE,
            L"%s  •  %s  •  %.1f FPS", name.c_str(), host.c_str(), agent.fps));
        D2D1_RECT_F text_rect = to_d2d(layout.label);
        text_rect.left += 20.0F;
        target_->DrawText(label.data(), static_cast<UINT32>(wcslen(label.data())),
            text_format_.Get(), text_rect, text_brush_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    ID2D1Bitmap* bitmap_for(const AgentSnapshot& agent) {
        if (!agent.frame) return nullptr;
        auto found = cache_.find(agent.guid);
        if (found != cache_.end() &&
            found->second.generation == agent.generation &&
            found->second.sequence == agent.frame->sequence) {
            return found->second.bitmap.Get();
        }
        const auto decoded = decoder_.decode(agent.frame->encoded_image);
        if (!decoded) return nullptr;
        ComPtr<ID2D1Bitmap> bitmap;
        const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(target_->CreateBitmap(
            D2D1::SizeU(decoded->width, decoded->height),
            decoded->pixels.data(), decoded->width * 4U,
            properties, &bitmap))) {
            return nullptr;
        }
        CachedBitmap cached{agent.generation, agent.frame->sequence,
                            std::move(bitmap)};
        ID2D1Bitmap* result = cached.bitmap.Get();
        cache_[agent.guid] = std::move(cached);
        return result;
    }

    std::optional<AgentSnapshot> hit_agent(const int x, const int y) const {
        const auto hit = LayoutEngine::hit_test(layouts_, x, y);
        if (!hit) return std::nullopt;
        if (const auto selected = router_.selection()) {
            const auto found = std::find_if(displayed_.begin(), displayed_.end(),
                [&selected](const AgentSnapshot& item) {
                    return item.guid == *selected;
                });
            if (found != displayed_.end() && *hit == 0U) return *found;
            return std::nullopt;
        }
        return *hit < displayed_.size()
            ? std::optional<AgentSnapshot>(displayed_[*hit]) : std::nullopt;
    }

    void toggle_focus(const int x, const int y) {
        if (router_.selection()) {
            router_.clear();
            return;
        }
        const auto agent = hit_agent(x, y);
        if (agent) router_.select(agent->guid);
    }

    void show_context_menu(const int x, const int y) {
        const auto agent = hit_agent(x, y);
        if (!agent) return;
        HMENU menu = CreatePopupMenu();
        if (menu == nullptr) return;
        AppendMenuW(menu, MF_STRING, 1, L"Rename...");
        POINT point{x, y};
        ClientToScreen(window_, &point);
        const UINT command = TrackPopupMenu(
            menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y,
            0, window_, nullptr);
        DestroyMenu(menu);
        if (command != 1) return;
        const auto renamed = RenamePrompt::show(
            window_, widen(agent->employee_name));
        if (!renamed) return;
        const auto utf8 = narrow(*renamed);
        if (!utf8 || !names_.save(agent->guid, *utf8)) {
            MessageBoxW(window_, L"Name must be non-empty valid UTF-8 and at most 255 bytes.",
                        L"Rename failed", MB_OK | MB_ICONERROR);
            return;
        }
        registry_.set_local_name(agent->guid, *utf8);
    }

    void toggle_fullscreen() {
        if (!fullscreen_) {
            previous_style_ = static_cast<DWORD>(
                GetWindowLongPtrW(window_, GWL_STYLE));
            GetWindowRect(window_, &previous_rect_);
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST),
                            &monitor);
            SetWindowLongPtrW(window_, GWL_STYLE,
                              static_cast<LONG_PTR>(previous_style_ &
                                                   ~WS_OVERLAPPEDWINDOW));
            SetWindowPos(window_, HWND_TOP, monitor.rcMonitor.left,
                monitor.rcMonitor.top,
                monitor.rcMonitor.right - monitor.rcMonitor.left,
                monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            fullscreen_ = true;
        } else {
            SetWindowLongPtrW(window_, GWL_STYLE,
                              static_cast<LONG_PTR>(previous_style_));
            SetWindowPos(window_, nullptr, previous_rect_.left,
                previous_rect_.top,
                previous_rect_.right - previous_rect_.left,
                previous_rect_.bottom - previous_rect_.top,
                SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
            fullscreen_ = false;
        }
    }

    HINSTANCE instance_;
    HWND window_ = nullptr;
    AgentRegistry& registry_;
    FocusRouter router_;
    NameStore& names_;
    JpegDecoder decoder_;
    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> write_factory_;
    ComPtr<IDWriteTextFormat> text_format_;
    ComPtr<ID2D1HwndRenderTarget> target_;
    ComPtr<ID2D1SolidColorBrush> text_brush_;
    ComPtr<ID2D1SolidColorBrush> online_brush_;
    ComPtr<ID2D1SolidColorBrush> stale_brush_;
    ComPtr<ID2D1SolidColorBrush> offline_brush_;
    std::map<protocol::AgentGuid, CachedBitmap> cache_;
    std::vector<AgentSnapshot> displayed_;
    std::vector<TileLayout> layouts_;
    bool fullscreen_ = false;
    DWORD previous_style_ = 0;
    RECT previous_rect_{};
};

ObserverWindow::ObserverWindow(HINSTANCE instance, AgentRegistry& registry,
                               ProfileSink& profile_sink, NameStore& names)
    : impl_(std::make_unique<Impl>(instance, registry, profile_sink, names)) {}

ObserverWindow::~ObserverWindow() = default;

int ObserverWindow::run(const int show_command) {
    return impl_->run(show_command);
}

}
