
#ifndef _V4L2_H_
#define _V4L2_H_


#ifdef __cplusplus
extern "C" {
#endif

int v4l2_open_device(const char * filename);
int v4l2_init_device(int fd, int *width, int *height, uint32 pixelformat);


#ifdef __cplusplus
}
#endif

#endif // _V4L2_H_




