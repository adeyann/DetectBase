#include "json/json.hpp"
#include "MgenLogger.h"

#include "EngineProfileParser.h"
#include "ServiceProfileBuilder.h"
#include "NetworkProfileParser.h"

#include "NetworkManager.h"
#include "EngineLoadBalancer.h"
#include "IOStreamManager.h"

#include "DETECTOR.h"
#include "InitMain.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>

#ifdef __SANITIZE_ADDRESS__
#include <sanitizer/lsan_interface.h>
#endif

using namespace MGEN;

// 단일 instance lock — main() 진입점에서 획득. process 종료 시 kernel 이 자동 해제.
//   목적: 동일 컨테이너 내에 DetectBase 가 두 번 떠서 NPU/cam 경쟁이 일어나는 사고 차단
//         (예: 운영자가 실수로 `DetectBase --version` 같이 인자 무시 호출 → 풀 서비스 spawn).
// 위치: /DetectBase/logs/ — DetectBase.log 쓰는 경로라 권한 확보됨.
//       lock file 자체에는 내용 안 씀 (flock 은 file 내용 무관 / inode 의 advisory lock).
static constexpr const char* DETECTBASE_LOCK_FILE = "/DetectBase/logs/.detectbase.lock";
int g_lock_fd = -1; // process lifetime — 의도적으로 close 안 함 (lock 유지).

std::atomic<bool>         g_terminate_flag    { false }; // 종료 요청 플래그
volatile sig_atomic_t     g_force_exit_count  { 0 };     // signal handler 진입 카운트 (async-signal-safe)

// Logger
void InitLogger( void );

// Signal Handler 등록
void RegisterSignalHandler( void (*handler)( int ) );

// Signal Handler
void ExitSignalHandler  ( int Signal );
void IgnoreSignalHandler( int Signal );

#ifdef __SANITIZE_ADDRESS__
// ASan/LSan runtime mid-run leak check (process 안 죽임).
//   kill -SIGUSR1 <pid> 로 trigger. 운영 중 누수 시간순 누적 분석.
void LeakCheckSignalHandler( int /*Signal*/ );
#endif

int main( int argc, char* argv[] )
{
	// -------------------------------------------------------------------------
	// argv guard — 모든 인자는 명시적 case 만 허용. 그 외는 silent ignore 안 하고 FATAL.
	//   배경: Main.cpp 가 과거 int main() 형태로 argv 무수신 → 어떤 인자라도 silent ignore.
	//         실수로 `DetectBase --version` 같은 통상적 호출이 풀 서비스를 한 번 더 spawn 하는
	//         사고로 이어졌음 (2026-05-26, NPU 양분 → DFPS 50% 하락).
	// -------------------------------------------------------------------------
	for( int i = 1; i < argc; ++i ) {
		const std::string a( argv[i] );
		if( a == "--version" || a == "-v" ) {
			std::cout << "DetectBase " << GetApplicationVersion() << std::endl;
			return 0;
		}
		if( a == "--help" || a == "-h" ) {
			std::cout
				<< "Usage: DetectBase\n"
				<< "  --version, -v   Print version and exit\n"
				<< "  --help,    -h   Print this help and exit\n"
				<< "(no positional or other flag arguments accepted)\n";
			return 0;
		}
		std::cerr << "[FATAL] Unknown argument: " << a
		          << " (use --help for usage)" << std::endl;
		return 2;
	}

	// -------------------------------------------------------------------------
	// Single-instance lock — argv 우회 (직접 호출 / 다른 인자 / supervisor 중복 spawn) 까지 차단.
	//   flock(2) advisory lock — process 종료 시 kernel 이 자동 해제, stale lock file 무관.
	//   Logger init 보다 먼저 수행 — duplicate process 가 log file 에 banner 찍지 못하게.
	// -------------------------------------------------------------------------
	g_lock_fd = open( DETECTBASE_LOCK_FILE, O_CREAT | O_RDWR, 0644 );
	if( g_lock_fd < 0 ) {
		std::cerr << "[FATAL] cannot open lock file " << DETECTBASE_LOCK_FILE
		          << ": " << std::strerror( errno ) << std::endl;
		return 4;
	}
	if( flock( g_lock_fd, LOCK_EX | LOCK_NB ) < 0 ) {
		std::cerr << "[FATAL] another DetectBase instance is running "
		          << "(lock " << DETECTBASE_LOCK_FILE << " held). Aborting."
		          << std::endl;
		// g_lock_fd 는 close 안 해도 process exit 시 자동 해제됨.
		return 3;
	}
	// g_lock_fd 는 의도적으로 close 안 함 — lock 을 process lifetime 동안 유지.

	// Logger initialize first
	InitLogger();

	// DETECTOR 서비스 단일 구성
	auto App = std::make_unique<Service_DETECTOR>();

	// SIGPIPE 무시 (소켓 통신 중 끊김 시 SIGPIPE 발생 방지)
	struct sigaction sa_pipe {};
	sa_pipe.sa_handler = SIG_IGN;
	sigemptyset( &sa_pipe.sa_mask );
	sigaction( SIGPIPE, &sa_pipe, nullptr );

	// 초기화 동안에는 IgnoreSignalHandler 등록 (강제 종료 방지)
	RegisterSignalHandler( IgnoreSignalHandler );

#ifdef __SANITIZE_ADDRESS__
	// SIGUSR1 → LSan recoverable leak check (mid-run, runtime 누수 시간순 분석용).
	//   ASan 빌드 시만 활성. production binary 영향 0.
	{
		struct sigaction sa_usr1 {};
		sa_usr1.sa_handler = LeakCheckSignalHandler;
		sigemptyset( &sa_usr1.sa_mask );
		sigaction( SIGUSR1, &sa_usr1, nullptr );
	}
#endif

	if( !App->Initialize() ) {
		App->Quit();
		return 1;  // F-F1-06: 비정상 종료 명시 (docker exit code → restart 트리거)
	}

	// 초기화 이후에는 정상 종료 핸들러로 교체
	RegisterSignalHandler( ExitSignalHandler );

	if( !App->Run() ) {
		App->Quit();
		return 1;  // F-F1-06: 비정상 종료 명시
	}

	App->WaitUntilQuitSignal( g_terminate_flag );

	return 0;
}

