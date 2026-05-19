#include "GeometricLogic.h"

#include <cmath>

// Mgensolution's default namespace
namespace MGEN::Abnormal
{
	const cv::Point getMidPointFromInferObject( const MGEN::InferObject& object ) noexcept
	{
		return cv::Point {
			static_cast<int>( object.bbox.x + ( object.bbox.w / 2.f ) ),
			static_cast<int>( object.bbox.y + ( object.bbox.h / 2.f ) )
		};
	}

	const bool isIncludePoint( const cv::Point& checkPoint, const std::vector<cv::Point>& p ) noexcept
	{
		/*
			특정 checkPoint 좌표에서 x축 평행한 반직선을 그었을 때, CrossPoint Count
			odd	 : Include
			even : Exclude
		*/
		int nCrosses = 0;
		const int polygon_size = static_cast<int>( p.size() );
		for( int i = 0; i < polygon_size; ++i ) {
			// 만약 다각형의 한 점과 일치한다면 그 즉시 inside 판정
			if( p[i] == checkPoint ) return true;

			// 한 점 i 와 그 다음 점 j 와의 관계
			const int j = ( i + 1 ) % polygon_size;

			// 점 CheckPoint가 선분 (p[i], p[j]) 의 y 좌표 사이에 있음
			if( ( p[i].y > checkPoint.y ) != ( p[j].y > checkPoint.y ) ) {
				// atX는 점 checkPoint 를 지나는 수평선과 선분 (p[i], p[j])의 교점 x좌표
				const double atX = ( p[j].x - p[i].x ) * ( checkPoint.y - p[i].y )
					/ static_cast<double>( p[j].y - p[i].y ) + static_cast<double>( p[i].x );
				// atX가 우측 반직선과의 교점이 맞다면 교점의 갯수 증가
				if( checkPoint.x < atX )
					nCrosses++;
			}
		}
		return nCrosses % 2 > 0;
	}

	const double getDistance( const MGEN::InferObject& first_box, const MGEN::InferObject& last_box ) noexcept
	{
		const int pre_x = static_cast<int>( first_box.bbox.x ) + static_cast<int>( first_box.bbox.w / 2.0f );
		const int pre_y = static_cast<int>( first_box.bbox.y ) + static_cast<int>( first_box.bbox.h / 2.0f );

		const int cur_x = static_cast<int>( last_box.bbox.x ) + static_cast<int>( last_box.bbox.w / 2.0f );
		const int cur_y = static_cast<int>( last_box.bbox.y ) + static_cast<int>( last_box.bbox.h / 2.0f );

		const double x = abs(pre_x - cur_x);
		const double y = abs(pre_y - cur_y);

		return sqrt(pow(x, 2) + pow(y, 2));
	}

	const bool onLineSegment( const cv::Point& p, const cv::Point& q, const cv::Point& r ) noexcept
	{
		if( q.x <= std::max( p.x, r.x ) && q.x >= std::min( p.x, r.x ) &&
			q.y <= std::max( p.y, r.y ) && q.y >= std::min( p.y, r.y ) )
			return true;
		return false;
	}

	const Abnormal::LineOrientation lineOrientation( const cv::Point& p, const cv::Point& q, const cv::Point& r ) noexcept
	{
		/*
			 To find orientation of ordered triplet (p, q, r).
			 The function returns following values
			 0 --> p, q and r are collinear
			 1 --> Clockwise
			 2 --> Counterclockwise
		*/
		const int val = ( q.y - p.y ) * ( r.x - q.x ) - ( q.x - p.x ) * ( r.y - q.y );

		if( val == 0 )
			return Abnormal::LineOrientation::OnLine;

		return ( val > 0 )
			? Abnormal::LineOrientation::Clockwise
			: Abnormal::LineOrientation::CounterClockwise;
	}

	const Abnormal::LineOrientation checkLineIntersect( const cv::Point& p1, const cv::Point& q1, const cv::Point& p2, const cv::Point& q2 ) noexcept
	{
		const auto o1 = lineOrientation( p1, q1, p2 );
		const auto o2 = lineOrientation( p1, q1, q2 );
		const auto o3 = lineOrientation( p2, q2, p1 );
		const auto o4 = lineOrientation( p2, q2, q1 );

		// General Case : OnLine | Clockwise | CounterClockwise
		if( o1 != o2 && o3 != o4 )
			return o4;

		// Special Cases
		if( o1 == Abnormal::LineOrientation::OnLine && onLineSegment( p1, p2, q1 ) ) return Abnormal::LineOrientation::OnLine;
		if( o2 == Abnormal::LineOrientation::OnLine && onLineSegment( p1, q2, q1 ) ) return Abnormal::LineOrientation::OnLine;
		if( o3 == Abnormal::LineOrientation::OnLine && onLineSegment( p2, p1, q2 ) ) return Abnormal::LineOrientation::OnLine;
		if( o4 == Abnormal::LineOrientation::OnLine && onLineSegment( p2, q1, q2 ) ) return Abnormal::LineOrientation::OnLine;

		// Doesn't fall in any of the above cases
		return Abnormal::LineOrientation::NotCrossed;
	}
}; // nsp : MGEN
