#include "MgenLogger.h"
#include "MgenFileSystem.h"
#include "CorrelationContext.h"
#include "MetricsRegistry.h"

#include <cstdio>
#include <cstdarg>

#include <iostream>

namespace MGEN { // Mgensolution's default namespace

    LoggerConfig::LoggerConfig()
        : log_print_out_type( LogType::Console )
        , log_prefix_colored( LogPrefix::OnColor )
        , log_reopen_seconds( DEFAULT_REOPEN_SECS )
        , log_save_file_name( "" )
    {
        //
    }

    LoggerConfig& LoggerConfig::setLogType( const LogType flag )
    {
        this->log_print_out_type = flag;
        return *this;
    }

    LoggerConfig& LoggerConfig::setPrefixUseColor( const LogPrefix flag )
    {
        this->log_prefix_colored = flag;
        return *this;
    }

    LoggerConfig& LoggerConfig::setReOpenIntervals( const unsigned int reopen_sec )
    {
        this->log_reopen_seconds = reopen_sec;
        return *this;
    }

    LoggerConfig& LoggerConfig::setLogSaveFile( const std::string& file_name )
    {
        this->log_save_file_name = file_name;
        return *this;
    }

    LoggerConfig& LoggerConfig::setEnableJson( const bool flag )
    {
        this->enable_json = flag;
        return *this;
    }

    bool LoggerConfig::isPrefixColored( void ) const
    {
        return this->log_prefix_colored == LogPrefix::OnColor;
    }

    const std::string Logger::timeStamp() noexcept
    {
        // get the time with thread-safe
        const auto tp = std::chrono::system_clock::now();
        const auto tt = std::chrono::system_clock::to_time_t( tp );

        std::tm gmt {};
        gmtime_r( &tt, &gmt );

        // Calculate seconds to decimal places
        std::chrono::duration<double> fractional_seconds =
            ( tp - std::chrono::system_clock::from_time_t( tt ) ) + std::chrono::seconds( gmt.tm_sec );

        // format the string
        std::string buffer( 32, '\0' );
        const int written = std::snprintf( &buffer.front(), buffer.size(), "\r %02d-%02d-%02d %02d:%02d:%06.3f |",
                    gmt.tm_year - 100, gmt.tm_mon + 1, gmt.tm_mday,
                ( gmt.tm_hour + 9/*KST*/ ) % 24, gmt.tm_min, fractional_seconds.count() );
        if( written > 0 ){
            buffer.resize( static_cast<size_t>( written ) );
        }
        return buffer;
    }

    const std::string Logger::timeStampISO8601() noexcept
    {
        // UTC ISO8601: yyyy-mm-ddThh:mm:ss.fffZ
        const auto tp = std::chrono::system_clock::now();
        const auto tt = std::chrono::system_clock::to_time_t( tp );

        std::tm utc {};
        gmtime_r( &tt, &utc );

        // ms 단위 분수 초
        const auto since_epoch = tp.time_since_epoch();
        const auto sec_floor   = std::chrono::duration_cast<std::chrono::seconds>( since_epoch );
        const auto millis      = std::chrono::duration_cast<std::chrono::milliseconds>( since_epoch - sec_floor ).count();

        char buf[64];
        std::snprintf( buf, sizeof( buf ), "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
            utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
            utc.tm_hour, utc.tm_min, utc.tm_sec,
            static_cast<long long>( millis ) );
        return std::string { buf };
    }

