/**
 * @file SwsContextManager.cpp
 * @brief SwsContextManager 클래스의 상세 로직을 구현합니다.
 */

#include "SwsContextManager.h" // 자신의 헤더 포함

namespace MGEN
{
    /**
     * @brief 타겟 해상도에 맞는 전처리 컨텍스트를 반환하며, 원본 해상도 변경 시 캐시를 초기화합니다.
     */
    FrameFormattingContext* SwsContextManager::GetContext( int target_w, int target_h, std::shared_ptr<AVFrame> frame )
    {
        /**
         * @details 방어적 코딩: 입력 프레임이 유효하지 않으면 컨텍스트를 제공할 수 없습니다.
         */
        if( frame == nullptr )
        {
            return nullptr;
        }

        /**
         * @details 원본 영상의 해상도가 변경되었는지 확인합니다.
         * 해상도가 변경되었다면 기존에 생성된 모든 타겟 컨텍스트(SwsContext 포함)는 유효하지 않으므로 초기화합니다.
         */
        if( current_src_w != frame->width || current_src_h != frame->height )
        {
            this->ClearAll();

            current_src_w = frame->width;
            current_src_h = frame->height;
        }

        /**
         * @brief ResolutionKey를 생성하여 맵에서 컨텍스트를 탐색하거나 생성합니다.
         */
        ResolutionKey key { target_w, target_h };

        /**
         * @note std::map::operator[]는 키가 없을 경우 객체를 기본 생성하여 레퍼런스를 반환합니다.
         */
        return &contexts[key];
    }

    /**
     * @brief 모든 캐시된 컨텍스트를 제거하고 상태 정보를 리셋합니다.
     */
    void SwsContextManager::ClearAll( void )
    {
        /**
         * @details 맵을 비움으로써 내부의 FrameFormattingContext들이 소멸되며,
         * shared_ptr로 관리되던 SwsContext들도 참조 카운트 감소에 의해 해제됩니다.
         */
        contexts.clear();

        current_src_w = 0;
        current_src_h = 0;
    }

} // namespace MGEN