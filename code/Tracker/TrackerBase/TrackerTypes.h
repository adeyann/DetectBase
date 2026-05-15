#ifndef _MGEN_TRACKER_TYPES_H_
#define _MGEN_TRACKER_TYPES_H_

namespace MGEN
{
    // Define for Tracking : Default
    constexpr int   NON_TRACKING_IDX   = -1; // Tracker 진행되지 않은 객체의 track id

    // Flag for Tracking
    using     TrackFlag = unsigned char;
    constexpr TrackFlag TRACK_ALL_OFF    = 0b0000'0000;
    constexpr TrackFlag TRACK_ON_PERSON  = 0b0000'0001;
    constexpr TrackFlag TRACK_ON_CAR     = 0b0000'0010;

} // namespace MGEN
#endif