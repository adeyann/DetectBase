#pragma once

#include "TrackerBase/TrackerTypes.h"
#include "InferObject.h"

#include <optional>
#include "SORT/MgenHungarian.h"
#include "SORT/MgenKalman.h"

#include <set>
#include <vector>
#include <memory>
#include <mutex>

#include <opencv2/opencv.hpp>
#include <opencv2/core/core.hpp>

namespace MGEN
{
    struct TrackStateGroups
    {
        std::set<int> allItems;
        std::set<int> matchedItems;
        std::set<int> unmatchedItems;

        void clearAll()
        {
            this->allItems.clear();
            this->matchedItems.clear();
            this->unmatchedItems.clear();
        }
    };

    class SORTTracker final
    {
    public:
        // constructor
        SORTTracker( void ) = delete;

        explicit SORTTracker( const ImageExpressStyle& track_in_style, const ImageExpressStyle& track_out_style );

        // destructor
        ~SORTTracker() = default;

        std::optional<std::vector<InferObject>>
        TrackObjects( const std::vector<InferObject>& objects, const int request_seq );

    private:
        // Const vars
        const size_t            max_track_size_;
        const unsigned int      max_age_;
        const unsigned int      min_hits_;
        const double            iou_threshold_;
        const ImageExpressStyle track_in_style_;
		const ImageExpressStyle track_out_style_;

        // For each alg
        std::unique_ptr<MgenHungarianAlgorithm>         hungarian_;
        std::vector<std::unique_ptr<MgenKalmanTracker>> kalmans_;

        // For SORT
        std::vector<StateXYWH>           predicted_boxes_;
        std::vector<std::vector<double>> cost_matrix_;
        std::vector<int>                 assignment_;
        TrackStateGroups                 track_case_;
        std::vector<cv::Point>           matched_pairs_;

        // variables
        unsigned int       trk_num_                     = 0;
        unsigned int       det_num_                     = 0;
        unsigned int       kalman_tracker_create_count_ = 1;
        // 현재 단일 thread (RtspDetectorUnit Loop) 만 TrackObjects() 호출 — defensive 보호.
        // 향후 multi-thread 호출 가능성에 대비.
        mutable std::mutex mtx_;

    private:
        double GetIOU( const StateXYWH& predict_box, const InferObject& infer_object ) const;
    };

} // namespace MGEN
