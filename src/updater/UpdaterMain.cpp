#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <aclapi.h>

#include "../platform/SecureElevation.hpp"
#include "UpdaterTransaction.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct Arguments {
    DWORD parentPid{0};
    std::filesystem::path source;
    std::filesystem::path install;
    std::filesystem::path backup;
    std::wstring version;
    std::wstring healthEvent;
};

[[nodiscard]] std::optional<Arguments>
ParseArguments() {
    int argc = 0;
    LPWSTR* argv =
        CommandLineToArgvW(
            GetCommandLineW(),
            &argc);

    if (!argv) {
        return std::nullopt;
    }

    Arguments args;
    bool apply = false;

    const auto cleanup =
        [&]() {
            LocalFree(argv);
        };

    for (int i = 1;
         i < argc;
         ++i) {
        const std::wstring_view key(
            argv[i]);

        if (key == L"--apply") {
            apply = true;
            continue;
        }

        if (i + 1 >= argc) {
            cleanup();
            return std::nullopt;
        }

        const std::wstring_view value(
            argv[++i]);

        if (key ==
            L"--parent-pid") {
            wchar_t* end = nullptr;
            const unsigned long pid =
                std::wcstoul(
                    value.data(),
                    &end,
                    10);

            if (!end ||
                *end != L'\0' ||
                pid == 0) {
                cleanup();
                return std::nullopt;
            }

            args.parentPid =
                static_cast<DWORD>(
                    pid);
        } else if (
            key == L"--source") {
            args.source =
                std::wstring(value);
        } else if (
            key == L"--install") {
            args.install =
                std::wstring(value);
        } else if (
            key == L"--backup") {
            args.backup =
                std::wstring(value);
        } else if (
            key == L"--version") {
            args.version =
                value;
        } else if (
            key ==
                L"--health-event") {
            args.healthEvent =
                value;
        } else {
            cleanup();
            return std::nullopt;
        }
    }

    cleanup();

    if (!apply ||
        args.parentPid == 0 ||
        args.source.empty() ||
        args.install.empty() ||
        args.backup.empty() ||
        args.version.empty() ||
        args.healthEvent.empty()) {
        return std::nullopt;
    }

    return args;
}

[[nodiscard]] bool
IsElevated() {
    HANDLE token = nullptr;

    if (!OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_QUERY,
            &token)) {
        return false;
    }

    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;

    const BOOL ok =
        GetTokenInformation(
            token,
            TokenElevation,
            &elevation,
            sizeof(elevation),
            &bytes);

    CloseHandle(token);

    return ok &&
        elevation.TokenIsElevated !=
            0;
}

[[nodiscard]] std::wstring
QuoteArgument(
    std::wstring_view value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;

    for (const wchar_t c : value) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }

        if (c == L'\"') {
            result.append(
                slashes * 2 + 1,
                L'\\');
            result.push_back(L'\"');
            slashes = 0;
            continue;
        }

        result.append(
            slashes,
            L'\\');
        slashes = 0;
        result.push_back(c);
    }

    result.append(
        slashes * 2,
        L'\\');
    result.push_back(L'\"');
    return result;
}

[[nodiscard]] bool
LaunchNormal(
    const std::filesystem::path& exe,
    std::wstring arguments,
    PROCESS_INFORMATION& process) {
    std::wstring command =
        QuoteArgument(
            exe.wstring());

    if (!arguments.empty()) {
        command += L" ";
        command += arguments;
    }

    std::vector<wchar_t> mutableCommand(
        command.begin(),
        command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);

    return CreateProcessW(
               exe.c_str(),
               mutableCommand.data(),
               nullptr,
               nullptr,
               FALSE,
               CREATE_UNICODE_ENVIRONMENT,
               nullptr,
               exe.parent_path()
                   .c_str(),
               &startup,
               &process) != FALSE;
}

[[nodiscard]] bool
LaunchWithShellToken(
    const std::filesystem::path& exe,
    std::wstring arguments,
    PROCESS_INFORMATION& process) {
    const HWND shell =
        GetShellWindow();

    if (!shell) {
        return false;
    }

    DWORD shellPid = 0;
    GetWindowThreadProcessId(
        shell,
        &shellPid);

    if (shellPid == 0) {
        return false;
    }

    HANDLE shellProcess =
        OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            shellPid);

    if (!shellProcess) {
        return false;
    }

    HANDLE shellToken = nullptr;

    if (!OpenProcessToken(
            shellProcess,
            TOKEN_DUPLICATE |
                TOKEN_ASSIGN_PRIMARY |
                TOKEN_QUERY,
            &shellToken)) {
        CloseHandle(shellProcess);
        return false;
    }

    HANDLE primaryToken = nullptr;

    const BOOL duplicated =
        DuplicateTokenEx(
            shellToken,
            MAXIMUM_ALLOWED,
            nullptr,
            SecurityImpersonation,
            TokenPrimary,
            &primaryToken);

    CloseHandle(shellToken);
    CloseHandle(shellProcess);

    if (!duplicated) {
        return false;
    }

    std::wstring command =
        QuoteArgument(
            exe.wstring());

    if (!arguments.empty()) {
        command += L" ";
        command += arguments;
    }

    std::vector<wchar_t> mutableCommand(
        command.begin(),
        command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);

    const BOOL created =
        CreateProcessWithTokenW(
            primaryToken,
            LOGON_WITH_PROFILE,
            exe.c_str(),
            mutableCommand.data(),
            CREATE_UNICODE_ENVIRONMENT,
            nullptr,
            exe.parent_path()
                .c_str(),
            &startup,
            &process);

    CloseHandle(primaryToken);

    return created != FALSE;
}

