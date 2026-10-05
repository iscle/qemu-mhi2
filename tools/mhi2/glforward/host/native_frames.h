/* Host copies of bridge-produced CWM buffers. The guest still receives the
 * pixels for native consumers; its compositor can reuse this copy without a
 * second trip through emulated RAM and a full texture-upload record. */
static struct NativeFrame {
    uint32_t id, width, height, owner;
    uint64_t used;
    unsigned char *pixels;
} native_frames[16];
static uint64_t native_frame_clock;

static void native_frame_store(uint32_t id, uint32_t width, uint32_t height,
                               unsigned char *pixels)
{
    struct NativeFrame *slot=&native_frames[0];
    for(unsigned i=0;i<16;i++){
        struct NativeFrame *f=&native_frames[i];
        if(f->pixels&&f->id==id){slot=f;break;}
        if(!f->pixels||f->used<slot->used)slot=f;
    }
    free(slot->pixels);
    *slot=(struct NativeFrame){id,width,height,current_pid,++native_frame_clock,pixels};
}

static uint32_t native_frame_upload(uint32_t id,uint32_t width,uint32_t height)
{
    for(unsigned i=0;i<16;i++){
        struct NativeFrame *f=&native_frames[i];
        if(f->pixels&&f->id==id&&f->width==width&&f->height==height){
            GLint alignment;
            glGetIntegerv(GL_UNPACK_ALIGNMENT,&alignment);
            glPixelStorei(GL_UNPACK_ALIGNMENT,1);
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,width,height,0,
                         GL_RGBA,GL_UNSIGNED_BYTE,f->pixels);
            glPixelStorei(GL_UNPACK_ALIGNMENT,alignment);
            f->used=++native_frame_clock;
            return 1;
        }
    }
    return 0;
}

static void native_frame_remove(uint32_t id)
{
    for(unsigned i=0;i<16;i++){
        struct NativeFrame *f=&native_frames[i];
        if(f->pixels&&f->id==id&&f->owner==current_pid){
            free(f->pixels);memset(f,0,sizeof(*f));
        }
    }
}
