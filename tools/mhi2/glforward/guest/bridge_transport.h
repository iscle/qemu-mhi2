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
struct ClientAttrib { u32 size, type, norm, stride, buffer, enabled; const void *pointer; };
struct Record { unsigned char *data; unsigned size, used, capacity, reply;
    u32 array_buffer, element_buffer; struct ClientAttrib attribs[16]; };
static void release_record(void *p)
{ struct Record *r=p; if(r){free(r->data);free(r);} }
static struct Record *current_record(void)
{
    if (__atomic_load_n(&record_key_state,__ATOMIC_ACQUIRE) != 2) {
        if (__sync_bool_compare_and_swap(&record_key_state,0,1)) {
            if(pthread_key_create(&record_key,release_record))abort();
            __atomic_store_n(&record_key_state,2,__ATOMIC_RELEASE);
        } else while(__atomic_load_n(&record_key_state,__ATOMIC_ACQUIRE)!=2)usleep(1000);
    }
    struct Record *r=pthread_getspecific(record_key);
    if(!r){r=malloc(sizeof(*r));if(!r)abort();
        for(unsigned i=0;i<sizeof(*r);i++)((unsigned char *)r)[i]=0;
        if(pthread_setspecific(record_key,r))abort();}
    return r;
}
static void unlock_record(void)
{ __asm__ volatile("dmb sy" ::: "memory"); *(volatile u32 *)(bridge+16)=0; }
static volatile u32 connection_state;
static int gl_conn(void)
{
    /* Publish the MMIO and bulk mappings together. Other rendering threads
     * must not observe bridge before bulk_size and g_fd are initialized. */
    for (;;) {
        if (__atomic_load_n(&connection_state,__ATOMIC_ACQUIRE)==2) return g_fd;
        if (__sync_bool_compare_and_swap(&connection_state,0,1)) break;
        usleep(1000);
    }
    bridge = mmap_device_memory(0, 12288, 0xb00, 0, 0x5f000000ULL);
    if (bridge == (void *)-1 || *(volatile u32 *)bridge != 0x474c4252) {
        bridge=0;__atomic_store_n(&connection_state,0,__ATOMIC_RELEASE);return -1;
    }
    if (*(volatile u32 *)(bridge+24) == 1024*1024) {
        bulk = mmap_device_memory(0, 2*1024*1024, 0xb00, 0, 0x5e000000ULL);
        if (bulk == (void *)-1) bulk=0;
        if (bulk) bulk_size=1024*1024;
    }
    g_fd=1;__atomic_store_n(&connection_state,2,__ATOMIC_RELEASE);
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
    r->size=size;r->used=20;r->reply=(op>=100&&op<=109)||op==120||op==121||op==123||op==124||op==126||op==127||op==131||op==132||op==138||(op>=134&&op<=137);
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
