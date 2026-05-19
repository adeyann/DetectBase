#pragma once

#include "SafeQueue.h"          // SafeQueue 템플릿 클래스 (스레드 안전 큐 구현 가정)
#include "ReplyDispatcher.h"    // ReplyDispatcher 템플릿 클래스
#include "ReplyDispatcherWithCleaner.h"
#include "MgenTypes.h"          // EventDataOnlyJson, EventDataWithRawImages 등 타입 정의 가정
#include "string_utils.h"       // is_string_like 타입 특성 정의 가정
#include "ServiceProfile.h"
#include "NetworkManager.h"

// 필요한 타입 정의를 위한 헤더
#include "InferObject.h"        // InferObject 타입 정의 가정
#include "json/json_fwd.hpp"    // nlohmann::json 전방 선언
#include "MgenProto.grpc.pb.h"  // gRPC 관련 타입 정의 가정 (필요시)
#include <libavformat/avformat.h> // AVFrame 정의

// STL
#include <string>
#include <memory>               // std::shared_ptr, std::make_shared
#include <unordered_map>        // std::unordered_map
#include <type_traits>          // std::decay_t, std::is_integral_v, std::is_same_v
#include <utility>              // std::move (필요시)
#include <variant>              // std::variant, std::get_if, std::monostate
#include <string_view>          // std::string_view (필요시)
#include <mutex>                // std::mutex, std::lock_guard

namespace MGEN
{
    /**
     * @enum QueueType
     * @brief 관리할 큐의 종류를 나타내는 열거형.
     * IOStreamManager 클래스의 함수들에서 사용됩니다.
     */
    enum class QueueType
    {
        SIO_EVENT,             // SocketIO 이벤트 큐 (nlohmann::json)
        GRPC_CLIENT_RSP,       // gRPC 클라이언트 응답 큐 (nlohmann::json)
        GRPC_SERVER_REQ_JSON,  // gRPC 서버 JSON 요청 큐 (EventDataOnlyJson)
        GRPC_SERVER_REQ_IMAGE, // gRPC 서버 이미지 요청 큐 (EventDataWithRawImages)
        RTSP_AVFRAME,          // RTSP 프록시 AVFrame 큐 (sptr<AVFrame>)
    };

    // gRPC 응답 처리를 위한 디스패처 타입 정의
    using GrpcEventJsonDispatcher = ReplyDispatcherWithCleaner<EventDataOnlyJson>;

    /**
     * @class IOStreamManager
     * @brief 다양한 종류의 스레드 안전 큐와 디스패처를 관리하는 클래스.
     *
     * SocketIO 이벤트, gRPC 요청/응답, RTSP 프레임 등 다양한 비동기 데이터를
     * 식별자(문자열 또는 정수)를 키로 사용하여 큐에 저장하고 관리합니다.
     * 내부 맵 접근 시 스레드 안전성을 보장하기 위해 뮤텍스를 사용합니다.
     */
    class IOStreamManager
    {
    private:
        // 각 큐가 저장할 요소 타입에 따른 SafeQueue 타입 정의
        using JsonQueue             = SafeQueue<nlohmann::json>;
        using GrpcEventJsonQueue    = SafeQueue<EventDataOnlyJson>;
        using GrpcEventImageQueue   = SafeQueue<EventDataWithRawImages>;
        using AVFramePtrQueue       = SafeQueue<std::shared_ptr<AVFrame>>;

        // 다양한 큐 타입의 shared_ptr를 담을 수 있는 variant 타입 정의
        using SptrQueueVariant = std::variant<
            std::monostate, // 비어있는 상태 또는 오류 상태 표현 가능
            std::shared_ptr<JsonQueue>,
            std::shared_ptr<GrpcEventJsonQueue>,
            std::shared_ptr<GrpcEventImageQueue>,
            std::shared_ptr<AVFramePtrQueue>
        >;

