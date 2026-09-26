#include "app/App.hpp"
#include "platform/InstanceIpc.hpp"
#include "ui/SettingsWindow.hpp"
#include "ui/ShortcutEditorDialog.hpp"
#include "ui/TopLevelWindowPresentation.hpp"

#include <commctrl.h>
#include <objbase.h>
#include <cassert>
#include <iostream>
#include <string_view>

#ifdef NDEBUG
#error "window_presentation_runtime_tests requires assertions"
#endif

namespace {

bool HasAboutHeading(HWND window) {
    bool found = false;
    EnumChildWindows(window, [](HWND child, LPARAM data) -> BOOL {
        wchar_t kind[32]{};
        wchar_t text[128]{};
        GetClassNameW(child, kind, 32);
        GetWindowTextW(child, text, 128);
        if (lstrcmpiW(kind, L"STATIC") == 0 &&
            IsWindowVisible(child) &&
            (std::wstring_view(text) == L"About" ||
             std::wstring_view(text) == L"关于")) {
            *reinterpret_cast<bool*>(data) = true;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}

LRESULT CALLBACK FirstShowProbe(HWND window, UINT message, WPARAM wParam,
                               LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    const auto result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_WINDOWPOSCHANGED &&
        (reinterpret_cast<WINDOWPOS*>(lParam)->flags & SWP_SHOWWINDOW)) {
        assert(HasAboutHeading(window));
        ++*reinterpret_cast<int*>(data);
    }
    return result;
}

HWND modalOwner{};
bool observedEditor{};

void CALLBACK CancelEditor(HWND, UINT, UINT_PTR timer, DWORD) {
    const HWND editor = FindWindowW(L"ALTRunNext.ShortcutEditor", nullptr);
    if (!editor) return;
    assert(IsWindowVisible(editor));
    assert(!IsWindowEnabled(modalOwner));
    observedEditor = true;
    KillTimer(nullptr, timer);
    PostMessageW(editor, WM_CLOSE, 0, 0);
}

} // namespace

int main() {
    using namespace altrun;
    namespace presentation = window_presentation;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HINSTANCE instance = GetModuleHandleW(nullptr);

    const HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Owner",
        WS_POPUP, -20000, -20000, 200, 100, nullptr, nullptr, instance, nullptr);
    assert(owner);
    {
        presentation::ScopedRedrawSuspend hidden(owner);
        hidden.Resume();
        assert(!IsWindowVisible(owner));
    }
    // A hidden Launcher's stale rectangle must not anchor an external dialog.
    const auto cursorGeometry = presentation::ResolveOwnedPopupGeometry(
        nullptr, instance, 500, 300);
    const auto hiddenGeometry = presentation::ResolveOwnedPopupGeometry(
        owner, instance, 500, 300);
    assert(EqualRect(&cursorGeometry.outer, &hiddenGeometry.outer));

    ShowWindow(owner, SW_SHOWNOACTIVATE);
    {
        presentation::ScopedRedrawSuspend outer(owner);
        assert(!IsWindowVisible(owner));
        {
            presentation::ScopedRedrawSuspend inner(owner);
        }
        assert(!IsWindowVisible(owner));
        outer.Resume();
        assert(IsWindowVisible(owner));
    }
    ShowWindow(owner, SW_HIDE);
    assert(!instance_ipc::GrantForegroundToWindow(nullptr));

    {
        // Exercise the actual Settings implementation, not just rectangle
        // math: the pre-fix Create() made this HWND visible via WM_SETREDRAW.
        App app(instance);
        SettingsWindow settings(app, instance);
        for (int attempt = 0; attempt < 6; ++attempt) {
            assert(settings.Create());
            const HWND window = FindWindowW(L"ALTRunNext.Settings", nullptr);
            assert(window && !IsWindowVisible(window));
            int firstShows = 0;
            assert(SetWindowSubclass(window, FirstShowProbe, 1,
                reinterpret_cast<DWORD_PTR>(&firstShows)));
            settings.ShowAbout();
            assert(IsWindowVisible(window));
            assert(HasAboutHeading(window));
            assert(firstShows == 1);
            RemoveWindowSubclass(window, FirstShowProbe, 1);
            // Visible reopen and minimized restore keep the About page.
            settings.ShowAbout();
            ShowWindow(window, SW_MINIMIZE);
            settings.ShowAbout();
            assert(!IsIconic(window) && HasAboutHeading(window));
            ShowWindow(window, SW_HIDE);
            settings.RefreshFromSettings();
            assert(!IsWindowVisible(window));
            DestroyWindow(window); // no position persistence in this fixture
        }

        modalOwner = owner;
        Command seed;
        seed.title = L"Runtime fixture";
        seed.target = L"C:\\fixture.exe";
        for (const bool enabled : {true, false}) {
            EnableWindow(owner, enabled);
            observedEditor = false;
            const auto timer = SetTimer(nullptr, 0, 20, CancelEditor);
            assert(timer);
            assert(!ShortcutEditorDialog::ShowNew(app, instance, owner, seed));
            KillTimer(nullptr, timer);
            assert(observedEditor);
            assert((IsWindowEnabled(owner) != FALSE) == enabled);
            assert(!IsWindowVisible(owner));
            assert(GetActiveWindow() != owner);
        }
    }
    DestroyWindow(owner);
    if (SUCCEEDED(com)) CoUninitialize();
    std::cout << "Actual Settings/About and modal presentation checks passed\n";
}
