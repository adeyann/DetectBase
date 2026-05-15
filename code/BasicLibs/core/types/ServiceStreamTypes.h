#ifndef _MGEN_IO_STREAM_TYPES_H_
#define _MGEN_IO_STREAM_TYPES_H_

namespace MGEN
{
    enum class ServiceStreamType : unsigned char
    {
        None,
        InternalQueueing,
        SocketIO,
        RTSP,
        GRPC
    };
}
#endif