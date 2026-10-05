/* Local QNX NvSS ABI adapter. Compressed H.264 crosses the graphics bridge;
 * macOS VideoToolbox owns the hardware decoder. No guest GPU commands run. */
#include "bridge_transport.h"
extern void *memset(void *,int,unsigned long);
struct Video { u32 id,width,height,configured,target; unsigned char attrs[64]; };
static u32 next_video;
static u32 video_request(u32 op,const void *data,u32 size)
{
    u32 status=0x80000000;
    if(gl_conn()<0)return status;
    rec(op,size);PART(data,size);
    if(recv_all(&status,4))return 0x80000000;
    return status;
}
u32 NvSSVideoOpen(struct Video **out,const u32 *config)
{
    if(!out||!config||config[0]>1||config[4])return 0x80000005;
    *out=0;
    struct Video *v=malloc(sizeof(*v));if(!v)return 0x80000001;
    memset(v,0,sizeof(*v));v->id=__sync_add_and_fetch(&next_video,1);
    /* Physical display dimensions returned by the original NvMedia query. */
    v->target=config[0]==0; /* NvSS 0=LVDS cluster, 1=HDMI center. */
    ((u32 *)v->attrs)[0]=v->target?448:800;
    ((u32 *)v->attrs)[1]=v->target?448:480;
    ((u32 *)v->attrs)[12]=0x3f800000;
    ((u32 *)v->attrs)[13]=0x3f800000;
    ((u32 *)v->attrs)[14]=0x3f800000;
    *out=v;return 0;
}
u32 NvSSVideoStreamConfigure(struct Video *v,const u32 *p)
{
    if(!v||!p||!p[0]||!p[1]||p[0]>2048||p[1]>2048)return 0x80000005;
    u32 args[4]={v->id,p[0],p[1],v->target};
    u32 result=video_request(134,args,sizeof(args));if(result)return result;
    v->width=p[0];v->height=p[1];v->configured=1;
    ((u32 *)v->attrs)[5]=p[0];((u32 *)v->attrs)[6]=p[1];
    ((u32 *)v->attrs)[9]=((u32 *)v->attrs)[0];
    ((u32 *)v->attrs)[10]=((u32 *)v->attrs)[1];
    return 0;
}
u32 NvSSVideoSetAttribs(struct Video *v,const unsigned char *a)
{
    if(!v||!a)return 0x80000005;
    if(!v->configured)return 0x80000000;
    if(a[8])memcpy(v->attrs+12,a+12,32);
    if(a[44])memcpy(v->attrs+48,a+48,12);
    if(a[60])v->attrs[61]=a[61];
    unsigned char packet[68];*(u32 *)packet=v->id;
    memcpy(packet+4,v->attrs,64);
    return video_request(136,packet,sizeof(packet));
}
u32 NvSSVideoGetAttribs(struct Video *v,void *out)
{
    if(!v||!out)return 0x80000005;
    if(!v->configured)return 0x80000000;
    memcpy(out,v->attrs,64);return 0;
}
u32 NvSSVideoDecode(struct Video *v,const u32 *buffer,u32 timeout)
{
    (void)timeout;
    if(!v||!buffer||!buffer[0]||!buffer[1]||buffer[1]>1024*1024)return 0x80000005;
    if(!v->configured||gl_conn()<0)return 0x80000000;
    u32 header[2]={v->id,buffer[1]},status=0x80000000;
    rec(135,8+buffer[1]);PART(header,8);PART((const void *)buffer[0],buffer[1]);
    if(recv_all(&status,4))return 0x80000000;
    return status;
}
u32 NvSSVideoClose(struct Video *v)
{
    if(!v)return 0x80000005;
    u32 result=v->configured?video_request(137,&v->id,4):0;
    free(v);return result;
}
