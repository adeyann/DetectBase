///////////////////////////////////////////////////////////////////////////////
// Hungarian.cpp: Implementation file for Class UniHungarianAlgorithm.
//
// This is a C++ wrapper with slight modification of a hungarian algorithm implementation by Markus Buehren.
// The original implementation is a few mex-functions for use in MATLAB, found here:
// http://www.mathworks.com/matlabcentral/fileexchange/6543-functions-for-the-rectangular-assignment-problem
//
// Both this code and the orignal code are published under the BSD license.
// by Cong Ma, 2016
//

#include "TrackerBase/TrackerTypes.h"
#include "SORT/MgenHungarian.h"

#include <math.h>
#include <float.h>

namespace MGEN
{
	HungarianMemoryUnit::HungarianMemoryUnit( const int max_calculate_size )
		: max_matrix_size( max_calculate_size )
	{
		const size_t line_size = max_matrix_size;
		const size_t sqre_size = line_size * line_size;

		dist_matrix_in  = new double [sqre_size]; // n^2
		dist_matrix     = new double [sqre_size]; // n^2
		assignment      = new int    [line_size]; // n
		covered_cols    = new bool   [line_size]; // n
		covered_rows    = new bool   [line_size]; // n
		star_matrix     = new bool   [sqre_size]; // n^2
		new_star_matrix = new bool   [sqre_size]; // n^2
		prime_matrix    = new bool   [sqre_size]; // n^2
	}

	HungarianMemoryUnit::~HungarianMemoryUnit()
	{
		delete[] dist_matrix_in;
		delete[] dist_matrix;
		delete[] assignment;
		delete[] covered_cols;
		delete[] covered_rows;
		delete[] star_matrix;
		delete[] new_star_matrix;
		delete[] prime_matrix;
	}

	MgenHungarianAlgorithm::MgenHungarianAlgorithm( const size_t max_tracking_num )
		: memory_unit( new HungarianMemoryUnit( static_cast<int>( max_tracking_num ) ) )
	{
		//
	}

	const size_t MgenHungarianAlgorithm::GetTrackingMaxSize( void ) const noexcept
	{
		if( this->memory_unit )
			return this->memory_unit->GetHungarianMatrixMaxSize();
		return 0;
	}

	//********************************************************//
	// A single function wrapper for solving assignment problem.
	//********************************************************//
	void MgenHungarianAlgorithm::Solve( const std::vector<std::vector<double>>& DistMatrix, std::vector<int>& Assignment )
	{
		const unsigned int nRows = DistMatrix.size();
		const unsigned int nCols = DistMatrix[0].size();

		const size_t dist_mat_in_size = sizeof( double ) * nRows * nCols;
		const size_t assignment_size  = sizeof( int    ) * nRows;

		// reset
		memset( memory_unit->dist_matrix_in, 0, dist_mat_in_size );
		memset( memory_unit->assignment,     0, assignment_size  );

		/**	memcpy but row-order
		 *  --------------------
		 *  Fill in the dist_matrix_in. Mind the index is "i + nRows * j".
		 *  Here the cost matrix of size MxN is defined as a double precision array of N*M elements.
		 *  In the solving functions matrices are seen to be saved MATLAB-internally in row-order.
		 *  (i.e. the matrix [1 2; 3 4] will be stored as a vector [1 3 2 4], NOT [1 2 3 4]).
		 */
		for( unsigned int i = 0; i < nRows; i++ )
			for( unsigned int j = 0; j < nCols; j++ )
				( memory_unit->dist_matrix_in )[i + nRows * j] = DistMatrix[i][j];

		// call solving function
		AssignmentOptimal( static_cast<int>( nRows ), static_cast<int>( nCols ) );

		Assignment.clear();
		// set result : assignment
		for( unsigned int r = 0; r < nRows; r++ )
			Assignment.push_back( ( memory_unit->assignment )[r] );
	}

