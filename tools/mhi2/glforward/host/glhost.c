#define _GNU_SOURCE
/*
 * glhost - Linux host renderer for the MIB2 GL-forward shim.
 *
 * Experimental QNX GLES/EGL forwarding over stdin/stdout, backed by a Mesa
 * surfaceless EGL pbuffer. Guest records use [u32 opcode][u32 length][payload].
 * Replies are written to stdout; diagnostics go to stderr. This is a local
 * bring-up prototype with incomplete APIs and context isolation.
 *
 * Payload layouts are paired with guest/gl_shim.c.
 *   1 eglInitialize{w,h} 2 glViewport 3 glClearColor 4 glClear 5 swap 6 glFinish
 *   7-14 shaders/programs  15-18 buffers  19-21 vertex attribs  22-30 uniforms
 *   31-32 draws  33-43 pipeline state  50-58 textures  100/101 sync getters
 */
#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <errno.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "known_shaders.h"

#define DEFAULT_PORT 5005
#define DEFAULT_W    800
#define DEFAULT_H    480
#define MAXOBJ       (1 << 20)

static int win;
static EGLDisplay display;
static EGLSurface surface;
static EGLConfig config;
static int win_w, win_h;
static uint32_t current_pid;
static uint32_t primary_display_pid;
static GLuint default_gmap[MAXOBJ];
static GLuint *gmap = default_gmap;        /* guest object id -> real GL object */

/* NVIDIA binaries bake the source-over operation into the fragment program.
 * GLES2 can express these three programs without framebuffer-fetch extensions:
 * emit premultiplied source, then blend ONE / ONE_MINUS_SRC_ALPHA. Road fill
 * keeps destination alpha. Scope the override to the draw, preserving guest
 * state and keeping shader/program metadata isolated between guest contexts. */
struct BlendMetadata {
    uint8_t shader[MAXOBJ], program[MAXOBJ];
    uint32_t fragment[MAXOBJ], active_program;
};
static struct BlendMetadata default_blend, *blend = &default_blend;

struct BlendState {
    GLboolean enabled;
    GLint src_rgb, dst_rgb, src_alpha, dst_alpha, equation_rgb, equation_alpha;
};

static void begin_binary_blend(unsigned mode, struct BlendState *saved)
{
    if (!mode) return;
    saved->enabled = glIsEnabled(GL_BLEND);
    glGetIntegerv(GL_BLEND_SRC_RGB, &saved->src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &saved->dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &saved->src_alpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &saved->dst_alpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &saved->equation_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &saved->equation_alpha);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                        mode == 2 ? GL_ZERO : GL_ONE,
                        mode == 2 ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
}

static void end_binary_blend(unsigned mode, const struct BlendState *saved)
{
    if (!mode) return;
    glBlendFuncSeparate(saved->src_rgb, saved->dst_rgb, saved->src_alpha, saved->dst_alpha);
    glBlendEquationSeparate(saved->equation_rgb, saved->equation_alpha);
    if (!saved->enabled) glDisable(GL_BLEND);
}

static GLuint M(uint32_t id) { return id < MAXOBJ ? gmap[id] : 0; }

/* read a little-endian i32/u32/f32 from a payload at byte offset */
static int32_t i32(const uint8_t *p, int off) { int32_t v; memcpy(&v, p + off, 4); return v; }
static uint32_t u32(const uint8_t *p, int off) { uint32_t v; memcpy(&v, p + off, 4); return v; }

#include "encoder.h"

static void dump_frame(const char *path)
{
    if (!win) return;
    size_t n = (size_t)win_w * win_h;
    unsigned char *rgba = malloc(n * 4);
    if (!rgba) return;
    glReadPixels(0, 0, win_w, win_h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    char temporary[4096];
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    FILE *f = fopen(temporary, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", win_w, win_h);
        for (int y = win_h - 1; y >= 0; y--) {       /* flip: GL origin bottom-left */
            const unsigned char *row = rgba + (size_t)y * win_w * 4;
            for (int x = 0; x < win_w; x++)
                fwrite(row + x * 4, 1, 3, f);          /* RGBA -> RGB */
        }
        if (fclose(f) == 0) rename(temporary, path);
    }
    free(rgba);
}

static int read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r == 0) return -1;
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        p += r; n -= (size_t)r;
    }
    return 0;
}

