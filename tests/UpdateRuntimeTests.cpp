#include "updater/UpdaterTransaction.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using altrun::updater::ApplyPackage;
using altrun::updater::Rollback;
using altrun::updater::TransactionJournal;
using altrun::updater::TransactionPaths;
using altrun::updater::ValidateSource;

namespace {

void Write(
    const fs::path& path,
    std::string_view value) {
    std::error_code ec;
    fs::create_directories(
        path.parent_path(),
        ec);
    assert(!ec);

    std::ofstream output(
        path,
        std::ios::binary |
            std::ios::trunc);
    assert(output);
    output << value;
    output.flush();
    assert(output.good());
}

std::string Read(
    const fs::path& path) {
    std::ifstream input(
        path,
        std::ios::binary);
    assert(input);

    return {
        std::istreambuf_iterator<char>{
            input},
        std::istreambuf_iterator<char>{},
    };
}

void PopulateSource(
    const fs::path& source) {
    Write(
        source / "ALTRunNext.exe",
        "new-app");
    Write(
        source / "Update.exe",
        "new-updater");
    Write(
        source / "Uninstall.exe",
        "new-uninstaller");
    Write(
        source / "VERSION",
        "0.8.0-beta.2\n");
}

void PopulateInstall(
    const fs::path& install) {
    Write(
        install / "ALTRunNext.exe",
        "old-app");
    Write(
        install / "Update.exe",
        "old-updater");
    Write(
        install / "Uninstall.exe",
        "old-uninstaller");
    Write(
        install / "VERSION",
        "0.8.0-beta.1\n");
}

TransactionPaths Paths(
    const fs::path& root) {
    return {
        .source =
            fs::absolute(
                root / "source"),
        .install =
            fs::absolute(
                root / "install"),
        .backup =
            fs::absolute(
                root / "backup"),
        .version =
            L"0.8.0-beta.2",
    };
}

} // namespace

int main() {
    const auto root =
        fs::temp_directory_path() /
        "ALTRunNext-UpdateTransactionTests";

    std::error_code ec;
    fs::remove_all(root, ec);
    ec.clear();
    fs::create_directories(root, ec);
    assert(!ec);

    {
        const auto caseRoot =
            root / "success-rollback";
        const auto paths =
            Paths(caseRoot);

        fs::create_directories(
            paths.source,
            ec);
        assert(!ec);
        fs::create_directories(
            paths.install,
            ec);
        assert(!ec);

        PopulateSource(
            paths.source);
        PopulateInstall(
            paths.install);

        Write(
            paths.source /
                "dict" /
                "new.txt",
            "dictionary");
        Write(
            paths.source /
                "shared" /
                "new.txt",
            "new-shared");
        Write(
            paths.source /
                "data" /
                "must-not-copy.txt",
            "package-data");
        Write(
            paths.install /
                "shared" /
                "keep.txt",
            "keep");
        Write(
            paths.install /
                "data" /
                "user.json",
            "personal");

        assert(ValidateSource(paths));

        TransactionJournal journal;
        assert(
            ApplyPackage(
                paths,
                journal));

        assert(
            Read(
                paths.install /
                "ALTRunNext.exe") ==
            "new-app");
        assert(
            Read(
                paths.install /
                "dict" /
                "new.txt") ==
            "dictionary");
        assert(
            Read(
                paths.install /
                "data" /
                "user.json") ==
            "personal");
        assert(
            !fs::exists(
                paths.install /
                "data" /
                "must-not-copy.txt"));

        Rollback(
            paths,
            journal);

        assert(
            Read(
                paths.install /
                "ALTRunNext.exe") ==
            "old-app");
        assert(
            Read(
                paths.install /
                "Update.exe") ==
            "old-updater");
        assert(
            Read(
                paths.install /
                "Uninstall.exe") ==
            "old-uninstaller");
        assert(
            Read(
                paths.install /
                "VERSION") ==
            "0.8.0-beta.1\n");
        assert(
            !fs::exists(
                paths.install /
                "dict"));
        assert(
            fs::is_directory(
                paths.install /
                "shared"));
        assert(
            Read(
                paths.install /
                "shared" /
                "keep.txt") ==
            "keep");
        assert(
            !fs::exists(
                paths.install /
                "shared" /
                "new.txt"));
        assert(
            Read(
                paths.install /
                "data" /
                "user.json") ==
            "personal");
    }

    {
        const auto caseRoot =
            root / "mid-apply-failure";
        const auto paths =
            Paths(caseRoot);

        fs::create_directories(
            paths.source,
            ec);
        assert(!ec);
        fs::create_directories(
            paths.install,
            ec);
        assert(!ec);

        PopulateSource(
            paths.source);
        PopulateInstall(
            paths.install);

        Write(
            paths.source /
                "00-first.txt",
            "first");
        Write(
            paths.source /
                "99-conflict.txt",
            "cannot-land");
        fs::create_directories(
            paths.install /
                "99-conflict.txt",
            ec);
        assert(!ec);

        TransactionJournal journal;

        assert(
            !ApplyPackage(
                paths,
                journal));
        assert(
            !journal.files.empty());

        Rollback(
            paths,
            journal);

        assert(
            !fs::exists(
                paths.install /
                "00-first.txt"));
        assert(
            fs::is_directory(
                paths.install /
                "99-conflict.txt"));
        assert(
            Read(
                paths.install /
                "ALTRunNext.exe") ==
            "old-app");
    }

    {
        const auto caseRoot =
            root / "source-reparse";
        const auto paths =
            Paths(caseRoot);

        fs::create_directories(
            paths.source,
            ec);
        assert(!ec);
        fs::create_directories(
            paths.install,
            ec);
        assert(!ec);

        PopulateSource(
            paths.source);
        PopulateInstall(
            paths.install);

        const auto external =
            caseRoot / "external";
        fs::create_directories(
            external,
            ec);
        assert(!ec);
        Write(
            external / "outside.txt",
            "outside");

        ec.clear();
        fs::create_directory_symlink(
            external,
            paths.source /
                "linked-dir",
            ec);

        if (!ec) {
            TransactionJournal
                journal;
            assert(
                !ApplyPackage(
                    paths,
                    journal));
            assert(
                !fs::exists(
                    paths.install /
                        "linked-dir"));
        }
    }

    fs::remove_all(root, ec);
    return 0;
}
