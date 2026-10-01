/* Experimental QNX GLES bridge, adapted from the workspace GL-forward prototype. */
extern long write(int,const void *,unsigned long);

/* GL-forward guest shim. Records: little-endian opcode, payload length, payload.
 * Transport: optional QEMU MMIO service at 0x5f000000. */
typedef unsigned int   u32;
typedef int            i32;
void glUniform2iv(i32,i32,const i32 *);
void glUniform3iv(i32,i32,const i32 *);
void glUniform4iv(i32,i32,const i32 *);

/* ---- libc / libsocket (resolved by ldqnx at load) ---- */
extern char *getenv(const char *);
extern int   atoi(const char *);
extern unsigned long strlen(const char *);
extern void *mmap_device_memory(void *, unsigned, int, int, unsigned long long);
extern int usleep(unsigned);
extern int getpid(void);
static volatile unsigned char *bridge;
static unsigned char *bulk;
static unsigned bulk_size;
extern void *memcpy(void *,const void *,unsigned long);
static int g_fd = -1;
static u32 g_id;
extern int pthread_key_create(unsigned *, void (*)(void *));
extern void *pthread_getspecific(unsigned);
extern int pthread_setspecific(unsigned, const void *);
extern void *malloc(unsigned long);
extern void *realloc(void *, unsigned long);
extern void free(void *);
extern void abort(void);
static unsigned record_key;
static volatile unsigned record_key_state;
struct Record { unsigned char *data; unsigned size, used, capacity, reply; };
static void release_record(void *p)
{ struct Record *r=p; if(r){free(r->data);free(r);} }
static struct Record *current_record(void)
{
    if (record_key_state != 2) {
        if (__sync_bool_compare_and_swap(&record_key_state,0,1)) {
            if(pthread_key_create(&record_key,release_record))abort();
            __sync_synchronize();record_key_state=2;
        } else while(record_key_state!=2)usleep(1000);
    }
    struct Record *r=pthread_getspecific(record_key);
    if(!r){r=malloc(sizeof(*r));if(!r)abort();
        r->data=0;r->capacity=r->used=r->size=r->reply=0;
        if(pthread_setspecific(record_key,r))abort();}
    return r;
}
static void unlock_record(void)
{ __asm__ volatile("dmb sy" ::: "memory"); *(volatile u32 *)(bridge+16)=0; }
static int gl_conn(void)
{
    if (!bridge) {
        /* QNX PROT_READ | PROT_WRITE | PROT_NOCACHE. */
        bridge = mmap_device_memory(0, 12288, 0xb00, 0, 0x5f000000ULL);
        if (bridge == (void *)-1) { bridge = 0; return -1; }
        if (*(volatile u32 *)bridge != 0x474c4252) { bridge = 0; return -1; }
        if (*(volatile u32 *)(bridge+24) == 1024*1024) {
            bulk = mmap_device_memory(0, 2*1024*1024, 0xb00, 0, 0x5e000000ULL);
            if (bulk == (void *)-1) bulk=0;
            if (bulk) bulk_size=1024*1024;
        }
        g_fd = 1;
    }
    return g_fd;
}
static void send_all(const void *p, unsigned long n)
{
    const unsigned char *c = p;
    if (gl_conn() < 0) return;
    while (n) {
        if (bulk) {
            unsigned chunk=n>bulk_size?bulk_size:n;
            memcpy(bulk,c,chunk);
            __asm__ volatile("dmb sy" ::: "memory");
            *(volatile u32 *)(bridge+28)=chunk;
            c+=chunk;n-=chunk;
            continue;
        }
        unsigned chunk = n > 4096 ? 4096 : n;
        unsigned i=0;
        for(;i+4<=chunk;i+=4){u32 v=c[i]|((u32)c[i+1]<<8)|((u32)c[i+2]<<16)|((u32)c[i+3]<<24);
            *(volatile u32 *)(bridge+0x1000+i)=v;}
        for(;i<chunk;i++)bridge[0x1000+i]=c[i];
        __asm__ volatile("dmb sy" ::: "memory");
        *(volatile u32 *)(bridge+4) = chunk;
        c += chunk; n -= chunk;
    }
}
static int recv_bytes(void *p, unsigned long n,int release)
{
    unsigned char *c = p;
    if (gl_conn() < 0) return -1;
    while (n) {
        unsigned tries = 0, available;
        while (!(available=*(volatile u32 *)(bridge+(bulk?32:8)))) {
            if (++tries > 10000) { unlock_record(); return -1; }
            usleep(1000);
        }
        unsigned chunk=available<n?available:n, i=0;
        if (bulk) {
            memcpy(c,bulk+bulk_size,chunk);
            __asm__ volatile("dmb sy" ::: "memory");
            *(volatile u32 *)(bridge+36)=chunk;
            c+=chunk;n-=chunk;
            continue;
        }
        for(;i+4<=chunk;i+=4){u32 v=*(volatile u32 *)(bridge+0x2000+i);
            for(unsigned j=0;j<4;j++)c[i+j]=v>>(8*j);}
        for(;i<chunk;i++)c[i]=bridge[0x2000+i];
        __asm__ volatile("dmb sy" ::: "memory");
        *(volatile u32 *)(bridge+20)=chunk;c+=chunk;n-=chunk;
    }
    if(release)unlock_record();
    return 0;
}

static int recv_all(void *p,unsigned long n){return recv_bytes(p,n,1);}

/* Build each complete command in thread-local storage before claiming MMIO.
 * Native clients can render from multiple threads and load both EGL/GLES DSOs. */
static void commit_record(struct Record *r)
{
    if(gl_conn()<0)return;
    while(*(volatile u32 *)(bridge+16))usleep(1000);
    send_all(r->data,r->size);
    if(!r->reply)unlock_record();
}
static void rec(u32 op,u32 total)
{
    struct Record *r=current_record();
    if(total>16*1024*1024)abort();
    unsigned size=20+total;
    if(size>r->capacity){r->data=realloc(r->data,size);if(!r->data)abort();r->capacity=size;}
    u32 *h=(u32 *)r->data;
    h[0]=119;h[1]=4;h[2]=getpid();h[3]=op;h[4]=total;
    r->size=size;r->used=20;r->reply=(op>=100&&op<=109)||op==120||op==121||op==123||op==124||op==126||op==127;
    if(!total)commit_record(r);
}
static void record_part(const void *p,unsigned n)
{
    if(!n)return;
    struct Record *r=current_record();
    if(n>r->size-r->used)abort();
    memcpy(r->data+r->used,p,n);
    r->used+=n;
    if(r->used==r->size)commit_record(r);
}
#define PART(p, n) record_part((p), (n))

/* simple all-int record */
static void emit_iv(u32 op, const i32 *v, u32 n)
{
    rec(op, n * 4);
    PART(v, n * 4);
}

static void native_register_api(u32 api);
void NvEglRegClientApi(unsigned api, void *unused)
{ (void)unused;native_register_api(api); }

static i32 egl_error = 0x3000;
static u32 bound_api = 0x30a0;
#include "native_window.h"
#include "native_encoder.h"
u32 eglQueryAPI(void) { return bound_api; }
u32 eglQueryContext(void *display,void *ctx,i32 attr,i32 *value)
{
    if(!display || !ctx || !value){egl_error=0x3006;return 0;}
    switch(attr){
    case 0x3028:*value=1;break; /* EGL_CONFIG_ID */
    case 0x3097:*value=bound_api;break;
    case 0x3098:*value=2;break;
    case 0x3086:*value=0x3084;break;
    default:egl_error=0x3004;return 0;
    }
    return 1;
}
u32 eglSurfaceAttrib(void *display,void *surface,i32 attr,i32 value)
{
    if(!display || !surface){egl_error=0x300d;return 0;}
    if(attr==0x3093 && value==0x3095)return 1; /* pbuffer remains preserved */
    egl_error=0x3004;return 0;
}
void glFinish(void);
u32 eglWaitClient(void) {glFinish();return 1;}
u32 eglBindTexImage(void *d,void *s,i32 buffer){egl_error=0x3009;return 0;}
u32 eglReleaseTexImage(void *d,void *s,i32 buffer){egl_error=0x3009;return 0;}
u32 eglCopyBuffers(void *d,void *s,void *target){egl_error=0x300a;return 0;}
void *eglCreatePixmapSurface(void *d,void *cfg,void *pix,const i32 *attrs)
{egl_error=0x300a;return 0;}
void *eglCreatePbufferFromClientBuffer(void *d,u32 type,void *buf,void *cfg,const i32 *attrs)
{egl_error=0x3009;return 0;}

