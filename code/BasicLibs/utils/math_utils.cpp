#include "math_utils.h"

namespace MGEN
{
    bool are_equal_float( const float& a, const float& b )
    {
        float epsilon = std::numeric_limits<float>::epsilon();
        return std::fabs( a - b ) < epsilon;
    }
} // namespace MGEN
