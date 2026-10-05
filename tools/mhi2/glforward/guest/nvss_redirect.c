/* Emulator-only acceleration of the six K5126 NvSS entry points.
 * Linked only into the QEMU EGL adapter, never into a Pico/HU payload.
 * The original NvSS file remains byte-identical: its private process mapping
 * is redirected after the loader resolves dependencies and before workers run.
 * This is API emulation, not validation of Tegra decode/display hardware. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/mman.h>

#define ENTRY(name, a, b) {#name, {a,b}, qemu_##name, NULL}
extern void qemu_NvSSVideoOpen(void), qemu_NvSSVideoGetAttribs(void);
extern void qemu_NvSSVideoSetAttribs(void), qemu_NvSSVideoDecode(void);
extern void qemu_NvSSVideoStreamConfigure(void), qemu_NvSSVideoClose(void);

void qemu_nvss_redirect_init(void)
{
    struct entry {const char *name; uint32_t expected[2]; void (*target)(void); uint32_t *code;};
    struct entry entries[]={
        ENTRY(NvSSVideoOpen,0xe92d47f0,0xe1a0a000),
        ENTRY(NvSSVideoGetAttribs,0xe52de004,0xe1a03000),
        ENTRY(NvSSVideoSetAttribs,0xe92d40f0,0xe3500000),
        ENTRY(NvSSVideoDecode,0xe92d41f0,0xe3500000),
        ENTRY(NvSSVideoStreamConfigure,0xe92d41f0,0xe3500000),
        ENTRY(NvSSVideoClose,0xe92d4030,0xe2505000),
    };
    /* Resolve the library itself: GAL's undefined NvSS symbols have nonzero
     * PLT addresses, which QNX returns through RTLD_DEFAULT. Those are not
     * the original library entry points. NOLOAD avoids introducing NvSS into
     * unrelated applications such as displaymanager/J9. Keep this reference
     * for process lifetime because the redirected entry points remain live. */
    void *library=dlopen("/mnt/app/armle/lib/libnvss_video.so",
                        RTLD_NOW|RTLD_LOCAL|RTLD_NOLOAD);
    if(!library)return;
    for(unsigned i=0;i<6;i++){
        entries[i].code=dlsym(library,entries[i].name);
        if(!entries[i].code || memcmp(entries[i].code,entries[i].expected,8)){
            printf("QEMU_NVSS redirect skipped: incompatible or already redirected %s\n",entries[i].name);
            return;
        }
    }
    long size=sysconf(_SC_PAGESIZE);
    if(size<4096 || (size&(size-1)))return;
    uintptr_t pages[6];void *copies[6]={0};unsigned count=0;
    for(unsigned i=0;i<6;i++){
        uintptr_t page=(uintptr_t)entries[i].code&~((uintptr_t)size-1);
        unsigned j=0;while(j<count && pages[j]!=page)j++;
        if(j==count){
            pages[count]=page;copies[count]=malloc(size);
            if(!copies[count]){for(unsigned k=0;k<count;k++)free(copies[k]);return;}
            memcpy(copies[count],(void *)page,size);count++;
        }
    }
    for(unsigned j=0;j<count;j++){
        void *page=(void *)pages[j];
        if(mmap(page,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0)!=page)abort();
        memcpy(page,copies[j],size);free(copies[j]);
        for(unsigned i=0;i<6;i++)if(((uintptr_t)entries[i].code&~((uintptr_t)size-1))==pages[j]){
            entries[i].code[0]=0xe51ff004; /* ldr pc,[pc,#-4] */
            entries[i].code[1]=(uint32_t)(uintptr_t)entries[i].target;
        }
        if(mprotect(page,size,PROT_READ|PROT_EXEC) || msync(page,size,MS_SYNC|MS_INVALIDATE_ICACHE))abort();
    }
    printf("QEMU_NVSS API acceleration active; six RAM entry points redirected; hardware driver bypassed\n");
    fflush(stdout);
}