/* ---------------- EGL ---------------- */
void *eglGetDisplay(void *id) { (void)id; return (void *)1; }
void *eglGetPlatformDisplay(u32 p, void *d, const void *a) { (void)p;(void)d;(void)a; return (void *)1; }

u32 eglInitialize(void *dpy, i32 *major, i32 *minor)
{
    (void)dpy;
    if (major) *major = 1;
    if (minor) *minor = 4;
    const char *ws = getenv("GLBRIDGE_W"), *hs = getenv("GLBRIDGE_H");
    i32 wh[2] = { ws ? atoi(ws) : 800, hs ? atoi(hs) : 480 };
    if (wh[0] <= 0) wh[0] = 800;
    if (wh[1] <= 0) wh[1] = 480;
    emit_iv(1, wh, 2);
    return 1;
}

u32 eglGetConfigs(void *dpy, void **configs, i32 size, i32 *num)
{ (void)dpy; if (configs && size > 0) configs[0] = (void *)1; if (num) *num = 1; return 1; }
u32 eglChooseConfig(void *dpy, const i32 *attr, void **configs, i32 size, i32 *num)
{ (void)dpy;(void)attr; if (configs && size > 0) configs[0] = (void *)1; if (num) *num = 1; return 1; }

u32 eglGetConfigAttrib(void *dpy, void *cfg, i32 attr, i32 *value)
{
    (void)dpy;(void)cfg;
    i32 v;
    switch (attr) {
    case 0x3024: case 0x3023: case 0x3022: case 0x3021: v = 8; break;
    case 0x3020: v = 32; break;
    case 0x3025: v = 24; break;
    case 0x3026: v = 8;  break;
    case 0x3028: v = 1;  break;
    case 0x302E: v = 1;  break;
    case 0x302F: v = 0;  break;
    case 0x3033: v = 0x0005; break;
    case 0x3040: v = 0x0044; break;
    case 0x3042: v = 0x0044; break;
    case 0x303F: v = 0x308E; break;
    case 0x3027: v = 0x3038; break;
    default: v = 0; break;
    }
    if (value) *value = v;
    return 1;
}

void *eglCreateWindowSurface(void *dpy, void *cfg, void *win, const i32 *attr)
{ (void)dpy;(void)cfg;(void)attr; return native_window_create(win); }
void *eglCreatePbufferSurface(void *dpy, void *cfg, const i32 *attr)
{ (void)dpy;(void)cfg;(void)attr; return (void *)1; }
void *eglCreateContext(void *dpy, void *cfg, void *share, const i32 *attr)
{ (void)dpy;(void)cfg;(void)share;(void)attr; return (void *)1; }
u32 eglMakeCurrent(void *dpy, void *draw, void *read, void *ctx)
{ (void)dpy;(void)draw;(void)read;(void)ctx; return 1; }

u32 eglQuerySurface(void *dpy, void *surf, i32 attr, i32 *value)
{
    (void)dpy;(void)surf;
    const char *ws = getenv("GLBRIDGE_W"), *hs = getenv("GLBRIDGE_H");
    i32 w = ws ? atoi(ws) : 800, h = hs ? atoi(hs) : 480;
    if ((u32)surf > 1) {
        struct NativeWindow *window = surf;
        w = window->buffers[0].width;
        h = window->buffers[0].height;
    }
    if (w <= 0) w = 800;
    if (h <= 0) h = 480;
    i32 v = 0;
    if (attr == 0x3057) v = w;        /* EGL_WIDTH */
    else if (attr == 0x3056) v = h;   /* EGL_HEIGHT */
    if (value) *value = v;
    return 1;
}
u32 eglSwapBuffers(void *dpy, void *surf) { (void)dpy; native_window_swap(surf); rec(5, 0); return 1; }
u32 eglSwapInterval(void *dpy, i32 n) { (void)dpy;(void)n; return 1; }
i32 eglGetError(void) {i32 e=egl_error;egl_error=0x3000;return e;}
u32 eglTerminate(void *dpy) { (void)dpy; return 1; }
u32 eglDestroySurface(void *dpy, void *s) { (void)dpy;(void)s; return 1; }
u32 eglDestroyContext(void *dpy, void *c) { (void)dpy;(void)c; return 1; }
u32 eglReleaseThread(void) { return 1; }
u32 eglBindAPI(u32 api) {if(api!=0x30a0){egl_error=0x300c;return 0;}bound_api=api;return 1;}
u32 eglWaitGL(void) { return 1; }
u32 eglWaitNative(i32 e) { (void)e; return 1; }
const char *eglQueryString(void *dpy, i32 name) { (void)dpy; if(name==0x3055)return "EGL_KHR_image EGL_KHR_image_base "; return "glshim-fwd"; }
void *eglGetProcAddress(const char *n);
void *eglGetCurrentDisplay(void) { return (void *)1; }
void *eglGetCurrentContext(void) { return (void *)1; }
void *eglGetCurrentSurface(i32 r) { (void)r; return (void *)1; }

/* ---------------- GLES2 frame ---------------- */
void glViewport(i32 x, i32 y, i32 w, i32 h) { i32 p[4] = { x, y, w, h }; emit_iv(2, p, 4); }
void glClearColor(float r, float g, float b, float a) { float p[4] = { r, g, b, a }; emit_iv(3, (i32 *)p, 4); }
void glClear(u32 mask) { emit_iv(4, (i32 *)&mask, 1); }
void glFinish(void) { rec(6, 0); }
void glFlush(void) { rec(6, 0); }

/* ---------------- shaders / programs ---------------- */
u32 glCreateShader(u32 type) { u32 id = __sync_add_and_fetch(&g_id,1); u32 p[2] = { id, type }; emit_iv(7, (i32 *)p, 2); return id; }
void glShaderSource(u32 sh, i32 count, const char *const *str, const i32 *len)
{
    u32 total = 0; i32 i;
    for (i = 0; i < count; i++)
        total += (len && len[i] >= 0) ? (u32)len[i] : (u32)strlen(str[i]);
    u32 h[2] = { sh, total };
    rec(8, 8 + total);
    PART(h, 8);
    for (i = 0; i < count; i++) {
        u32 l = (len && len[i] >= 0) ? (u32)len[i] : (u32)strlen(str[i]);
        PART(str[i], l);
    }
}
void glCompileShader(u32 sh) { emit_iv(9, (i32 *)&sh, 1); }
u32 glCreateProgram(void) { u32 id = __sync_add_and_fetch(&g_id,1); emit_iv(10, (i32 *)&id, 1); return id; }
void glAttachShader(u32 pid, u32 sid) { u32 p[2] = { pid, sid }; emit_iv(11, (i32 *)p, 2); }
void glLinkProgram(u32 pid) { emit_iv(12, (i32 *)&pid, 1); }
void glUseProgram(u32 pid) { emit_iv(13, (i32 *)&pid, 1); }
void glBindAttribLocation(u32 pid, u32 index, const char *name)
{
    u32 l = (u32)strlen(name); u32 h[3] = { pid, index, l };
    rec(14, 12 + l); PART(h, 12); PART(name, l);
}
void glDeleteShader(u32 s) { (void)s; }
void glDeleteProgram(u32 p) { (void)p; }
void glDetachShader(u32 p, u32 s) { (void)p;(void)s; }

/* shader/program queries the guest may check: report success */
void glGetShaderiv(u32 s, u32 pname, i32 *p) { u32 v[2]={s,pname}; emit_iv(102,(i32 *)v,2); i32 out=0; recv_all(&out,4); if(p)*p=out; }
void glGetProgramiv(u32 pr, u32 pname, i32 *p) { u32 v[2]={pr,pname}; emit_iv(103,(i32 *)v,2); i32 out=0; recv_all(&out,4); if(p)*p=out; }
void glGetShaderInfoLog(u32 s, i32 buf, i32 *len, char *log) { (void)s;(void)buf; if (len) *len = 0; if (log && buf > 0) log[0] = 0; }
void glGetProgramInfoLog(u32 p, i32 buf, i32 *len, char *log) { (void)p;(void)buf; if (len) *len = 0; if (log && buf > 0) log[0] = 0; }

