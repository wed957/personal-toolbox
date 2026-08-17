#include "display_manager.hpp"
#include "../resource.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using mux::ApplyRequest;
using mux::DisplayManager;
using mux::DisplaySnapshot;
using mux::OperationResult;

namespace {

constexpr wchar_t kWindowClass[] = L"MUX.MainWindow";
constexpr wchar_t kTargetName[] = L"P25W2GC";
constexpr UINT kRefreshTimer = 1;
constexpr UINT kRefreshDelayMs = 350;
constexpr UINT kJobCompleted = WM_APP + 1;
constexpr UINT kInitialRefresh = WM_APP + 2;
constexpr LRESULT kJobHandled = 0x4D53;

enum ControlId {
    kTitle = 100,
    kSubtitle,
    kOnlyTarget,
    kDisplayList,
    kStatus,
    kEnableAll,
    kRestore,
    kRefresh,
    kApply,
};

enum class JobKind {
    Apply,
    Restore,
};

struct JobResult {
    JobKind kind = JobKind::Apply;
    OperationResult operation;
    DisplaySnapshot previous;
};

void DeliverJobResult(HWND destination, std::unique_ptr<JobResult> result) {
    JobResult* raw_result = result.release();
    if (SendMessageW(
            destination, kJobCompleted, 0,
            reinterpret_cast<LPARAM>(raw_result)) != kJobHandled) {
        delete raw_result;
    }
}

int Scale(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

class App {
public:
    explicit App(HINSTANCE instance) : instance_(instance) {}

    int Run(int show_command) {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
        InitCommonControlsEx(&controls);

        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = WindowProc;
        window_class.hInstance = instance_;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hIcon = static_cast<HICON>(LoadImageW(
            instance_, MAKEINTRESOURCEW(IDI_MUX), IMAGE_ICON,
            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
            LR_DEFAULTCOLOR));
        window_class.hIconSm = static_cast<HICON>(LoadImageW(
            instance_, MAKEINTRESOURCEW(IDI_MUX), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
            LR_DEFAULTCOLOR));
        window_class.hbrBackground =
            reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = kWindowClass;
        if (RegisterClassExW(&window_class) == 0) {
            return 1;
        }

        window_ = CreateWindowExW(
            0, kWindowClass, L"MUX", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 760, 590, nullptr, nullptr, instance_,
            this);
        if (window_ == nullptr) {
            return 2;
        }

        ShowWindow(window_, show_command);
        UpdateWindow(window_);

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

private:
    static LRESULT CALLBACK WindowProc(
        HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        App* app = reinterpret_cast<App*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            app = static_cast<App*>(create->lpCreateParams);
            app->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(app));
        }
        return app != nullptr ? app->HandleMessage(message, wparam, lparam)
                              : DefWindowProcW(window, message, wparam, lparam);
    }

    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
            case WM_CREATE:
                return OnCreate() ? 0 : -1;
            case WM_SIZE:
                Layout();
                return 0;
            case WM_DPICHANGED:
                OnDpiChanged(wparam, lparam);
                return 0;
            case WM_COMMAND:
                OnCommand(LOWORD(wparam));
                return 0;
            case WM_NOTIFY:
                OnNotify(reinterpret_cast<NMHDR*>(lparam));
                return 0;
            case WM_TIMER:
                if (wparam == kRefreshTimer) {
                    KillTimer(window_, kRefreshTimer);
                    if (!busy_) {
                        RefreshDisplays();
                    }
                }
                return 0;
            case WM_DISPLAYCHANGE:
            case WM_DEVICECHANGE:
                if (!busy_) {
                    SetTimer(window_, kRefreshTimer, kRefreshDelayMs, nullptr);
                }
                return 0;
            case kJobCompleted:
                FinishJob(reinterpret_cast<JobResult*>(lparam));
                return kJobHandled;
            case kInitialRefresh:
                RefreshDisplays();
                SetBusy(false);
                return 0;
            case WM_GETMINMAXINFO: {
                auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
                limits->ptMinTrackSize.x = Scale(620, dpi_);
                limits->ptMinTrackSize.y = Scale(480, dpi_);
                return 0;
            }
            case WM_CLOSE:
                if (busy_) {
                    SetStatus(L"正在等待 Windows 完成显示切换…");
                    MessageBeep(MB_ICONINFORMATION);
                    return 0;
                }
                DestroyWindow(window_);
                return 0;
            case WM_DESTROY:
                DeleteObject(font_);
                DeleteObject(title_font_);
                PostQuitMessage(0);
                return 0;
            default:
                return DefWindowProcW(window_, message, wparam, lparam);
        }
    }

    bool OnCreate() {
        dpi_ = GetDpiForWindow(window_);
        CreateFonts();

        title_ = CreateControl(L"STATIC", L"MUX", SS_LEFT, kTitle);
        subtitle_ = CreateControl(
            L"STATIC", L"选择要保留在 Windows 桌面中的显示器",
            SS_LEFT, kSubtitle);
        only_button_ = CreateControl(
            L"BUTTON", L"仅保留 P25W2GC", BS_PUSHBUTTON, kOnlyTarget);
        list_ = CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
                LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kDisplayList),
            instance_, nullptr);
        status_ = CreateControl(L"STATIC", L"正在读取显示器…", SS_LEFT,
                                kStatus);
        enable_all_button_ = CreateControl(
            L"BUTTON", L"全部启用", BS_PUSHBUTTON, kEnableAll);
        restore_button_ = CreateControl(
            L"BUTTON", L"恢复扩展布局", BS_PUSHBUTTON, kRestore);
        refresh_button_ =
            CreateControl(L"BUTTON", L"刷新", BS_PUSHBUTTON, kRefresh);
        apply_button_ = CreateControl(
            L"BUTTON", L"应用所选", BS_DEFPUSHBUTTON, kApply);

        if (list_ == nullptr || apply_button_ == nullptr) {
            return false;
        }

        for (HWND control : AllControls()) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_),
                         TRUE);
        }
        SendMessageW(title_, WM_SETFONT, reinterpret_cast<WPARAM>(title_font_),
                     TRUE);

        ListView_SetExtendedListViewStyle(
            list_, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                       LVS_EX_LABELTIP);
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(L"显示器");
        column.cx = Scale(280, dpi_);
        ListView_InsertColumn(list_, 0, &column);
        column.pszText = const_cast<wchar_t*>(L"当前状态");
        column.cx = Scale(110, dpi_);
        column.iSubItem = 1;
        ListView_InsertColumn(list_, 1, &column);
        column.pszText = const_cast<wchar_t*>(L"显示模式");
        column.cx = Scale(190, dpi_);
        column.iSubItem = 2;
        ListView_InsertColumn(list_, 2, &column);

        Layout();
        SetBusy(true, L"正在读取显示器…");
        return PostMessageW(window_, kInitialRefresh, 0, 0) != FALSE;
    }

    HWND CreateControl(
        const wchar_t* class_name, const wchar_t* text, DWORD style, int id) {
        return CreateWindowExW(
            0, class_name, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(id), instance_,
            nullptr);
    }

    std::vector<HWND> AllControls() const {
        return {title_,          subtitle_,       only_button_, list_,
                status_,         enable_all_button_, restore_button_,
                refresh_button_, apply_button_};
    }

    void CreateFonts() {
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        SystemParametersInfoW(
            SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
        metrics.lfMessageFont.lfHeight = -Scale(15, dpi_);
        wcscpy_s(metrics.lfMessageFont.lfFaceName, L"Segoe UI");
        font_ = CreateFontIndirectW(&metrics.lfMessageFont);

        LOGFONTW title = metrics.lfMessageFont;
        title.lfHeight = -Scale(25, dpi_);
        title.lfWeight = FW_SEMIBOLD;
        title_font_ = CreateFontIndirectW(&title);
    }

    void OnDpiChanged(WPARAM wparam, LPARAM lparam) {
        dpi_ = HIWORD(wparam);
        const auto* suggested = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(
            window_, nullptr, suggested->left, suggested->top,
            suggested->right - suggested->left,
            suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);

        DeleteObject(font_);
        DeleteObject(title_font_);
        CreateFonts();
        for (HWND control : AllControls()) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_),
                         TRUE);
        }
        SendMessageW(title_, WM_SETFONT, reinterpret_cast<WPARAM>(title_font_),
                     TRUE);
        Layout();
    }

    void Layout() {
        if (title_ == nullptr) {
            return;
        }

        RECT client{};
        GetClientRect(window_, &client);
        const int width = client.right;
        const int height = client.bottom;
        const int margin = Scale(24, dpi_);
        const int content_width = std::max(0, width - margin * 2);

        MoveWindow(title_, margin, Scale(20, dpi_), content_width,
                   Scale(34, dpi_), TRUE);
        MoveWindow(subtitle_, margin, Scale(55, dpi_), content_width,
                   Scale(24, dpi_), TRUE);
        MoveWindow(only_button_, margin, Scale(88, dpi_), content_width,
                   Scale(48, dpi_), TRUE);

        const int bottom_buttons_y = height - Scale(58, dpi_);
        const int status_y = bottom_buttons_y - Scale(32, dpi_);
        const int list_y = Scale(153, dpi_);
        MoveWindow(list_, margin, list_y, content_width,
                   std::max(Scale(120, dpi_), status_y - list_y - Scale(8, dpi_)),
                   TRUE);
        MoveWindow(status_, margin, status_y, content_width, Scale(24, dpi_),
                   TRUE);

        const int gap = Scale(8, dpi_);
        const int small = Scale(112, dpi_);
        const int apply_width = Scale(132, dpi_);
        MoveWindow(enable_all_button_, margin, bottom_buttons_y, small,
                   Scale(36, dpi_), TRUE);
        MoveWindow(restore_button_, margin + small + gap, bottom_buttons_y,
                   Scale(140, dpi_), Scale(36, dpi_), TRUE);
        MoveWindow(refresh_button_,
                   width - margin - apply_width - gap - Scale(92, dpi_),
                   bottom_buttons_y, Scale(92, dpi_), Scale(36, dpi_), TRUE);
        MoveWindow(apply_button_, width - margin - apply_width, bottom_buttons_y,
                   apply_width, Scale(36, dpi_), TRUE);

        ListView_SetColumnWidth(list_, 0, std::max(Scale(220, dpi_),
                                                   content_width - Scale(310, dpi_)));
        ListView_SetColumnWidth(list_, 1, Scale(105, dpi_));
        ListView_SetColumnWidth(list_, 2, Scale(185, dpi_));
    }

    void RefreshDisplays() {
        std::wstring error;
        if (!manager_.Refresh(error)) {
            SetStatus(error);
            MessageBoxW(window_, error.c_str(), L"读取显示器失败",
                        MB_OK | MB_ICONERROR);
            return;
        }
        PopulateList();
    }

    void PopulateList() {
        suppress_list_events_ = true;
        ListView_DeleteAllItems(list_);
        row_keys_.clear();

        const auto& targets = manager_.targets();
        for (size_t index = 0; index < targets.size(); ++index) {
            const auto& target = targets[index];
            std::wstring label = target.name;
            if (!target.gdi_name.empty()) {
                label += L"  (" + target.gdi_name + L")";
            }

            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = static_cast<int>(index);
            item.pszText = label.data();
            item.lParam = static_cast<LPARAM>(index);
            ListView_InsertItem(list_, &item);
            ListView_SetItemText(
                list_, static_cast<int>(index), 1,
                const_cast<wchar_t*>(target.active ? L"已启用" : L"未启用"));

            std::wstring mode = L"自动";
            if (target.width != 0 && target.height != 0) {
                mode = std::to_wstring(target.width) + L" × " +
                       std::to_wstring(target.height);
                if (target.active && target.refresh_hz > 1.0) {
                    const int rounded = static_cast<int>(
                        std::lround(target.refresh_hz));
                    mode += L" @ " + std::to_wstring(rounded) + L" Hz";
                }
            }
            ListView_SetItemText(list_, static_cast<int>(index), 2, mode.data());
            ListView_SetCheckState(list_, static_cast<int>(index), target.active);
            row_keys_.push_back(target.key);
        }
        suppress_list_events_ = false;
        UpdateSelectionStatus();
    }

    std::vector<std::wstring> SelectedKeys() const {
        std::vector<std::wstring> keys;
        for (size_t index = 0; index < row_keys_.size(); ++index) {
            if (ListView_GetCheckState(list_, static_cast<int>(index))) {
                keys.push_back(row_keys_[index]);
            }
        }
        return keys;
    }

    void UpdateSelectionStatus() {
        const size_t selected = SelectedKeys().size();
        size_t active = 0;
        for (const auto& target : manager_.targets()) {
            active += target.active ? 1u : 0u;
        }
        SetStatus(
            L"已选择 " + std::to_wstring(selected) + L" 台，当前启用 " +
            std::to_wstring(active) + L" 台");
        EnableWindow(apply_button_, !busy_ && selected != 0);
    }

    void SetStatus(const std::wstring& text) {
        SetWindowTextW(status_, text.c_str());
    }

    void SetBusy(bool busy, const std::wstring& status = {}) {
        busy_ = busy;
        EnableWindow(list_, !busy);
        EnableWindow(only_button_, !busy);
        EnableWindow(enable_all_button_, !busy);
        EnableWindow(restore_button_, !busy);
        EnableWindow(refresh_button_, !busy);
        EnableWindow(apply_button_, !busy && !SelectedKeys().empty());
        if (!status.empty()) {
            SetStatus(status);
        }
    }

    void OnCommand(int id) {
        if (busy_) {
            return;
        }
        switch (id) {
            case kOnlyTarget:
                BeginOnlyTarget();
                break;
            case kEnableAll:
                BeginEnableAll();
                break;
            case kRestore:
                BeginRestore();
                break;
            case kRefresh:
                RefreshDisplays();
                break;
            case kApply:
                BeginApply(SelectedKeys());
                break;
            default:
                break;
        }
    }

    void OnNotify(const NMHDR* notification) {
        if (notification == nullptr || notification->hwndFrom != list_ ||
            suppress_list_events_) {
            return;
        }
        if (notification->code == LVN_ITEMCHANGED) {
            const auto* changed = reinterpret_cast<const NMLISTVIEW*>(notification);
            if ((changed->uChanged & LVIF_STATE) != 0) {
                UpdateSelectionStatus();
            }
        }
    }

    void BeginOnlyTarget() {
        ApplyRequest request;
        std::wstring error;
        if (!manager_.CreateOnlyByNameRequest(kTargetName, request, error)) {
            MessageBoxW(window_, error.c_str(), L"无法切换显示器",
                        MB_OK | MB_ICONWARNING);
            SetStatus(error);
            return;
        }
        StartApply(std::move(request), L"正在仅保留 P25W2GC…");
    }

    void BeginEnableAll() {
        std::vector<std::wstring> keys;
        for (const auto& target : manager_.targets()) {
            keys.push_back(target.key);
        }
        BeginApplyWithFreshList(keys, L"正在启用全部显示器…");
    }

    void BeginApply(const std::vector<std::wstring>& selected) {
        BeginApplyWithFreshList(selected, L"正在应用所选显示器…");
    }

    void BeginApplyWithFreshList(
        const std::vector<std::wstring>& selected,
        const std::wstring& progress_text) {
        ApplyRequest request;
        std::wstring error;
        if (!manager_.CreateApplyRequest(selected, request, error)) {
            MessageBoxW(window_, error.c_str(), L"无法应用显示布局",
                        MB_OK | MB_ICONWARNING);
            SetStatus(error);
            return;
        }
        StartApply(std::move(request), progress_text);
    }

    void StartApply(ApplyRequest request, const std::wstring& progress_text) {
        if (request.no_change) {
            const OperationResult result = DisplayManager::Execute(std::move(request));
            SetStatus(result.message);
            return;
        }

        DisplaySnapshot previous = request.rollback;
        SetBusy(true, progress_text);
        const HWND destination = window_;
        std::thread(
            [destination, request = std::move(request),
             previous = std::move(previous)]() mutable {
                auto result = std::make_unique<JobResult>();
                result->kind = JobKind::Apply;
                result->previous = std::move(previous);
                result->operation = DisplayManager::Execute(std::move(request));
                DeliverJobResult(destination, std::move(result));
            })
            .detach();
    }

    void BeginRestore() {
        SetBusy(true, L"正在恢复显示布局…");
        const HWND destination = window_;
        const bool exact = has_previous_;
        DisplaySnapshot snapshot = previous_;
        std::thread([destination, exact, snapshot = std::move(snapshot)]() mutable {
            auto result = std::make_unique<JobResult>();
            result->kind = JobKind::Restore;
            result->operation = exact
                                    ? DisplayManager::Restore(std::move(snapshot))
                                    : DisplayManager::RestoreExtended();
            DeliverJobResult(destination, std::move(result));
        }).detach();
    }

    void FinishJob(JobResult* raw_result) {
        std::unique_ptr<JobResult> result(raw_result);
        if (result == nullptr) {
            SetBusy(false, L"显示切换返回了无效结果。" );
            return;
        }

        if (result->kind == JobKind::Apply &&
            (result->operation.success || result->operation.recovery_needed)) {
            // Keep the exact pre-switch snapshot even if apply and rollback both
            // fail, so the user can explicitly retry recovery.
            previous_ = std::move(result->previous);
            has_previous_ = true;
            SetWindowTextW(restore_button_, L"恢复上次布局");
        } else if (result->operation.success &&
                   result->kind == JobKind::Restore) {
                previous_ = {};
                has_previous_ = false;
                SetWindowTextW(restore_button_, L"恢复扩展布局");
        }

        SetBusy(false, result->operation.message);
        if (!result->operation.success) {
            MessageBoxW(window_, result->operation.message.c_str(),
                        L"显示切换失败", MB_OK | MB_ICONERROR);
        }

        std::wstring error;
        if (manager_.Refresh(error)) {
            PopulateList();
            SetStatus(result->operation.message);
        } else {
            SetStatus(error);
        }
    }

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND title_ = nullptr;
    HWND subtitle_ = nullptr;
    HWND only_button_ = nullptr;
    HWND list_ = nullptr;
    HWND status_ = nullptr;
    HWND enable_all_button_ = nullptr;
    HWND restore_button_ = nullptr;
    HWND refresh_button_ = nullptr;
    HWND apply_button_ = nullptr;
    HFONT font_ = nullptr;
    HFONT title_font_ = nullptr;
    UINT dpi_ = 96;
    bool busy_ = false;
    bool suppress_list_events_ = false;
    bool has_previous_ = false;
    DisplayManager manager_;
    DisplaySnapshot previous_;
    std::vector<std::wstring> row_keys_;
};

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show_command) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    App app(instance);
    return app.Run(show_command);
}
