#pragma once

#include "ServiceStreamTypes.h"

#include <set>
#include <vector>
#include <unordered_map>
#include <optional>
#include <string>

namespace MGEN
{
    constexpr int SERVICE_BLOCK_ID_NOT_SET     = -1; // 아직 해당 서비스 블록의 아이디가 할당되지 않은 상태
    constexpr int SERVICE_BLOCK_ID_BOOT_LOADER =  0; // 해당 서비스 블록이 최초 실행 ( 부트 로더 ) 일 경우의 아이디
    constexpr int SERVICE_BLOCK_ID_EXTERN_SERV = 99;

    enum class ServiceBlockModuleType
    {
        DETECTOR_RTSP,          // DETECTOR 분기에서 RTSP 스트리밍 받아서 각 카메라 별 AI Detection
        DETECTOR_SocketIO,      //
    };

    enum class ServiceBlockBuildType
    {
        Unique,
        ByUnit,
    };

    /**
     * @brief 단일 서비스 블록을 구성하는 프로필 정의
     *
     * 각 블록은 여러 개의 입력 블록(다중 입력)을 가질 수 있고,
     * 여러 개의 출력 블록으로 연결될 수 있는 방향성 비순환 그래프(DAG) 구조를 따른다.
     */
    class ServiceBlockProfile
    {
    public:
        ServiceBlockProfile() = delete;
        ServiceBlockProfile( int block_id, ServiceBlockModuleType module_type, ServiceBlockBuildType build_type );

        // ID 및 타입 정보
        int GetCurrID( void ) const noexcept;
        ServiceBlockModuleType GetServiceBlockModuleType( void ) const noexcept;
        ServiceBlockBuildType  GetServiceBlockBuildType ( void ) const noexcept;

        // 입력 관련
        bool HasInput( int from_id ) const;
        void AddInput( int from_id, ServiceStreamType stream_type );
        void RemoveInput( int from_id );

        // 출력 관련
        bool HasOutput( int to_id ) const;
        void AddOutput( int to_id, ServiceStreamType stream_type );
        void RemoveOutput( int to_id );

        // Getter
        const std::unordered_map<int, ServiceStreamType>& GetInputs( void ) const noexcept;
        std::optional<ServiceStreamType> GetInputStreamType( int from_id ) const;
        const std::unordered_map<int, ServiceStreamType>& GetOutputs( void ) const noexcept;
        std::optional<ServiceStreamType> GetOutputStreamType( int to_id ) const;

        // 유틸리티
        bool IsSource( void ) const;
        bool IsSink( void ) const;

    private:
        const int service_block_id_;                // 고유 블록 ID
        const ServiceBlockModuleType module_type_; // 서비스 구현 타입
        const ServiceBlockBuildType  build_type_;   // 서비스 블록의 인스턴스가 유일한지 유닛 단위로 구현해야 하는지

        std::unordered_map<int, ServiceStreamType> input_sources_;  // 입력 블록 목록 (입력 블록 ID → 스트림 타입)
        std::unordered_map<int, ServiceStreamType> output_targets_; // 출력 블록 목록 (출력 블록 ID → 스트림 타입)
    };

    /**
     * @brief 서비스 전체 흐름을 DAG 형태로 구성하는 클래스
     *
     * 각 블록 간 연결은 입력/출력 스트림 타입을 포함하며,
     * 외부 입력/출력, 역방향 인덱스(reverse input map)를 포함한다.
     */
    class EntireServiceBlockProfileGraph
    {
    public:
        // 블록 추가 (ID + 타입만 등록)
        bool AddServiceBlock( int id, ServiceBlockModuleType module_type, ServiceBlockBuildType build_type );

        // 스트림 연결 추가 (from → to)
        bool AddStreamPipeline( int from_id, int to_id, ServiceStreamType stream_type );

        // 외부에서 입력 스트림이 들어올 경우
        bool AddInputFromExtern( int to_id, ServiceStreamType stream_type );

        // 출력 스트림을 외부로 연결할 경우
        bool AddOutputToExtern( int from_id, ServiceStreamType stream_type );

        // 블록 존재 여부
        bool HasBlock( int id ) const;

        // 블록 조회
        std::optional<ServiceBlockProfile> GetBlockProfile( int id ) const;
        std::optional<ServiceBlockProfile> GetBlockProfile( ServiceBlockModuleType type ) const;

        // 블록 목록
        std::vector<int> GetAllBlockIDs( void ) const;

        // 외부 출력 목록
        std::set<ServiceStreamType> GetExternServiceStreamList( void ) const;

        // 정렬 및 검증
        std::vector<int> GetExecutionOrder( void ) const;
        bool DetectCycle( void ) const;

        // 서비스 타입/스트림 타입 검색

        // 시각화
        std::string ToDOTGraph( const std::string& graph_name = "ServiceGraph" ) const;

    private:
        std::unordered_map<int, ServiceBlockProfile> blocks_;            // 전체 블록 목록
        std::unordered_map<int, ServiceStreamType>   extern_streams_;    // 외부 출력 스트림 목록
        std::unordered_map<int, std::set<int>>       reverse_input_map_; // 입력 블록을 역으로 추적하기 위한 인덱스
    };

} // namespace MGEN