/* ---------------- buffers ---------------- */
void glGenBuffers(i32 n, u32 *b)
{
    if (!b) return;
    rec(15, 4 + (u32)n * 4);
    u32 cnt = (u32)n; PART(&cnt, 4);
    for (i32 i = 0; i < n; i++) { b[i] = __sync_add_and_fetch(&g_id,1); PART(&b[i], 4); }
}
void glBindBuffer(u32 target, u32 id) { u32 p[2] = { target, id }; emit_iv(16, (i32 *)p, 2); }
void glBufferData(u32 target, i32 size, const void *data, u32 usage)
{
    u32 sz = size < 0 ? 0 : (u32)size;
    u32 dn = data ? sz : 0;
    u32 h[3] = { target, sz, usage };
    rec(17, 12 + dn); PART(h, 12); if (dn) PART(data, dn);
}
void glBufferSubData(u32 target, i32 offset, i32 size, const void *data)
{
    u32 sz = size < 0 ? 0 : (u32)size;
    u32 dn = data ? sz : 0;
    u32 h[3] = { target, (u32)offset, sz };
    rec(18, 12 + dn); PART(h, 12); if (dn) PART(data, dn);
}
void glDeleteBuffers(i32 n, const u32 *b) { (void)n;(void)b; }

/* ---------------- vertex attribs ---------------- */
void glVertexAttribPointer(u32 index, i32 size, u32 type, u32 norm, i32 stride, const void *ptr)
{
    u32 p[6] = { index, (u32)size, type, norm ? 1u : 0u, (u32)stride, (u32)(unsigned long)ptr };
    emit_iv(19, (i32 *)p, 6);
}
void glEnableVertexAttribArray(u32 index) { emit_iv(20, (i32 *)&index, 1); }
void glDisableVertexAttribArray(u32 index) { emit_iv(21, (i32 *)&index, 1); }

/* ---------------- uniforms ---------------- */
static void uni_vec(u32 op, i32 loc, i32 count, u32 per, const float *v)
{
    u32 h[2] = { (u32)loc, (u32)count };
    u32 fn = (u32)count * per * 4;
    rec(op, 8 + fn); PART(h, 8); if (fn) PART(v, fn);
}
static void uni_mat(u32 op, i32 loc, i32 count, u32 transpose, u32 elems, const float *v)
{
    u32 h[3] = { (u32)loc, (u32)count, transpose };
    u32 fn = (u32)count * elems * 4;
    rec(op, 12 + fn); PART(h, 12); if (fn) PART(v, fn);
}
void glUniform1fv(i32 loc, i32 count, const float *v) { uni_vec(22, loc, count, 1, v); }
void glUniform2fv(i32 loc, i32 count, const float *v) { uni_vec(23, loc, count, 2, v); }
void glUniform3fv(i32 loc, i32 count, const float *v) { uni_vec(24, loc, count, 3, v); }
void glUniform4fv(i32 loc, i32 count, const float *v) { uni_vec(25, loc, count, 4, v); }
void glUniformMatrix2fv(i32 loc, i32 count, u32 t, const float *v) { uni_mat(26, loc, count, t, 4, v); }
void glUniformMatrix3fv(i32 loc, i32 count, u32 t, const float *v) { uni_mat(27, loc, count, t, 9, v); }
void glUniformMatrix4fv(i32 loc, i32 count, u32 t, const float *v) { uni_mat(28, loc, count, t, 16, v); }
void glUniform1f(i32 loc, float v) { float p[2]; ((i32 *)p)[0] = loc; p[1] = v; rec(29, 8); PART(p, 8); }
void glUniform1i(i32 loc, i32 v) { i32 p[2] = { loc, v }; emit_iv(30, p, 2); }

/* ---------------- draws ---------------- */
void glDrawArrays(u32 mode, i32 first, i32 count) { i32 p[3] = { (i32)mode, first, count }; emit_iv(31, p, 3); }
void glDrawElements(u32 mode, i32 count, u32 type, const void *indices)
{ u32 p[4] = { mode, (u32)count, type, (u32)(unsigned long)indices }; emit_iv(32, (i32 *)p, 4); }

/* ---------------- pipeline state ---------------- */
void glEnable(u32 cap) { emit_iv(33, (i32 *)&cap, 1); }
void glDisable(u32 cap) { emit_iv(34, (i32 *)&cap, 1); }
void glBlendFunc(u32 s, u32 d) { u32 p[2] = { s, d }; emit_iv(35, (i32 *)p, 2); }
void glDepthFunc(u32 f) { emit_iv(36, (i32 *)&f, 1); }
void glCullFace(u32 m) { emit_iv(37, (i32 *)&m, 1); }
void glFrontFace(u32 m) { emit_iv(38, (i32 *)&m, 1); }
void glDepthMask(u32 flag) { emit_iv(39, (i32 *)&flag, 1); }
void glClearDepthf(float d) { rec(40, 4); PART(&d, 4); }
void glClearDepth(double d) { (void)d; }   /* desktop-GL variant; default 1.0 is fine */
void glLineWidth(float w) { rec(42, 4); PART(&w, 4); }
void glPolygonOffset(float factor, float units) { float p[2] = { factor, units }; rec(43, 8); PART(p, 8); }

/* ---------------- textures ---------------- */
void glGenTextures(i32 n, u32 *t)
{
    if (!t) return;
    rec(50, 4 + (u32)n * 4);
    u32 cnt = (u32)n; PART(&cnt, 4);
    for (i32 i = 0; i < n; i++) { t[i] = __sync_add_and_fetch(&g_id,1); PART(&t[i], 4); }
}
void glBindTexture(u32 target, u32 id) { u32 p[2] = { target, id }; emit_iv(51, (i32 *)p, 2); native_texture_bind(target,id); }
static u32 tex_bytes(u32 fmt, u32 type, i32 w, i32 h)
{
    u32 ch = 4;
    if (fmt == 0x1907) ch = 3;        /* GL_RGB */
    else if (fmt == 0x1909) ch = 1;   /* GL_LUMINANCE / DEPTH */
    else if (fmt == 0x190A) ch = 2;   /* GL_LUMINANCE_ALPHA */
    else if (fmt == 0x1906) ch = 1;   /* GL_ALPHA */
    u32 bp = ch;
    if (type == 0x8363 || type == 0x8033 || type == 0x8034) bp = 2; /* 565/4444/5551 packed */
    if (w < 0) w = 0; if (h < 0) h = 0;
    return (u32)w * (u32)h * bp;
}
void glTexImage2D(u32 target, i32 level, i32 ifmt, i32 w, i32 h, i32 border, u32 fmt, u32 type, const void *data)
{
    u32 sz = data ? tex_bytes(fmt, type, w, h) : 0;
    u32 hd[9] = { target, (u32)level, (u32)ifmt, (u32)w, (u32)h, (u32)border, fmt, type, sz };
    rec(52, 36 + sz); PART(hd, 36); if (sz) PART(data, sz);
}
void glTexParameteri(u32 target, u32 pname, i32 param) { i32 p[3] = { (i32)target, (i32)pname, param }; emit_iv(53, p, 3); }
void glTexParameterf(u32 target, u32 pname, float param) { u32 h[2] = { target, pname }; rec(58, 12); PART(h, 8); PART(&param, 4); }
void glActiveTexture(u32 texture) { emit_iv(54, (i32 *)&texture, 1); }
void glPixelStorei(u32 pname, i32 param) { i32 p[2] = { (i32)pname, param }; emit_iv(55, p, 2); }
void glTexSubImage2D(u32 target, i32 level, i32 x, i32 y, i32 w, i32 h, u32 fmt, u32 type, const void *data)
{
    u32 sz = data ? tex_bytes(fmt, type, w, h) : 0;
    u32 hd[8] = { target, (u32)level, (u32)x, (u32)y, (u32)w, (u32)h, fmt, type };
    /* pack sz as a 9th word for symmetry with glhost */
    u32 hd2[9]; for (int i=0;i<8;i++) hd2[i]=hd[i]; hd2[8]=sz;
    rec(56, 36 + sz); PART(hd2, 36); if (sz) PART(data, sz);
}
void glGenerateMipmap(u32 target) { emit_iv(57, (i32 *)&target, 1); }

/* ---------------- synchronous getters ---------------- */
i32 glGetAttribLocation(u32 pid, const char *name)
{
    u32 l = (u32)strlen(name); u32 h[2] = { pid, l };
    rec(100, 8 + l); PART(h, 8); PART(name, l);
    i32 loc = -1; recv_all(&loc, 4); return loc;
}
i32 glGetUniformLocation(u32 pid, const char *name)
{
    u32 l = (u32)strlen(name); u32 h[2] = { pid, l };
    rec(101, 8 + l); PART(h, 8); PART(name, l);
    i32 loc = -1; recv_all(&loc, 4); return loc;
}