    std::string Logger::JsonEscape( const std::string& s )
    {
        std::string out;
        out.reserve( s.size() + 8 );
        for( char c : s ) {
            switch( c ) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b";  break;
                case '\f': out += "\\f";  break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if( static_cast<unsigned char>( c ) < 0x20 ) {
                        char ub[8];
                        std::snprintf( ub, sizeof( ub ), "\\u%04x", static_cast<unsigned char>( c ) );
                        out += ub;
                    } else {
                        out += c;
                    }
            }
        }
        return out;
    }

    const char* Logger::LogLevelName( const LogLevel level ) noexcept
    {
        switch( level ) {
            case LogLevel::TRACE: return "TRACE";
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO";
            case LogLevel::WARN:  return "WARN";
            case LogLevel::ERROR: return "ERROR";
        }
        return "UNKNOWN";
    }

    std::string Logger::FormatJsonLine( const std::string& msg, const LogLevel level )
    {
        // JSON 한 줄: {"ts":"...","lvl":"...","msg":"...","correlation_id":"..."}
        // correlation_id 는 thread-local 에 설정된 경우만 첨부 (빈 값이면 필드 생략).
        const std::string& cid = CorrelationContext::Get();

        std::string out;
        out.reserve( msg.size() + 128 );
        out += "{\"ts\":\"";
        out += timeStampISO8601();
        out += "\",\"lvl\":\"";
        out += LogLevelName( level );
        out += "\",\"msg\":\"";
        out += JsonEscape( msg );
        out += '"';
        if( !cid.empty() ) {
            out += ",\"correlation_id\":\"";
            out += JsonEscape( cid );
            out += '"';
        }
        out += "}\n";
        return out;
    }

    const LogPrefixMap off_color_prefix {
        { LogLevel::TRACE, " [TRACE] " },
        { LogLevel::DEBUG, " [DEBUG] " },
        { LogLevel::INFO,  " [INFO] "  },
        { LogLevel::WARN,  " [WARN] "  },
        { LogLevel::ERROR, " [ERROR] " }
    };
    const LogPrefixMap on_color_prefix {
        { LogLevel::TRACE, " [\x1b[37;1mTRACE\x1b[0m] " }, // White
        { LogLevel::DEBUG, " [\x1b[36;1mDEBUG\x1b[0m] " }, // Sky
        { LogLevel::INFO,  " [\x1b[32;1mINFO\x1b[0m] "  }, // Green
        { LogLevel::WARN,  " [\x1b[33;1mWARN\x1b[0m] "  }, // Yellow
        { LogLevel::ERROR, " [\x1b[31;1mERROR\x1b[0m] " }  // Red
    };

    ConsoleLogger::ConsoleLogger( const LoggerConfig& cfg ) : Logger( cfg )
        , prefixes( cfg.isPrefixColored() ? on_color_prefix : off_color_prefix )
        , json_enabled_( cfg.isJsonEnabled() )
    {
        //
    }

    void ConsoleLogger::log( const std::string& msg, const LogLevel level )
    {
        if( level < LOG_LEVEL_CUTOFF )
            return;

        if( json_enabled_ ) {
            log( Logger::FormatJsonLine( msg, level ) );
            return;
        }

        std::string out;
        out.reserve( msg.length() + 64 );
        out.append( Logger::timeStamp() );
        out.append( prefixes.find( level )->second );
        out.append( msg );
        out.push_back( '\n' );
        log( out );
    }

    void ConsoleLogger::log( const std::string& msg )
    {
        // cout is thread-safe, to avoid multiple threads interleaving on one line
        // though, we make sure to only call the << operator once on std::cout
        // otherwise the << operators from different threads could interleave
        // obviously we don't care if flushes interleave
        // std::lock_guard<std::mutex> lk{lock};
        std::cout << msg;
        std::cout.flush();
    }

    void ConsoleLogger::logClose( void )
    {
        //
    }

    FileLogger::FileLogger( const LoggerConfig& cfg ) : Logger( cfg )
        , json_enabled_( cfg.isJsonEnabled() )
    {
        // grab the file name
        const auto name = cfg.getLogSaveFileName();

        // set file name
        if( name.empty() ){
            // throw 대신 stderr 출력. file_name 빈 채로 객체 살아있음 → 다음 reOpen 도 실패
            // → log() 의 file_stream << 가 silently fail. 프로세스는 계속 동작.
            std::fprintf( stderr, "MGEN::FileLogger - empty 'file_name' config. logger disabled.\n" );
            return;
        }
        this->file_name = name;

        // if we specify an interval
        this->re_open_intervals = std::chrono::seconds( cfg.getLogReOpenSecond() );

        // crack the file open;
        reOpen();
    }

    void FileLogger::log( const std::string& msg, const LogLevel level )
    {
        if( level < LOG_LEVEL_CUTOFF )
            return;

        if( json_enabled_ ) {
            log( Logger::FormatJsonLine( msg, level ) );
            return;
        }

        std::string out;
        out.reserve( msg.length() + 64 );
        out.append( Logger::timeStamp() );
        out.append( off_color_prefix.find( level )->second );
        out.append( msg );
        out.push_back( '\n' );
        log( out );
    }

    void FileLogger::log( const std::string& msg )
    {
        lock.lock();
        file_stream << msg;
        file_stream.flush();
        lock.unlock();
        this->reOpen();
    }

    void FileLogger::logClose( void )
    {
        if( file_stream.is_open() )
            file_stream.close();
    }

    void FileLogger::reOpen()
    {
        // NEW-3: 빈 file_name (logger disabled) 시 매 호출 path 생성/open 시도 회피.
        if( file_name.empty() ) return;

        // check if it should be closed and reopened
        const auto now = std::chrono::system_clock::now();

        std::lock_guard<std::mutex> lck { lock };

        // 지정된 시간 간격(re_open_intervals)보다 크면 파일 재오픈
        if( now - last_re_open > re_open_intervals ) {

            last_re_open = now;

            // 기존 파일 스트림 닫기
            if( file_stream.is_open() ){
                file_stream.close();
            }

            // 파일 존재 여부 확인 및 디렉터리 자동 생성
            MGEN::fs::path logFilePath { file_name };
            MGEN::fs::path logDir = logFilePath.parent_path();

            if( !logDir.empty() && !MGEN::fs::exists( logDir ) ){
                MGEN::fs::create_directories( logDir );
            }

            // 파일 다시 열기 (기존 파일 유지 & 새로운 로그 추가)
            file_stream.open( file_name, std::ios::out | std::ios::app );
            if( !file_stream.is_open() ){
                // throw 대신 stderr 출력. file_stream 미열림 상태로 둠
                // → 다음 log() 의 file_stream << 가 silently fail (ios::failbit 설정).
                // 다음 reOpen 주기에서 재시도 가능.
                std::fprintf( stderr, "MGEN::FileLogger - Failed to reopen log file: %s\n", file_name.c_str() );
            }
        }
    }

    LoggerFactory::LoggerFactory()
    {
        creators.emplace( LogType::Console, []( const LoggerConfig& cfg )->std::unique_ptr<Logger> { return std::make_unique<ConsoleLogger>( cfg ); } );
        creators.emplace( LogType::File,    []( const LoggerConfig& cfg )->std::unique_ptr<Logger> { return std::make_unique<FileLogger>( cfg );    } );
    }

    std::unique_ptr<Logger> LoggerFactory::produce( const LoggerConfig& cfg ) const
    {
        auto it = creators.find( cfg.getLogPrintOutType() );

        if( it != creators.end() )
            return it->second( cfg );

        // Unknown LogType 은 enum class 라 컴파일 시점에 거의 차단됨. runtime 도달 시 비정상.
        // throw 대신 stderr 로 알림 후 nullptr 반환 (호출자 get_logger 가 검사).
        std::fprintf( stderr, "MGEN::LoggerFactory::produce - Unknown LogType (%d). Returning nullptr.\n",
            static_cast<int>( cfg.getLogPrintOutType() ) );
        return nullptr;
    }

    LoggerFactory& get_factory()
    {
        static LoggerFactory factory_singleton {};
        return factory_singleton;
    }

    Logger* get_logger( const LoggerConfig& config )
    {
        static std::unique_ptr<Logger> singleton( get_factory().produce( config ) );
        if( !singleton )
            return nullptr;
        return singleton.get();
    }

    void initLogger( const LoggerConfig& config )
    {
        get_logger( config );
    }

    static void logInternal( const std::string& message, const LogLevel type )
    {
        // Logger 자체가 throw 하면 catch 외에는 복구할 수단이 없으므로,
        // 외부 의존(파일 IO 등) 보호 차원에서 try/catch 유지.
        try {
            if( auto logger = get_logger(); logger != nullptr ) logger->log( message, type );
        }
        catch( int error_code ) {
            std::printf( "MGEN::Logger - %s():%d occured errors, code = %d, msg = %s\n",
                    __func__, __LINE__, error_code, message.c_str() );
            // 운영 가시성: Logger 자체 실패 카운트. 메트릭 호출도 실패 가능 → nested try.
            try {
                MGEN::MetricsRegistry::Instance().IncrementCounter(
                    "detectbase_errors_total", { { "type", "logger_fail" } } );
            } catch( ... ) { /* metric 도 실패 시 stderr 만 — 위에서 이미 출력함 */ }
        }
        catch( ... ) {
            std::printf( "MGEN::Logger - %s():%d occured errors, unknowned error, msg = %s\n",
                    __func__, __LINE__, message.c_str() );
            try {
                MGEN::MetricsRegistry::Instance().IncrementCounter(
                    "detectbase_errors_total", { { "type", "logger_fail" } } );
            } catch( ... ) { /* metric 도 실패 시 stderr 만 */ }
        }
    }

    void logTrace( const std::string& message )
    {
        MGEN::logInternal( message, LogLevel::TRACE );
    }

    void logDebug( const std::string& message )
    {
        MGEN::logInternal( message, LogLevel::DEBUG );
    }

    void logInfo ( const std::string& message )
    {
        MGEN::logInternal( message, LogLevel::INFO  );
    }

    void logWarn ( const std::string& message )
    {
        MGEN::logInternal( message, LogLevel::WARN  );
    }

    void logError( const std::string& message )
    {
        MGEN::logInternal( message, LogLevel::ERROR );
    }

    std::string GetLogString( const char* fmt, ... )
    {
        // F-F6-03: thread_local buffer 로 매 호출마다 200KB 스택 할당 + zero-init 비용 회피.
        // vsnprintf 가 결과 문자열에 자동 null termination → 사전 zero-init 불필요.
        thread_local char buffer[MAX_LOG_MSG_LEN];

        va_list args {};
        va_start( args, fmt );
        vsnprintf( buffer, MAX_LOG_MSG_LEN, fmt, args );
        va_end( args );

        return std::string { buffer };
    }

}; // namespace MGEN
