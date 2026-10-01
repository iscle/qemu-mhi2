/* Exercise guest frame pacing and bounded RM reads without a QNX runtime. */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
typedef unsigned u32;
typedef int i32;
struct NvSurface {u32 width,height,format,layout,pitch,mem,offset,kind;};
struct NativeImage {struct NvSurface surface;};
static unsigned reads,largest,bytes,clock_ms;
static void read_pixels(u32 mem,u32 offset,void *out,u32 count)
{(void)mem;(void)offset;reads++;bytes+=count;if(count>largest)largest=count;memset(out,0,count);}
static struct {void (*read)(u32,u32,void *,u32);} native={read_pixels};
static int native_init(void){return 1;}
static void rec(u32 op,u32 n){(void)op;(void)n;}
#define PART(p,n) ((void)(p),(void)(n))
static int recv_bytes(void *p,unsigned long n,int release){(void)release;memset(p,0,n);return 0;}
static int recv_all(void *p,unsigned long n){return recv_bytes(p,n,1);}
static void unlock_record(void){}
static void emit_iv(u32 op,i32 *v,unsigned n){(void)op;(void)v;(void)n;}
#define clock_gettime encoder_test_clock
/* This test exercises frame submission, not the 32-bit callback ABI. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
#pragma GCC diagnostic ignored "-Wpointer-to-int-cast"
#include "../guest/native_encoder.h"
#pragma GCC diagnostic pop
int encoder_test_clock(int clock,struct EncoderTime *out)
{assert(clock==2);out->sec=clock_ms/1000;out->nsec=(clock_ms%1000)*1000000;return 0;}
int main(void)
{
    struct NativeEncoder e={.id=1,.width=800,.height=480,.fps=5,.rate=2000000,.started=1};
    struct NativeImage image={.surface={.width=800,.height=480,.pitch=3200}};
    for(clock_ms=0;clock_ms<1000;clock_ms++)assert(mhi2EncoderFrame(&e,&image,0)==0);
    assert(reads==5*47);assert(bytes==5*800*480*4);assert(largest<=32768);
    unsigned before=bytes;
    e.last_frame_ms=0xfffffff0u;e.timed=1;clock_ms=50;
    assert(mhi2EncoderFrame(&e,&image,0)==0 && bytes==before);
    clock_ms=200;assert(mhi2EncoderFrame(&e,&image,0)==0 && bytes==before+800*480*4);
    unsigned char settings[8]={10,0,0,0};u32 rate=8333;memcpy(settings+4,&rate,4);
    assert(mhi2EncoderSet(&e,settings)==0);clock_ms=299;before=reads;
    assert(mhi2EncoderFrame(&e,&image,0)==0 && reads==before);
    clock_ms=300;image.surface.pitch=3216;
    assert(mhi2EncoderFrame(&e,&image,0)==0 && reads==before+480);
    puts("PASS: configured cadence, timer wrap, rate changes and bounded image reads");
}
