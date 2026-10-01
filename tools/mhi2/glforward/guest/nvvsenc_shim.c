/* Local experimental encoder shim; delegates to the common EGL bridge. */
extern void *dlopen(const char *,int),*dlsym(void *,const char *);
static void *bridge;
static void *resolve(const char *name){if(!bridge)bridge=dlopen("libEGL.so",2);return bridge?dlsym(bridge,name):0;}
unsigned NvVideoStreamEncoderOpen(unsigned type,const void *settings,void **out)
{unsigned (*fn)(unsigned,const void *,void **)=resolve("mhi2EncoderOpen");return fn?fn(type,settings,out):0x80000001;}
unsigned NvVideoStreamEncoderEncodeFrame(void *encoder,void *image,int keyframe)
{unsigned (*fn)(void *,void *,int)=resolve("mhi2EncoderFrame");return fn?fn(encoder,image,keyframe):0x80000001;}
unsigned NvVideoStreamEncoderSetAttributes(void *encoder,const void *settings)
{unsigned (*fn)(void *,const void *)=resolve("mhi2EncoderSet");return fn?fn(encoder,settings):0x80000001;}
unsigned NvVideoStreamEncoderClose(void *encoder)
{unsigned (*fn)(void *)=resolve("mhi2EncoderClose");return fn?fn(encoder):0x80000001;}