    public:
        /**
         * @brief 지정된 타입과 키에 해당하는 SafeQueue를 생성하고 등록합니다.
         * @tparam K 키의 타입 (문자열 유사 타입 또는 정수 타입).
         * @param type 생성할 큐의 종류 (MGEN::QueueType).
         * @param key 큐를 식별하는 키.
         * @return 성공 시 true, 실패(지원하지 않는 타입 또는 키) 시 false.
         * @note 내부 맵 접근은 뮤텍스로 보호됩니다.
         */
        template <typename K>
        bool RegisterSafeQueue( const QueueType type, const K& key )
        {
            std::lock_guard<std::mutex> lock(map_mutex_); // 뮤텍스 잠금

            using Key = std::decay_t<K>; // 키 타입의 decay된 타입 (const, & 제거)

            // 키 타입이 문자열 유사 타입인 경우
            if constexpr ( is_string_like<Key>::value ){
                std::string k = std::string {key}; // 문자열로 변환
                switch( type )
                {
                    case QueueType::SIO_EVENT:
                        sio_event_internal_q_map_[k] = std::make_shared<JsonQueue>();
                        return true;
                    case QueueType::GRPC_CLIENT_RSP:
                        grpc_client_respond_internal_q_map_[k] = std::make_shared<JsonQueue>();
                        return true;
                    case QueueType::GRPC_SERVER_REQ_JSON:
                        grpc_server_request_json_internal_q_map_[k] = std::make_shared<GrpcEventJsonQueue>();
                        return true;
                    case QueueType::GRPC_SERVER_REQ_IMAGE:
                        grpc_server_request_image_internal_q_map_[k] = std::make_shared<GrpcEventImageQueue>();
                        return true;
                    default: // 해당 키 타입과 매칭되지 않는 QueueType
                        return false;
                }
            }
            // 키 타입이 정수 타입인 경우
            else if constexpr ( std::is_integral_v<Key> ){
                int k = static_cast<int>(key); // 정수로 변환
                switch( type )
                {
                    case QueueType::RTSP_AVFRAME:
                        rtsp_proxy_frame_internal_q_map_[k] = std::make_shared<AVFramePtrQueue>();
                        return true;
                    default: // 해당 키 타입과 매칭되지 않는 QueueType
                        return false;
                }
            }
            // 지원하지 않는 키 타입인 경우
            else {
                return false;
            }
        } // 뮤텍스 자동 해제 (lock_guard 소멸)

        /**
         * @brief 지정된 타입과 키에 해당하는 SafeQueue의 shared_ptr를 가져옵니다.
         * @tparam T 큐에 저장될 **요소**의 타입 (e.g., nlohmann::json, sptr<AVFrame>).
         * @tparam K 키의 타입 (문자열 유사 타입 또는 정수 타입).
         * @param type 가져올 큐의 종류 (MGEN::QueueType).
         * @param key 큐를 식별하는 키.
         * @return 해당 큐의 std::shared_ptr<SafeQueue<T>>. 큐가 없거나 타입이 맞지 않으면 nullptr.
         * @note 내부 맵 접근은 뮤텍스로 보호됩니다.
         * @warning 반환된 포인터가 nullptr인지 반드시 확인하고 사용해야 합니다.
         */
        template <typename T, typename K> // T는 요소 타입
        std::shared_ptr<SafeQueue<T>> GetSafeQueue( const QueueType type, const K& key )
        {
            // GetSafeQueueAuto를 호출하여 variant 형태로 큐 포인터를 가져옴 (내부에서 뮤텍스 사용)
            SptrQueueVariant v = this->GetSafeQueueAuto( type, key );

            // variant를 실제 요청된 타입(SafeQueue<T>)의 shared_ptr로 변환 시도
            // GetSafeQueueAs 내부에서는 타입 검사를 수행함
            return this->GetSafeQueueAs<SafeQueue<T>>( v );
        }

        /**
         * @brief 지정된 키에 해당하는 gRPC 응답 디스패처를 생성하고 등록합니다.
         * @tparam K 키의 타입 (문자열 유사 타입만 지원).
         * @param key 디스패처를 식별하는 키.
         * @return 성공 시 true, 실패(키 타입 오류 또는 메모리 할당 실패) 시 false.
         * @note 내부 맵 접근은 뮤텍스로 보호됩니다.
         */
        template <typename K>
        bool RegisterDispatcher(const K& key)
        {
            std::lock_guard<std::mutex> lock(map_mutex_); // 뮤텍스 잠금

            using Key = std::decay_t<K>;
            if constexpr ( is_string_like<Key>::value ){
                std::string k = std::string{key};

                auto disp = std::make_shared<GrpcEventJsonDispatcher>();
                if (!disp) return false; // 메모리 할당 실패 확인

                grpc_server_respond_dispatcher_map_[k] = disp;
                return true;
            }
            return false; // 문자열 유사 타입이 아닌 경우 실패
        } // 뮤텍스 자동 해제

        /**
         * @brief 지정된 키에 해당하는 gRPC 응답 디스패처의 shared_ptr를 가져옵니다.
         * @param key 디스패처를 식별하는 키 (std::string).
         * @return 해당 디스패처의 std::shared_ptr<GrpcEventJsonDispatcher>. 없으면 nullptr.
         * @note 내부 맵 접근은 뮤텍스로 보호됩니다.
         * @warning 반환된 포인터가 nullptr인지 반드시 확인하고 사용해야 합니다.
         */
        std::shared_ptr<GrpcEventJsonDispatcher> GetDispatcher( const std::string& key );

