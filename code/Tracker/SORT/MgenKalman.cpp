#include "SORT/MgenKalman.h"
#include "MgenLogger.h"

namespace MGEN
{
	/*
		using StateXYWH = cv::Rect_<float>;

		StateXYWH 에서 XYWH 형식으로 저장하고,
		correct 나 init_kf 할 때 cx/cy/s/r 형식으로 수정하는 방식으로 구현되어 있음.
	*/

	MgenKalmanTracker::MgenKalmanTracker(
		const InferTrackID       uuid,
		const InferObject&       init_object,
		const ImageExpressStyle& track_in_style,
		const ImageExpressStyle& track_out_style )
		: track_id          ( uuid )
		, class_id          ( init_object.class_id )
		, engine_id         ( init_object.infer_engine_id )
		, track_in_style    ( track_in_style    )
		, track_out_style   ( track_out_style   )
		, last_update_score ( init_object.score )
		, time_since_update ( 0 )
		, hits              ( 0 )
		, hit_streak        ( 0 )
		, age               ( 0 )
	{
		this->InitKf( MgenKalmanTracker::get_StateXYWH( init_object, track_in_style, track_out_style ) );
	}

	// Predict the estimated bounding box.
	const StateXYWH MgenKalmanTracker::predict()
	{
		// predict for cv::KalmanFilter
		cv::Mat p = kf.predict();

		// for compare 'is_over_age()'
		age++;

		// check continuous
		if( time_since_update++ > 0 )
			hit_streak = 0;

		// return for hungarian algorithm
		return get_StateXYWH( p );
	}

	// Update the state vector with observed bounding box.
	void MgenKalmanTracker::update( const InferObject& object )
	{
		// if class mismatched, skip update
		if( class_id != object.class_id )
			return;

		// reset
		time_since_update = 0;

		// add:continuous
		hit_streak++;

		// add:correct
		hits++;

		// extract [x,y,w,h]
		const StateXYWH stateMat = MgenKalmanTracker::get_StateXYWH( object, track_in_style, track_out_style );

		// measurement ( xywh -> xysr )
		measurement.at<float>( 0, 0 ) = stateMat.x + stateMat.width  / 2;
		measurement.at<float>( 1, 0 ) = stateMat.y + stateMat.height / 2;
		measurement.at<float>( 2, 0 ) = stateMat.area();
		measurement.at<float>( 3, 0 ) = stateMat.width / stateMat.height;

		// upadte last score
		last_update_score = object.score;

		// correct for cv::KalmanFilter
		kf.correct( measurement );
	}

	namespace
	{
		// Kalman 출력 좌표 정수화 시, 매우 작은 부동소수 노이즈를 흡수하기 위한 패딩
		constexpr float PAD_ZERO = 1e-5f;

		// 좌상단이 음수이지만 중심좌표가 양수일 때, 0에 가깝게 보정하기 위한 epsilon
		constexpr float NEAR_ZERO_PIXEL = 0.01f;

		inline float pixelize( const float v ) noexcept
		{
			return ( std::fabs( v ) > DBL_EPSILON ) ? std::floor( v + PAD_ZERO ) : 0.0f;
		}
	}

	const InferObject MgenKalmanTracker::get_predict_track_object( void ) const noexcept
	{
		StateXYWH state = MgenKalmanTracker::get_StateXYWH( this->kf.statePost );

		// formatting (Kalman 출력은 float, 화면 좌표계로는 정수 픽셀로 변환)
		const float x = pixelize( state.x      );
		const float y = pixelize( state.y      );
		const float w = pixelize( state.width  );
		const float h = pixelize( state.height );

		// Build InferObject
		InferObject object {};

		object.infer_engine_id = engine_id;
		object.class_id        = class_id;
		object.track_id        = track_id;
		object.score           = last_update_score;
		object.bbox.x          = ( x < 0.0f ) ? 0.0f : x;
		object.bbox.y          = ( y < 0.0f ) ? 0.0f : y;
		object.bbox.w          = ( w < 0.0f ) ? 0.0f : w;
		object.bbox.h          = ( h < 0.0f ) ? 0.0f : h;
		object.xy_ref_type     = BBoxReferenceType::ltx_type;
		object.coord_format    = BBoxCoordinateFormat::pixel_int;

		if( object.xy_ref_type  != track_out_style.ref_type ||
			object.coord_format != track_out_style.format   ){

			ImageExpressStyle predict_style = track_out_style;

			predict_style.ref_type = object.xy_ref_type;
			predict_style.format   = object.coord_format;

			ConvertInferObjectCoordinate( object, predict_style, track_out_style );
		}

		return object;
	}

