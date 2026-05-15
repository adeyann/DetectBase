#pragma once

#include <string_view>
#include <string>
#include <chrono>

namespace MGEN
{
    // V-05: chrono_literals 헤더에서 노출 안 함 (사용처 없음).

    bool IsValidFile(const std::string& file_path);

    std::string GetFileBaseName(const std::string& file_path);
    std::string GetAbsolutePath(const std::string& file_path);

    std::string ConcatPath(const std::string& dir, const std::string& file);

    bool MakeDirectoryWhenNotExist( std::string_view dir_path_view );

} // namespace MGEN