        /**
         * @brief 지정된 타입과 키에 해당하는 SafeQueue를 등록 해제하고 관련 메모리를 정리합니다.
         * @tparam K 키의 타입 (문자열 유사 타입 또는 정수 타입).
         * @param type 등록 해제할 큐의 종류 (MGEN::QueueType).
         * @param key 큐를 식별하는 키.
         * @return 성공적으로 제거했으면 true, 해당 큐가 없었으면 false.
         * @note 내부 맵 접근은 뮤텍스로 보호됩니다. shared_ptr의 참조 카운트가 0이 되면 큐 객체는 자동으로 소멸됩니다.
         */
        template <typename K>
        bool UnregisterSafeQueue( const QueueType type, const K& key )
        {
            std::lock_guard<std::mutex> lock(map_mutex_); // 뮤텍스 잠금

            using Key = std::decay_t<K>;
            size_t erased_count = 0; // 제거된 요소 개수

            if constexpr ( is_string_like<Key>::value ){
                std::string k = std::string {key};
                switch( type )
                {
                    case QueueType::SIO_EVENT:             erased_count = sio_event_internal_q_map_.erase(k);                 break;
                    case QueueType::GRPC_CLIENT_RSP:       erased_count = grpc_client_respond_internal_q_map_.erase(k);       break;
                    case QueueType::GRPC_SERVER_REQ_JSON:  erased_count = grpc_server_request_json_internal_q_map_.erase(k);  break;
                    case QueueType::GRPC_SERVER_REQ_IMAGE: erased_count = grpc_server_request_image_internal_q_map_.erase(k); break;
                    default: return false; // 해당 키 타입과 매칭되지 않는 QueueType
                }
            }
            else if constexpr ( std::is_integral_v<Key> ){
                int k = static_cast<int>(key);
                switch( type )
                {
                    case QueueType::RTSP_AVFRAME:          erased_count = rtsp_proxy_frame_internal_q_map_.erase(k); break;
                    default: return false; // 해당 키 타입과 매칭되지 않는 QueueType
                }
            } else {
                return false; // 지원하지 않는 키 타입
            }
            return erased_count > 0; // 1개 이상 제거되었으면 true
        } // 뮤텍스 자동 해제

        /**
         * @brief ClearAll = 영구 정리. 등록된 모든 큐 / 디스패처 제거.
         *        호출 후 다시 Build*Pipeline() 등 재구성은 하지 않는 일회성 종료.
         * @note 내부 맵 접근은 뮤텍스로 보호됩니다.
         */
        void ClearAll( void );

        /**
         * @brief 인자로 주어진 프록시 ID 리스트에 대해 새로운 AVFramePtrQueue들을 할당합니다.
         * @return 할당 성공한 unit id 목록
         */
        std::set<MGEN::Type::UnitID> RegisterRtspProxies( const std::set<MGEN::Type::UnitID>& unit_set );

        /**
         * @brief 인자로 주어진 프록시 ID 리스트에 대해 AVFramePtrQueue들을 unlink 합니다.
         * @return 해제 성공한 unit id 목록
         */
        std::set<MGEN::Type::UnitID> UnregisterRtspProxies( const std::set<MGEN::Type::UnitID>& unit_set );

        bool Ready( const ServiceProfile& service_profile, const std::shared_ptr<NetworkManager>& network_manager );

    private:
        // --- Private 멤버 함수 ---
        bool ReadyImpl_SIO ( const ServiceProfile& service_profile, const std::shared_ptr<NetworkManager>& network_manager );
        bool ReadyImpl_RTSP( const ServiceProfile& service_profile, const std::shared_ptr<NetworkManager>& network_manager );

        /**
         * @brief 지정된 타입과 키에 해당하는 SafeQueue의 shared_ptr를 variant 형태로 가져옵니다. (내부 사용)
         * @tparam K 키의 타입.
         * @param type 가져올 큐의 종류 (MGEN::QueueType).
         * @param key 큐를 식별하는 키.
         * @return SptrQueueVariant 객체. 큐를 찾으면 해당 큐의 shared_ptr를, 못 찾으면 std::monostate를 포함.
         * @note 이 함수는 외부에서 직접 호출되지 않고 GetSafeQueue 내부에서 사용됩니다. 뮤텍스 잠금을 포함합니다.
         */
        template <typename K>
        SptrQueueVariant GetSafeQueueAuto( const QueueType type, const K& key )
        {
             std::lock_guard<std::mutex> lock(map_mutex_); // 뮤텍스 잠금

            using Key = std::decay_t<K>;
            if constexpr ( is_string_like<Key>::value ){
                std::string k = std::string {key};
                switch( type )
                {
                    case QueueType::SIO_EVENT:
                        if(auto ptr = lookup(sio_event_internal_q_map_, k)) return ptr;
                        break;
                    case QueueType::GRPC_CLIENT_RSP:
                        if(auto ptr = lookup(grpc_client_respond_internal_q_map_, k)) return ptr;
                        break;
                    case QueueType::GRPC_SERVER_REQ_JSON:
                        if(auto ptr = lookup(grpc_server_request_json_internal_q_map_, k)) return ptr;
                        break;
                    case QueueType::GRPC_SERVER_REQ_IMAGE:
                        if(auto ptr = lookup(grpc_server_request_image_internal_q_map_, k)) return ptr;
                        break;
                    default:
                        break;
                }
            }
            else if constexpr ( std::is_integral_v<Key> ){
                int k = static_cast<int>(key);
                switch( type )
                {
                    case QueueType::RTSP_AVFRAME:
                        if(auto ptr = lookup(rtsp_proxy_frame_internal_q_map_, k)) return ptr;
                        break;
                    default:
                        break;
                }
            }
            // 큐를 찾지 못했거나 지원하지 않는 타입/키 조합인 경우
            return std::monostate{}; // 비어있는 상태 반환
        } // 뮤텍스 자동 해제

