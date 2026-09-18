#pragma once
#include <filesystem>
#include <string>

inline std::filesystem::path fixture (const std::string& name)
{
    return std::filesystem::path{ STILLS_FIXTURE_DIR } / name;
}
