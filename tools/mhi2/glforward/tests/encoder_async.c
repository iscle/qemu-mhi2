#define _GNU_SOURCE
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
static uint32_t u32(const uint8_t *p,unsigned o){uint32_t v;memcpy(&v,p+o,4);return v;}
#include "../host/encoder.h"
int main(int argc,char **argv)
{
    assert(argc==3);
#ifndef _WIN32
    signal(SIGPIPE,SIG_IGN);
#endif
    unsigned width=800,height=480;size_t size=24+width*height*4;
    uint8_t *p=calloc(1,size);assert(p);
    uint32_t args[6]={1,width,height,5,2000000,1};memcpy(p,args,24);
    /* Changing frames exercise encoding and MPEG-TS sync-byte/alignment checks. */
    FILE *out=fopen(argv[2],"wb");assert(out);double worst=0;unsigned bytes=0,full=0;
    bool stalled=!strcmp(argv[1],"stalled");
    for(unsigned frame=0;frame<(stalled?20:30);frame++){
        for(unsigned i=24;i<size;i+=4){p[i]=(i/4+frame*5)%256;p[i+1]=frame*8;p[i+2]=(i/(width*4))%256;p[i+3]=255;}
        double begin=encoder_now();unsigned status=encoder_submit(123,p);
        double elapsed=encoder_now()-begin;if(elapsed>worst)worst=elapsed;
        assert(status==0||status==2);full+=status==2;
        double until=encoder_now()+(stalled?.001:.04);
        do {
            uint8_t *data;unsigned count=encoder_poll(123,1,&data);assert(count!=UINT32_MAX);
            if(count){assert(count%188==0);for(unsigned i=0;i<count;i+=188)assert(data[i]==0x47);
                assert(fwrite(data,1,count,out)==count);bytes+=count;free(data);}
            usleep(1000);
        }while(encoder_now()<until);
    }
    if(!stalled){
        double until=encoder_now()+2;
        while(encoder_now()<until){uint8_t *data;unsigned count=encoder_poll(123,1,&data);assert(count!=UINT32_MAX);
            if(count){assert(count%188==0);for(unsigned i=0;i<count;i+=188)assert(data[i]==0x47);
                assert(fwrite(data,1,count,out)==count);bytes+=count;free(data);}
            usleep(1000);}
        assert(bytes>188*10);
    }else assert(full>0);
    double close_begin=encoder_now();encoder_close(123,1);double close_seconds=encoder_now()-close_begin;
    assert(close_seconds<.05);
    double stop_begin=encoder_now();int stop_failed=0;
    for(unsigned i=0;i<4;i++)stop_failed|=encoder_stop(&encoders[i]);
    double stop_seconds=encoder_now()-stop_begin;
    assert(!stop_failed);assert(stop_seconds<3.0);
    assert(fclose(out)==0);free(p);
    printf("%s: max submit %.3f ms, full=%u, TS=%u bytes, close-request %.3f ms, full-stop %.3f ms\n",
           argv[1],worst*1000,full,bytes,close_seconds*1000,stop_seconds*1000);
    assert(worst<.05);return 0;
}