static void ensure_window(int w, int h)
{
    if (win) return;
    if (w <= 0 || w > 8192) w = DEFAULT_W;
    if (h <= 0 || h > 8192) h = DEFAULT_H;

    display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    EGLint major, minor, count;
    const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
    const EGLint pb[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE};
    const EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    if (!eglInitialize(display, &major, &minor) ||
        !eglChooseConfig(display, attrs, &config, 1, &count) || !count) {
        fprintf(stderr, "EGL initialization failed %x\n", eglGetError()); exit(1);
    }
    surface = eglCreatePbufferSurface(display, config, pb);
    EGLContext ctx = eglCreateContext(display, config, EGL_NO_CONTEXT, ca);
    if (!eglMakeCurrent(display, surface, surface, ctx)) {
        fprintf(stderr, "EGL context failed %x\n", eglGetError()); exit(1);
    }
    win = 1;
    win_w = w; win_h = h;
    fprintf(stderr, "glhost: window %dx%d, GLES = %s | %s\n",
            w, h, glGetString(GL_VERSION), glGetString(GL_RENDERER));
    glViewport(0, 0, w, h);
    glClearColor(0.05f, 0.05f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    dump_frame("/tmp/glhost_frame.ppm");
    eglSwapBuffers(display, surface);
}

/* Separate guest processes have separate object namespaces and GL state. */
static void select_client(uint32_t pid)
{
    struct Client { uint32_t pid; EGLContext context; EGLSurface surface; GLuint *objects;
                    struct BlendMetadata *blend; };
    static struct Client clients[32];
    static unsigned count;
    if (pid == current_pid) return;
    ensure_window(0,0);
    unsigned i;
    for (i=0;i<count;i++) if (clients[i].pid==pid) break;
    if (i==count) {
        if (count==32) { fprintf(stderr,"Too many graphics clients\n"); exit(1); }
        const EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
        const EGLint pb[]={EGL_WIDTH,win_w,EGL_HEIGHT,win_h,EGL_NONE};
        clients[i].pid=pid;
        clients[i].context=eglCreateContext(display,config,EGL_NO_CONTEXT,ca);
        clients[i].surface=eglCreatePbufferSurface(display,config,pb);
        clients[i].objects=calloc(MAXOBJ,sizeof(GLuint));
        clients[i].blend=calloc(1,sizeof(struct BlendMetadata));
        if (!clients[i].objects || !clients[i].blend || clients[i].context==EGL_NO_CONTEXT || clients[i].surface==EGL_NO_SURFACE) exit(1);
        count++;
    }
    surface=clients[i].surface;
    if (!eglMakeCurrent(display,surface,surface,clients[i].context)) exit(1);
    gmap=clients[i].objects;
    blend=clients[i].blend;
    current_pid=pid;
}

static void pump_events(void) {}

static void check_shader(const char *what, GLuint obj, int is_prog)
{
    GLint ok = 1;
    if (is_prog) glGetProgramiv(obj, GL_LINK_STATUS, &ok);
    else         glGetShaderiv(obj, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]; GLsizei n = 0;
        if (is_prog) glGetProgramInfoLog(obj, sizeof(log), &n, log);
        else         glGetShaderInfoLog(obj, sizeof(log), &n, log);
        log[n < (GLsizei)sizeof(log) ? n : (GLsizei)sizeof(log) - 1] = 0;
        fprintf(stderr, "glhost: %s FAILED: %s\n", what, log);
    }
}

