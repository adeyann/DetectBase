#include "file_utils.h"
#include "MgenLogger.h"
#include "MgenFileSystem.h"

#include <string>

namespace MGEN
{
    namespace fs = MGEN::fs;

    bool IsValidFile(const std::string& file_path)
    {
        fs::path path(file_path);
        return fs::exists(path) && fs::is_regular_file(path);
    }

    std::string GetFileBaseName(const std::string& file_path)
    {
        fs::path path(file_path);
        return path.filename().string();
    }

    std::string GetAbsolutePath(const std::string& file_path)
    {
        fs::path path(file_path);
        return fs::absolute(path).string();
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    std::string ConcatPath(const std::string& dir, const std::string& file)
    {
        fs::path base(dir);
        base /= file;
        return base.string();
    }

    bool MakeDirectoryWhenNotExist( std::string_view dir_path_view )
    {
        fs::path        dir_path( dir_path_view );
        std::error_code ec;

        if( !fs::exists( dir_path, ec ) )
        {
            // error_code 인자로 throw 없는 non-throwing 오버로드 사용
            const bool created = fs::create_directories( dir_path, ec );
            return created && !ec;
        }
        return true;
    }
}
