/* CWM frame reuse across independent GL process contexts, including fallback. */
#define main glhost_main
#include "glhost.c"
#undef main
#include <assert.h>

static unsigned char *capture(float red,float green,float blue)
{
    glClearColor(red,green,blue,1);glClear(GL_COLOR_BUFFER_BIT);
    unsigned char *p=malloc(8*8*4);
    glReadPixels(0,0,8,8,GL_RGBA,GL_UNSIGNED_BYTE,p);
    assert(glGetError()==GL_NO_ERROR);return p;
}
static void check_pixel(GLuint texture,unsigned red,unsigned green,unsigned blue)
{
    GLuint fbo;glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
    unsigned char p[4];glReadPixels(3,3,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p);
    assert(p[0]==red&&p[1]==green&&p[2]==blue&&p[3]==255);
    glBindFramebuffer(GL_FRAMEBUFFER,0);glDeleteFramebuffers(1,&fbo);
}
int main(void)
{
    ensure_window(8,8);select_client(100);
    native_frame_store(17,8,8,capture(1,0,0));
    select_client(200);
    GLuint tex;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT,8);
    assert(native_frame_upload(17,8,8));check_pixel(tex,255,0,0);
    GLint alignment;glGetIntegerv(GL_UNPACK_ALIGNMENT,&alignment);assert(alignment==8);
    assert(!native_frame_upload(17,7,8)&&!native_frame_upload(18,8,8));
    native_frame_remove(17);assert(native_frame_upload(17,8,8)); /* reader isn't owner */
    select_client(100);native_frame_store(17,8,8,capture(0,0,1));
    select_client(200);assert(native_frame_upload(17,8,8));check_pixel(tex,0,0,255);
    select_client(100);native_frame_remove(17);
    select_client(200);assert(!native_frame_upload(17,8,8));
    /* A hole before an existing entry must not leave duplicate stale frames. */
    select_client(100);
    native_frame_store(1,8,8,capture(1,0,0));native_frame_store(2,8,8,capture(1,0,0));
    native_frame_remove(1);native_frame_store(2,8,8,capture(0,1,0));
    unsigned copies=0;for(unsigned i=0;i<16;i++)copies+=native_frames[i].pixels&&native_frames[i].id==2;
    assert(copies==1);
    select_client(200);assert(native_frame_upload(2,8,8));check_pixel(tex,0,255,0);
    select_client(100);native_frame_remove(2);
    uint32_t request[7]={0,0,8,8,GL_RGBA,GL_UNSIGNED_BYTE,2};
    assert(valid_record(131,28,(uint8_t *)request));
    assert(!valid_record(131,24,(uint8_t *)request));
    request[2]=4096;assert(!valid_record(131,28,(uint8_t *)request));
    puts("PASS: cross-process CWM pixels, frame updates, owner cleanup, cache misses, GL state, record validation");
    return 0;
}