/* Reject a broken command boundary before it reaches Mesa or object tables. */
static int valid_record(uint32_t op,uint32_t len,const uint8_t *p)
{
    static const int sizes[120] = {
        [1]=9,
        [2]=17,
        [3]=17,
        [4]=5,
        [5]=1,
        [6]=1,
        [7]=9,
        [9]=5,
        [10]=5,
        [11]=9,
        [12]=5,
        [13]=5,
        [16]=9,
        [19]=25,
        [20]=5,
        [21]=5,
        [29]=9,
        [30]=9,
        [31]=13,
        [32]=17,
        [33]=5,
        [34]=5,
        [35]=9,
        [36]=5,
        [37]=5,
        [38]=5,
        [39]=5,
        [40]=5,
        [42]=5,
        [43]=9,
        [51]=9,
        [53]=13,
        [54]=5,
        [55]=9,
        [57]=5,
        [58]=13,
        [60]=9,
        [61]=9,
        [62]=21,
        [63]=17,
        [64]=17,
        [65]=17,
        [66]=17,
        [67]=5,
        [68]=5,
        [69]=13,
        [70]=13,
        [71]=17,
        [72]=5,
        [73]=9,
        [75]=9,
        [76]=17,
        [77]=17,
        [84]=21,
        [85]=9,
        [86]=17,
        [87]=9,
        [102]=9,
        [103]=9,
        [104]=5,
        [105]=1,
        [106]=5,
        [107]=5,
        [108]=9,
        [109]=5,
        [119]=5
    };
    if(op==127)return len>=24 && u32(p,4)>0 && u32(p,4)<=2048 && u32(p,8)>0 && u32(p,8)<=2048 &&
        !(u32(p,4)&1) && !(u32(p,8)&1) && u32(p,12)>0 && u32(p,12)<=60 &&
        u32(p,16)>=64000 && u32(p,16)<=20000000 && u32(p,20)<=1 && (uint64_t)u32(p,4)*u32(p,8)*4==len-24;
    if(op==122 || op==126)return len==4;
    if(op==123 || op==124)return len==12 && u32(p,8)<=4096;
    if(op==125)return len==4 && u32(p,0)==1;
    if(op==120)return len==24 && i32(p,8)>0 && i32(p,8)<=2048 &&
        i32(p,12)>0 && i32(p,12)<=2048 && u32(p,16)==GL_RGBA && u32(p,20)==GL_UNSIGNED_BYTE;
    if(op>=120)return 0;
    if(sizes[op] && len!=(unsigned)sizes[op]-1)return 0;
    if(op==7 || op==10 || op==12 || op==13)return u32(p,0)<MAXOBJ;
    if(op==11)return u32(p,0)<MAXOBJ && u32(p,4)<MAXOBJ;
    if(op==15 || op==50 || op==78 || op==79) {
        if(len<4 || (len-4)%4 || u32(p,0)!=(len-4)/4)return 0;
        for(unsigned i=4;i<len;i+=4)if(u32(p,i)>=MAXOBJ)return 0;
        return 1;
    }
    if(op==8 || op==100 || op==101)return len>=8 && u32(p,0)<MAXOBJ && u32(p,4)==len-8;
    if(op==14)return len>=12 && u32(p,8)==len-12;
    if(op==17)return len>=12 && (len==12 || u32(p,4)==len-12);
    if(op==18)return len>=12 && u32(p,8)==len-12;
    if(op>=22 && op<=25)return len>=8 && (uint64_t)u32(p,4)*(op-21)*4==len-8;
    if(op>=26 && op<=28)return len>=12 && (uint64_t)u32(p,4)*(op-24)*(op-24)*4==len-12;
    if(op>=80 && op<=83)return len>=8 && (uint64_t)u32(p,4)*(op-79)*4==len-8;
    if(op==52 || op==56)return len>=36 && u32(p,32)==len-36;
    if(op==110) {
        if(len<12 || (uint64_t)u32(p,0)*4+u32(p,8)!=len-12)return 0;
        for(unsigned i=0;i<u32(p,0);i++)if(u32(p,12+4*i)>=MAXOBJ)return 0;
        return 1;
    }
    return sizes[op]!=0;
}