	//********************************************************//
	// Solve optimal solution for assignment problem using Munkres algorithm, also known as Hungarian Algorithm.
	//********************************************************//
	void MgenHungarianAlgorithm::AssignmentOptimal( int nOfRows, int nOfColumns )
	{
		double* ptrDistMatrixCur = nullptr;
		double* ptrDistMatrixEnd = nullptr;
		double* ptrColumnEnd     = nullptr;

		// C-style 변수 일괄 init pattern (재할당 전 default 0).
		// cppcheck-suppress-begin unreadVariable
		double curValue    = 0.0f;
		double minValue    = 0.0f;
		int    nOfElements = 0;
		int    minDim      = 0;
		int    row         = 0;
		int    col         = 0;
		// cppcheck-suppress-end unreadVariable

		/* initialization */
		for( row = 0; row < nOfRows; row++ )
			memory_unit->assignment[row] = -1;

		/* generate working copy of distance Matrix */
		/* check if all matrix elements are positive */
		nOfElements = nOfRows * nOfColumns;
		ptrDistMatrixEnd = memory_unit->dist_matrix + nOfElements;

		for( row = 0; row < nOfElements; row++ ){
			curValue = memory_unit->dist_matrix_in[row];
			memory_unit->dist_matrix[row] = curValue;
		}

		memset( memory_unit->covered_cols,    0, sizeof( bool ) * nOfColumns  );
		memset( memory_unit->covered_rows,    0, sizeof( bool ) * nOfRows     );
		memset( memory_unit->star_matrix,     0, sizeof( bool ) * nOfElements );
		memset( memory_unit->new_star_matrix, 0, sizeof( bool ) * nOfElements );
		memset( memory_unit->prime_matrix,    0, sizeof( bool ) * nOfElements );

		/* preliminary steps */
		if( nOfRows <= nOfColumns ) {
			minDim = nOfRows;

			for( row = 0; row < nOfRows; row++ ) {
				/* find the smallest element in the row */
				ptrDistMatrixCur = memory_unit->dist_matrix + row;
				minValue         = *ptrDistMatrixCur;
				ptrDistMatrixCur += nOfRows;

				while( ptrDistMatrixCur < ptrDistMatrixEnd ) {
					curValue = *ptrDistMatrixCur;
					if( curValue < minValue )
						minValue = curValue;
					ptrDistMatrixCur += nOfRows;
				}

				/* subtract the smallest element from each element of the row */
				ptrDistMatrixCur = memory_unit->dist_matrix + row;
				while( ptrDistMatrixCur < ptrDistMatrixEnd ) {
					*ptrDistMatrixCur -= minValue;
					ptrDistMatrixCur  += nOfRows;
				}
			}

			/* Steps 1 and 2a */
			for( row = 0; row < nOfRows; row++ )
				for( col = 0; col < nOfColumns; col++ )
					if( fabs( memory_unit->dist_matrix[row + nOfRows * col] ) < DBL_EPSILON )
						if( !memory_unit->covered_cols[col] )
						{
							memory_unit->star_matrix[row + nOfRows * col] = true;
							memory_unit->covered_cols[col] = true;
							break;
						}

		}
		else /* if(nOfRows > nOfColumns) */
		{
			minDim = nOfColumns;

			for( col = 0; col < nOfColumns; col++ ) {
				/* find the smallest element in the column */
				// bugprone-implicit-widening: int*int → ptrdiff_t cast 명시 (큰 matrix 대비 방어).
				ptrDistMatrixCur = memory_unit->dist_matrix + static_cast<size_t>( nOfRows ) * col;
				ptrColumnEnd = ptrDistMatrixCur + nOfRows;

				minValue = *ptrDistMatrixCur++;
				while( ptrDistMatrixCur < ptrColumnEnd ) {
					curValue = *ptrDistMatrixCur++;
					if( curValue < minValue )
						minValue = curValue;
				}

				/* subtract the smallest element from each element of the column */
				ptrDistMatrixCur = memory_unit->dist_matrix + static_cast<size_t>( nOfRows ) * col;
				while( ptrDistMatrixCur < ptrColumnEnd )
					*ptrDistMatrixCur++ -= minValue;
			}

			/* Steps 1 and 2a */
			for( col = 0; col < nOfColumns; col++ )
				for( row = 0; row < nOfRows; row++ )
					if( fabs( memory_unit->dist_matrix[row + nOfRows * col] ) < DBL_EPSILON )
						if( !memory_unit->covered_rows[row] )
						{
							memory_unit->star_matrix[row + nOfRows * col] = true;
							memory_unit->covered_cols[col] = true;
							memory_unit->covered_rows[row] = true;
							break;
						}

			for( row = 0; row < nOfRows; row++ )
				memory_unit->covered_rows[row] = false;
		}

		/* move to step 2b */
		step2b( memory_unit->assignment,   memory_unit->dist_matrix,
				memory_unit->star_matrix,  memory_unit->new_star_matrix,
				memory_unit->prime_matrix, memory_unit->covered_cols,
				memory_unit->covered_rows, nOfRows, nOfColumns, minDim );
	}

