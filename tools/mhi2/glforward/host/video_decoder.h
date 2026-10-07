/* Local macOS H.264 forwarding experiment. Require VideoToolbox hardware;
 * a missing/unsupported accelerator is an error, never a software fallback. */
#ifdef __APPLE__
#include <VideoToolbox/VideoToolbox.h>
#include <CoreVideo/CoreVideo.h>
#endif
#include <pthread.h>
#define VIDEO_ERROR 0x80000003u
#define VIDEO_MAX_PACKET (1024u*1024)
struct VideoDecoder {
    uint32_t owner,id,width,height,target;
    bool used,hidden,hardware;
    int source[4],destination[4];
    uint8_t *sps,*pps,*rgba;
    size_t sps_size,pps_size;
    unsigned frames,published_frames;

#ifdef __APPLE__
    CMVideoFormatDescriptionRef format;
    VTDecompressionSessionRef session;
    CVPixelBufferRef output;
    OSStatus output_status;
#endif
    pthread_mutex_t lock;
};
static struct VideoDecoder video_decoders[4];
static void video_display_changed(void);
static struct VideoDecoder *video_find(uint32_t owner,uint32_t id)
{
    for(unsigned i=0;i<4;i++)if(video_decoders[i].used&&
        video_decoders[i].owner==owner&&video_decoders[i].id==id)return &video_decoders[i];
    return NULL;
}
#ifdef __APPLE__
#include "video_decoder_videotoolbox.h"
#else
/* No native NvSS decoder backend on this host. Never acknowledge an open
 * that cannot produce frames: the guest must see the unsupported operation. */
static uint32_t video_open(uint32_t owner, const uint8_t *p)
{
    (void)owner;
    (void)p;
    fprintf(stderr, "videobridge: native H.264 decoding requires macOS VideoToolbox\n");
    return VIDEO_ERROR;
}
static uint32_t video_decode(uint32_t owner, const uint8_t *p)
{
    (void)owner;
    (void)p;
    return VIDEO_ERROR;
}
static uint32_t video_attributes(uint32_t owner, const uint8_t *p)
{
    (void)owner;
    (void)p;
    return VIDEO_ERROR;
}
static void video_close(uint32_t owner, uint32_t id)
{
    (void)owner;
    (void)id;
}
#endif
/* NvSS uses a separate video output plane. Its visibility and crop are
 * controlled by the guest, independent of the GL compositor's frame rate. */
static void video_composite(uint8_t *rgba,unsigned w,unsigned h)
{
    for(unsigned i=0;i<4;i++){
        struct VideoDecoder *v=&video_decoders[i];if(!v->used||v->hidden||!v->rgba||v->target)continue;
        int *d=v->destination,*s=v->source;
        for(int y=0;y<d[3]&&y+d[1]<(int)h;y++)for(int x=0;x<d[2]&&x+d[0]<(int)w;x++){
            unsigned sx=s[0]+(uint64_t)x*s[2]/d[2],sy=s[1]+(uint64_t)y*s[3]/d[3];
            memcpy(rgba+((size_t)(h-1-y-d[1])*w+x+d[0])*4,v->rgba+((size_t)sy*v->width+sx)*4,4);
        }
    }
}
