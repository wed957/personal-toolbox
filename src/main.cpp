#include "display_manager.hpp"
#include "../resource.h"

#include <commctrl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

using mux::ApplyRequest;
using mux::DisplayManager;
using mux::DisplaySnapshot;
using mux::OperationResult;

namespace {

constexpr wchar_t kWindowClass[] = L"MUX.MainWindow";
constexpr wchar_t kTargetName[] = L"P25W2GC";
constexpr UINT kRefreshTimer = 1;
constexpr UINT kJobPollTimer = 2;
constexpr UINT kRefreshDelayMs = 350;
constexpr UINT kJobPollDelayMs = 50;
constexpr UINT kJobCompleted = WM_APP + 1;
constexpr UINT kInitialRefresh = WM_APP + 2;

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
    Refresh,
    Apply,
    Restore,
};

enum class ApplyIntent {
    Selected,
    OnlyTarget,
    All,
};

struct JobResult {
    JobKind kind = JobKind::Apply;
    OperationResult operation;
    DisplaySnapshot previous;
    DisplayManager manager;
    std::vector<std::wstring> preserved_selection;
    std::wstring refresh_error;
    UINT64 topology_generation = 0;
    bool manager_refreshed = false;
    bool preserve_selection = false;
    bool consumes_topology_events = false;
};

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
        shutting_down_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
        {
            std::lock_guard<std::mutex> lock(job_mutex_);
            completed_job_.reset();
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
        LRESULT result = 0;
        try {
            result = app != nullptr
                         ? app->HandleMessage(message, wparam, lparam)
                         : DefWindowProcW(window, message, wparam, lparam);
        } catch (...) {
            if (app != nullptr) {
                app->HandleUiFailure();
            }
            result = 0;
        }
        if (message == WM_NCDESTROY && app != nullptr) {
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            app->window_ = nullptr;
        }
        return result;
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
                if (wparam == kJobPollTimer) {
                    if (job_ready_.load(std::memory_order_acquire)) {
                        CompleteJob();
                    }
                } else if (wparam == kRefreshTimer) {
                    KillTimer(window_, kRefreshTimer);
                    if (!busy_) {
                        BeginRefresh(true, false);
                    } else {
                        refresh_pending_ = true;
                    }
                }
                return 0;
            case WM_DISPLAYCHANGE:
            case WM_DEVICECHANGE:
                topology_generation_.fetch_add(1, std::memory_order_release);
                refresh_pending_ = true;
                if (!busy_) {
                    SetTimer(window_, kRefreshTimer, kRefreshDelayMs, nullptr);
                }
                return 0;
            case kJobCompleted:
                if (wparam == active_job_id_) {
                    CompleteJob();
                }
                return 0;
            case kInitialRefresh:
                BeginRefresh(false, true);
                return 0;
            case WM_GETMINMAXINFO: {
                auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
                limits->ptMinTrackSize.x = Scale(620, dpi_);
                limits->ptMinTrackSize.y = Scale(480, dpi_);
                return 0;
            }
            case WM_CLOSE:
                DestroyWindow(window_);
                return 0;
            case WM_DESTROY:
                shutting_down_.store(true, std::memory_order_release);
                KillTimer(window_, kRefreshTimer);
                KillTimer(window_, kJobPollTimer);
                DeleteObject(font_);
                DeleteObject(title_font_);
                font_ = nullptr;
                title_font_ = nullptr;
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

    static bool ContainsKey(
        const std::vector<std::wstring>& keys, const std::wstring& key) {
        return std::any_of(keys.begin(), keys.end(), [&key](const auto& value) {
            return CompareStringOrdinal(
                       value.c_str(), static_cast<int>(value.size()),
                       key.c_str(), static_cast<int>(key.size()), TRUE) ==
                   CSTR_EQUAL;
        });
    }

    void PopulateList(
        const std::vector<std::wstring>* preserved_selection = nullptr) {
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
            const int row = ListView_InsertItem(list_, &item);
            if (row < 0) {
                continue;
            }
            ListView_SetItemText(
                list_, row, 1,
                const_cast<wchar_t*>(target.active ? L"已启用" : L"未启用"));

            std::wstring mode = L"自动";
            if (target.active && target.width != 0 && target.height != 0) {
                mode = std::to_wstring(target.width) + L" × " +
                       std::to_wstring(target.height);
                if (target.refresh_hz > 1.0) {
                    const int rounded = static_cast<int>(
                        std::lround(target.refresh_hz));
                    mode += L" @ " + std::to_wstring(rounded) + L" Hz";
                }
            } else if (!target.active && target.preferred_width != 0 &&
                       target.preferred_height != 0) {
                mode = L"首选 " + std::to_wstring(target.preferred_width) +
                       L" × " + std::to_wstring(target.preferred_height);
            }
            ListView_SetItemText(list_, row, 2, mode.data());
            const bool checked =
                preserved_selection != nullptr
                    ? ContainsKey(*preserved_selection, target.key)
                    : target.active;
            ListView_SetCheckState(list_, row, checked);
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
                BeginRefresh(true, false);
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

    template <typename Task>
    bool LaunchJob(
        JobKind kind, const std::wstring& progress_text, Task&& task) {
        if (worker_.joinable()) {
            SetStatus(L"后台任务仍在结束，请稍后重试。");
            return false;
        }

        SetBusy(true, progress_text);
        const HWND destination = window_;
        UINT_PTR job_id = ++next_job_id_;
        if (job_id == 0) {
            job_id = ++next_job_id_;
        }
        active_job_id_ = job_id;
        try {
            worker_ = std::thread(
                [this, destination, job_id, kind,
                 task = std::forward<Task>(task)]() mutable noexcept {
                    try {
                        auto result = task();
                        PublishJobResult(
                            destination, job_id, std::move(result));
                    } catch (...) {
                        PublishJobFailure(destination, job_id, kind);
                    }
                });
            SetTimer(window_, kJobPollTimer, kJobPollDelayMs, nullptr);
            return true;
        } catch (...) {
            active_job_id_ = 0;
            SetBusy(false, L"无法启动后台任务，请重试。");
            return false;
        }
    }

    void PublishJobResult(
        HWND destination, UINT_PTR job_id,
        std::unique_ptr<JobResult> result) noexcept {
        if (result == nullptr ||
            shutting_down_.load(std::memory_order_acquire)) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(job_mutex_);
            if (shutting_down_.load(std::memory_order_relaxed)) {
                return;
            }
            completed_job_ = std::move(result);
        }
        job_ready_.store(true, std::memory_order_release);
        PostMessageW(destination, kJobCompleted, job_id, 0);
    }

    void PublishJobFailure(
        HWND destination, UINT_PTR job_id, JobKind kind) noexcept {
        try {
            auto result = std::make_unique<JobResult>();
            result->kind = kind;
            result->operation.error_code = ERROR_UNHANDLED_EXCEPTION;
            result->operation.message = L"后台任务异常终止，未继续操作显示布局。";
            PublishJobResult(destination, job_id, std::move(result));
        } catch (...) {
            job_ready_.store(true, std::memory_order_release);
            PostMessageW(destination, kJobCompleted, job_id, 0);
        }
    }

    void CompleteJob() {
        if (!worker_.joinable()) {
            return;
        }
        KillTimer(window_, kJobPollTimer);
        job_ready_.store(false, std::memory_order_release);
        active_job_id_ = 0;
        if (worker_.joinable()) {
            worker_.join();
        }

        std::unique_ptr<JobResult> result;
        {
            std::lock_guard<std::mutex> lock(job_mutex_);
            result = std::move(completed_job_);
        }
        FinishJob(std::move(result));
    }

    bool RefreshJobManager(JobResult& result) {
        const UINT64 generation_before =
            topology_generation_.load(std::memory_order_acquire);
        result.manager_refreshed =
            result.manager.Refresh(result.refresh_error);
        const UINT64 generation_after =
            topology_generation_.load(std::memory_order_acquire);
        result.topology_generation = generation_after;
        result.consumes_topology_events =
            result.manager_refreshed && generation_before == generation_after;
        return result.manager_refreshed;
    }

    void BeginRefresh(bool preserve_selection, bool initial) {
        refresh_pending_ = false;
        std::vector<std::wstring> selection;
        if (preserve_selection) {
            selection = SelectedKeys();
        }
        const std::wstring progress =
            initial ? L"正在读取显示器…" : L"正在刷新显示器…";
        if (!LaunchJob(
                JobKind::Refresh, progress,
                [this, selection = std::move(selection),
                 preserve_selection]() mutable {
                    auto result = std::make_unique<JobResult>();
                    result->kind = JobKind::Refresh;
                    result->preserve_selection = preserve_selection;
                    result->preserved_selection = std::move(selection);
                    RefreshJobManager(*result);
                    result->operation.success = result->manager_refreshed;
                    result->operation.error_code =
                        result->manager_refreshed ? ERROR_SUCCESS
                                                  : ERROR_GEN_FAILURE;
                    result->operation.message =
                        result->manager_refreshed
                            ? L"显示器列表已刷新。"
                            : result->refresh_error;
                    return result;
                })) {
            refresh_pending_ = true;
        }
    }

    void BeginOnlyTarget() {
        BeginApplyWithFreshList(
            {}, L"正在仅保留 P25W2GC…", ApplyIntent::OnlyTarget);
    }

    void BeginEnableAll() {
        BeginApplyWithFreshList(
            {}, L"正在启用全部显示器…", ApplyIntent::All);
    }

    void BeginApply(std::vector<std::wstring> selected) {
        BeginApplyWithFreshList(
            std::move(selected), L"正在应用所选显示器…",
            ApplyIntent::Selected);
    }

    void BeginApplyWithFreshList(
        std::vector<std::wstring> selected,
        const std::wstring& progress_text,
        ApplyIntent intent) {
        DisplayManager manager = manager_;
        const bool refresh_before_apply = refresh_pending_;
        refresh_pending_ = false;
        if (!LaunchJob(
                JobKind::Apply, progress_text,
                [this, manager = std::move(manager),
                 selected = std::move(selected), intent,
                 refresh_before_apply]() mutable {
                    auto result = std::make_unique<JobResult>();
                    result->kind = JobKind::Apply;
                    result->manager = std::move(manager);

                    std::wstring error;
                    if (refresh_before_apply &&
                        !RefreshJobManager(*result)) {
                        result->operation.error_code = ERROR_GEN_FAILURE;
                        result->operation.message = result->refresh_error;
                        return result;
                    }

                    ApplyRequest request;
                    if (intent == ApplyIntent::All) {
                        selected.clear();
                        for (const auto& target : result->manager.targets()) {
                            selected.push_back(target.key);
                        }
                    }
                    const bool created =
                        intent == ApplyIntent::OnlyTarget
                            ? result->manager.CreateOnlyByNameRequest(
                                  kTargetName, request, error)
                            : result->manager.CreateApplyRequest(
                                  selected, request, error);
                    if (!created) {
                        result->operation.error_code = ERROR_INVALID_DATA;
                        result->operation.message = error;
                    } else {
                        const bool no_change = request.no_change;
                        result->previous = request.rollback;
                        result->operation =
                            DisplayManager::Execute(std::move(request));
                        if (no_change) {
                            result->manager_refreshed = true;
                            return result;
                        }
                    }

                    RefreshJobManager(*result);
                    return result;
                })) {
            refresh_pending_ = true;
        }
    }

    void BeginRestore() {
        const bool exact = has_previous_;
        DisplaySnapshot snapshot = previous_;
        DisplayManager manager = manager_;
        refresh_pending_ = false;
        if (!LaunchJob(
                JobKind::Restore, L"正在恢复显示布局…",
                [this, exact, snapshot = std::move(snapshot),
                 manager = std::move(manager)]() mutable {
                    auto result = std::make_unique<JobResult>();
                    result->kind = JobKind::Restore;
                    result->manager = std::move(manager);
                    result->operation =
                        exact
                            ? DisplayManager::Restore(std::move(snapshot))
                            : DisplayManager::RestoreExtended();
                    RefreshJobManager(*result);
                    return result;
                })) {
            refresh_pending_ = true;
        }
    }

    void FinishJob(std::unique_ptr<JobResult> result) {
        if (result == nullptr) {
            SetBusy(false, L"后台任务未返回有效结果，请重试。");
            return;
        }

        if (result->kind == JobKind::Apply &&
            !result->operation.no_change &&
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

        if (result->manager_refreshed) {
            if (result->consumes_topology_events &&
                result->topology_generation ==
                    topology_generation_.load(std::memory_order_acquire)) {
                refresh_pending_ = false;
            }
            manager_ = std::move(result->manager);
            PopulateList(
                result->preserve_selection ? &result->preserved_selection
                                           : nullptr);
        } else if (result->kind != JobKind::Refresh) {
            refresh_pending_ = true;
        }

        const std::wstring status =
            !result->operation.message.empty()
                ? result->operation.message
                : result->refresh_error;
        SetBusy(false, status);
        if (!result->operation.success && result->kind != JobKind::Refresh) {
            MessageBoxW(window_, result->operation.message.c_str(),
                        L"显示切换失败", MB_OK | MB_ICONERROR);
        }
        SchedulePendingRefresh();
    }

    void SchedulePendingRefresh() {
        if (refresh_pending_ && window_ != nullptr && !busy_) {
            SetTimer(window_, kRefreshTimer, kRefreshDelayMs, nullptr);
        }
    }

    void HandleUiFailure() noexcept {
        busy_ = false;
        if (status_ != nullptr) {
            SetWindowTextW(status_, L"界面操作失败，请刷新后重试。");
        }
        for (HWND control : {list_, only_button_, enable_all_button_,
                             restore_button_, refresh_button_, apply_button_}) {
            if (control != nullptr) {
                EnableWindow(control, TRUE);
            }
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
    bool refresh_pending_ = false;
    std::atomic_bool shutting_down_{false};
    std::atomic_bool job_ready_{false};
    std::atomic_uint64_t topology_generation_{0};
    UINT_PTR next_job_id_ = 0;
    UINT_PTR active_job_id_ = 0;
    std::thread worker_;
    std::mutex job_mutex_;
    std::unique_ptr<JobResult> completed_job_;
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