	/********************************************************/
	// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
	void MgenHungarianAlgorithm::BuildAssignmentVector( int* assignment, bool* starMatrix, int nOfRows, int nOfColumns )
	{
		int row, col;
		for( row = 0; row < nOfRows; row++ )
			for( col = 0; col < nOfColumns; col++ )
				if( starMatrix[row + nOfRows * col] ) {
					assignment[row] = col;
					break;
				}
	}

	/********************************************************/
	void MgenHungarianAlgorithm::step2a( int* assignment, double* distMatrix, bool* starMatrix, bool* newStarMatrix, bool* primeMatrix, bool* coveredColumns, bool* coveredRows, int nOfRows, int nOfColumns, int minDim )
	{
		int col;

		/* cover every column containing a starred zero */
		for( col = 0; col < nOfColumns; col++ ) {
			// bugprone-implicit-widening: int*int → ptrdiff_t cast 명시.
			bool* starMatrixTemp = starMatrix + static_cast<size_t>( nOfRows ) * col;
			const bool* const ptrColumnEnd = starMatrixTemp + nOfRows;
			while( starMatrixTemp < ptrColumnEnd ) {
				if( *starMatrixTemp++ ) {
					coveredColumns[col] = true;
					break;
				}
			}
		}
		/* move to step 3 */
		step2b( assignment, distMatrix, starMatrix, newStarMatrix, primeMatrix, coveredColumns, coveredRows, nOfRows, nOfColumns, minDim );
	}

	/********************************************************/
	void MgenHungarianAlgorithm::step2b( int* assignment, double* distMatrix, bool* starMatrix, bool* newStarMatrix, bool* primeMatrix, bool* coveredColumns, bool* coveredRows, int nOfRows, int nOfColumns, int minDim )
	{
		int col;
		int nOfCoveredColumns;

		/* count covered columns */
		nOfCoveredColumns = 0;
		for( col = 0; col < nOfColumns; col++ )
			if( coveredColumns[col] )
				nOfCoveredColumns++;

		if( nOfCoveredColumns == minDim ) {
			/* algorithm finished */
			BuildAssignmentVector( assignment, starMatrix, nOfRows, nOfColumns );
		}
		else {
			/* move to step 3 */
			step3( assignment, distMatrix, starMatrix, newStarMatrix, primeMatrix, coveredColumns, coveredRows, nOfRows, nOfColumns, minDim );
		}
	}

	/********************************************************/
	void MgenHungarianAlgorithm::step3( int* assignment, double* distMatrix, bool* starMatrix, bool* newStarMatrix, bool* primeMatrix, bool* coveredColumns, bool* coveredRows, int nOfRows, int nOfColumns, int minDim )
	{
		bool zerosFound;
		int row;
		int col;
		int starCol;

		zerosFound = true;
		while( zerosFound ) {
			zerosFound = false;
			for( col = 0; col < nOfColumns; col++ )
				if( !coveredColumns[col] )
					for( row = 0; row < nOfRows; row++ )
						if( ( !coveredRows[row] ) && ( fabs( distMatrix[row + nOfRows * col] ) < DBL_EPSILON ) ) {

							/* prime zero */
							primeMatrix[row + nOfRows * col] = true;

							/* find starred zero in current row */
							for( starCol = 0; starCol < nOfColumns; starCol++ )
								if( starMatrix[row + nOfRows * starCol] )
									break;

							if( starCol == nOfColumns ) /* no starred zero found */ {
								/* move to step 4 */
								step4( assignment, distMatrix, starMatrix, newStarMatrix, primeMatrix, coveredColumns, coveredRows, nOfRows, nOfColumns, minDim, row, col );
								return;
							}
							else {
								coveredRows[row] = true;
								coveredColumns[starCol] = false;
								zerosFound = true;
								break;
							}
						}
		}
		/* move to step 5 */
		step5( assignment, distMatrix, starMatrix, newStarMatrix, primeMatrix, coveredColumns, coveredRows, nOfRows, nOfColumns, minDim );
	}