static void serve(int cfd)
{
    uint8_t *p = NULL;
    size_t cap = 0;
    unsigned long frames = 0;
    uint32_t request_pid = 0;
    const char *capture_path=getenv("MHI2_GL_CAPTURE");
    FILE *capture = capture_path && *capture_path ? fopen(capture_path, "wb") : NULL;
    bool trace = getenv("MHI2_GL_TRACE") != NULL;
    bool dump_clients = getenv("MHI2_GL_DUMP_CLIENTS") != NULL;

    for (;;) {
        uint32_t hdr[2];
        if (read_full(cfd, hdr, 8) < 0) break;
        uint32_t op = hdr[0], len = hdr[1];
        if (len > 64 * 1024 * 1024) break;
        if (trace) fprintf(stderr, "glhost op=%u len=%u\n", op, len);
        if (len > cap) {
            cap = len + 4096;
            p = realloc(p, cap);
            if (!p) { fprintf(stderr, "glhost: OOM %u\n", len); break; }
        }
        if (len && read_full(cfd, p, len) < 0) break;
        if(capture){fwrite(hdr,1,8,capture);fwrite(p,1,len,capture);fflush(capture);}
        if(!valid_record(op,len,p)){
            fprintf(stderr,"Invalid graphics record: op=%u len=%u\n",op,len);
            break;
        }
        pump_events();
        /* Encoder traffic has no GL state. Polling must not flush another
         * client's renderer by repeatedly making the encoder owner's context
         * current. Owner selection takes effect lazily for graphics commands. */
        if(op!=119 && op!=122 && op!=126 && op!=127)select_client(request_pid);

        switch (op) {
        case 119: request_pid=u32(p,0); break;
        case 125: primary_display_pid=current_pid;break;
        case 122: encoder_close(request_pid,u32(p,0));break;
        case 123: case 124: {
            uint32_t count=u32(p,8),header[3]={0};
            char name[4096]={0};GLsizei length=0;GLint size=0;GLenum type=0;
            if(op==123)glGetActiveAttrib(M(u32(p,0)),u32(p,4),count,&length,&size,&type,name);
            else glGetActiveUniform(M(u32(p,0)),u32(p,4),count,&length,&size,&type,name);
            header[0]=length;header[1]=size;header[2]=type;
            if(write(STDOUT_FILENO,header,12)!=12)return;
            unsigned done=0;while(done<count){ssize_t n=write(STDOUT_FILENO,name+done,count-done);
                if(n<0&&errno==EINTR)continue;if(n<=0)return;done+=n;}
            break;
        }
        case 127: {
            uint32_t status=encoder_submit(request_pid,p);
            if(write(STDOUT_FILENO,&status,4)!=4)return;
            break;
        }
        case 126: {
            uint8_t *data;uint32_t count=encoder_poll(request_pid,u32(p,0),&data);
            if(write(STDOUT_FILENO,&count,4)!=4){free(data);return;}
            size_t off=0;while(data&&off<count){ssize_t n=write(STDOUT_FILENO,data+off,count-off);
                if(n<0&&errno==EINTR)continue;if(n<=0){free(data);return;}off+=n;}
            free(data);break;
        }
        case 120: {
            size_t size=(size_t)u32(p,8)*u32(p,12)*4;
            unsigned char *pixels=calloc(1,size);if(!pixels)return;
            GLint alignment;glGetIntegerv(GL_PACK_ALIGNMENT,&alignment);
            glPixelStorei(GL_PACK_ALIGNMENT,1);
            glReadPixels(i32(p,0),i32(p,4),i32(p,8),i32(p,12),GL_RGBA,GL_UNSIGNED_BYTE,pixels);
            glPixelStorei(GL_PACK_ALIGNMENT,alignment);
            size_t done=0;
            while(done<size){ssize_t n=write(STDOUT_FILENO,pixels+done,size-done);
                if(n<0&&errno==EINTR)continue;
                if(n<=0){free(pixels);return;}done+=n;}
            free(pixels);break;
        }
        case 1:  ensure_window(len >= 8 ? i32(p, 0) : 0, len >= 8 ? i32(p, 4) : 0); break;
        case 2:  ensure_window(0, 0); glViewport(i32(p,0), i32(p,4), i32(p,8), i32(p,12)); break;
        case 3:  ensure_window(0, 0); { float c[4]; memcpy(c, p, 16);
                 glClearColor(c[0], c[1], c[2], c[3]); } break;
        case 4:  ensure_window(0, 0); glClear(u32(p, 0)); break;
        case 5:  ensure_window(0, 0);
                 if(!primary_display_pid || current_pid==primary_display_pid)
                     dump_frame("/tmp/glhost_frame.ppm");
                 if(dump_clients){char path[128];snprintf(path,sizeof(path),"/tmp/mhi2-client-%u.ppm",current_pid);dump_frame(path);}
                 eglSwapBuffers(display, surface);
                 if (frames < 3 || (frames % 120) == 0)
                     fprintf(stderr, "glhost: %lu frames swapped\n", frames + 1);
                 frames++; break;
        case 6:  ensure_window(0, 0); glFinish(); break;

        case 7: { /* glCreateShader {id, type} */
            uint32_t id = u32(p,0), type = u32(p,4);
            blend->shader[id]=0;
            gmap[id] = glCreateShader(type); } break;
        case 8: { /* glShaderSource {id, len, src} */
            uint32_t id = u32(p,0); GLint sl = (GLint)u32(p,4);
            const GLchar *src = (const GLchar *)(p + 8);
            char path[128];snprintf(path,sizeof(path),"/tmp/mhi2-source-%u.glsl",id);
            FILE *f=fopen(path,"wb");if(f){fwrite(src,1,sl,f);fclose(f);}
            blend->shader[id]=0;
            glShaderSource(M(id), 1, &src, &sl); } break;
        case 9:  glCompileShader(M(u32(p,0))); check_shader("compile", M(u32(p,0)), 0); break;
        case 10:
            blend->program[u32(p,0)]=0;blend->fragment[u32(p,0)]=0;
            gmap[u32(p,0)] = glCreateProgram(); break;
        case 11: {
            GLint type=0;glGetShaderiv(M(u32(p,4)),GL_SHADER_TYPE,&type);
            if(type==GL_FRAGMENT_SHADER)blend->fragment[u32(p,0)]=u32(p,4);
            glAttachShader(M(u32(p,0)), M(u32(p,4))); break;
        }
        case 12:
            glLinkProgram(M(u32(p,0))); check_shader("link", M(u32(p,0)), 1);
            blend->program[u32(p,0)]=blend->shader[blend->fragment[u32(p,0)]];break;
        case 13: {
            uint32_t id=u32(p,0);GLint active=0;
            glUseProgram(M(id));glGetIntegerv(GL_CURRENT_PROGRAM,&active);
            if((GLuint)active==M(id))blend->active_program=id;break;
        }
        case 14: { /* glBindAttribLocation {pid, index, namelen, name} */
            uint32_t pid = u32(p,0), idx = u32(p,4), nl = u32(p,8);
            char nm[256]; uint32_t k = nl < 255 ? nl : 255; memcpy(nm, p+12, k); nm[k]=0;
            glBindAttribLocation(M(pid), idx, nm); } break;

        case 15: { /* glGenBuffers {n, ids[n]} */
            uint32_t n = u32(p,0);
            for (uint32_t i = 0; i < n; i++) { GLuint b; glGenBuffers(1, &b); gmap[u32(p,4+i*4)] = b; }
            } break;
        case 16: glBindBuffer(u32(p,0), M(u32(p,4))); break;
        case 17: { /* glBufferData {target, size, usage, data} */
            uint32_t tgt = u32(p,0), sz = u32(p,4), usage = u32(p,8);
            const void *data = (len >= 12 + sz && sz) ? (p + 12) : NULL;
            glBufferData(tgt, sz, data, usage); } break;
        case 18: { /* glBufferSubData {target, offset, size, data} */
            uint32_t tgt = u32(p,0), off = u32(p,4), sz = u32(p,8);
            glBufferSubData(tgt, off, sz, (len >= 12 + sz && sz) ? (p + 12) : NULL); } break;

        case 19: /* glVertexAttribPointer {index,size,type,norm,stride,offset} */
            glVertexAttribPointer(u32(p,0), i32(p,4), u32(p,8), u32(p,12) ? GL_TRUE : GL_FALSE,
                                  i32(p,16), (const void *)(uintptr_t)u32(p,20)); break;
        case 20: glEnableVertexAttribArray(u32(p,0)); break;
        case 21: glDisableVertexAttribArray(u32(p,0)); break;

        case 22: glUniform1fv(i32(p,0), i32(p,4), (const GLfloat *)(p+8)); break;
        case 23: glUniform2fv(i32(p,0), i32(p,4), (const GLfloat *)(p+8)); break;
        case 24: glUniform3fv(i32(p,0), i32(p,4), (const GLfloat *)(p+8)); break;
        case 25: glUniform4fv(i32(p,0), i32(p,4), (const GLfloat *)(p+8)); break;
        case 26: glUniformMatrix2fv(i32(p,0), i32(p,4), u32(p,8)?GL_TRUE:GL_FALSE, (const GLfloat *)(p+12)); break;
        case 27: glUniformMatrix3fv(i32(p,0), i32(p,4), u32(p,8)?GL_TRUE:GL_FALSE, (const GLfloat *)(p+12)); break;
        case 28: glUniformMatrix4fv(i32(p,0), i32(p,4), u32(p,8)?GL_TRUE:GL_FALSE, (const GLfloat *)(p+12)); break;
        case 29: { float v; memcpy(&v, p+4, 4); glUniform1f(i32(p,0), v); } break;
        case 30: glUniform1i(i32(p,0), i32(p,4)); break;

        case 31: case 32: {
            unsigned mode=blend->program[blend->active_program];
            struct BlendState saved;
            begin_binary_blend(mode,&saved);
            if(op==31)glDrawArrays(u32(p,0),i32(p,4),i32(p,8));
            else glDrawElements(u32(p,0),i32(p,4),u32(p,8),(const void *)(uintptr_t)u32(p,12));
            end_binary_blend(mode,&saved);break;
        }

        case 33: glEnable(u32(p,0)); break;
        case 34: glDisable(u32(p,0)); break;
        case 35: glBlendFunc(u32(p,0), u32(p,4)); break;
        case 36: glDepthFunc(u32(p,0)); break;
        case 37: glCullFace(u32(p,0)); break;
        case 38: glFrontFace(u32(p,0)); break;
        case 39: glDepthMask(u32(p,0) ? GL_TRUE : GL_FALSE); break;
        case 40: { float d; memcpy(&d, p, 4); glClearDepthf(d); } break;
        case 42: { float w; memcpy(&w, p, 4); glLineWidth(w); } break;
        case 43: { float f[2]; memcpy(f, p, 8); glPolygonOffset(f[0], f[1]); } break;

        case 50: { uint32_t n = u32(p,0);
            for (uint32_t i = 0; i < n; i++) { GLuint t; glGenTextures(1, &t); gmap[u32(p,4+i*4)] = t; }
            } break;
        case 51: glBindTexture(u32(p,0), M(u32(p,4))); break;
        case 52: { /* glTexImage2D {target,level,ifmt,w,h,border,fmt,type,size,data} */
            uint32_t sz = u32(p,32);
            glTexImage2D(u32(p,0), i32(p,4), i32(p,8), i32(p,12), i32(p,16), i32(p,20),
                         u32(p,24), u32(p,28), (sz && len >= 36 + sz) ? (p + 36) : NULL); } break;
        case 53: glTexParameteri(u32(p,0), u32(p,4), i32(p,8)); break;
        case 54: glActiveTexture(u32(p,0)); break;
        case 55: glPixelStorei(u32(p,0), i32(p,4)); break;
        case 56: { /* glTexSubImage2D {target,level,x,y,w,h,fmt,type,size,data} */
            uint32_t sz = u32(p,32);
            glTexSubImage2D(u32(p,0), i32(p,4), i32(p,8), i32(p,12), i32(p,16), i32(p,20),
                            u32(p,24), u32(p,28), (sz && len >= 36 + sz) ? (p + 36) : NULL); } break;
        case 57: glGenerateMipmap(u32(p,0)); break;
        case 58: { float v; memcpy(&v, p+8, 4); glTexParameterf(u32(p,0), u32(p,4), v); } break;

        case 110: {
            static unsigned binary_id;
            char path[128];
            uint32_t count=u32(p,0), format=u32(p,4), size=u32(p,8);
            snprintf(path,sizeof(path),"/tmp/mhi2-shader-%u-%x.bin",binary_id++,format);
            FILE *f=fopen(path,"wb");
            if(f){fwrite(p+12+count*4,1,size,f);fclose(f);}
            fprintf(stderr,"shader binary %s count=%u size=%u\n",path,count,size);
            const uint8_t *binary = p+12+count*4;
            uint64_t hash = UINT64_C(14695981039346656037);
            for (unsigned i=0;i<size;i++) hash=(hash^binary[i])*UINT64_C(1099511628211);
            bool matched=false;
            for (unsigned i=0;i<sizeof(shaders)/sizeof(shaders[0]);i++) {
                if (shaders[i].size==size && shaders[i].hash==hash && count==1) {
                    GLuint shader=M(u32(p,12));
                    blend->shader[u32(p,12)]=shaders[i].blend;
                    glShaderSource(shader,1,&shaders[i].source,NULL);
                    glCompileShader(shader);check_shader("translated shader",shader,0);
                    matched=true;break;
                }
            }
            if (!matched) fprintf(stderr,"UNSUPPORTED NVIDIA shader hash=%016llx\n",(unsigned long long)hash);

            break;
        }
        case 104: {
            GLint out[16]={0};
            GLenum pname = u32(p,0);
            if (pname != GL_COMPRESSED_TEXTURE_FORMATS &&
                pname != GL_SHADER_BINARY_FORMATS &&
                pname != GL_NUM_COMPRESSED_TEXTURE_FORMATS &&
                pname != GL_NUM_SHADER_BINARY_FORMATS) {
                glGetIntegerv(pname,out);
            }
            if(write(STDOUT_FILENO,out,sizeof(out)) != sizeof(out))return;
            break;
        }
        case 106: {
            GLboolean out[16]={0};
            GLenum pname = u32(p,0);
            if (pname != GL_COMPRESSED_TEXTURE_FORMATS &&
                pname != GL_SHADER_BINARY_FORMATS &&
                pname != GL_NUM_COMPRESSED_TEXTURE_FORMATS &&
                pname != GL_NUM_SHADER_BINARY_FORMATS) {
                glGetBooleanv(pname,out);
            }
            if(write(STDOUT_FILENO,out,sizeof(out))!=sizeof(out))return;
            break;
        }
        case 107: {
            GLfloat out[16]={0};
            GLenum pname = u32(p,0);
            if (pname != GL_COMPRESSED_TEXTURE_FORMATS &&
                pname != GL_SHADER_BINARY_FORMATS &&
                pname != GL_NUM_COMPRESSED_TEXTURE_FORMATS &&
                pname != GL_NUM_SHADER_BINARY_FORMATS) {
                glGetFloatv(pname,out);
            }
            if(write(STDOUT_FILENO,out,sizeof(out))!=sizeof(out))return;
            break;
        }
        case 108: {
            GLint out[3]={0};glGetShaderPrecisionFormat(u32(p,0),u32(p,4),out,out+2);
            if(write(STDOUT_FILENO,out,sizeof(out))!=sizeof(out))return;
            break;
        }
        case 105: {
            int32_t out=glGetError();
            if(write(STDOUT_FILENO,&out,4)!=4)return;
            break;
        }
        case 102: case 103: {
            GLint out = 0;
            if (op == 102) glGetShaderiv(M(u32(p,0)),u32(p,4),&out);
            else glGetProgramiv(M(u32(p,0)),u32(p,4),&out);
            if (write(STDOUT_FILENO,&out,4) != 4) return;
            break;
        }
        case 60: glBindFramebuffer(u32(p,0),M(u32(p,4))); break;
        case 61: glBindRenderbuffer(u32(p,0),M(u32(p,4))); break;
        case 62: glFramebufferTexture2D(u32(p,0),u32(p,4),u32(p,8),M(u32(p,12)),i32(p,16)); break;
        case 63: glRenderbufferStorage(u32(p,0),u32(p,4),i32(p,8),i32(p,12)); break;
        case 64: glFramebufferRenderbuffer(u32(p,0),u32(p,4),u32(p,8),M(u32(p,12))); break;
        case 65: glScissor(i32(p,0),i32(p,4),i32(p,8),i32(p,12)); break;
        case 66: glColorMask(u32(p,0),u32(p,4),u32(p,8),u32(p,12)); break;
        case 67: glClearStencil(i32(p,0)); break;
        case 68: glStencilMask(u32(p,0)); break;
        case 69: glStencilFunc(u32(p,0),i32(p,4),u32(p,8)); break;
        case 70: glStencilOp(u32(p,0),u32(p,4),u32(p,8)); break;
        case 71: glBlendFuncSeparate(u32(p,0),u32(p,4),u32(p,8),u32(p,12)); break;
        case 72: glBlendEquation(u32(p,0)); break;
        case 73: glBlendEquationSeparate(u32(p,0),u32(p,4)); break;
        case 75: glStencilMaskSeparate(u32(p,0),u32(p,4)); break;
        case 76: glStencilFuncSeparate(u32(p,0),u32(p,4),i32(p,8),u32(p,12)); break;
        case 77: glStencilOpSeparate(u32(p,0),u32(p,4),u32(p,8),u32(p,12)); break;
        case 78: {unsigned count=u32(p,0);for(unsigned i=0;i<count;i++){GLuint o;glGenFramebuffers(1,&o);gmap[u32(p,4+4*i)]=o;}break;}
        case 79: {unsigned count=u32(p,0);for(unsigned i=0;i<count;i++){GLuint o;glGenRenderbuffers(1,&o);gmap[u32(p,4+4*i)]=o;}break;}
        case 109: {uint32_t out=glCheckFramebufferStatus(u32(p,0));if(write(STDOUT_FILENO,&out,4)!=4)return;break;}
        case 80: glUniform1iv(i32(p,0),i32(p,4),(const GLint *)(p+8));break;
        case 81: glUniform2iv(i32(p,0),i32(p,4),(const GLint *)(p+8));break;
        case 82: glUniform3iv(i32(p,0),i32(p,4),(const GLint *)(p+8));break;
        case 83: glUniform4iv(i32(p,0),i32(p,4),(const GLint *)(p+8));break;
        case 84: {float v[4];memcpy(v,p+4,16);glVertexAttrib4fv(u32(p,0),v);break;}
        case 85: {float v[2];memcpy(v,p,8);glDepthRangef(v[0],v[1]);break;}
        case 86: {float v[4];memcpy(v,p,16);glBlendColor(v[0],v[1],v[2],v[3]);break;}
        case 87: glHint(u32(p,0),u32(p,4));break;
        case 100: case 101: { /* sync getters {pid, namelen, name} -> i32 */
            uint32_t pid = u32(p,0), nl = u32(p,4);
            char nm[256]; uint32_t k = nl < 255 ? nl : 255; memcpy(nm, p+8, k); nm[k]=0;
            GLint loc = (op == 100) ? glGetAttribLocation(M(pid), nm)
                                    : glGetUniformLocation(M(pid), nm);
            int32_t out = loc;
            if (write(STDOUT_FILENO, &out, 4) != 4) { fprintf(stderr, "glhost: getter reply failed\n"); }
            } break;

        default: /* unknown: drained, stay in sync */ break;
        }
    }
    for(unsigned i=0;i<4;i++)encoder_stop(&encoders[i]);
    if(capture)fclose(capture);
    free(p);
    fprintf(stderr, "glhost: client disconnected after %lu frames\n", frames);
}

int main(int argc, char **argv)
{
    signal(SIGPIPE,SIG_IGN);
    if (argc == 2 && strcmp(argv[1], "--check-shaders") == 0) {
        ensure_window(DEFAULT_W, DEFAULT_H);
        int failed = 0;
        for (size_t i = 0; i < sizeof(shaders) / sizeof(shaders[0]); i++) {
            GLuint shader = glCreateShader(strstr(shaders[i].source, "gl_Position") ?
                                           GL_VERTEX_SHADER : GL_FRAGMENT_SHADER);
            glShaderSource(shader, 1, &shaders[i].source, NULL);
            glCompileShader(shader);
            GLint compiled = 0;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
            check_shader("self-check", shader, 0);
            failed += !compiled;
            glDeleteShader(shader);
        }
        fprintf(stderr, "glhost: %zu translated shaders checked, %d failed\n",
                sizeof(shaders) / sizeof(shaders[0]), failed);
        return failed != 0;
    }
    serve(STDIN_FILENO);
    return 0;
}
