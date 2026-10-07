/* macOS hardware H.264 decoder backend. Included by video_decoder.h. */
static void video_end_session(struct VideoDecoder *v)
{
    if(v->session){VTDecompressionSessionWaitForAsynchronousFrames(v->session);
        VTDecompressionSessionInvalidate(v->session);CFRelease(v->session);v->session=NULL;}
    if(v->format){CFRelease(v->format);v->format=NULL;}
    v->hardware=false;
}
static void video_close(uint32_t owner,uint32_t id)
{
    struct VideoDecoder *v=video_find(owner,id);if(!v)return;
    video_end_session(v);
    if(v->output)CVPixelBufferRelease(v->output);
    free(v->sps);free(v->pps);free(v->rgba);pthread_mutex_destroy(&v->lock);
    memset(v,0,sizeof(*v));video_display_changed();
}
static uint32_t video_open(uint32_t owner,const uint8_t *p)
{
    uint32_t id=u32(p,0),w=u32(p,4),h=u32(p,8);
    if(!w||!h||w>2048||h>2048)return VIDEO_ERROR;
    video_close(owner,id);
    for(unsigned i=0;i<4;i++)if(!video_decoders[i].used){
        struct VideoDecoder *v=&video_decoders[i];
        v->owner=owner;v->id=id;v->width=w;v->height=h;
        v->source[2]=w;v->source[3]=h;v->destination[2]=800;v->destination[3]=480;
        pthread_mutex_init(&v->lock,NULL);v->used=true;return 0;
    }
    return VIDEO_ERROR;
}
static void video_output(void *opaque,void *frame,OSStatus status,VTDecodeInfoFlags flags,
                         CVImageBufferRef image,CMTime pts,CMTime duration)
{
    struct VideoDecoder *v=opaque;(void)frame;(void)flags;(void)pts;(void)duration;
    pthread_mutex_lock(&v->lock);v->output_status=status;
    if(image){if(v->output)CVPixelBufferRelease(v->output);v->output=CVPixelBufferRetain(image);}
    pthread_mutex_unlock(&v->lock);
}
static int video_start_session(struct VideoDecoder *v)
{
    const uint8_t *sets[2]={v->sps,v->pps};size_t sizes[2]={v->sps_size,v->pps_size};
    OSStatus status=CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL,2,sets,sizes,4,&v->format);
    if(status)goto fail;
    CMVideoDimensions dims=CMVideoFormatDescriptionGetDimensions(v->format);
    if(dims.width!=v->width||dims.height!=v->height){status=-1;goto fail;}
    const void *key=kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder;
    const void *value=kCFBooleanTrue;
    CFDictionaryRef spec=CFDictionaryCreate(NULL,&key,&value,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    int pixel_format=kCVPixelFormatType_32BGRA;
    CFNumberRef pixel=CFNumberCreate(NULL,kCFNumberIntType,&pixel_format);
    const void *pixel_key=kCVPixelBufferPixelFormatTypeKey;
    CFDictionaryRef attrs=CFDictionaryCreate(NULL,&pixel_key,(const void **)&pixel,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    VTDecompressionOutputCallbackRecord callback={video_output,v};
    status=VTDecompressionSessionCreate(NULL,v->format,spec,attrs,&callback,&v->session);
    CFRelease(attrs);CFRelease(pixel);CFRelease(spec);
    if(status)goto fail;
    CFTypeRef hardware=NULL;
    status=VTSessionCopyProperty(v->session,kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,NULL,&hardware);
    v->hardware=!status&&hardware==kCFBooleanTrue;if(hardware)CFRelease(hardware);
    if(!v->hardware){status=-1;goto fail;}
    VTSessionSetProperty(v->session,kVTDecompressionPropertyKey_RealTime,kCFBooleanTrue);
    fprintf(stderr,"videobridge: VideoToolbox hardware=1 owner=%u id=%u %ux%u\n",v->owner,v->id,v->width,v->height);
    return 0;
fail:
    fprintf(stderr,"videobridge: hardware session failed: %d\n",(int)status);
    video_end_session(v);return -1;
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
    uint8_t *sample=malloc(n+n/3+16);if(!sample)return VIDEO_ERROR;
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
        if(!type||type>23)goto done;
        if(type==1||type==5)has_picture=true;
        sample[used++]=len>>24;sample[used++]=len>>16;sample[used++]=len>>8;sample[used++]=len;
        memcpy(sample+used,p+start,len);used+=len;
    }
    if(!has_picture){result=0;goto done;}
    if(!v->sps||!v->pps)goto done;
    if(!v->session&&video_start_session(v))goto done;
    CMBlockBufferRef block=NULL;CMSampleBufferRef buffer=NULL;
    OSStatus status=CMBlockBufferCreateWithMemoryBlock(NULL,NULL,used,NULL,NULL,0,used,0,&block);
    if(!status)status=CMBlockBufferReplaceDataBytes(sample,block,0,used);
    if(!status)status=CMSampleBufferCreateReady(NULL,block,v->format,1,0,NULL,1,&used,&buffer);
    v->output_status=0;
    if(!status)status=VTDecompressionSessionDecodeFrame(v->session,buffer,0,NULL,NULL);
    if(!status)status=VTDecompressionSessionWaitForAsynchronousFrames(v->session);
    if(buffer)CFRelease(buffer);if(block)CFRelease(block);
    pthread_mutex_lock(&v->lock);
    if(!status)status=v->output_status;
    CVPixelBufferRef output=v->output;v->output=NULL;
    pthread_mutex_unlock(&v->lock);
    if(!status&&output){
        if(CVPixelBufferGetWidth(output)!=v->width||CVPixelBufferGetHeight(output)!=v->height||
           CVPixelBufferGetPixelFormatType(output)!=kCVPixelFormatType_32BGRA)status=-1;
        else if(CVPixelBufferLockBaseAddress(output,kCVPixelBufferLock_ReadOnly))status=-1;
        else{
            if(!v->rgba)v->rgba=malloc((size_t)v->width*v->height*4);
            if(!v->rgba)status=-1;
            else{const uint8_t *base=CVPixelBufferGetBaseAddress(output);size_t stride=CVPixelBufferGetBytesPerRow(output);
                for(unsigned y=0;y<v->height;y++)for(unsigned x=0;x<v->width;x++){
                    const uint8_t *s=base+y*stride+x*4;uint8_t *d=v->rgba+((size_t)y*v->width+x)*4;
                    d[0]=s[2];d[1]=s[1];d[2]=s[0];d[3]=255;
                }
                if(++v->frames==1||v->frames%300==0)fprintf(stderr,"videobridge: decoded %u hardware frames\n",v->frames);
                video_display_changed();
            }
            CVPixelBufferUnlockBaseAddress(output,kCVPixelBufferLock_ReadOnly);
        }
    }
    if(output)CVPixelBufferRelease(output);
    if(status)fprintf(stderr,"videobridge: decode failed: %d\n",(int)status);
    result=status?VIDEO_ERROR:0;
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