        /**
         * @brief SptrQueueVariant에서 특정 타입의 SafeQueue 포인터를 안전하게 추출합니다. (내부 사용)
         * @tparam QueueT 추출하려는 실제 큐 타입 (e.g., SafeQueue<nlohmann::json>, AVFramePtrQueue).
         * @param variant GetSafeQueueAuto에서 반환된 variant 객체.
         * @return variant 안에 요청된 타입의 shared_ptr가 있으면 해당 포인터를, 없으면 nullptr를 반환.
         */
        template <typename QueueT> // QueueT는 SafeQueue<ElementType> 형태
        std::shared_ptr<QueueT> GetSafeQueueAs( const SptrQueueVariant& variant )
        {
            if( auto ptr_to_sptr = std::get_if<std::shared_ptr<QueueT>>(&variant) ){
                return *ptr_to_sptr;
            }
            return nullptr;
        }

        /**
         * @brief std::unordered_map에서 키를 안전하게 검색하는 헬퍼 함수.
         * @tparam Map 맵 타입.
         * @tparam Key 키 타입.
         * @param m 검색 대상 맵.
         * @param key 찾을 키.
         * @return 키를 찾으면 값(shared_ptr)을 반환하고, 찾지 못하면 nullptr를 반환.
         * @note 이 함수는 뮤텍스 잠금을 직접 처리하지 않으므로, 호출 전에 잠금이 필요합니다.
         */
        template<typename Map, typename Key>
        auto lookup(Map& m, const Key& key) -> std::shared_ptr<typename Map::mapped_type::element_type>
        {
            // map_mutex_ 잠금이 이미 걸려있다고 가정하고 find 수행
            auto it = m.find(key);
            return (it != m.end()) ? it->second : nullptr;
        }

        bool IsReadyPipelineRTSP      ( void ) const noexcept { return this->is_ready_rtsp_pipeline_;        }
        bool IsReadyPipelineSocketIO  ( void ) const noexcept { return this->is_ready_socket_io_pipeline_;   }

    private:
        // 내부 맵 접근을 동기화하기 위한 뮤텍스
        std::mutex map_mutex_;

        // 키 타입과 값 타입(큐 또는 디스패처의 shared_ptr)을 저장하는 맵 타입 별칭
        template <typename K, typename V>
        using QueueMap = std::unordered_map<K, std::shared_ptr<V>>;

        template <typename K, typename V>
        using DispatcherMap = std::unordered_map<K, std::shared_ptr<ReplyDispatcherWithCleaner<V>>>;

        // 각 QueueType 및 키 타입에 따른 큐/디스패처 저장 맵
        // SocketIO
        QueueMap<std::string, JsonQueue>              sio_event_internal_q_map_;
        // GRPC
        QueueMap<std::string, JsonQueue>              grpc_client_respond_internal_q_map_;
        QueueMap<std::string, GrpcEventJsonQueue>     grpc_server_request_json_internal_q_map_;
        QueueMap<std::string, GrpcEventImageQueue>    grpc_server_request_image_internal_q_map_;
        DispatcherMap<std::string, EventDataOnlyJson> grpc_server_respond_dispatcher_map_;
        // RTSP
        QueueMap<int, AVFramePtrQueue>                rtsp_proxy_frame_internal_q_map_;

        // 각 프로토콜 별 데이터 파이프라인 세팅이 준비되어 있는지에 대한 boolean
        bool is_ready_socket_io_pipeline_   = false;
        bool is_ready_rtsp_pipeline_        = false;
    }; // class IOStreamManager

    // MGEN::QueueType에 대한 짧은 별칭 (선택 사항)
    using IOQType = QueueType; // 이제 IOStreamManager:: 스코프 지정 불필요

} // namespace MGEN