/* Host encoder workers. Only native EGLImage pixels enter this pipeline.
 * Graphics submission and TS retrieval never wait for FFmpeg or the GPU. */
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <pthread.h>
#include <sys/wait.h>
#include <time.h>
extern char **environ;
#define ENC_LIMIT (4u * 1024 * 1024)
struct EncoderJob { uint32_t args[6]; uint8_t *pixels; size_t size; };
struct Encoder {
    uint32_t owner,id;
    pthread_t thread;
    pthread_mutex_t lock;
    bool used,stop,done,failed;
    struct EncoderJob jobs[2];
    unsigned head,count;
    uint8_t output[ENC_LIMIT];
    size_t received;
    unsigned submitted,completed,dropped;
};
static struct Encoder encoders[4];
static double encoder_now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void encoder_child_stop(pid_t child,int input,int output)
{
    if(input>=0)close(input);if(output>=0)close(output);
    if(child<=0)return;
    kill(child,SIGTERM);
    double deadline=encoder_now()+1;
    while(waitpid(child,NULL,WNOHANG)==0&&encoder_now()<deadline)usleep(1000);
    if(waitpid(child,NULL,WNOHANG)==0){kill(child,SIGKILL);waitpid(child,NULL,0);}
}
/* Darwin has no pipe2. CLOEXEC_DEFAULT below also closes descriptors
 * created by another encoder thread between pipe() and fcntl(). */
