/* Shared bounded NvSS H.264 protocol and output planes on Linux and macOS. */
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#define VIDEO_ERROR 0x80000003u
#define VIDEO_MAX_PACKET (1024u*1024)
struct VideoDecoder {
    uint32_t owner,id,width,height,target;
    bool used,hidden,hardware,software_only,require_hardware;
    int source[4],destination[4];
    uint8_t *sps,*pps,*rgba;
    size_t sps_size,pps_size;
    unsigned frames,published_frames,session_frames;
    AVCodecContext *codec;
    struct SwsContext *scaler;
    enum AVPixelFormat hw_format;
};
static struct VideoDecoder video_decoders[4];
static void video_display_changed(void);
static struct VideoDecoder *video_find(uint32_t owner,uint32_t id)
{
    for(unsigned i=0;i<4;i++)if(video_decoders[i].used&&
        video_decoders[i].owner==owner&&video_decoders[i].id==id)return &video_decoders[i];
    return NULL;
}
#include "video_decoder_ffmpeg.h"
static void video_close(uint32_t owner,uint32_t id)
{
    struct VideoDecoder *v=video_find(owner,id);if(!v)return;
    video_end_session(v);
    free(v->sps);free(v->pps);free(v->rgba);
    memset(v,0,sizeof(*v));video_display_changed();
}
static uint32_t video_open(uint32_t owner,const uint8_t *p)
{
    uint32_t id=u32(p,0),w=u32(p,4),h=u32(p,8);
    const char *mode=getenv("MHI2_DECODER");
    if(mode && strcmp(mode,"auto") && strcmp(mode,"hardware") && strcmp(mode,"software"))return VIDEO_ERROR;
    if(!w||!h||w>2048||h>2048||!avcodec_find_decoder(AV_CODEC_ID_H264))return VIDEO_ERROR;
    video_close(owner,id);
    for(unsigned i=0;i<4;i++)if(!video_decoders[i].used){
        struct VideoDecoder *v=&video_decoders[i];
        v->owner=owner;v->id=id;v->width=w;v->height=h;
        v->source[2]=w;v->source[3]=h;v->destination[2]=800;v->destination[3]=480;
        v->software_only=mode&&!strcmp(mode,"software");
        v->require_hardware=(mode&&!strcmp(mode,"hardware"))||getenv("MHI2_DECODE_DEVICE");
        if(v->software_only && v->require_hardware){memset(v,0,sizeof(*v));return VIDEO_ERROR;}
        v->used=true;return 0;
    }
    return VIDEO_ERROR;
}
static unsigned video_start_code(const uint8_t *p,size_t n)
{
    if(n>=4&&!p[0]&&!p[1]&&!p[2]&&p[3]==1)return 4;
    if(n>=3&&!p[0]&&!p[1]&&p[2]==1)return 3;
    return 0;
}
static int video_parameter(struct VideoDecoder *v,bool sps,const uint8_t *p,size_t n)
{
    if(!n||n>65536)return -1;
    uint8_t **data=sps?&v->sps:&v->pps;size_t *size=sps?&v->sps_size:&v->pps_size;
    if(n==*size&&!memcmp(*data,p,n))return 0;
    uint8_t *copy=malloc(n);if(!copy)return -1;memcpy(copy,p,n);
    video_end_session(v);free(*data);*data=copy;*size=n;return 0;
}
static uint32_t video_decode(uint32_t owner,const uint8_t *p)
{
    struct VideoDecoder *v=video_find(owner,u32(p,0));
    size_t n=u32(p,4);p+=8;if(!v||!n||n>VIDEO_MAX_PACKET)return VIDEO_ERROR;
    bool annex=video_start_code(p,n)!=0,has_picture=false;
    uint8_t *sample=calloc(1,n+n/3+16+AV_INPUT_BUFFER_PADDING_SIZE);if(!sample)return VIDEO_ERROR;
    size_t off=0,used=0;uint32_t result=VIDEO_ERROR;
    while(off<n){
        size_t start,end;
        if(annex){unsigned sc=video_start_code(p+off,n-off);if(!sc)goto done;
            start=off+sc;end=start;while(end<n&&!video_start_code(p+end,n-end))end++;
            off=end;while(end>start&&p[end-1]==0)end--;
        }else{
            if(n-off<4)goto done;
            uint32_t len=((uint32_t)p[off]<<24)|((uint32_t)p[off+1]<<16)|((uint32_t)p[off+2]<<8)|p[off+3];
            start=off+4;if(!len||len>n-start)goto done;end=start+len;off=end;
        }
        if(end==start)continue;
        unsigned type=p[start]&31;size_t len=end-start;
        if(type==7||type==8){if(video_parameter(v,type==7,p+start,len))goto done;continue;}
        if((p[start]&0x80)||!type||type>23)goto done;
        if(type==1||type==5)has_picture=true;
        sample[used++]=0;sample[used++]=0;sample[used++]=0;sample[used++]=1;
        memcpy(sample+used,p+start,len);used+=len;
    }
    if(!has_picture){result=0;goto done;}
    if(!v->sps||!v->pps)goto done;
    if(!v->codec && video_start_session(v, false))goto done;
    result=video_submit(v,sample,used)?VIDEO_ERROR:0;
done:
    free(sample);return result;
}
static uint32_t video_attributes(uint32_t owner,const uint8_t *p)
{
    struct VideoDecoder *v=video_find(owner,u32(p,0));if(!v)return VIDEO_ERROR;
    p+=4;
    int source[4],dest[4];memcpy(source,p+12,16);memcpy(dest,p+28,16);
    if(source[0]<0||source[1]<0||source[2]<=0||source[3]<=0||
       (int64_t)source[0]+source[2]>v->width||(int64_t)source[1]+source[3]>v->height||
       dest[0]<0||dest[1]<0||dest[2]<=0||dest[3]<=0||
       (int64_t)dest[0]+dest[2]>2048||(int64_t)dest[1]+dest[3]>2048)return VIDEO_ERROR;
    memcpy(v->source,source,16);memcpy(v->destination,dest,16);v->hidden=p[61]!=0;
    v->published_frames=~0u; /* crop/visibility changes republish retained video */
    video_display_changed();return 0;
}
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