	/********************************************************/
	// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
	void MgenHungarianAlgorithm::step4( int* assignment, double* distMatrix, bool* starMatrix, bool* newStarMatrix, bool* primeMatrix, bool* coveredColumns, bool* coveredRows, int nOfRows, int nOfColumns, int minDim, int row, int col )
	{
		int n;
		int starRow;
		int starCol;
		int nOfElements = nOfRows * nOfColumns;

		/* generate temporary copy of starMatrix */
		for( n = 0; n < nOfElements; n++ )
			newStarMatrix[n] = starMatrix[n];

		/* star current zero */
		newStarMatrix[row + nOfRows * col] = true;

		/* find starred zero in current column */
		starCol = col;
		for( starRow = 0; starRow < nOfRows; starRow++ )
			if( starMatrix[starRow + nOfRows * starCol] )
				break;

		while( starRow < nOfRows ) {

			/* unstar the starred zero */
			newStarMatrix[starRow + nOfRows * starCol] = false;

			/* find primed zero in current row */
			const int primeRow = starRow;
			int       primeCol;
			for( primeCol = 0; primeCol < nOfColumns; primeCol++ )
				if( primeMatrix[primeRow + nOfRows * primeCol] )
					break;

			/* star the primed zero */
			newStarMatrix[primeRow + nOfRows * primeCol] = true;

			/* find starred zero in current column */
			starCol = primeCol;
			for( starRow = 0; starRow < nOfRows; starRow++ )
				if( starMatrix[starRow + nOfRows * starCol] )
					break;
		}

		/* use temporary copy as new starMatrix */
		/* delete all primes, uncover all rows */
		for( n = 0; n < nOfElements; n++ ) {
			primeMatrix[n] = false;
			starMatrix[n] = newStarMatrix[n];
		}
		for( n = 0; n < nOfRows; n++ )
			coveredRows[n] = false;

		/* move to step 2a */
		step2a( assignment, distMatrix, starMatrix, newStarMatrix, primeMatrix, coveredColumns, coveredRows, nOfRows, nOfColumns, minDim );
	}

	/********************************************************/
	void MgenHungarianAlgorithm::step5( int* assignment, double* distMatrix, bool* starMatrix, bool* newStarMatrix, bool* primeMatrix, bool* coveredColumns, bool* coveredRows, int nOfRows, int nOfColumns, int minDim )
	{
		double h;
		double value;
		int    row;
		int    col;

		/* find smallest uncovered element h */
		h = DBL_MAX;
		for( row = 0; row < nOfRows; row++ )
			if( !coveredRows[row] )
				for( col = 0; col < nOfColumns; col++ )
					if( !coveredColumns[col] ) {
						value = distMatrix[row + nOfRows * col];
						if( value < h )
							h = value;
					}

		/* add h to each covered row */
		for( row = 0; row < nOfRows; row++ )
			if( coveredRows[row] )
				for( col = 0; col < nOfColumns; col++ )
					distMatrix[row + nOfRows * col] += h;

		/* subtract h from each uncovered column */
		for( col = 0; col < nOfColumns; col++ )
			if( !coveredColumns[col] )
				for( row = 0; row < nOfRows; row++ )
					distMatrix[row + nOfRows * col] -= h;

		/* move to step 3 */
		step3( assignment, distMatrix, starMatrix, newStarMatrix, primeMatrix, coveredColumns, coveredRows, nOfRows, nOfColumns, minDim );
	}

} // namespace MGEN
