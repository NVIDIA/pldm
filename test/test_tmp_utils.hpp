#pragma once

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace pldm::test
{

inline std::filesystem::path tempRoot()
{
    if (const char* testTmpDir = std::getenv("TEST_TMPDIR");
        testTmpDir != nullptr && *testTmpDir != '\0')
    {
        return testTmpDir;
    }

    return std::filesystem::temp_directory_path();
}

inline std::filesystem::path ensureTempDir(std::string_view name = {})
{
    auto root = tempRoot();
    if (!name.empty())
    {
        root /= name;
    }
    std::filesystem::create_directories(root);
    return root;
}

inline std::filesystem::path makeTempDir(const char* pattern)
{
    auto templ = (ensureTempDir() / pattern).string();
    std::vector<char> buffer(templ.begin(), templ.end());
    buffer.push_back('\0');
    auto* created = ::mkdtemp(buffer.data());
    if (created == nullptr)
    {
        throw std::runtime_error("mkdtemp failed");
    }
    return created;
}

inline std::filesystem::path makeTempFile(const char* pattern)
{
    auto templ = (ensureTempDir() / pattern).string();
    std::vector<char> buffer(templ.begin(), templ.end());
    buffer.push_back('\0');
    const int fd = ::mkstemp(buffer.data());
    if (fd < 0)
    {
        throw std::runtime_error("mkstemp failed");
    }
    ::close(fd);
    return buffer.data();
}

} // namespace pldm::test
