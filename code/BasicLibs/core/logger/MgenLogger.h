#ifndef __MGEN_LOGGER_H__
#define __MGEN_LOGGER_H__

/** -------------------------------------------------------
 *  MgenSolution's Thread-safe Logger
 * --------------------------------------------------------
 *  Author      : KIM UnYoung
 *  Last Update : 2025.03.18
 * --------------------------------------------------------
 *  Ref. : https://gist.github.com/39f2e39273c625d96790.git
 * -------------------------------------------------------- */

#include <string>
#include <chrono>
#include <mutex>
#include <memory>
#include <fstream>
#include <unordered_map>
#include <functional>

/* ----------------------------------------------------------------------------------------------------------------
 | Define Shortcut
 +--------------------------------------------------------------------------------------------------------------- */
#define MLOG_TRACE( fmt, args... ) MGEN::logTrace( MGEN::GetLogString(fmt, ##args) )
#define MLOG_DEBUG( fmt, args... ) MGEN::logDebug( MGEN::GetLogString(fmt, ##args) )
#define MLOG_INFO( fmt, args... )  MGEN::logInfo ( MGEN::GetLogString(fmt, ##args) )
#define MLOG_WARN( fmt, args... )  MGEN::logWarn ( MGEN::GetLogString(fmt, ##args) )
#define MLOG_ERROR( fmt, args... ) MGEN::logError( MGEN::GetLogString(fmt, ##args) )
/* ---------------------------------------------------------------------------------------------------------------- */

namespace MGEN { // Mgensolution's default namespace

    // Const define values for setting
    constexpr size_t MAX_LOG_MSG_LEN     = 204800; /* 200 KB (200 * 1024) */
    constexpr uint   DEFAULT_REOPEN_SECS = 3600;

    // Const define values for error handling
    constexpr int ERROR_LOG_PTR_FREE_ALREADY = -1;
    constexpr int ERROR_LOG_MSG_LEN_OVERFLOW = -2;

    // The Log levels we support
    enum class LogLevel  : uint8_t { TRACE, DEBUG, INFO, WARN, ERROR };
    enum class LogPrefix : uint8_t { OffColor, OnColor };
    enum class LogType   : uint8_t { NotSet, Console, File };

    // set constexpr log cut level
    #if defined(DEBUG_MODE)
      constexpr LogLevel LOG_LEVEL_CUTOFF = LogLevel::TRACE;
    #else
      constexpr LogLevel LOG_LEVEL_CUTOFF = LogLevel::INFO;
    #endif

    // Logger Configure Setting Class
    class LoggerConfig
    {
    public:
        // Constructor
        LoggerConfig();

        // Destructor
        ~LoggerConfig() = default;

        // Setter
        LoggerConfig& setLogType( const LogType flag );
        LoggerConfig& setPrefixUseColor( const LogPrefix flag );
        LoggerConfig& setReOpenIntervals( const unsigned int reopen_sec );
        LoggerConfig& setLogSaveFile( const std::string& file_name );
        LoggerConfig& setEnableJson( const bool flag );

        // Getter
        LogType      getLogPrintOutType( void ) const { return this->log_print_out_type; }
        LogPrefix    getLogPrefixUseClr( void ) const { return this->log_prefix_colored; }
        unsigned int getLogReOpenSecond( void ) const { return this->log_reopen_seconds; }
        std::string  getLogSaveFileName( void ) const { return this->log_save_file_name; }

        // Checker
        bool isPrefixColored( void ) const;
        bool isJsonEnabled  ( void ) const { return this->enable_json; }

    private:
        LogType      log_print_out_type = LogType::Console;
        LogPrefix    log_prefix_colored = LogPrefix::OnColor;
        unsigned int log_reopen_seconds = DEFAULT_REOPEN_SECS;
        std::string  log_save_file_name = "";
        // P53: JSON 한 줄 형식 출력. default = true.
        // false 로 두면 기존 KST 텍스트 형식 그대로 (즉시 원복 수단).
        bool         enable_json        = true;
    }; // cls:LoggerConfig

    // Need to define it because there is no hasher for enumeration classes
    struct log_lvl_enum_hasher {
        template <typename T> std::size_t operator()( T t ) const {
            return static_cast<std::size_t>( t );
        }
    };
    using LogPrefixMap = std::unordered_map<LogLevel, std::string, log_lvl_enum_hasher>;

