#include "json/json.hpp"
#include "MgenLogger.h"

#include "EngineProfileParser.h"
#include "ServiceProfileBuilder.h"
#include "NetworkProfileParser.h"

#include "NetworkManager.h"
#include "EngineLoadBalancer.h"
#include "IOStreamManager.h"

#include "DETECTOR.h"

#include <atomic>
#include <csignal>
#include <cstring>
#include <unistd.h>

#ifdef __SANITIZE_ADDRESS__
#include <sanitizer/lsan_interface.h>
#endif

using namespace MGEN;

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

int main()
{
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
	LoggerConfig conf {};
	if( std::string{ PROGRAM_BUILD_TYPE } == std::string { "Release" } ) {
		MGEN::initLogger(
			conf.setLogType( MGEN::LogType::File )
				.setLogSaveFile( std::string { APPLICATION_LOG_PATH } )
				.setReOpenIntervals( 60/*S*/ * 60/*M*/ * 24/*H*/ * 28/*Day*/ )
		);
		std::cout << "LOG SAVE PATH : " << APPLICATION_LOG_PATH << std::endl;
	}
	else {
		MGEN::initLogger( conf.setLogType( MGEN::LogType::Console ).setPrefixUseColor( MGEN::LogPrefix::OnColor ) );
	}
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
