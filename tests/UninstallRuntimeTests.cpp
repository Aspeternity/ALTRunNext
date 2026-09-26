// Include the production deletion/validation implementation; tests only touch a
// uniquely named temporary fixture, never a user's registration or service.
#define wWinMain UninstallEntryForTests
#include "../src/uninstaller/UninstallMain.cpp"
#undef wWinMain
#include <cassert>
#include <iostream>

int main() {
    const auto base = std::filesystem::temp_directory_path() /
        (L"ALTRun-Uninstall-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(base);
    const auto makeInstall = [&](std::wstring_view name) {
        auto path = base / name;
        std::filesystem::create_directories(path / L"data");
        for (auto file : {L"ALTRunNext.exe", L"VERSION", L"Uninstall.exe"})
            std::ofstream(path / file) << "fixture";
        assert(ValidateInstallRoot(path));
        return path;
    };
    {
        const auto install = makeInstall(L"只读文件 preserve");
        const auto user = install / L"data" / L"commands.json";
        std::ofstream(user) << "keep me";
        const auto readonly = install / L"readonly.txt";
        std::ofstream(readonly) << "read only";
        assert(SetFileAttributesW(readonly.c_str(), FILE_ATTRIBUTE_READONLY));
        RemovalFailure failure;
        assert(RemoveInstallation(install, false, failure, nullptr));
        assert(std::filesystem::exists(user));
        assert(!std::filesystem::exists(readonly));
        assert(!std::filesystem::exists(install / kRecoveryMarker));
    }
    {
        const auto install = makeInstall(L"locked retry");
        const auto locked = install / L"locked.bin";
        std::ofstream(locked) << "lock";
        HANDLE handle = CreateFileW(locked.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        assert(handle != INVALID_HANDLE_VALUE);
        RemovalFailure failure;
        assert(!RemoveInstallation(install, false, failure, nullptr));
        assert(failure.path == locked && failure.error == ERROR_SHARING_VIOLATION);
        assert(ValidateInstallRoot(install));
        assert(HasRecoveryMarker(install));
        CloseHandle(handle);
        assert(RemoveInstallation(install, false, failure, nullptr));
    }
    {
        const auto install = makeInstall(L"partial retry");
        assert(WriteRecoveryMarker(install));
        std::filesystem::remove(install / L"ALTRunNext.exe");
        std::filesystem::remove(install / L"VERSION");
        assert(ValidateInstallRoot(install));
        DirectoryHandle lease;
        RemovalFailure failure;
        assert(AcquireDirectoryDeleteLease(install, lease, failure, 100));
        assert(RemoveInstallation(install, true, failure, &lease));
        assert(!std::filesystem::exists(install));
    }
    {
        const auto install = makeInstall(L"final failure retry");
        RemovalFailure failure;
        // Simulate failure at the final root operation, after the anchors are gone.
        assert(!RemoveInstallation(install, true, failure, nullptr));
        assert(failure.error == ERROR_INVALID_HANDLE);
        assert(HasRecoveryMarker(install) && ValidateInstallRoot(install));
        DirectoryHandle lease;
        assert(AcquireDirectoryDeleteLease(install, lease, failure, 100));
        assert(RemoveInstallation(install, true, failure, &lease));
    }
    {
        // Windows junction creation does not need the symlink privilege.
        const auto install = makeInstall(L"junction");
        const auto outside = base / L"outside";
        std::filesystem::create_directories(outside);
        const auto sentinel = outside / L"keep.txt";
        std::ofstream(sentinel) << "outside installation";
        const auto junction = install / L"linked";
        std::wstring command = L"cmd.exe /d /c mklink /J " + QuoteArgument(junction.wstring()) + L" " + QuoteArgument(outside.wstring());
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        assert(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, base.c_str(), &startup, &process));
        assert(WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0);
        DWORD code = 1;
        assert(GetExitCodeProcess(process.hProcess, &code) && code == 0);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        assert(!ValidateInstallRoot(junction));
        RemovalFailure failure;
        assert(RemoveAllWithRetry(junction, failure));
        assert(std::filesystem::exists(sentinel));
    }
    RemovalFailure failure;
    assert(RemoveAllWithRetry(base, failure));
    std::cout << "Uninstall production cleanup regressions passed\n";
}
