#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>

#include "Common/output_paths.h"

namespace
{

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct WorkingDirectoryGuard
{
    std::filesystem::path original;
    std::filesystem::path temporary;

    ~WorkingDirectoryGuard()
    {
        std::error_code ec;
        std::filesystem::current_path(original, ec);
        std::filesystem::remove_all(temporary, ec);
    }
};

} // namespace

int main()
{
    try {
        const std::filesystem::path original = std::filesystem::current_path();
        require(common::StartupDir() == original, "startup directory differs from the initial CWD");
        require(common::StartupDir().is_absolute(), "startup directory is not absolute");
        require(common::OutputPath() == original, "empty output path differs from the startup directory");
        const auto record_path = common::OutputPath("debug/record/Ziplines.json");
        require(record_path == original / "debug" / "record" / "Ziplines.json", "record path is not under the startup directory");

        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        auto temporary = std::filesystem::temp_directory_path() / "maaend-output-paths-";
        temporary += std::to_string(stamp);
        require(std::filesystem::create_directory(temporary), "failed to create the temporary directory");
        WorkingDirectoryGuard guard { original, temporary };
        std::filesystem::current_path(temporary);

        require(common::StartupDir() == original, "startup directory changed after chdir");
        require(common::OutputPath("debug/record/Ziplines.json") == record_path, "output path changed after chdir");
        require(
            common::OutputPath("cache/cpp-algo/WebView2") == original / "cache" / "cpp-algo" / "WebView2",
            "cache path changed after chdir");
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
    return 0;
}
