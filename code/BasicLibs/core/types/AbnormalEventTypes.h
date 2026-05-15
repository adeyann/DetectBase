#ifndef _MGEN_ABNORMAL_EVENT_TYPES_H_
#define _MGEN_ABNORMAL_EVENT_TYPES_H_

namespace MGEN::Abnormal
{
    enum class EventClass : int
    {
        None                = -1,  // not set
        // 사람 침입 감지
        LineIntrusion       = 202,  // 사람 경계선 침입
        AreaIntrusion       = 203,  // 사람 영역 침입
        // 차량 침입 감지
        VehicleIntrusion    = 209,  // 차량 경계선 침입
        VehicleParking      = 210,  // 차량 영역 침입 (체류/주정차)
    };
}
#endif