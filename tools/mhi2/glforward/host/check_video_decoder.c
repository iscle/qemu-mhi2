/* Decode a real Annex-B/AVCC fixture on either host and check pixels,
 * guest crop/visibility, owner isolation, malformed input and teardown. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
static uint32_t u32(const uint8_t *p,int off){uint32_t n;memcpy(&n,p+off,4);return n;}
static unsigned changes;
static void video_display_changed(void){changes++;}
#include "video_decoder.h"
int main(int argc,char **argv)
{
    assert(argc==2||argc==3);bool avcc=argc==3&&!strcmp(argv[2],"avcc");
    bool unavailable=argc==3&&!strcmp(argv[2],"unavailable");FILE *f=fopen(argv[1],"rb");assert(f);
    fseek(f,0,SEEK_END);size_t n=ftell(f);rewind(f);
    uint8_t *data=malloc(n+8);assert(fread(data+8,1,n,f)==n);fclose(f);
    uint32_t args[3]={1,800,480};assert(!video_open(20,(uint8_t *)args));
    /* AUD separates complete access units; include SPS/PPS with first frame. */
    size_t start=0;
    for(size_t i=1;i<=n;i++){
        unsigned sc=i<n?video_start_code(data+8+i,n-i):0;
        if(i==n||(sc&&(data[8+i+sc]&31)==9)){
            if(i<n&&i>0&&!data[8+i-1])continue; /* don't split twice inside a 4-byte prefix */
            size_t bytes=i-start;uint8_t *packet=malloc(bytes+8);
            ((uint32_t *)packet)[0]=1;((uint32_t *)packet)[1]=bytes;memcpy(packet+8,data+8+start,bytes);
            if(avcc){
                uint8_t *converted=calloc(1,bytes+bytes/3+32);
                size_t pos=0,out=8;
                while(pos<bytes){
                    unsigned prefix=video_start_code(packet+8+pos,bytes-pos);assert(prefix);
                    size_t start=pos+prefix,end=start;
                    while(end<bytes&&!video_start_code(packet+8+end,bytes-end))end++;
                    pos=end;while(end>start&&!packet[8+end-1])end--;
                    size_t len=end-start;if(!len)continue;
                    converted[out++]=len>>24;converted[out++]=len>>16;converted[out++]=len>>8;converted[out++]=len;
                    memcpy(converted+out,packet+8+start,len);out+=len;
                }
                ((uint32_t *)converted)[0]=1;((uint32_t *)converted)[1]=out-8;
                free(packet);packet=converted;
            }
            assert(video_decode(21,packet)==VIDEO_ERROR);
            if(unavailable){
                assert(video_decode(20,packet)==VIDEO_ERROR);
                assert(!video_find(20,1)->frames);video_close(20,1);
                free(packet);free(data);
                puts("PASS: forced unavailable hardware produces no frames and reports failure");
                return 0;
            }
            assert(!video_decode(20,packet));free(packet);start=i;
        }
    }
    struct VideoDecoder *v=video_find(20,1);assert(v&&v->frames==5&&v->rgba);
    if(getenv("MHI2_DECODER")&&!strcmp(getenv("MHI2_DECODER"),"hardware"))assert(v->hardware);
    if(getenv("MHI2_DECODER")&&!strcmp(getenv("MHI2_DECODER"),"software"))assert(!v->hardware);
    unsigned char *pixel=v->rgba+400*4;fprintf(stderr,"pixel=%u,%u,%u,%u frames=%u\n",pixel[0],pixel[1],pixel[2],pixel[3],v->frames);assert(pixel[0]>240&&pixel[1]<12&&pixel[2]<12);
    uint8_t attrs[68]={0};*(uint32_t *)attrs=1;
    uint32_t *a=(uint32_t *)(attrs+4);a[5]=800;a[6]=480;a[7]=10;a[8]=20;a[9]=100;a[10]=80;
    assert(!video_attributes(20,attrs));
    uint8_t *frame=calloc(800*480,4);video_composite(frame,800,480);
    assert(frame[((480-1-20)*800+10)*4]>240);assert(frame[0]==0);
    /* A live cluster decoder must never composite over the center screen. */
    v->target=1;memset(frame,0,800*480*4);video_composite(frame,800,480);
    assert(frame[((480-1-20)*800+10)*4]==0);v->target=0;
    attrs[4+61]=1;assert(!video_attributes(20,attrs));memset(frame,0,800*480*4);
    video_composite(frame,800,480);assert(frame[((480-1-20)*800+10)*4]==0);
    a[5]=801;assert(video_attributes(20,attrs)==VIDEO_ERROR);
    uint32_t bad[]={1,4,0xffffffff};assert(video_decode(20,(uint8_t *)bad)==VIDEO_ERROR);
    video_close(21,1);assert(video_find(20,1));video_close(20,1);assert(!video_find(20,1));
    /* Bounded dimensions and owner-specific slots, including full-table failure. */
    args[1]=2049;assert(video_open(20,(uint8_t *)args)==VIDEO_ERROR);args[1]=800;
    for(unsigned owner=1;owner<=4;owner++)assert(!video_open(owner,(uint8_t *)args));
    assert(video_open(5,(uint8_t *)args)==VIDEO_ERROR);
    video_close(2,1);assert(!video_open(5,(uint8_t *)args));
    for(unsigned owner=1;owner<=5;owner++)video_close(owner,1);
    free(data);free(frame);assert(changes>0);
    puts("PASS: H.264 frames, red pixels, crop, visibility, owner isolation, bounds and cleanup");
}
