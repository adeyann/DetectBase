#pragma once

// STL :: C++
#include <vector>    // std::vector
#include <algorithm> // std::min, std::max
#include <limits>    // numeric_limits : epsilon

// OpenCV
#include <opencv2/core.hpp> // cv::Point

// BasicLibs
#include "InferObject.h"

// Mgensolution's default namespace
namespace MGEN::Abnormal
{
	/**
	 * Function to find the midpoint coordinates of the InferObject
	 *
	 * @param object - InferObject to find the midpoint
	 * @return midpoint
	 */
	const cv::Point getMidPointFromInferObject( const MGEN::InferObject& object ) noexcept;

	/**
	 * Determine if a point is inside a polygon
	 *
	 * @param point	: Coordinates to be determined
	 * @param roi	: Coordinates of each vertex of a polygon
	 */
	const bool isIncludePoint( const cv::Point& point, const std::vector<cv::Point>& roi ) noexcept;

	const double getDistance( const MGEN::InferObject& first_box, const MGEN::InferObject& last_box ) noexcept;

	/**
	 * Given three collinear points p, q, r, the function checks if point q lies on line segment 'pr'
	 *
	 * @param p,q,r : three points on a line
	 */
	const bool onLineSegment( const cv::Point& p, const cv::Point& q, const cv::Point& r ) noexcept;

	// Point p, q, r LineOrientation
	enum class LineOrientation
	{
		NotCrossed = -1,
		OnLine,
		Clockwise,
		CounterClockwise
	};

	/**
	 * Determine which direction is when three dots are placed in order
	 *
	 * @param p,q,r - Three points to determine line direction (in order)
	 * @return The direction of bending when lines are drawn in that order: p, q, r
	 */
	const LineOrientation
	lineOrientation( const cv::Point& p, const cv::Point& q, const cv::Point& r ) noexcept;

	/**
	 * Determine whether two given line segments intersect, O(1) Algorithm
	 *
	 * @param p1, q1 - edge point of first line
	 * @param p2, q2 - edge point of second line
	 * @return If not 'LineOrientation::NotCrossed', the two line segments intersect (or coincide).
	 */
	const LineOrientation
	checkLineIntersect( const cv::Point& p1, const cv::Point& q1, const cv::Point& p2, const cv::Point& q2 ) noexcept;

	/**
	 * Fixes values outside the specified range to values within the range
	 *
	 * @retrun
		If origin_value is min_value, min_value is returned.
		If origin_value is greater than or equal to max_value, max_value is returned.
		Otherwise, origin_value is returned as is.
	 */
	template <typename T>
	const T clamp( const T min_value, const T origin_value, const T max_value ) noexcept
	{
		return std::min<T>( std::max<T>( origin_value, min_value ), max_value );
	}

}; // nsp