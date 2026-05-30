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

        // race-safe: 다른 thread 가 동시에 같은 dir 을 생성하면 우리 호출의 created 가 false
        // 가 되지만 ec 는 빈 채. 따라서 `created && !ec` 으로 판정하면 spurious false 가 됨
        // (자정 dir rollover 시점 cam 별 EventThreadRunner 동시 호출에서 관측).
        // fs::create_directories 의 표준 행동: 이미 존재하면 ec 빈 채로 false 반환 = 성공.
        // 따라서 created 는 무시하고 ec 만 본다.
        fs::create_directories( dir_path, ec );
        if( !ec ) return true;

        // ec 있음 — race 시점 transient EEXIST 가능. dir 실재 여부 재확인 후 결과 결정.
        std::error_code ec_check;
        return fs::exists( dir_path, ec_check ) && !ec_check;
    }
}
