/* Local experiment: publish host-rendered pixels through the firmware's CWM.
 * ABI recovered from K3342 libnvwsi/libnvcwm/libKD in the shared Ghidra project.
 * NvRmSurface is eight 32-bit words; EGL target 0x3135 imports this structure.
 */
extern void *dlopen(const char *,int);
extern void *dlsym(void *,const char *);
extern int snprintf(char *,unsigned long,const char *,...);
extern int memcmp(const void *,const void *,unsigned long);
extern void *memcpy(void *,const void *,unsigned long);
extern void *memset(void *,int,unsigned long);
void glReadPixels(i32,i32,i32,i32,u32,u32,void *);
void glTexImage2D(u32,i32,i32,i32,i32,i32,u32,u32,const void *);
struct NvSurface {u32 width,height,format,layout,pitch,mem,offset,kind;};
struct NativeWindow {u32 window;struct NvSurface buffers[3];void *pointers[3], *mapped[3];u32 ids[3],index;};
struct NativeImage {struct NvSurface surface;unsigned char *cached;void *mapped;u32 id,cached_texture;};
static struct {
    void *rm,*cwm;u32 device;
    int (*open)(u32 *,u32);
    int (*create)(u32,u32 *,u32);
    int (*alloc)(u32,const void *,u32,u32,u32);
    void (*free)(u32);
    void (*read)(u32,u32,void *,u32);
    int (*map)(u32,u32,u32,u32,void **);
    void (*unmap)(u32,void *,u32);
    u32 (*id)(u32);
    u32 (*size)(u32);
    int (*from_id)(u32,u32 *);
    void (*get_size)(u32,u32 *,u32 *);
    int (*set_surfaces)(u32,void *,u32,i32,u32);
    int (*get_surface)(u32,u32 *,void *,u32 *);
    int (*swap)(u32,u32,u32,u32,unsigned long long,const void *);
} native;
static int native_init_once(void)
{
    if(native.device)return 1;
    native.rm=dlopen("libnvrm.so",2);native.cwm=dlopen("libnvcwm.so",2);
    if(!native.rm||!native.cwm)return 0;
#define RM(field,name) native.field=dlsym(native.rm,name);if(!native.field)return 0
#define CW(field,name) native.field=dlsym(native.cwm,name);if(!native.field)return 0
    RM(open,"NvRmOpen");RM(create,"NvRmMemHandleCreate");RM(alloc,"NvRmMemAlloc");
    RM(read,"NvRmMemRead");RM(free,"NvRmMemHandleFree");RM(map,"NvRmMemMap");RM(unmap,"NvRmMemUnmap");
    RM(id,"NvRmMemGetId");RM(size,"NvRmMemGetSize");RM(from_id,"NvRmMemHandleFromId");
    CW(get_size,"NvCwmWindowGetSize");CW(set_surfaces,"NvCwmWindowSetSurfaces");
    CW(get_surface,"NvCwmWindowGetRenderSurface");CW(swap,"NvCwmWindowSwap");
#undef RM
#undef CW
    return native.open(&native.device,0)==0;
}
static volatile u32 native_init_state;
static int native_init(void)
{
    for (;;) {
        if(__atomic_load_n(&native_init_state,__ATOMIC_ACQUIRE)==2)return 1;
        if(__sync_bool_compare_and_swap(&native_init_state,0,1))break;
        usleep(1000);
    }
    int ok=native_init_once();
    __atomic_store_n(&native_init_state,ok?2:0,__ATOMIC_RELEASE);
    return ok;
}
static void *native_window_create(void *window)
{
    u32 handle=(u32)window,w=0,h=0;
    char log[160];
    /* Native displaymanager's physical screen, distinct from CWM clients. */
    if(handle==0xdb000001){i32 primary=1;emit_iv(125,&primary,1);}
    if((handle&0xffff0000)!=0xbeeb0000||!native_init()){
        int n=snprintf(log,sizeof(log),"glbridge: native window %x has no CWM adapter\n",handle);write(2,log,n);return (void *)1;}
    native.get_size(handle,&w,&h);
    if(!w||!h||w>2048||h>2048)return 0;
    struct NativeWindow *s=malloc(sizeof(*s));if(!s)return 0;memset(s,0,sizeof(*s));s->window=handle;
    for(unsigned i=0;i<3;i++){
        struct NvSurface *b=&s->buffers[i];
        b->width=w;b->height=h;b->format=0x20168815;b->layout=0;b->pitch=w*4;
        if(native.create(native.device,&b->mem,w*h*4)||native.alloc(b->mem,0,0,4096,0))goto fail;
        /* K5126 NvRmMemMap(handle, offset, size, access, out): access bits
         * 1/2 are read/write. Keep the uncached allocation mapped for its
         * lifetime instead of copying every frame through nvmap devctlv. */
        if(native.map(b->mem,0,w*h*4,3,&s->mapped[i]))goto fail;
        memset(s->mapped[i],0,w*h*4);s->pointers[i]=b;s->ids[i]=native.id(b->mem);
    }
    if(native.set_surfaces(handle,s->pointers,3,0,1))goto fail;
    int n=snprintf(log,sizeof(log),"glbridge: CWM window %x publishes %ux%u, 3 RGBA buffers\n",handle,w,h);write(2,log,n);
    return s;
fail:
    for(unsigned i=0;i<3;i++){
        if(s->mapped[i])native.unmap(s->buffers[i].mem,s->mapped[i],w*h*4);
        if(s->buffers[i].mem)native.free(s->buffers[i].mem);
    }
    free(s);return 0;
}
static void native_window_swap(void *surface)
{
    if((u32)surface<=1)return;
    struct NativeWindow *s=surface;
    if(s->index>=3)return;
    struct NvSurface *b=&s->buffers[s->index];
    /* CWM EGL images retain the GLES bottom-left row order. TCG has coherent
     * RAM; the barrier orders publication before the CWM swap notification. */
    u32 readback[7]={0,0,b->width,b->height,0x1908,0x1401,s->ids[s->index]};
    emit_iv(131,(i32 *)readback,7);
    if(recv_all(s->mapped[s->index],b->width*b->height*4))return;
    __sync_synchronize();
    u32 fence[2]={~0u,0};
    int r=native.swap(s->window,1,s->index,0,0,fence);
    if(!r)native.get_surface(s->window,&s->index,0,0);
}
static void native_window_destroy(void *surface)
{
    if((u32)surface<=1)return;
    struct NativeWindow *s=surface;
    for(unsigned i=0;i<3;i++){
        struct NvSurface *b=&s->buffers[i];
        emit_iv(133,(i32 *)&s->ids[i],1);
        if(s->mapped[i])native.unmap(b->mem,s->mapped[i],b->height*b->pitch);
        if(b->mem)native.free(b->mem);
    }
    free(s);
}
void *eglCreateImageKHR(void *dpy,void *ctx,u32 target,void *buffer,const i32 *attrs)
{
    (void)dpy;(void)ctx;(void)attrs;
    if(target!=0x3135||!buffer||!native_init()){egl_error=0x300c;return 0;}
    struct NvSurface *b=buffer;
    if(!b->width||!b->height||b->width>2048||b->height>2048||b->layout||b->pitch<b->width*4||
       b->pitch>8192||b->offset>native.size(b->mem)||b->height>(native.size(b->mem)-b->offset)/b->pitch){egl_error=0x300c;return 0;}
    struct NativeImage *image=malloc(sizeof(*image));if(!image)return 0;
    image->surface=*b;image->cached=0;image->mapped=0;image->cached_texture=0;
    image->id=native.id(b->mem);
    if(native.from_id(image->id,&image->surface.mem)){free(image);return 0;}
    if(native.map(image->surface.mem,b->offset,b->pitch*b->height,1,&image->mapped)){
        native.free(image->surface.mem);free(image);egl_error=0x3003;return 0;
    }
    return image;
}
static struct {u32 texture;struct NativeImage *image;} image_bindings[256];
static u32 native_bound_texture;
static void native_upload_image(u32 target,struct NativeImage *image)
{
    struct NvSurface *b=&image->surface;
    u32 request[3]={image->id,b->width,b->height},hit=0;
    if(!b->offset&&b->pitch==b->width*4&&b->format==0x20168815){
        emit_iv(132,(i32 *)request,3);
        if(recv_all(&hit,4)==0&&hit)return;
    }
    unsigned char *pixels=malloc(b->pitch*b->height);if(!pixels)return;
    __sync_synchronize();
    memcpy(pixels,image->mapped,b->pitch*b->height);
    if(b->pitch!=b->width*4)for(u32 y=0;y<b->height;y++)
        for(u32 x=0;x<b->width*4;x++)pixels[y*b->width*4+x]=pixels[y*b->pitch+x];
    unsigned bytes=b->width*b->height*4;
    if(!image->cached||image->cached_texture!=native_bound_texture||memcmp(image->cached,pixels,bytes)){
        glTexImage2D(target,0,0x1908,b->width,b->height,0,0x1908,0x1401,pixels);
        free(image->cached);image->cached=pixels;image->cached_texture=native_bound_texture;
    }else free(pixels);
}
static void native_texture_bind(u32 target,u32 texture)
{
    if(target!=0xde1)return;
    native_bound_texture=texture;
    if(!texture)return;
    for(unsigned i=0;i<256;i++)if(image_bindings[i].texture==texture&&image_bindings[i].image){
        native_upload_image(target,image_bindings[i].image);break;}
}
u32 eglDestroyImageKHR(void *dpy,void *p)
{
    (void)dpy;if(!p)return 0;struct NativeImage *image=p;
    for(unsigned i=0;i<256;i++)if(image_bindings[i].image==image){image_bindings[i].texture=0;image_bindings[i].image=0;}
    native.unmap(image->surface.mem,image->mapped,image->surface.pitch*image->surface.height);
    native.free(image->surface.mem);free(image->cached);free(image);return 1;
}
void glEGLImageTargetTexture2DOES(u32 target,void *p)
{
    if(!p||target!=0xde1||!native_bound_texture)return;
    unsigned slot=256;
    for(unsigned i=0;i<256;i++)if(image_bindings[i].texture==native_bound_texture){slot=i;break;}
    if(slot==256)for(unsigned i=0;i<256;i++)if(!image_bindings[i].image){slot=i;break;}
    if(slot==256)return;
    image_bindings[slot].texture=native_bound_texture;image_bindings[slot].image=p;
    native_upload_image(target,p);
}
/* libKD uses this API table to release CWM buffers after an EGL consumer.
 * Host-rendered commands are synchronous at the pixel handoff, so no Tegra
 * GPU context or GPU fence is returned to the native window manager. */
static u32 native_api_unsupported(void){return 3;}
static u32 native_api_no_context(void){return 0;}
static u32 native_api_current(void){return 1;}
static u32 native_api_image(void *p,u32 *out)
{
    if(!p||!out)return 4;
    struct NvSurface *b=&((struct NativeImage *)p)->surface;
    memset(out,0,0x7c);memcpy(out,b,sizeof(*b));out[5]=0;
    out[0x60/4]=1;out[0x64/4]=native.id(b->mem);return 0;
}
static void native_register_api(u32 api)
{
    static void *callbacks[36];static u32 clients[16][30];
    if(api!=4&&api!=7)return;
    for(unsigned i=0;i<36;i++)callbacks[i]=(void *)native_api_unsupported;
    callbacks[1]=(void *)native_api_image;
    callbacks[5]=(void *)native_api_no_context;
    callbacks[6]=(void *)native_api_current;
    callbacks[7]=(void *)native_api_no_context;
    void *lib=dlopen(api==4?"libKD.so":"libnvvsenc.so",2);
    int (*init)(void *,void *)=lib?dlsym(lib,api==4?"NvKdApiInit":"NvtEglInit"):0;
    if(init)init(clients[api],callbacks);
}