/* misc still faked locally */
i32 glGetError(void) {rec(105,0);i32 e=0;recv_all(&e,4);return e;}
const char *glGetString(u32 name)
{
    switch (name) {
    case 0x1F00: return "glshim-fwd";
    case 0x1F01: return "glshim-fwd-renderer";
    case 0x1F02: return "OpenGL ES 2.0 (glshim-fwd)";
    case 0x8B8C: return "OpenGL ES GLSL ES 1.00";
    case 0x1F03: return "";
    default:     return "";
    }
}
void glGenFramebuffers(i32 n,u32 *objects){if(n<0||!objects)return;rec(78,4+4*n);PART(&n,4);for(i32 i=0;i<n;i++){objects[i]=__sync_add_and_fetch(&g_id,1);PART(&objects[i],4);}}
void glGenRenderbuffers(i32 n,u32 *objects){if(n<0||!objects)return;rec(79,4+4*n);PART(&n,4);for(i32 i=0;i<n;i++){objects[i]=__sync_add_and_fetch(&g_id,1);PART(&objects[i],4);}}
void glGetIntegerv(u32 pname, i32 *p) {
    emit_iv(104,(i32 *)&pname,1);i32 v[16];if(recv_all(v,64)<0)return;
    unsigned n=(pname==0xba2 || pname==0xc10 || pname==0xc23)?4:(pname==0xd3a?2:1);
    if(p)for(unsigned i=0;i<n;i++)p[i]=v[i];
}
void glGetFloatv(u32 pname,float *p) {
 emit_iv(107,(i32 *)&pname,1);float v[16];if(recv_all(v,64)<0)return;
 unsigned n=(pname==0xc22 || pname==0x8005)?4:(pname==0xb70 || pname==0x846d || pname==0x846e?2:1);
 if(p)for(unsigned i=0;i<n;i++)p[i]=v[i];
}

/* QNX system logger stubs: some demos/HMI binaries import slogf and it is not
 * exported by this rootfs's libc.so.3 -- satisfy it via our (NEEDED) shim. */
int slogf(void) { return 0; }
int vslogf(void) { return 0; }
int slog2f(void) { return 0; }
int slog2c(void) { return 0; }
int slog2_register(void) { return 0; }