    // Logger base class, not pure virtual so you can use as a null logger if you want
    class Logger
    {
    public:
        // Default constructor delete
        Logger() = delete;

        // must construct with config, but default Logger class not work use that
        explicit Logger( const LoggerConfig& cfg ) {};

        // Destructor must virtual
        virtual ~Logger() = default;

        virtual void log( const std::string& msg, const LogLevel level ) = 0;
        virtual void log( const std::string& msg ) = 0;
        virtual void logClose( void ) = 0;

    protected:
        // 기존 KST 텍스트용. format : 'yr-mo-dy hr:mn:sc.xxx' (KST = UTC+9 하드코딩)
        static const std::string timeStamp() noexcept;

        // JSON 모드용. format : 'yr-mo-dyThr:mn:sc.xxxZ' (UTC ISO8601)
        static const std::string timeStampISO8601() noexcept;

        // JSON string field escape (",\,\b,\f,\n,\r,\t, control chars).
        // nlohmann::json 의존 없이 가벼운 직접 구현.
        static std::string JsonEscape( const std::string& s );

        // LogLevel → "TRACE"/"DEBUG"/"INFO"/"WARN"/"ERROR"
        static const char* LogLevelName( const LogLevel level ) noexcept;

        // JSON 한 줄 조립. msg/level/ts 자동 첨부. 끝에 '\n' 포함.
        // Stage B 에서 correlation_id/src 필드도 여기서 추가될 예정.
        static std::string FormatJsonLine( const std::string& msg, const LogLevel level );

    protected:
        mutable std::mutex lock;
    }; // cls:Logger

    // Logger that writes to console std out
    class ConsoleLogger : public Logger
    {
    public:
        // Default constructor delete
        ConsoleLogger() = delete;

        // must construct with config
        explicit ConsoleLogger( const LoggerConfig& cfg );

        virtual void log( const std::string& msg, const LogLevel level ) override final;
        virtual void log( const std::string& msg ) override final;
        virtual void logClose( void ) override final;

    protected:
        const LogPrefixMap prefixes;
        const bool         json_enabled_;
    }; // cls:ConsoleLogger

    // Logger that writes to File
    class FileLogger : public Logger
    {
    public:
        // Default constructor delete
        FileLogger() = delete;

        // must construct with config
        explicit FileLogger( const LoggerConfig& cfg );

        // Destructor
        ~FileLogger() { logClose(); }

        virtual void log( const std::string& msg, const LogLevel level ) override final;
        virtual void log( const std::string& msg ) override final;
        virtual void logClose( void ) override final;

    protected:
        void reOpen( void );

        // NEW-5: ctor 의 빈 file_name early return 시 멤버 미초기화 → log() 호출 시 reOpen 비교 UB 차단.
        //        헤더 default-init 으로 garbage 값 차단. 정상 경로는 ctor 가 덮어씀.
        std::string   file_name;
        std::ofstream file_stream;
        std::chrono::seconds                  re_open_intervals { std::chrono::seconds( DEFAULT_REOPEN_SECS ) };
        std::chrono::system_clock::time_point last_re_open      {};
        const bool    json_enabled_;
    }; // cls:FileLogger

    // A factory that can create Loggers ( that derive from 'Logger' ) via function pointers
    using LoggerCreator = std::function<std::unique_ptr<Logger>(const LoggerConfig&)>;
    class LoggerFactory
    {
    public:
        // Constructor
        LoggerFactory();

        // Producer
        std::unique_ptr<Logger> produce( const LoggerConfig& cfg ) const;

    protected:
        std::unordered_map<LogType, LoggerCreator, log_lvl_enum_hasher> creators;
    }; // cls:LoggerFactory

    LoggerFactory& get_factory(); // statically get a factory
    Logger*        get_logger( const LoggerConfig& config = LoggerConfig {} ); // get at the singleton
    void           initLogger( const LoggerConfig& config = LoggerConfig {} ); // configure the singleton (ONCE ONLY)

    // MACROS
    void logTrace( const std::string& message );
    void logDebug( const std::string& message );
    void logInfo ( const std::string& message );
    void logWarn ( const std::string& message );
    void logError( const std::string& message );

    // wrapping variable number of arguments format-string to std::string
    std::string GetLogString( const char* fmt, ... );

}; // namespace MGEN
#endif