void InitLogger( void )
{
	// 빌드 type 무관 File logger 사용 — monitor.sh 의 file 기반 grep (reset/wd/eos/err/warn 등)
	// 추적 capability 가 운영 + 진단 모두에 필수. 이전 Debug → Console logger 분기는 file 로그 0 bytes
	// 결함 (2026-05-28 진단 중 발견 — InferenceCounter [DFPS] / DBG_PROF / event_detected 모두 file 에 미보존).
	LoggerConfig conf {};
	MGEN::initLogger(
		conf.setLogType( MGEN::LogType::File )
			.setLogSaveFile( std::string { APPLICATION_LOG_PATH } )
			.setReOpenIntervals( 60/*S*/ * 60/*M*/ * 24/*H*/ * 28/*Day*/ )
	);
	std::cout << "LOG SAVE PATH : " << APPLICATION_LOG_PATH << " (build=" << PROGRAM_BUILD_TYPE << ")" << std::endl;
}

// SIGINT 핸들러를 등록하는 헬퍼 (sigaction 사용으로 동작 일관성 보장)
void RegisterSignalHandler( void (*handler)( int ) )
{
	struct sigaction sa {};
	sa.sa_handler = handler;
	sigemptyset( &sa.sa_mask );
	sa.sa_flags = SA_RESTART;
	sigaction( SIGINT, &sa, nullptr );
}

// Handler about signal/interupt
void ExitSignalHandler( int Signal )
{
	g_terminate_flag.store( true, std::memory_order_relaxed );
	std::ignore = Signal; // 경고 방지
}

// 초기화 중 SIGINT 처리: 5회까지 무시, 이후 종료. async-signal-safe 함수만 사용
void IgnoreSignalHandler( int Signal )
{
	if( ++g_force_exit_count <= 5 ) {
		// MLOG_* 는 async-signal-safe 가 아니므로 write(2) 로 직접 출력
		static const char msg[] = "[WARN] Program cannot be terminated while Program loading.\n";
		ssize_t written = write( STDERR_FILENO, msg, sizeof( msg ) - 1 );
		std::ignore = written; // 경고 방지
		return;
	}
	ExitSignalHandler( Signal );
}

#ifdef __SANITIZE_ADDRESS__
// SIGUSR1 → LSan recoverable leak check (process 안 죽임).
//   ASan run 중간에 leak 상태 stderr 출력. 시간 구간 별 누적 분석용.
void LeakCheckSignalHandler( int /*Signal*/ )
{
	static const char msg[] = "[ASAN] mid-run leak check triggered (SIGUSR1)\n";
	ssize_t written = write( STDERR_FILENO, msg, sizeof( msg ) - 1 );
	std::ignore = written;
	__lsan_do_recoverable_leak_check();
}
#endif