/* ---- noop stubs for the rest of the exported ABI ---- */
int glBeginPerfMonitorAMD(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glBeginPerfMonitorAMD\n";write(2,msg,sizeof(msg)-1);} return 0; }
void glBindFramebuffer(u32 target,u32 object){u32 p[]={(u32)target,(u32)object};emit_iv(60,(i32 *)p,2);}
void glBindRenderbuffer(u32 target,u32 object){u32 p[]={(u32)target,(u32)object};emit_iv(61,(i32 *)p,2);}
int glBindVertexArrayOES(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glBindVertexArrayOES\n";write(2,msg,sizeof(msg)-1);} return 0; }
void glBlendColor(float r,float g,float b,float a){float v[4]={r,g,b,a};rec(86,16);PART(v,16);}
void glBlendEquation(u32 mode){u32 p[]={(u32)mode};emit_iv(72,(i32 *)p,1);}
void glBlendEquationSeparate(u32 rgb,u32 alpha){u32 p[]={(u32)rgb,(u32)alpha};emit_iv(73,(i32 *)p,2);}
void glBlendFuncSeparate(u32 sr,u32 dr,u32 sa,u32 da){u32 p[]={(u32)sr,(u32)dr,(u32)sa,(u32)da};emit_iv(71,(i32 *)p,4);}
u32 glCheckFramebufferStatus(u32 target){emit_iv(109,(i32 *)&target,1);u32 out=0;recv_all(&out,4);return out;}
void glClearStencil(i32 value){u32 p[]={(u32)value};emit_iv(67,(i32 *)p,1);}
void glColorMask(u32 r,u32 g,u32 b,u32 a){u32 p[]={(u32)r,(u32)g,(u32)b,(u32)a};emit_iv(66,(i32 *)p,4);}
int glCompressedTexImage2D(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glCompressedTexImage2D\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glCompressedTexImage3DOES(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glCompressedTexImage3DOES\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glCompressedTexSubImage2D(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glCompressedTexSubImage2D\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glCompressedTexSubImage3DOES(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glCompressedTexSubImage3DOES\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glCopyTexImage2D(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glCopyTexImage2D\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glCopyTexSubImage2D(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glCopyTexSubImage2D\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glCopyTexSubImage3DOES(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glCopyTexSubImage3DOES\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glDeleteFencesNV(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glDeleteFencesNV\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glDeleteFramebuffers(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glDeleteFramebuffers\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glDeletePerfMonitorsAMD(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glDeletePerfMonitorsAMD\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glDeleteRenderbuffers(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glDeleteRenderbuffers\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glDeleteTextures(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glDeleteTextures\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glDeleteVertexArraysOES(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glDeleteVertexArraysOES\n";write(2,msg,sizeof(msg)-1);} return 0; }
void glDepthRangef(float near,float far){float v[2]={near,far};rec(85,8);PART(v,8);}
int glDisableDriverControlQCOM(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glDisableDriverControlQCOM\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glDiscardFramebufferEXT(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glDiscardFramebufferEXT\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glEGLImageTargetRenderbufferStorageOES(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glEGLImageTargetRenderbufferStorageOES\n";write(2,msg,sizeof(msg)-1);} return 0; }

int glEnableDriverControlQCOM(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glEnableDriverControlQCOM\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glEndPerfMonitorAMD(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glEndPerfMonitorAMD\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glEndTilingQCOM(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glEndTilingQCOM\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glExtGetBufferPointervQCOM(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glExtGetBufferPointervQCOM\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glExtGetBuffersQCOM(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glExtGetBuffersQCOM\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glExtGetFramebuffersQCOM(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glExtGetFramebuffersQCOM\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glExtGetProgramBinarySourceQCOM(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glExtGetProgramBinarySourceQCOM\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glExtGetProgramsQCOM(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glExtGetProgramsQCOM\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glExtGetRenderbuffersQCOM(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glExtGetRenderbuffersQCOM\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glExtGetShadersQCOM(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glExtGetShadersQCOM\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glExtGetTexLevelParameterivQCOM(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glExtGetTexLevelParameterivQCOM\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glExtGetTexSubImageQCOM(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glExtGetTexSubImageQCOM\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glExtGetTexturesQCOM(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glExtGetTexturesQCOM\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glExtIsProgramBinaryQCOM(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glExtIsProgramBinaryQCOM\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glExtTexObjectStateOverrideiQCOM(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glExtTexObjectStateOverrideiQCOM\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glFinishFenceNV(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glFinishFenceNV\n";write(2,msg,sizeof(msg)-1);} return 0; }
void glFramebufferRenderbuffer(u32 target,u32 attachment,u32 rbTarget,u32 object){u32 p[]={(u32)target,(u32)attachment,(u32)rbTarget,(u32)object};emit_iv(64,(i32 *)p,4);}
void glFramebufferTexture2D(u32 target,u32 attachment,u32 textureTarget,u32 object,i32 level){u32 p[]={(u32)target,(u32)attachment,(u32)textureTarget,(u32)object,(u32)level};emit_iv(62,(i32 *)p,5);}
int glFramebufferTexture3DOES(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glFramebufferTexture3DOES\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glGenFencesNV(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGenFencesNV\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glGenPerfMonitorsAMD(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGenPerfMonitorsAMD\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glGenVertexArraysOES(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGenVertexArraysOES\n";write(2,msg,sizeof(msg)-1);} return 0; }
static void get_active(u32 op,u32 program,u32 index,i32 capacity,i32 *length,i32 *size,u32 *type,char *name)
{
    if(capacity<0)return;
    u32 count=capacity>4096?4096:(u32)capacity;
    u32 args[3]={program,index,count},result[3]={0};
    emit_iv(op,(i32 *)args,3);
    if(recv_bytes(result,12,0))return;
    char scratch[4096];
    if(count){if(recv_all(scratch,count))return;if(name)memcpy(name,scratch,count);}
    else unlock_record();
    if(length)*length=result[0];if(size)*size=result[1];if(type)*type=result[2];
}
void glGetActiveAttrib(u32 program,u32 index,i32 capacity,i32 *length,i32 *size,u32 *type,char *name)
{get_active(123,program,index,capacity,length,size,type,name);}
void glGetActiveUniform(u32 program,u32 index,i32 capacity,i32 *length,i32 *size,u32 *type,char *name)
{get_active(124,program,index,capacity,length,size,type,name);}
int glGetAttachedShaders(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGetAttachedShaders\n";write(2,msg,sizeof(msg)-1);} return 0; }
void glGetBooleanv(u32 pname,unsigned char *p) {
 emit_iv(106,(i32 *)&pname,1);unsigned char v[16];if(recv_all(v,16)<0)return;
 unsigned n=pname==0xc23?4:1;if(p)for(unsigned i=0;i<n;i++)p[i]=v[i];
}
int glGetBufferParameteriv(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGetBufferParameteriv\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glGetBufferPointervOES(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGetBufferPointervOES\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glGetDriverControlStringQCOM(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGetDriverControlStringQCOM\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glGetDriverControlsQCOM(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGetDriverControlsQCOM\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glGetFenceivNV(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGetFenceivNV\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glGetFramebufferAttachmentParameteriv(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGetFramebufferAttachmentParameteriv\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glGetPerfMonitorCounterDataAMD(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGetPerfMonitorCounterDataAMD\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glGetPerfMonitorCounterInfoAMD(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGetPerfMonitorCounterInfoAMD\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glGetPerfMonitorCounterStringAMD(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGetPerfMonitorCounterStringAMD\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glGetPerfMonitorCountersAMD(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGetPerfMonitorCountersAMD\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glGetPerfMonitorGroupStringAMD(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGetPerfMonitorGroupStringAMD\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glGetPerfMonitorGroupsAMD(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGetPerfMonitorGroupsAMD\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glGetProgramBinaryOES(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGetProgramBinaryOES\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glGetRenderbufferParameteriv(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGetRenderbufferParameteriv\n";write(2,msg,sizeof(msg)-1);} return 0; }
void glGetShaderPrecisionFormat(u32 shader,u32 precision,i32 *range,i32 *bits) {
 u32 p[2]={shader,precision};emit_iv(108,(i32 *)p,2);i32 v[3]={0,0,0};recv_all(v,12);
 if(range){range[0]=v[0];range[1]=v[1];}if(bits)*bits=v[2];
}
int glGetShaderSource(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGetShaderSource\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glGetTexParameterfv(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGetTexParameterfv\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glGetTexParameteriv(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGetTexParameteriv\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glGetTexStreamDeviceAttributeivIMG(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGetTexStreamDeviceAttributeivIMG\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glGetTexStreamDeviceNameIMG(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGetTexStreamDeviceNameIMG\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glGetUniformfv(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGetUniformfv\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glGetUniformiv(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGetUniformiv\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glGetVertexAttribPointerv(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGetVertexAttribPointerv\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glGetVertexAttribfv(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGetVertexAttribfv\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glGetVertexAttribiv(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glGetVertexAttribiv\n";write(2,msg,sizeof(msg)-1);} return 0; }
void glHint(u32 target,u32 mode){u32 v[2]={target,mode};emit_iv(87,(i32 *)v,2);}
int glIsBuffer(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glIsBuffer\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glIsEnabled(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glIsEnabled\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glIsFenceNV(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glIsFenceNV\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glIsFramebuffer(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glIsFramebuffer\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glIsProgram(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glIsProgram\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glIsRenderbuffer(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glIsRenderbuffer\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glIsShader(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glIsShader\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glIsTexture(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glIsTexture\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glIsVertexArrayOES(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glIsVertexArrayOES\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glMapBufferOES(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glMapBufferOES\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glMultiDrawArraysEXT(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glMultiDrawArraysEXT\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glMultiDrawElementsEXT(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glMultiDrawElementsEXT\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glProgramBinaryOES(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glProgramBinaryOES\n";write(2,msg,sizeof(msg)-1);} return 0; }
void glReadPixels(i32 x,i32 y,i32 w,i32 h,u32 format,u32 type,void *pixels)
{
    /* RGBA8 is the native CWM handoff format used by this bridge. */
    if(w<0||h<0||w>2048||h>2048||format!=0x1908||type!=0x1401||!pixels)return;
    if(!w||!h)return;
    i32 args[6]={x,y,w,h,(i32)format,(i32)type};
    emit_iv(120,args,6);recv_all(pixels,(unsigned)w*h*4);
}
int glReleaseShaderCompiler(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glReleaseShaderCompiler\n";write(2,msg,sizeof(msg)-1);} return 0; }
void glRenderbufferStorage(u32 target,u32 format,i32 width,i32 height){u32 p[]={(u32)target,(u32)format,(u32)width,(u32)height};emit_iv(63,(i32 *)p,4);}
int glSampleCoverage(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glSampleCoverage\n";write(2,msg,sizeof(msg)-1);} return 0; }
void glScissor(i32 x,i32 y,i32 width,i32 height){u32 p[]={(u32)x,(u32)y,(u32)width,(u32)height};emit_iv(65,(i32 *)p,4);}
int glSelectPerfMonitorCountersAMD(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glSelectPerfMonitorCountersAMD\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glSetFenceNV(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glSetFenceNV\n";write(2,msg,sizeof(msg)-1);} return 0; }
void glShaderBinary(i32 count,const u32 *shaders,u32 format,const void *binary,i32 length)
{
    if(count < 0 || length < 0) return;
    u32 h[3]={(u32)count,format,(u32)length};
    rec(110,12+4*count+length);PART(h,12);PART(shaders,4*count);PART(binary,length);
}
int glStartTilingQCOM(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glStartTilingQCOM\n";write(2,msg,sizeof(msg)-1);} return 0; }
void glStencilFunc(u32 func,i32 ref,u32 mask){u32 p[]={(u32)func,(u32)ref,(u32)mask};emit_iv(69,(i32 *)p,3);}
void glStencilFuncSeparate(u32 face,u32 func,i32 ref,u32 mask){u32 p[]={(u32)face,(u32)func,(u32)ref,(u32)mask};emit_iv(76,(i32 *)p,4);}
void glStencilMask(u32 mask){u32 p[]={(u32)mask};emit_iv(68,(i32 *)p,1);}
void glStencilMaskSeparate(u32 face,u32 mask){u32 p[]={(u32)face,(u32)mask};emit_iv(75,(i32 *)p,2);}
void glStencilOp(u32 fail,u32 zfail,u32 pass){u32 p[]={(u32)fail,(u32)zfail,(u32)pass};emit_iv(70,(i32 *)p,3);}
void glStencilOpSeparate(u32 face,u32 fail,u32 zfail,u32 pass){u32 p[]={(u32)face,(u32)fail,(u32)zfail,(u32)pass};emit_iv(77,(i32 *)p,4);}
int glTestFenceNV(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glTestFenceNV\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glTexBindStreamIMG(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glTexBindStreamIMG\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glTexImage3DOES(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glTexImage3DOES\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glTexParameterfv(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glTexParameterfv\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glTexParameteriv(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glTexParameteriv\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glTexSubImage3DOES(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glTexSubImage3DOES\n";write(2,msg,sizeof(msg)-1);} return 0; }
void glUniform1iv(i32 loc,i32 count,const i32 *v){if(count<0)return;u32 h[2]={(u32)loc,(u32)count};rec(80,8+count*4);PART(h,8);PART(v,count*4);}
void glUniform2f(i32 loc,float x,float y){float v[]={x,y};uni_vec(23,loc,1,2,v);}
void glUniform2i(i32 loc,i32 x,i32 y){i32 v[]={x,y};glUniform2iv(loc,1,v);}
void glUniform2iv(i32 loc,i32 count,const i32 *v){if(count<0)return;u32 h[2]={(u32)loc,(u32)count};rec(81,8+count*8);PART(h,8);PART(v,count*8);}
void glUniform3f(i32 loc,float x,float y,float z){float v[]={x,y,z};uni_vec(24,loc,1,3,v);}
void glUniform3i(i32 loc,i32 x,i32 y,i32 z){i32 v[]={x,y,z};glUniform3iv(loc,1,v);}
void glUniform3iv(i32 loc,i32 count,const i32 *v){if(count<0)return;u32 h[2]={(u32)loc,(u32)count};rec(82,8+count*12);PART(h,8);PART(v,count*12);}
void glUniform4f(i32 loc,float x,float y,float z,float w){float v[]={x,y,z,w};uni_vec(25,loc,1,4,v);}
void glUniform4i(i32 loc,i32 x,i32 y,i32 z,i32 w){i32 v[]={x,y,z,w};glUniform4iv(loc,1,v);}
void glUniform4iv(i32 loc,i32 count,const i32 *v){if(count<0)return;u32 h[2]={(u32)loc,(u32)count};rec(83,8+count*16);PART(h,8);PART(v,count*16);}
int glUnmapBufferOES(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glUnmapBufferOES\n";write(2,msg,sizeof(msg)-1);} return 0; }
int glValidateProgram(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glValidateProgram\n";write(2,msg,sizeof(msg)-1);} return 0; }
void glVertexAttrib1f(u32 index,float x){float v[]={x,0.0,0.0,1.0};rec(84,20);PART(&index,4);PART(v,16);}
void glVertexAttrib1fv(u32 index,const float *v){glVertexAttrib1f(index,v[0]);}
void glVertexAttrib2f(u32 index,float x,float y){float v[]={x,y,0.0,1.0};rec(84,20);PART(&index,4);PART(v,16);}
void glVertexAttrib2fv(u32 index,const float *v){glVertexAttrib2f(index,v[0],v[1]);}
void glVertexAttrib3f(u32 index,float x,float y,float z){float v[]={x,y,z,1.0};rec(84,20);PART(&index,4);PART(v,16);}
void glVertexAttrib3fv(u32 index,const float *v){glVertexAttrib3f(index,v[0],v[1],v[2]);}
void glVertexAttrib4f(u32 index,float x,float y,float z,float w){float v[]={x,y,z,w};rec(84,20);PART(&index,4);PART(v,16);}
void glVertexAttrib4fv(u32 index,const float *v){glVertexAttrib4f(index,v[0],v[1],v[2],v[3]);}
int glVertexAttribPointerBounds(void) { static int seen; if(!seen){seen=1;const char msg[]="glbridge: unsupported glVertexAttribPointerBounds\n";write(2,msg,sizeof(msg)-1);} return 0; }

extern int strcmp(const char *,const char *);
void *eglGetProcAddress(const char *n)
{
    if (!strcmp(n,"eglCreateImageKHR")) return (void *)&eglCreateImageKHR;
    if (!strcmp(n,"eglDestroyImageKHR")) return (void *)&eglDestroyImageKHR;
    if (!strcmp(n,"eglBindAPI")) return (void *)&eglBindAPI;
    if (!strcmp(n,"eglChooseConfig")) return (void *)&eglChooseConfig;
    if (!strcmp(n,"eglCreateContext")) return (void *)&eglCreateContext;
    if (!strcmp(n,"eglCreatePbufferSurface")) return (void *)&eglCreatePbufferSurface;
    if (!strcmp(n,"eglCreateWindowSurface")) return (void *)&eglCreateWindowSurface;
    if (!strcmp(n,"eglDestroyContext")) return (void *)&eglDestroyContext;
    if (!strcmp(n,"eglDestroySurface")) return (void *)&eglDestroySurface;
    if (!strcmp(n,"eglGetConfigAttrib")) return (void *)&eglGetConfigAttrib;
    if (!strcmp(n,"eglGetConfigs")) return (void *)&eglGetConfigs;
    if (!strcmp(n,"eglGetCurrentContext")) return (void *)&eglGetCurrentContext;
    if (!strcmp(n,"eglGetCurrentDisplay")) return (void *)&eglGetCurrentDisplay;
    if (!strcmp(n,"eglGetCurrentSurface")) return (void *)&eglGetCurrentSurface;
    if (!strcmp(n,"eglGetDisplay")) return (void *)&eglGetDisplay;
    if (!strcmp(n,"eglGetError")) return (void *)&eglGetError;
    if (!strcmp(n,"eglGetPlatformDisplay")) return (void *)&eglGetPlatformDisplay;
    if (!strcmp(n,"eglInitialize")) return (void *)&eglInitialize;
    if (!strcmp(n,"eglMakeCurrent")) return (void *)&eglMakeCurrent;
    if (!strcmp(n,"eglQueryString")) return (void *)&eglQueryString;
    if (!strcmp(n,"eglQuerySurface")) return (void *)&eglQuerySurface;
    if (!strcmp(n,"eglReleaseThread")) return (void *)&eglReleaseThread;
    if (!strcmp(n,"eglSwapBuffers")) return (void *)&eglSwapBuffers;
    if (!strcmp(n,"eglSwapInterval")) return (void *)&eglSwapInterval;
    if (!strcmp(n,"eglTerminate")) return (void *)&eglTerminate;
    if (!strcmp(n,"eglWaitGL")) return (void *)&eglWaitGL;
    if (!strcmp(n,"eglWaitNative")) return (void *)&eglWaitNative;
    if (!strcmp(n,"glActiveTexture")) return (void *)&glActiveTexture;
    if (!strcmp(n,"glAttachShader")) return (void *)&glAttachShader;
    if (!strcmp(n,"glBeginPerfMonitorAMD")) return (void *)&glBeginPerfMonitorAMD;
    if (!strcmp(n,"glBindAttribLocation")) return (void *)&glBindAttribLocation;
    if (!strcmp(n,"glBindBuffer")) return (void *)&glBindBuffer;
    if (!strcmp(n,"glBindFramebuffer")) return (void *)&glBindFramebuffer;
    if (!strcmp(n,"glBindRenderbuffer")) return (void *)&glBindRenderbuffer;
    if (!strcmp(n,"glBindTexture")) return (void *)&glBindTexture;
    if (!strcmp(n,"glBindVertexArrayOES")) return (void *)&glBindVertexArrayOES;
    if (!strcmp(n,"glBlendColor")) return (void *)&glBlendColor;
    if (!strcmp(n,"glBlendEquation")) return (void *)&glBlendEquation;
    if (!strcmp(n,"glBlendEquationSeparate")) return (void *)&glBlendEquationSeparate;
    if (!strcmp(n,"glBlendFunc")) return (void *)&glBlendFunc;
    if (!strcmp(n,"glBlendFuncSeparate")) return (void *)&glBlendFuncSeparate;
    if (!strcmp(n,"glBufferData")) return (void *)&glBufferData;
    if (!strcmp(n,"glBufferSubData")) return (void *)&glBufferSubData;
    if (!strcmp(n,"glCheckFramebufferStatus")) return (void *)&glCheckFramebufferStatus;
    if (!strcmp(n,"glClear")) return (void *)&glClear;
    if (!strcmp(n,"glClearColor")) return (void *)&glClearColor;
    if (!strcmp(n,"glClearDepth")) return (void *)&glClearDepth;
    if (!strcmp(n,"glClearDepthf")) return (void *)&glClearDepthf;
    if (!strcmp(n,"glClearStencil")) return (void *)&glClearStencil;
    if (!strcmp(n,"glColorMask")) return (void *)&glColorMask;
    if (!strcmp(n,"glCompileShader")) return (void *)&glCompileShader;
    if (!strcmp(n,"glCompressedTexImage2D")) return (void *)&glCompressedTexImage2D;
    if (!strcmp(n,"glCompressedTexImage3DOES")) return (void *)&glCompressedTexImage3DOES;
    if (!strcmp(n,"glCompressedTexSubImage2D")) return (void *)&glCompressedTexSubImage2D;
    if (!strcmp(n,"glCompressedTexSubImage3DOES")) return (void *)&glCompressedTexSubImage3DOES;
    if (!strcmp(n,"glCopyTexImage2D")) return (void *)&glCopyTexImage2D;
    if (!strcmp(n,"glCopyTexSubImage2D")) return (void *)&glCopyTexSubImage2D;
    if (!strcmp(n,"glCopyTexSubImage3DOES")) return (void *)&glCopyTexSubImage3DOES;
    if (!strcmp(n,"glCreateProgram")) return (void *)&glCreateProgram;
    if (!strcmp(n,"glCreateShader")) return (void *)&glCreateShader;
    if (!strcmp(n,"glCullFace")) return (void *)&glCullFace;
    if (!strcmp(n,"glDeleteBuffers")) return (void *)&glDeleteBuffers;
    if (!strcmp(n,"glDeleteFencesNV")) return (void *)&glDeleteFencesNV;
    if (!strcmp(n,"glDeleteFramebuffers")) return (void *)&glDeleteFramebuffers;
    if (!strcmp(n,"glDeletePerfMonitorsAMD")) return (void *)&glDeletePerfMonitorsAMD;
    if (!strcmp(n,"glDeleteProgram")) return (void *)&glDeleteProgram;
    if (!strcmp(n,"glDeleteRenderbuffers")) return (void *)&glDeleteRenderbuffers;
    if (!strcmp(n,"glDeleteShader")) return (void *)&glDeleteShader;
    if (!strcmp(n,"glDeleteTextures")) return (void *)&glDeleteTextures;
    if (!strcmp(n,"glDeleteVertexArraysOES")) return (void *)&glDeleteVertexArraysOES;
    if (!strcmp(n,"glDepthFunc")) return (void *)&glDepthFunc;
    if (!strcmp(n,"glDepthMask")) return (void *)&glDepthMask;
    if (!strcmp(n,"glDepthRangef")) return (void *)&glDepthRangef;
    if (!strcmp(n,"glDetachShader")) return (void *)&glDetachShader;
    if (!strcmp(n,"glDisable")) return (void *)&glDisable;
    if (!strcmp(n,"glDisableDriverControlQCOM")) return (void *)&glDisableDriverControlQCOM;
    if (!strcmp(n,"glDisableVertexAttribArray")) return (void *)&glDisableVertexAttribArray;
    if (!strcmp(n,"glDiscardFramebufferEXT")) return (void *)&glDiscardFramebufferEXT;
    if (!strcmp(n,"glDrawArrays")) return (void *)&glDrawArrays;
    if (!strcmp(n,"glDrawElements")) return (void *)&glDrawElements;
    if (!strcmp(n,"glEGLImageTargetRenderbufferStorageOES")) return (void *)&glEGLImageTargetRenderbufferStorageOES;
    if (!strcmp(n,"glEGLImageTargetTexture2DOES")) return (void *)&glEGLImageTargetTexture2DOES;
    if (!strcmp(n,"glEnable")) return (void *)&glEnable;
    if (!strcmp(n,"glEnableDriverControlQCOM")) return (void *)&glEnableDriverControlQCOM;
    if (!strcmp(n,"glEnableVertexAttribArray")) return (void *)&glEnableVertexAttribArray;
    if (!strcmp(n,"glEndPerfMonitorAMD")) return (void *)&glEndPerfMonitorAMD;
    if (!strcmp(n,"glEndTilingQCOM")) return (void *)&glEndTilingQCOM;
    if (!strcmp(n,"glExtGetBufferPointervQCOM")) return (void *)&glExtGetBufferPointervQCOM;
    if (!strcmp(n,"glExtGetBuffersQCOM")) return (void *)&glExtGetBuffersQCOM;
    if (!strcmp(n,"glExtGetFramebuffersQCOM")) return (void *)&glExtGetFramebuffersQCOM;
    if (!strcmp(n,"glExtGetProgramBinarySourceQCOM")) return (void *)&glExtGetProgramBinarySourceQCOM;
    if (!strcmp(n,"glExtGetProgramsQCOM")) return (void *)&glExtGetProgramsQCOM;
    if (!strcmp(n,"glExtGetRenderbuffersQCOM")) return (void *)&glExtGetRenderbuffersQCOM;
    if (!strcmp(n,"glExtGetShadersQCOM")) return (void *)&glExtGetShadersQCOM;
    if (!strcmp(n,"glExtGetTexLevelParameterivQCOM")) return (void *)&glExtGetTexLevelParameterivQCOM;
    if (!strcmp(n,"glExtGetTexSubImageQCOM")) return (void *)&glExtGetTexSubImageQCOM;
    if (!strcmp(n,"glExtGetTexturesQCOM")) return (void *)&glExtGetTexturesQCOM;
    if (!strcmp(n,"glExtIsProgramBinaryQCOM")) return (void *)&glExtIsProgramBinaryQCOM;
    if (!strcmp(n,"glExtTexObjectStateOverrideiQCOM")) return (void *)&glExtTexObjectStateOverrideiQCOM;
    if (!strcmp(n,"glFinish")) return (void *)&glFinish;
    if (!strcmp(n,"glFinishFenceNV")) return (void *)&glFinishFenceNV;
    if (!strcmp(n,"glFlush")) return (void *)&glFlush;
    if (!strcmp(n,"glFramebufferRenderbuffer")) return (void *)&glFramebufferRenderbuffer;
    if (!strcmp(n,"glFramebufferTexture2D")) return (void *)&glFramebufferTexture2D;
    if (!strcmp(n,"glFramebufferTexture3DOES")) return (void *)&glFramebufferTexture3DOES;
    if (!strcmp(n,"glFrontFace")) return (void *)&glFrontFace;
    if (!strcmp(n,"glGenBuffers")) return (void *)&glGenBuffers;
    if (!strcmp(n,"glGenFencesNV")) return (void *)&glGenFencesNV;
    if (!strcmp(n,"glGenFramebuffers")) return (void *)&glGenFramebuffers;
    if (!strcmp(n,"glGenPerfMonitorsAMD")) return (void *)&glGenPerfMonitorsAMD;
    if (!strcmp(n,"glGenRenderbuffers")) return (void *)&glGenRenderbuffers;
    if (!strcmp(n,"glGenTextures")) return (void *)&glGenTextures;
    if (!strcmp(n,"glGenVertexArraysOES")) return (void *)&glGenVertexArraysOES;
    if (!strcmp(n,"glGenerateMipmap")) return (void *)&glGenerateMipmap;
    if (!strcmp(n,"glGetActiveAttrib")) return (void *)&glGetActiveAttrib;
    if (!strcmp(n,"glGetActiveUniform")) return (void *)&glGetActiveUniform;
    if (!strcmp(n,"glGetAttachedShaders")) return (void *)&glGetAttachedShaders;
    if (!strcmp(n,"glGetAttribLocation")) return (void *)&glGetAttribLocation;
    if (!strcmp(n,"glGetBooleanv")) return (void *)&glGetBooleanv;
    if (!strcmp(n,"glGetBufferParameteriv")) return (void *)&glGetBufferParameteriv;
    if (!strcmp(n,"glGetBufferPointervOES")) return (void *)&glGetBufferPointervOES;
    if (!strcmp(n,"glGetDriverControlStringQCOM")) return (void *)&glGetDriverControlStringQCOM;
    if (!strcmp(n,"glGetDriverControlsQCOM")) return (void *)&glGetDriverControlsQCOM;
    if (!strcmp(n,"glGetError")) return (void *)&glGetError;
    if (!strcmp(n,"glGetFenceivNV")) return (void *)&glGetFenceivNV;
    if (!strcmp(n,"glGetFloatv")) return (void *)&glGetFloatv;
    if (!strcmp(n,"glGetFramebufferAttachmentParameteriv")) return (void *)&glGetFramebufferAttachmentParameteriv;
    if (!strcmp(n,"glGetIntegerv")) return (void *)&glGetIntegerv;
    if (!strcmp(n,"glGetPerfMonitorCounterDataAMD")) return (void *)&glGetPerfMonitorCounterDataAMD;
    if (!strcmp(n,"glGetPerfMonitorCounterInfoAMD")) return (void *)&glGetPerfMonitorCounterInfoAMD;
    if (!strcmp(n,"glGetPerfMonitorCounterStringAMD")) return (void *)&glGetPerfMonitorCounterStringAMD;
    if (!strcmp(n,"glGetPerfMonitorCountersAMD")) return (void *)&glGetPerfMonitorCountersAMD;
    if (!strcmp(n,"glGetPerfMonitorGroupStringAMD")) return (void *)&glGetPerfMonitorGroupStringAMD;
    if (!strcmp(n,"glGetPerfMonitorGroupsAMD")) return (void *)&glGetPerfMonitorGroupsAMD;
    if (!strcmp(n,"glGetProgramBinaryOES")) return (void *)&glGetProgramBinaryOES;
    if (!strcmp(n,"glGetProgramInfoLog")) return (void *)&glGetProgramInfoLog;
    if (!strcmp(n,"glGetProgramiv")) return (void *)&glGetProgramiv;
    if (!strcmp(n,"glGetRenderbufferParameteriv")) return (void *)&glGetRenderbufferParameteriv;
    if (!strcmp(n,"glGetShaderInfoLog")) return (void *)&glGetShaderInfoLog;
    if (!strcmp(n,"glGetShaderPrecisionFormat")) return (void *)&glGetShaderPrecisionFormat;
    if (!strcmp(n,"glGetShaderSource")) return (void *)&glGetShaderSource;
    if (!strcmp(n,"glGetShaderiv")) return (void *)&glGetShaderiv;
    if (!strcmp(n,"glGetString")) return (void *)&glGetString;
    if (!strcmp(n,"glGetTexParameterfv")) return (void *)&glGetTexParameterfv;
    if (!strcmp(n,"glGetTexParameteriv")) return (void *)&glGetTexParameteriv;
    if (!strcmp(n,"glGetTexStreamDeviceAttributeivIMG")) return (void *)&glGetTexStreamDeviceAttributeivIMG;
    if (!strcmp(n,"glGetTexStreamDeviceNameIMG")) return (void *)&glGetTexStreamDeviceNameIMG;
    if (!strcmp(n,"glGetUniformLocation")) return (void *)&glGetUniformLocation;
    if (!strcmp(n,"glGetUniformfv")) return (void *)&glGetUniformfv;
    if (!strcmp(n,"glGetUniformiv")) return (void *)&glGetUniformiv;
    if (!strcmp(n,"glGetVertexAttribPointerv")) return (void *)&glGetVertexAttribPointerv;
    if (!strcmp(n,"glGetVertexAttribfv")) return (void *)&glGetVertexAttribfv;
    if (!strcmp(n,"glGetVertexAttribiv")) return (void *)&glGetVertexAttribiv;
    if (!strcmp(n,"glHint")) return (void *)&glHint;
    if (!strcmp(n,"glIsBuffer")) return (void *)&glIsBuffer;
    if (!strcmp(n,"glIsEnabled")) return (void *)&glIsEnabled;
    if (!strcmp(n,"glIsFenceNV")) return (void *)&glIsFenceNV;
    if (!strcmp(n,"glIsFramebuffer")) return (void *)&glIsFramebuffer;
    if (!strcmp(n,"glIsProgram")) return (void *)&glIsProgram;
    if (!strcmp(n,"glIsRenderbuffer")) return (void *)&glIsRenderbuffer;
    if (!strcmp(n,"glIsShader")) return (void *)&glIsShader;
    if (!strcmp(n,"glIsTexture")) return (void *)&glIsTexture;
    if (!strcmp(n,"glIsVertexArrayOES")) return (void *)&glIsVertexArrayOES;
    if (!strcmp(n,"glLineWidth")) return (void *)&glLineWidth;
    if (!strcmp(n,"glLinkProgram")) return (void *)&glLinkProgram;
    if (!strcmp(n,"glMapBufferOES")) return (void *)&glMapBufferOES;
    if (!strcmp(n,"glMultiDrawArraysEXT")) return (void *)&glMultiDrawArraysEXT;
    if (!strcmp(n,"glMultiDrawElementsEXT")) return (void *)&glMultiDrawElementsEXT;
    if (!strcmp(n,"glPixelStorei")) return (void *)&glPixelStorei;
    if (!strcmp(n,"glPolygonOffset")) return (void *)&glPolygonOffset;
    if (!strcmp(n,"glProgramBinaryOES")) return (void *)&glProgramBinaryOES;
    if (!strcmp(n,"glReadPixels")) return (void *)&glReadPixels;
    if (!strcmp(n,"glReleaseShaderCompiler")) return (void *)&glReleaseShaderCompiler;
    if (!strcmp(n,"glRenderbufferStorage")) return (void *)&glRenderbufferStorage;
    if (!strcmp(n,"glSampleCoverage")) return (void *)&glSampleCoverage;
    if (!strcmp(n,"glScissor")) return (void *)&glScissor;
    if (!strcmp(n,"glSelectPerfMonitorCountersAMD")) return (void *)&glSelectPerfMonitorCountersAMD;
    if (!strcmp(n,"glSetFenceNV")) return (void *)&glSetFenceNV;
    if (!strcmp(n,"glShaderBinary")) return (void *)&glShaderBinary;
    if (!strcmp(n,"glShaderSource")) return (void *)&glShaderSource;
    if (!strcmp(n,"glStartTilingQCOM")) return (void *)&glStartTilingQCOM;
    if (!strcmp(n,"glStencilFunc")) return (void *)&glStencilFunc;
    if (!strcmp(n,"glStencilFuncSeparate")) return (void *)&glStencilFuncSeparate;
    if (!strcmp(n,"glStencilMask")) return (void *)&glStencilMask;
    if (!strcmp(n,"glStencilMaskSeparate")) return (void *)&glStencilMaskSeparate;
    if (!strcmp(n,"glStencilOp")) return (void *)&glStencilOp;
    if (!strcmp(n,"glStencilOpSeparate")) return (void *)&glStencilOpSeparate;
    if (!strcmp(n,"glTestFenceNV")) return (void *)&glTestFenceNV;
    if (!strcmp(n,"glTexBindStreamIMG")) return (void *)&glTexBindStreamIMG;
    if (!strcmp(n,"glTexImage2D")) return (void *)&glTexImage2D;
    if (!strcmp(n,"glTexImage3DOES")) return (void *)&glTexImage3DOES;
    if (!strcmp(n,"glTexParameterf")) return (void *)&glTexParameterf;
    if (!strcmp(n,"glTexParameterfv")) return (void *)&glTexParameterfv;
    if (!strcmp(n,"glTexParameteri")) return (void *)&glTexParameteri;
    if (!strcmp(n,"glTexParameteriv")) return (void *)&glTexParameteriv;
    if (!strcmp(n,"glTexSubImage2D")) return (void *)&glTexSubImage2D;
    if (!strcmp(n,"glTexSubImage3DOES")) return (void *)&glTexSubImage3DOES;
    if (!strcmp(n,"glUniform1f")) return (void *)&glUniform1f;
    if (!strcmp(n,"glUniform1fv")) return (void *)&glUniform1fv;
    if (!strcmp(n,"glUniform1i")) return (void *)&glUniform1i;
    if (!strcmp(n,"glUniform1iv")) return (void *)&glUniform1iv;
    if (!strcmp(n,"glUniform2f")) return (void *)&glUniform2f;
    if (!strcmp(n,"glUniform2fv")) return (void *)&glUniform2fv;
    if (!strcmp(n,"glUniform2i")) return (void *)&glUniform2i;
    if (!strcmp(n,"glUniform2iv")) return (void *)&glUniform2iv;
    if (!strcmp(n,"glUniform3f")) return (void *)&glUniform3f;
    if (!strcmp(n,"glUniform3fv")) return (void *)&glUniform3fv;
    if (!strcmp(n,"glUniform3i")) return (void *)&glUniform3i;
    if (!strcmp(n,"glUniform3iv")) return (void *)&glUniform3iv;
    if (!strcmp(n,"glUniform4f")) return (void *)&glUniform4f;
    if (!strcmp(n,"glUniform4fv")) return (void *)&glUniform4fv;
    if (!strcmp(n,"glUniform4i")) return (void *)&glUniform4i;
    if (!strcmp(n,"glUniform4iv")) return (void *)&glUniform4iv;
    if (!strcmp(n,"glUniformMatrix2fv")) return (void *)&glUniformMatrix2fv;
    if (!strcmp(n,"glUniformMatrix3fv")) return (void *)&glUniformMatrix3fv;
    if (!strcmp(n,"glUniformMatrix4fv")) return (void *)&glUniformMatrix4fv;
    if (!strcmp(n,"glUnmapBufferOES")) return (void *)&glUnmapBufferOES;
    if (!strcmp(n,"glUseProgram")) return (void *)&glUseProgram;
    if (!strcmp(n,"glValidateProgram")) return (void *)&glValidateProgram;
    if (!strcmp(n,"glVertexAttrib1f")) return (void *)&glVertexAttrib1f;
    if (!strcmp(n,"glVertexAttrib1fv")) return (void *)&glVertexAttrib1fv;
    if (!strcmp(n,"glVertexAttrib2f")) return (void *)&glVertexAttrib2f;
    if (!strcmp(n,"glVertexAttrib2fv")) return (void *)&glVertexAttrib2fv;
    if (!strcmp(n,"glVertexAttrib3f")) return (void *)&glVertexAttrib3f;
    if (!strcmp(n,"glVertexAttrib3fv")) return (void *)&glVertexAttrib3fv;
    if (!strcmp(n,"glVertexAttrib4f")) return (void *)&glVertexAttrib4f;
    if (!strcmp(n,"glVertexAttrib4fv")) return (void *)&glVertexAttrib4fv;
    if (!strcmp(n,"glVertexAttribPointer")) return (void *)&glVertexAttribPointer;
    if (!strcmp(n,"glVertexAttribPointerBounds")) return (void *)&glVertexAttribPointerBounds;
    if (!strcmp(n,"glViewport")) return (void *)&glViewport;
    return 0;
}
