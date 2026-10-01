/* NvVideoStreamEncoder ABI forwarding; native displaymanager and ISO driver
 * continue to own frame selection, output queueing, and MOST transmission. */
extern int pthread_create(u32 *,const void *,void *(*)(void *),void *);
extern int pthread_join(u32,void **);
struct EncoderTime {i32 sec,nsec;};
extern int clock_gettime(int,struct EncoderTime *);
struct NativeEncoder {
    u32 id,width,height,fps,rate,opaque,mode;
    void (*callback)(void *,const u32 *);
    u32 thread,started,stop,last_frame_ms,timed;
};
static u32 encoder_serial;
u32 mhi2EncoderOpen(u32 type,const unsigned char *settings,void **result)
{
    if(type||!settings||!result)return 0x80000002;
    const u32 *words=(const u32 *)settings;
    u32 w=*(const unsigned short *)(settings+0x10),h=*(const unsigned short *)(settings+0x12);
    u32 fps=*(const unsigned short *)(settings+0x1c),rate=words[8];
    if(words[5]||words[6]!=1||!words[16]||!w||!h||w>2048||h>2048||(w&1)||(h&1))return 0x80000004;
    if(!fps||fps>60)return 0x80000004;
    if(rate<=20000000/240)rate*=240;
    if(rate<64000||rate>20000000)return 0x80000004;
    struct NativeEncoder *e=malloc(sizeof(*e));if(!e)return 0x80000001;
    *e=(struct NativeEncoder){__sync_add_and_fetch(&encoder_serial,1),w,h,fps,rate,words[9],words[6],(void *)words[16]};
    *result=e;
    char log[160];int n=snprintf(log,sizeof(log),"glbridge: native MOST encoder %u %ux%u fps=%u rate=%u\n",e->id,w,h,fps,rate);write(2,log,n);
    return 0;
}
/* The callback delivers a byte stream, not an image ownership token. Poll
 * complete TS packets on a separate thread and never hold the bridge lock
 * while waiting for the codec. Close joins this thread before freeing state. */
static void *encoder_receive(void *opaque)
{
    struct NativeEncoder *e=opaque;
    while(!__atomic_load_n(&e->stop,__ATOMIC_ACQUIRE)){
        rec(126,4);PART(&e->id,4);
        u32 count=0;if(recv_bytes(&count,4,0))break;
        if(count==~0u){unlock_record();write(2,"glbridge: async encoder failed\n",30);break;}
        if(!count){unlock_record();usleep(20000);continue;}
        if(count>188*256||count%188)abort();
        unsigned char *stream=malloc(count);if(!stream)abort();
        if(recv_all(stream,count)){free(stream);break;}
        if(!__atomic_load_n(&e->stop,__ATOMIC_ACQUIRE)){
            u32 event[5]={count,(u32)stream,e->opaque,e->mode,0};
            e->callback(e,event);
        }
        free(stream);
    }
    return 0;
}
u32 mhi2EncoderFrame(void *encoder,void *image,int keyframe)
{
    (void)keyframe;struct NativeEncoder *e=encoder;
    if(!e||!image)return 0x80000002;
    /* EGL images may have been created through the GLES DSO's exports. The
     * encoder resolves this entry through EGL, whose private RM table must
     * therefore be initialized independently before reading the image. */
    if(!native_init())return 0x80000001;
    struct NvSurface *b=&((struct NativeImage *)image)->surface;
    static unsigned reported;
    if(reported++<4){char log[160];int n=snprintf(log,sizeof(log),
        "glbridge: MOST input encoder=%u image=%p %ux%u layout=%u pitch=%u\n",
        e->id,image,b->width,b->height,b->layout,b->pitch);write(2,log,n);}
    if(b->width!=e->width||b->height!=e->height||b->layout)return 0x80000004;
    /* The display can refresh faster than the encoder's configured cadence.
     * Drop surplus presentations before copying pixels, without sleeping while
     * the display manager holds its own render locks. QNX monotonic clock=2. */
    struct EncoderTime now;
    if(!clock_gettime(2,&now)){
        u32 ms=(u32)now.sec*1000+(u32)now.nsec/1000000;
        u32 elapsed=ms-e->last_frame_ms;
        if(e->timed&&elapsed<1000&&elapsed*e->fps<1000)return 0;
        e->last_frame_ms=ms;e->timed=1;
    }
    unsigned size=e->width*e->height*4;
    if(size>16*1024*1024-20)return 0x80000004;
    unsigned char *pixels=malloc(size);if(!pixels)return 0x80000001;
    /* Bound each resource-manager transfer while avoiding one IPC per row. */
    u32 bottom_up=b->pitch==e->width*4;
    if(bottom_up){
        for(u32 off=0;off<size;){
            u32 count=size-off;if(count>32768)count=32768;
            native.read(b->mem,b->offset+off,pixels+off,count);off+=count;
        }
    }else for(u32 y=0;y<e->height;y++)
        native.read(b->mem,b->offset+(e->height-1-y)*b->pitch,pixels+y*e->width*4,e->width*4);
    u32 args[6]={e->id,e->width,e->height,e->fps,e->rate,bottom_up};
    rec(127,24+size);PART(args,24);PART(pixels,size);free(pixels);
    u32 status=1;if(recv_all(&status,4))return 0x80000001;
    if(status==1)return 0x80000001;
    if(status>2)return 0x80000001;
    if(!e->started){
        if(pthread_create(&e->thread,0,encoder_receive,e))return 0x80000001;
        e->started=1;
    }
    return 0;
}
u32 mhi2EncoderSet(void *encoder,const unsigned char *settings)
{
    struct NativeEncoder *e=encoder;if(!e||!settings)return 0x80000002;
    u32 fps=*(const unsigned short *)settings,rate=*(const u32 *)(settings+4);
    if(rate>20000000/240||!fps||fps>60||rate*240<64000)return 0x80000004;
    e->fps=fps;e->rate=rate*240;return 0;
}
u32 mhi2EncoderClose(void *encoder)
{
    struct NativeEncoder *e=encoder;if(!e)return 0;
    __atomic_store_n(&e->stop,1,__ATOMIC_RELEASE);
    if(e->started)pthread_join(e->thread,0);
    emit_iv(122,(i32 *)&e->id,1);u32 event[5]={0,0,e->opaque,e->mode,1};e->callback(e,event);free(e);return 0;
}