[[nodiscard]] bool
LaunchMain(
    const std::filesystem::path& exe,
    std::wstring arguments,
    PROCESS_INFORMATION& process) {
    if (IsElevated()) {
        if (LaunchWithShellToken(
                exe,
                arguments,
                process)) {
            return true;
        }
    }

    return LaunchNormal(
        exe,
        std::move(arguments),
        process);
}

[[nodiscard]] HANDLE
CreateHealthEvent(
    std::wstring_view name) {
    HANDLE token = nullptr;

    if (!OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_QUERY,
            &token)) {
        return nullptr;
    }

    DWORD required = 0;
    GetTokenInformation(
        token,
        TokenUser,
        nullptr,
        0,
        &required);

    if (required == 0 ||
        GetLastError() !=
            ERROR_INSUFFICIENT_BUFFER) {
        CloseHandle(token);
        return nullptr;
    }

    std::vector<std::byte>
        storage(required);
    auto* tokenUser =
        reinterpret_cast<TOKEN_USER*>(
            storage.data());

    if (!GetTokenInformation(
            token,
            TokenUser,
            tokenUser,
            required,
            &required)) {
        CloseHandle(token);
        return nullptr;
    }

    CloseHandle(token);

    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions =
        EVENT_MODIFY_STATE;
    access.grfAccessMode =
        SET_ACCESS;
    access.grfInheritance =
        NO_INHERITANCE;
    access.Trustee.TrusteeForm =
        TRUSTEE_IS_SID;
    access.Trustee.TrusteeType =
        TRUSTEE_IS_USER;
    access.Trustee.ptstrName =
        static_cast<LPWSTR>(
            tokenUser->User.Sid);

    PACL acl = nullptr;

    if (SetEntriesInAclW(
            1,
            &access,
            nullptr,
            &acl) !=
        ERROR_SUCCESS) {
        return nullptr;
    }

    SECURITY_DESCRIPTOR descriptor{};

    if (!InitializeSecurityDescriptor(
            &descriptor,
            SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(
            &descriptor,
            TRUE,
            acl,
            FALSE)) {
        LocalFree(acl);
        return nullptr;
    }

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength =
        sizeof(attributes);
    attributes.lpSecurityDescriptor =
        &descriptor;
    attributes.bInheritHandle = FALSE;

    HANDLE event =
        CreateEventW(
            &attributes,
            TRUE,
            FALSE,
            std::wstring(name).c_str());

    LocalFree(acl);
    return event;
}

void CleanupSelfLater() {
    altrun::win::
        ScheduleTemporaryWorkerSelfCleanup();
}

} // namespace

int WINAPI wWinMain(
    HINSTANCE,
    HINSTANCE,
    PWSTR,
    int) {
    const auto parsed =
        ParseArguments();

    if (!parsed) {
        return 2;
    }

    const Arguments& args =
        *parsed;

    const altrun::updater::
        TransactionPaths transaction{
            .source = args.source,
            .install = args.install,
            .backup = args.backup,
            .version = args.version,
        };

    if (!altrun::updater::
             ValidateSource(
                 transaction) ||
        !WaitForParent(
            args.parentPid)) {
        return 2;
    }

    altrun::updater::
        TransactionJournal journal;

    if (!altrun::updater::
             ApplyPackage(
                 transaction,
                 journal)) {
        altrun::updater::
            Rollback(
                transaction,
                journal);
        return 3;
    }

    HANDLE healthEvent =
        CreateHealthEvent(
            args.healthEvent);

    if (!healthEvent) {
        altrun::updater::Rollback(
            transaction,
            journal);
        return 4;
    }

    const auto mainExe =
        args.install /
        L"ALTRunNext.exe";

    std::wstring mainArguments =
        L"--post-update-health-event " +
        QuoteArgument(
            args.healthEvent);

    PROCESS_INFORMATION child{};

    if (!LaunchMain(
            mainExe,
            mainArguments,
            child)) {
        CloseHandle(healthEvent);
        altrun::updater::Rollback(
            transaction,
            journal);

        PROCESS_INFORMATION restored{};
        if (LaunchMain(
                mainExe,
                L"",
                restored)) {
            CloseHandle(restored.hThread);
            CloseHandle(restored.hProcess);
        }

        return 5;
    }

    CloseHandle(child.hThread);

    const DWORD wait =
        WaitForSingleObject(
            healthEvent,
            30000);

    CloseHandle(healthEvent);

    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(
            child.hProcess,
            ERROR_GEN_FAILURE);
        WaitForSingleObject(
            child.hProcess,
            5000);
        CloseHandle(child.hProcess);

        altrun::updater::Rollback(
            transaction,
            journal);

        PROCESS_INFORMATION restored{};
        if (LaunchMain(
                mainExe,
                L"",
                restored)) {
            CloseHandle(restored.hThread);
            CloseHandle(restored.hProcess);
        }

        return 6;
    }

    CloseHandle(child.hProcess);

    std::error_code ec;
    std::filesystem::remove_all(
        args.backup,
        ec);
    ec.clear();
    std::filesystem::remove_all(
        args.source,
        ec);

    CleanupSelfLater();
    return 0;
}
