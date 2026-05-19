#include "SORT/SORTTracker.h"
#include "MgenLogger.h"

#include <cfloat>

namespace MGEN
{
    namespace
    {
        // SORT Tracker 알고리즘 파라미터 (외부 노출 불필요)
        constexpr int   SORT_TRACKING_MAX_NUM        = 200; // SORT Tracker 알고리즘 트래킹 객체 최대 수
        constexpr int   SORT_TRACKER_DEFAULT_MAX_AGE = 10;
        constexpr int   SORT_TRACKER_DEFAULT_MIN_HIT = 3;
        constexpr float SORT_TRACKER_IOU_THRESHOLD   = 0.7f;
    }

    SORTTracker::SORTTracker( const ImageExpressStyle& track_in_style, const ImageExpressStyle& track_out_style  )
        : max_track_size_  ( SORT_TRACKING_MAX_NUM )
        , max_age_         ( SORT_TRACKER_DEFAULT_MAX_AGE )
        , min_hits_        ( SORT_TRACKER_DEFAULT_MIN_HIT )
        , iou_threshold_   ( SORT_TRACKER_IOU_THRESHOLD )
        , track_in_style_  ( track_in_style )
        , track_out_style_ ( track_out_style )
        , hungarian_       ( std::make_unique<MgenHungarianAlgorithm>( max_track_size_ ) )
    {
        predicted_boxes_.reserve( max_track_size_ );
        assignment_.reserve( max_track_size_ );
    }

    double SORTTracker::GetIOU( const StateXYWH& predict_state, const InferObject& infer_object ) const
    {
        StateXYWH tracking_state = MgenKalmanTracker::get_StateXYWH( infer_object, track_in_style_, track_out_style_ );

        const double denom = ( predict_state | tracking_state ).area();
        if( fabs(denom) < DBL_EPSILON )
            return 0.0f;

        return ( ( predict_state & tracking_state ).area() / static_cast<double>( denom ) );
    }