static int encoder_pipe(int fd[2])
{
#ifdef __APPLE__
    if (pipe(fd)) {
        return -1;
    }
    if (fcntl(fd[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(fd[1], F_SETFD, FD_CLOEXEC) < 0) {
        int saved_errno = errno;
        close(fd[0]);
        close(fd[1]);
        errno = saved_errno;
        return -1;
    }
    return 0;
#else
    return pipe2(fd, O_CLOEXEC);
#endif
}
static pid_t encoder_spawn(const uint32_t *a,int *input,int *output)
{
    int in[2],out[2];
    if(encoder_pipe(in))return -1;
    if(encoder_pipe(out)){close(in[0]);close(in[1]);return -1;}
    char dimensions[32],frequency[16],bitrate[24];
    snprintf(dimensions,sizeof(dimensions),"%ux%u",a[1],a[2]);
    snprintf(frequency,sizeof(frequency),"%u",a[3]);snprintf(bitrate,sizeof(bitrate),"%u",a[4]);
    const char *device=getenv("MHI2_VAAPI_DEVICE");
    bool hw=device&&*device;
    const char *filter=hw?(a[5]?"vflip,format=nv12,hwupload":"format=nv12,hwupload"):
                           (a[5]?"vflip,format=yuv420p":"format=yuv420p");
    char *args[80];unsigned n=0;
#define ARG(x) args[n++]=(char *)(x)
    ARG("ffmpeg");ARG("-hide_banner");ARG("-loglevel");ARG("error");
    ARG("-filter_threads");ARG("1");
    if(hw){ARG("-vaapi_device");ARG(device);}
    ARG("-probesize");ARG("32");ARG("-analyzeduration");ARG("0");
    ARG("-f");ARG("rawvideo");ARG("-pixel_format");ARG("rgba");
    ARG("-video_size");ARG(dimensions);ARG("-framerate");ARG(frequency);
    ARG("-i");ARG("pipe:0");ARG("-an");ARG("-vf");ARG(filter);
    ARG("-c:v");ARG(hw?"h264_vaapi":"libopenh264");
    ARG("-threads");ARG("1");
    ARG("-profile:v");ARG("constrained_baseline");ARG("-bf");ARG("0");
    ARG("-g");ARG("30");ARG("-b:v");ARG(bitrate);
    if(hw){ARG("-async_depth");ARG("1");}else{ARG("-rc_mode");ARG("bitrate");}
    ARG("-mpegts_flags");ARG("+resend_headers");ARG("-muxdelay");ARG("0");
    ARG("-muxpreload");ARG("0");ARG("-flush_packets");ARG("1");
    ARG("-f");ARG("mpegts");ARG("pipe:1");args[n]=NULL;
#undef ARG
    posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions,in[0],STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions,out[1],STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions,in[1]);posix_spawn_file_actions_addclose(&actions,out[0]);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#ifdef __APPLE__
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
    pid_t child=-1;int error=posix_spawnp(&child,"ffmpeg",&actions,&attr,args,environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);close(in[0]);close(out[1]);
    if(error){close(in[1]);close(out[0]);return -1;}
    fcntl(in[1],F_SETFL,O_NONBLOCK);fcntl(out[0],F_SETFL,O_NONBLOCK);
    *input=in[1];*output=out[0];return child;
}
static void *encoder_worker(void *opaque)
{
    struct Encoder *e=opaque;struct EncoderJob job={0};
    pid_t child=-1;int input=-1,output=-1;uint32_t config[6]={0};
    size_t sent=0;double progress=encoder_now();
    for(;;){
        pthread_mutex_lock(&e->lock);
        bool stop=e->stop;
        if(!job.pixels&&e->count){job=e->jobs[e->head];e->head=(e->head+1)%2;e->count--;sent=0;progress=encoder_now();}
        size_t room=ENC_LIMIT-e->received;
        pthread_mutex_unlock(&e->lock);
        if(stop)break;
        if(job.pixels&&(child<0||memcmp(config+1,job.args+1,5*sizeof(uint32_t)))){
            encoder_child_stop(child,input,output);input=output=-1;
            child=encoder_spawn(job.args,&input,&output);
            if(child<0)break;
            memcpy(config,job.args,sizeof(config));
            fprintf(stderr,"glhost: async encoder %u/%u %ux%u %u fps (%s)\n",e->owner,e->id,config[1],config[2],config[3],getenv("MHI2_VAAPI_DEVICE")?"h264_vaapi":"libopenh264");
        }
        struct pollfd fds[2]={{input,job.pixels?POLLOUT:0,0},{output,room?POLLIN:0,0}};
        int ready=poll(fds,2,5);if(ready<0){if(errno==EINTR)continue;break;}
        if(fds[0].revents&POLLOUT){
            ssize_t n=write(input,job.pixels+sent,job.size-sent);
            if(n>0){sent+=n;progress=encoder_now();}else if(errno!=EAGAIN&&errno!=EINTR)break;
            if(sent==job.size){free(job.pixels);job.pixels=NULL;pthread_mutex_lock(&e->lock);e->completed++;pthread_mutex_unlock(&e->lock);}
        }
        if(fds[1].revents&POLLIN){
            uint8_t data[65536];ssize_t n=read(output,data,room<sizeof(data)?room:sizeof(data));
            if(n>0){pthread_mutex_lock(&e->lock);memcpy(e->output+e->received,data,n);e->received+=n;pthread_mutex_unlock(&e->lock);progress=encoder_now();}
            else if(n==0||(errno!=EAGAIN&&errno!=EINTR))break;
        }
        if((fds[0].revents|fds[1].revents)&(POLLERR|POLLHUP|POLLNVAL))break;
        if(job.pixels&&encoder_now()-progress>5)break;
    }
    free(job.pixels);encoder_child_stop(child,input,output);
    pthread_mutex_lock(&e->lock);
    for(unsigned i=0;i<e->count;i++)free(e->jobs[(e->head+i)%2].pixels);
    e->count=0;e->failed=!e->stop;e->done=true;
    fprintf(stderr,"glhost: encoder %u/%u submitted=%u delivered-to-codec=%u dropped=%u failed=%d\n",e->owner,e->id,e->submitted,e->completed,e->dropped,e->failed);
    pthread_mutex_unlock(&e->lock);return NULL;
}
static struct Encoder *encoder_find(uint32_t owner,uint32_t id)
{for(unsigned i=0;i<4;i++)if(encoders[i].used&&encoders[i].owner==owner&&encoders[i].id==id)return &encoders[i];return NULL;}
static void encoder_reap(void)
{
    for(unsigned i=0;i<4;i++){
        struct Encoder *e=&encoders[i];if(!e->used)continue;
        pthread_mutex_lock(&e->lock);bool done=e->done&&e->stop;pthread_mutex_unlock(&e->lock);
        if(done){pthread_join(e->thread,NULL);pthread_mutex_destroy(&e->lock);memset(e,0,sizeof(*e));}
    }
}
/* Submit status: 0 accepted, 1 encoder failed, 2 queue full (drop new frame).
 * Two waiting images plus one in flight; an overloaded codec cannot queue
 * unbounded latency or stall the graphics command loop. */
static uint32_t encoder_submit(uint32_t owner,const uint8_t *p)
{
    encoder_reap();uint32_t id=u32(p,0);struct Encoder *e=encoder_find(owner,id);
    if(!e){
        for(unsigned i=0;i<4;i++)if(!encoders[i].used){e=&encoders[i];break;}
        if(!e)return 1;
        e->owner=owner;e->id=id;pthread_mutex_init(&e->lock,NULL);e->used=true;
        if(pthread_create(&e->thread,NULL,encoder_worker,e)){pthread_mutex_destroy(&e->lock);memset(e,0,sizeof(*e));return 1;}
    }
    pthread_mutex_lock(&e->lock);
    if(e->stop||e->failed){pthread_mutex_unlock(&e->lock);return 1;}
    if(e->count==2){e->dropped++;pthread_mutex_unlock(&e->lock);return 2;}
    size_t size=(size_t)u32(p,4)*u32(p,8)*4;
    uint8_t *pixels=malloc(size);if(!pixels){pthread_mutex_unlock(&e->lock);return 1;}
    memcpy(pixels,p+24,size);
    struct EncoderJob *j=&e->jobs[(e->head+e->count)%2];
    memcpy(j->args,p,24);j->pixels=pixels;j->size=size;e->count++;e->submitted++;
    pthread_mutex_unlock(&e->lock);return 0;
}
/* Return complete 188-byte TS packets immediately, not "one frame after a
 * quiet period". Native ISO packetization accepts arbitrary TS packet groups. */
static uint32_t encoder_poll(uint32_t owner,uint32_t id,uint8_t **data)
{
    *data=NULL;struct Encoder *e=encoder_find(owner,id);if(!e)return UINT32_MAX;
    pthread_mutex_lock(&e->lock);
    size_t n=(e->received/188)*188;if(n>188*256)n=188*256;
    if(n){*data=malloc(n);if(*data){memcpy(*data,e->output,n);e->received-=n;memmove(e->output,e->output+n,e->received);}else n=0;}
    uint32_t result=n?n:(e->failed?UINT32_MAX:0);
    pthread_mutex_unlock(&e->lock);return result;
}
static void encoder_close(uint32_t owner,uint32_t id)
{struct Encoder *e=encoder_find(owner,id);if(e){pthread_mutex_lock(&e->lock);e->stop=true;pthread_mutex_unlock(&e->lock);}}
static void encoder_stop(struct Encoder *e)
{if(e->used){encoder_close(e->owner,e->id);pthread_join(e->thread,NULL);pthread_mutex_destroy(&e->lock);memset(e,0,sizeof(*e));}}
