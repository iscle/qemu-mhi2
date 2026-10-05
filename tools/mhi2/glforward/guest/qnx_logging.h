/* QNX 6.5 keeps slogf in libc.a, not the shared libc used by this shim.
 * Preserve formatting and consumed-byte semantics while routing diagnostics
 * to the emulator console. NvOsDebugNprintf/KD retries any unconsumed bytes;
 * returning zero for a nonempty message deadlocks the display dispatcher. */
#include <stdarg.h>
extern int vsnprintf(char *,unsigned long,const char *,va_list);
int vslogf(int code,int severity,const char *format,va_list args)
{
    (void)code;(void)severity;
    if(!format)return -1;
    char message[1024];
    int length=vsnprintf(message,sizeof(message),format,args);
    if(length<0)return -1;
    unsigned size=(unsigned)length;
    if(size>=sizeof(message))size=sizeof(message)-1;
    unsigned sent=0;
    while(sent<size){long n=write(2,message+sent,size-sent);if(n<=0)return -1;sent+=(unsigned)n;}
    if(!size||message[size-1]!='\n')write(2,"\n",1);
    return (int)size;
}
int slogf(int code,int severity,const char *format,...)
{
    va_list args;va_start(args,format);
    int result=vslogf(code,severity,format,args);
    va_end(args);return result;
}