    std::optional<std::vector<InferObject>> SORTTracker::TrackObjects( const std::vector<InferObject>& objects, const int request_seq )
    {
        if( objects.empty() || this->max_track_size_ <= 0 ) {
            return std::nullopt;
        }

        std::unique_lock<std::mutex> lck { this->mtx_ };
        try {
            // copy
            auto object_copies = objects;

            // when first, initialize
            if( kalmans_.empty() == true ) {
                for( const auto& object : object_copies )
                {
                    // NEW-4: make_unique 는 throw 또는 non-null 반환. nullptr 검사 dead code 제거.
                    auto kalman_uptr = std::make_unique<MgenKalmanTracker>(
                        this->kalman_tracker_create_count_,
                        object,
                        this->track_in_style_,
                        this->track_out_style_
                    );

                    kalmans_.push_back( std::move(kalman_uptr) );
                    this->kalman_tracker_create_count_++;
                }
                return std::nullopt;
            }

            // reset
            predicted_boxes_.clear();

            for( auto it = kalmans_.begin(); it != kalmans_.end(); ) {
                // Kalman predict
                StateXYWH predict_result = (*it)->predict();

                // 좌상단이 정확히 (0,0) 인 트래커도 보존. 음수만 erase.
                if( predict_result.x >= 0.0f && predict_result.y >= 0.0f ) {
                    predicted_boxes_.push_back( std::move( predict_result ) );
                    it++;
                }
                else {
                    it = kalmans_.erase( it );
                }
            }

            trk_num_ = predicted_boxes_.size();
            det_num_ = object_copies.size();

            bool is_over_tracking_size = false;
            if( trk_num_ > max_track_size_ ) {
                MLOG_WARN( "The number of tracking objects exceeds the maximum number of objects to detect. ( %d > %d )\n", trk_num_, max_track_size_ );
                is_over_tracking_size = true;
            }
            if( det_num_ > max_track_size_ ) {
                MLOG_WARN( "The number of detected objects exceeds the maximum number of objects to detect. ( %d > %d )\n", det_num_, max_track_size_ );
                is_over_tracking_size = true;
            }

            if( is_over_tracking_size == true ) {
                for( auto it = kalmans_.begin(); it != kalmans_.end(); ) {
                    if( it != kalmans_.end() && (*it)->is_over_age( max_age_ ) )
                        it  = kalmans_.erase( it );
                    else
                        it++;
                }
                return std::nullopt;
            }

            if( trk_num_ == 0 ) {
                return std::nullopt;
            }

            cost_matrix_.clear();
            cost_matrix_.resize( trk_num_, std::vector<double>( det_num_, 0 ) );

            for( uint i = 0; i < trk_num_; ++i )
                for( uint j = 0; j < det_num_; ++j )
                    cost_matrix_[i][j] = 1 - this->GetIOU( predicted_boxes_[i], object_copies[j] );

            assignment_.clear();
            hungarian_->Solve( cost_matrix_, assignment_ );

            track_case_.clearAll();
            if( det_num_ > trk_num_ ) {
                for( uint n = 0; n < det_num_; ++n )
                    track_case_.allItems.insert( static_cast<int>( n ) );

                for( uint m = 0; m < trk_num_; ++m )
                    track_case_.matchedItems.insert( assignment_[m] );

                std::set_difference (
                    // Diff_1 range
                    track_case_.allItems.begin(), track_case_.allItems.end(),

                    // Diff_2 range
                    track_case_.matchedItems.begin(), track_case_.matchedItems.end(),

                    // Result Save Pos.
                    std::insert_iterator<std::set<int>> (
                        track_case_.unmatchedItems,        // target
                        track_case_.unmatchedItems.begin() // start idx
                    )
                );
            }

            matched_pairs_.clear();
            for( uint i = 0; i < trk_num_; ++i ) {
                if( assignment_[i] == -1 )
                    continue;

                if( cost_matrix_[i][assignment_[i]] > iou_threshold_ )
                    track_case_.unmatchedItems.insert( assignment_[i] );
                else
                    matched_pairs_.emplace_back( i, assignment_[i] );
            }

            for( const auto& pt : matched_pairs_ )
                kalmans_[pt.x]->update( object_copies[pt.y] );

            for( const auto& umd : track_case_.unmatchedItems )
            {
                // NEW-4: make_unique 는 throw 또는 non-null 반환. nullptr 검사 dead code 제거.
                auto kalman_uptr = std::make_unique<MgenKalmanTracker>(
                    this->kalman_tracker_create_count_,
                    object_copies[umd],
                    this->track_in_style_,
                    this->track_out_style_
                );

                kalmans_.push_back( std::move(kalman_uptr) );
                this->kalman_tracker_create_count_++;
            }

            std::vector<InferObject> results;
            results.reserve( kalmans_.size() );

            for( auto it = kalmans_.begin(); it != kalmans_.end(); ) {
                if( (*it)->is_detected_track_object( min_hits_, request_seq ) == true ) {
                    results.push_back( (*it)->get_predict_track_object() );
                }

                if( it != kalmans_.end() && (*it)->is_over_age( max_age_ ) )
                    it = kalmans_.erase( it );
                else
                    it++;
            }

            // track id 가 int 범위 한계 근처에 도달하면 0부터 다시 시작 (오버플로우 방지)
            constexpr unsigned int TRACK_ID_CYCLE_THRESHOLD = 2'000'000'000;
            if( this->kalman_tracker_create_count_ > TRACK_ID_CYCLE_THRESHOLD ) {
                this->kalman_tracker_create_count_ = 0;
            }

            return results;
        }
        catch( const std::out_of_range& e ) {
            MLOG_ERROR( "%s() => MgenTracker with out of range exception: %s", __func__, e.what() );
        }
        catch( const std::exception& e ) {
            MLOG_ERROR( "%s() => MgenTracker with exception: %s", __func__, e.what() );
        }
        catch( ... ) {
            MLOG_ERROR( "%s() => MgenTracker unknown exception occurred", __func__ );
        }
        return std::nullopt;
    }

} // namespace MGEN
