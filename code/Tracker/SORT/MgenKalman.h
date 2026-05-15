#ifndef _MGEN_KALMAN_H_
#define _MGEN_KALMAN_H_

#include "opencv2/video/tracking.hpp"
#include "opencv2/highgui/highgui.hpp"

#include "InferObject.h"
#include "TrackerBase/TrackerTypes.h"

namespace MGEN
{
	using StateXYWH = cv::Rect_<float>;

	class MgenKalmanTracker
	{
	public:

		// Default Constructor
		explicit MgenKalmanTracker(
			const InferTrackID       uuid,
			const InferObject        init_object,
			const ImageExpressStyle& track_in_style,
			const ImageExpressStyle& track_out_style );

		// Destructor
		~MgenKalmanTracker() = default;

		/**
		 * cv::KalmanFilter.predict() wrapper method
		 *
		 * @return [ltx, lty, w, h] cv::Rect_<float> formmated cv::KalmanFilter.predict() result
		 */
		const StateXYWH predict( void );

		/**
		 * cv::KalmanFilter.correct() wrapper method
		 *
		 * @param object : Actual input values( xywh ) to correct Kalman filter( xysr )
		 */
		void update( const InferObject& object );

		/**
		 * A function to check whether the tracker has been updated or expires
		 * after a certain lifetime
		 *
		 * @param max_age : lifetime threshold
		 * @return true if the life cycle has not expired, false if it has expired.
		 */
		const bool is_over_age( const unsigned int max_age ) const noexcept;

		/**
		 * Function to check if a valid tracking prediction exists
		 *
		 * @param min_hits : Minimum number of condition matches
		 * @param init_threshold : The tracker passes the condition only for the first N times
		 * @return true if valid tracking prediction exists
		 */
		const bool is_detected_track_object( const unsigned int min_hits, const unsigned int init_threshold ) const noexcept;

		/**
		 * When is_detected_track_object() is true, a function that
		 * returns the actual predicted value
		 *
		 * @return tracking predictions converted to MGEN::InferObject format
		 */
		const InferObject get_predict_track_object( void ) const noexcept;

		// Static Getter
		static StateXYWH get_StateXYWH( const InferObject& ojb_box, const ImageExpressStyle& src_style, const ImageExpressStyle& dst_style );
		static StateXYWH get_StateXYWH( const float cx, const float cy, const float s, const float r );
		static StateXYWH get_StateXYWH( const cv::Mat& mat );

	private:
		// initialize Kalman filter
		void InitKf( StateXYWH stateMat );

	private:
		const InferTrackID  track_id;
		const InferClassID  class_id;
		const InferEngineID engine_id;

		const ImageExpressStyle track_in_style;
		const ImageExpressStyle track_out_style;

		InferScore   last_update_score;
		unsigned int time_since_update;
		unsigned int hits;
		unsigned int hit_streak;
		unsigned int age;

		cv::KalmanFilter kf;
		cv::Mat measurement;
	};

} // namespace MGEN

#endif