	StateXYWH MgenKalmanTracker::get_StateXYWH( const InferObject& obj, const ImageExpressStyle& src_style, const ImageExpressStyle& dst_style )
	{
		InferObject infer_object = obj;
		ConvertInferObjectCoordinate( infer_object, src_style, dst_style );

		// letf-top based x, y : ltx, lty
		if( infer_object.xy_ref_type == BBoxReferenceType::ltx_type ) {
			return StateXYWH {
				infer_object.bbox.x,
				infer_object.bbox.y,
				infer_object.bbox.w,
				infer_object.bbox.h
			};
		}
		// BBoxReferenceType::cx_type - cx, cy
		return StateXYWH {
			infer_object.bbox.x - infer_object.bbox.w / 2,
			infer_object.bbox.y - infer_object.bbox.h / 2,
			infer_object.bbox.w,
			infer_object.bbox.h
		};
	}

	// Convert bounding box from [cx,cy,s,r] to [x,y,w,h] style.
	// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
	StateXYWH MgenKalmanTracker::get_StateXYWH( float cx, float cy, float s, float r )
	{
		float w = std::sqrt( s * r );
		// w 는 sqrt 결과 → 항상 ≥ 0. 0/0 division 차단을 위한 epsilon 검사만.
		float h = ( w < DBL_EPSILON ) ? s : s / w;
		float x = ( cx - w / 2 );
		float y = ( cy - h / 2 );

		// 좌상단이 음수이지만 원본 중심 좌표는 양수일 때, 0에 가까운 값으로 보정
		if( x < 0.0 && ( cx - w / 2.0 ) > 0.0 )
			x = NEAR_ZERO_PIXEL;
		if( y < 0.0 && ( cy - h / 2.0 ) > 0.0 )
			y = NEAR_ZERO_PIXEL;

		return StateXYWH { x, y, w, h };
	}

	StateXYWH MgenKalmanTracker::get_StateXYWH( const cv::Mat& mat )
	{
		return MgenKalmanTracker::get_StateXYWH
		(
			mat.at<float>( 0, 0 ), mat.at<float>( 1, 0 ),
			mat.at<float>( 2, 0 ), mat.at<float>( 3, 0 )
		);
	}

	void MgenKalmanTracker::InitKf( const StateXYWH& stateMat )
	{
		int stateNum   = 7;
		int measureNum = 4;

		this->kf          = cv::KalmanFilter( stateNum, measureNum, 0 );
		this->measurement = cv::Mat::zeros( measureNum, 1, CV_32F );

		kf.transitionMatrix = ( cv::Mat_<float>( stateNum, stateNum ) <<
			1, 0, 0, 0, 1, 0, 0,
			0, 1, 0, 0, 0, 1, 0,
			0, 0, 1, 0, 0, 0, 1,
			0, 0, 0, 1, 0, 0, 0,
			0, 0, 0, 0, 1, 0, 0,
			0, 0, 0, 0, 0, 1, 0,
			0, 0, 0, 0, 0, 0, 1 );

		cv::setIdentity( kf.measurementMatrix );
		cv::setIdentity( kf.processNoiseCov,     cv::Scalar::all( 1e-2 ) );
		cv::setIdentity( kf.measurementNoiseCov, cv::Scalar::all( 1e-1 ) );
		cv::setIdentity( kf.errorCovPost,        cv::Scalar::all( 1 )    );

		// initialize state vector with bounding box in [cx,cy,s,r] style
		kf.statePost.at<float>( 0, 0 ) = stateMat.x + stateMat.width  / 2;
		kf.statePost.at<float>( 1, 0 ) = stateMat.y + stateMat.height / 2;
		kf.statePost.at<float>( 2, 0 ) = stateMat.area();
		kf.statePost.at<float>( 3, 0 ) = stateMat.width / stateMat.height;
	}

	const bool MgenKalmanTracker::is_over_age( const unsigned int max_age ) const noexcept
	{
		return this->time_since_update > max_age;
	}

	const bool MgenKalmanTracker::is_detected_track_object( const unsigned int min_hits, const unsigned int init_threshold ) const noexcept
	{
		if( ( this->time_since_update < 1 ) &&
			( this->hit_streak >= min_hits || init_threshold <= min_hits ) )
			return true;
		return false;
	}


} // namespace MGEN
