#pragma once

///////////////////////////////////////////////////////////////////////////////
// Hungarian.h: Header file for Class HungarianAlgorithm.
//
// This is a C++ wrapper with slight modification of a hungarian algorithm implementation by Markus Buehren.
// The original implementation is a few mex-functions for use in MATLAB, found here:
// http://www.mathworks.com/matlabcentral/fileexchange/6543-functions-for-the-rectangular-assignment-problem
//
// Both this code and the orignal code are published under the BSD license.
// by Cong Ma, 2016
//

#include <iostream>
#include <vector>
#include <memory>

#include <stdlib.h>
#include <unistd.h>
#include <string.h>

namespace MGEN
{
	class HungarianMemoryUnit
	{
	public:
		// Default constructor
		explicit HungarianMemoryUnit( const int max_calculate_size );

		// Copy constructor : delete
		HungarianMemoryUnit( const HungarianMemoryUnit& rhs ) = delete;

		// Copy assignment : delete
		HungarianMemoryUnit& operator=( const HungarianMemoryUnit& rhs ) = delete;

		// Move constructor : delete
		HungarianMemoryUnit( HungarianMemoryUnit&& rhs ) = delete;

		// Move assignment : delete
		HungarianMemoryUnit& operator=( const HungarianMemoryUnit&& rhs ) = delete;

		// Default destructor
		~HungarianMemoryUnit();

		// Getter
		const size_t GetHungarianMatrixMaxSize( void ) const noexcept { return this->max_matrix_size; }

	public:
		// Maximum matrix size when calculating Hungarian algorithm
		const size_t max_matrix_size;

		double*   dist_matrix_in  = nullptr;
		double*   dist_matrix     = nullptr;
		int*      assignment      = nullptr;
		bool*     covered_cols    = nullptr;
		bool*     covered_rows    = nullptr;
		bool*     star_matrix     = nullptr;
		bool*     new_star_matrix = nullptr;
		bool*     prime_matrix    = nullptr;
	};

	class MgenHungarianAlgorithm
	{
	public:
		// Constructor
		MgenHungarianAlgorithm( void ) = delete;

		explicit MgenHungarianAlgorithm( const size_t max_tracking_num );

		// Destructor
		~MgenHungarianAlgorithm() = default;

		// Run
		void Solve( const std::vector<std::vector<double>>& DistMatrix, std::vector<int>& Assignment );

		// Getter
		const size_t GetTrackingMaxSize() const noexcept;

	private:
		std::unique_ptr<HungarianMemoryUnit> memory_unit;

	private:
		void AssignmentOptimal( int nOfRows, int nOfColumns );
		void BuildAssignmentVector( int* assignment, bool* starMatrix, int nOfRows, int nOfColumns );

		void step2a( int* assignment, double* distMatrix, bool* starMatrix, bool* newStarMatrix, bool* primeMatrix, bool* coveredColumns, bool* coveredRows, int nOfRows, int nOfColumns, int minDim );
		void step2b( int* assignment, double* distMatrix, bool* starMatrix, bool* newStarMatrix, bool* primeMatrix, bool* coveredColumns, bool* coveredRows, int nOfRows, int nOfColumns, int minDim );
		void step3 ( int* assignment, double* distMatrix, bool* starMatrix, bool* newStarMatrix, bool* primeMatrix, bool* coveredColumns, bool* coveredRows, int nOfRows, int nOfColumns, int minDim );
		void step4 ( int* assignment, double* distMatrix, bool* starMatrix, bool* newStarMatrix, bool* primeMatrix, bool* coveredColumns, bool* coveredRows, int nOfRows, int nOfColumns, int minDim, int row, int col );
		void step5 ( int* assignment, double* distMatrix, bool* starMatrix, bool* newStarMatrix, bool* primeMatrix, bool* coveredColumns, bool* coveredRows, int nOfRows, int nOfColumns, int minDim );
	};
} // namespace MGEN
