/* Windows adapter. Existing Encoder queues/protocol stay shared.
 * libuv owns process handles and asynchronous native stdin/stdout pipes. */
#include <uv.h>
#include <signal.h>

struct EncoderUV {
    uv_loop_t loop;
    uv_process_t child;
    uv_pipe_t input,output;
    uv_write_t write;
    uv_timer_t timer;
    struct Encoder *encoder;
    bool spawned,exited,closing,failed,writing,reading,timer_initialized;
    size_t sent,write_size;
    double progress;
    char read_buffer[65536];
};

static void encoder_uv_exit(uv_process_t *process,int64_t status,int signal_number)
{
    struct EncoderUV *s=process->data;
    s->exited=true;
    if(!s->closing){
        s->failed=true;
        fprintf(stderr,"glhost: encoder child %d exited status=%lld signal=%d\n",
                process->pid,(long long)status,signal_number);
    }
    uv_close((uv_handle_t *)process,NULL);
}

static void encoder_uv_alloc(uv_handle_t *handle,size_t suggested,uv_buf_t *buffer)
{
    (void)suggested;
    struct EncoderUV *s=handle->data;
    pthread_mutex_lock(&s->encoder->lock);
    size_t room=ENC_LIMIT-s->encoder->received;
    pthread_mutex_unlock(&s->encoder->lock);
    *buffer=uv_buf_init(s->read_buffer,(unsigned)(room<sizeof(s->read_buffer)?room:sizeof(s->read_buffer)));
}

static void encoder_uv_read(uv_stream_t *stream,ssize_t count,const uv_buf_t *buffer)
{
    struct EncoderUV *s=stream->data;
    if(count>0){
        pthread_mutex_lock(&s->encoder->lock);
        size_t room=ENC_LIMIT-s->encoder->received;
        if((size_t)count>room)s->failed=true;
        else{
            memcpy(s->encoder->output+s->encoder->received,buffer->base,count);
            s->encoder->received+=count;
        }
        room=ENC_LIMIT-s->encoder->received;
        pthread_mutex_unlock(&s->encoder->lock);
        s->progress=encoder_now();
        if(!room){uv_read_stop(stream);s->reading=false;}
    }else if(count<0 && !s->closing){
        s->failed=true;
        fprintf(stderr,"glhost: encoder stdout %s\n",uv_strerror((int)count));
    }
}

static void encoder_uv_written(uv_write_t *request,int status)
{
    struct EncoderUV *s=request->data;
    s->writing=false;
    if(status){if(!s->closing)s->failed=true;}
    else{s->sent+=s->write_size;s->progress=encoder_now();}
}

static void encoder_uv_close_handle(uv_handle_t *handle)
{
    if(!uv_is_closing(handle))uv_close(handle,NULL);
}

static void encoder_uv_tick(uv_timer_t *timer){(void)timer;}

static void encoder_uv_stop(struct EncoderUV *s)
{
    s->closing=true;
    encoder_uv_close_handle((uv_handle_t *)&s->input);
    encoder_uv_close_handle((uv_handle_t *)&s->output);
    if(s->timer_initialized){
        uv_timer_stop(&s->timer);encoder_uv_close_handle((uv_handle_t *)&s->timer);
    }
    if(s->spawned && !s->exited)uv_process_kill(&s->child,SIGTERM);
    double deadline=encoder_now()+1;
    while(uv_loop_alive(&s->loop) && encoder_now()<deadline){
        uv_run(&s->loop,UV_RUN_NOWAIT);uv_sleep(1);
    }
    if(s->spawned && !s->exited)uv_process_kill(&s->child,SIGKILL);
    uv_run(&s->loop,UV_RUN_DEFAULT);
    int error=uv_loop_close(&s->loop);
    if(error)fprintf(stderr,"glhost: encoder loop close %s\n",uv_strerror(error));
}

static int encoder_uv_start(struct EncoderUV *s,struct Encoder *e,const uint32_t *a)
{
    memset(s,0,sizeof(*s));s->encoder=e;s->progress=encoder_now();
    if(uv_loop_init(&s->loop))return -1;
    if(uv_pipe_init(&s->loop,&s->input,0)){
        uv_loop_close(&s->loop);return -1;
    }
    if(uv_pipe_init(&s->loop,&s->output,0)){
        uv_close((uv_handle_t *)&s->input,NULL);
        uv_run(&s->loop,UV_RUN_DEFAULT);uv_loop_close(&s->loop);return -1;
    }
    s->output.data=s;s->child.data=s;s->write.data=s;
    if(uv_timer_init(&s->loop,&s->timer)){encoder_uv_stop(s);return -1;}
    s->timer_initialized=true;
    if(uv_timer_start(&s->timer,encoder_uv_tick,5,5)){encoder_uv_stop(s);return -1;}
    const char *codec=encoder_codec();
    const char *executable=getenv("MHI2_ENCODER_EXE");
    if(!executable || !*executable)executable="ffmpeg.exe";
    if(!codec || (strcmp(codec,"libx264") && strcmp(codec,"libopenh264"))){
        encoder_uv_stop(s);return -1;
    }
    char dimensions[32],frequency[16],bitrate[24];
    snprintf(dimensions,sizeof(dimensions),"%ux%u",a[1],a[2]);
    snprintf(frequency,sizeof(frequency),"%u",a[3]);snprintf(bitrate,sizeof(bitrate),"%u",a[4]);
    char *arguments[80];unsigned n=0;
#define ARG(x) arguments[n++]=(char *)(x)
    ARG(executable);ARG("-hide_banner");ARG("-loglevel");ARG("error");
    ARG("-filter_threads");ARG("1");ARG("-probesize");ARG("32");ARG("-analyzeduration");ARG("0");
    ARG("-f");ARG("rawvideo");ARG("-pixel_format");ARG("rgba");
    ARG("-video_size");ARG(dimensions);ARG("-framerate");ARG(frequency);
    ARG("-i");ARG("pipe:0");ARG("-an");ARG("-vf");ARG(a[5]?"vflip,format=yuv420p":"format=yuv420p");
    ARG("-c:v");ARG(codec);ARG("-threads");ARG("1");
    ARG("-profile:v");ARG(!strcmp(codec,"libopenh264")?"constrained_baseline":"baseline");
    ARG("-bf");ARG("0");ARG("-g");ARG("30");ARG("-b:v");ARG(bitrate);
    if(!strcmp(codec,"libx264")){ARG("-preset");ARG("ultrafast");ARG("-tune");ARG("zerolatency");}
    else{ARG("-rc_mode");ARG("bitrate");}
    ARG("-mpegts_flags");ARG("+resend_headers");ARG("-muxdelay");ARG("0");
    ARG("-muxpreload");ARG("0");ARG("-flush_packets");ARG("1");
    ARG("-f");ARG("mpegts");ARG("pipe:1");arguments[n]=NULL;
#undef ARG
    uv_stdio_container_t stdio[3]={0};
    stdio[0].flags=UV_CREATE_PIPE|UV_READABLE_PIPE;stdio[0].data.stream=(uv_stream_t *)&s->input;
    stdio[1].flags=UV_CREATE_PIPE|UV_WRITABLE_PIPE;stdio[1].data.stream=(uv_stream_t *)&s->output;
    stdio[2].flags=UV_INHERIT_FD;stdio[2].data.fd=STDERR_FILENO;
    uv_process_options_t options={0};
    options.file=executable;options.args=arguments;options.exit_cb=encoder_uv_exit;
    options.stdio=stdio;options.stdio_count=3;options.flags=UV_PROCESS_WINDOWS_HIDE;
    int error=uv_spawn(&s->loop,&s->child,&options);
    if(error){
        fprintf(stderr,"glhost: encoder spawn %s\n",uv_strerror(error));
        uv_close((uv_handle_t *)&s->child,NULL);encoder_uv_stop(s);return -1;
    }
    s->spawned=true;
    fprintf(stderr,"glhost: native encoder child PID=%d\n",s->child.pid);
    return 0;
}

static void *encoder_worker(void *opaque)
{
    struct Encoder *e=opaque;struct EncoderJob job={0};struct EncoderUV session;
    bool active=false,failed=false;uint32_t config[6]={0};
    for(;;){
        pthread_mutex_lock(&e->lock);
        bool stop=e->stop;
        if(!job.pixels && e->count){
            job=e->jobs[e->head];e->head=(e->head+1)%2;e->count--;
            if(active)session.progress=encoder_now();
        }
        size_t room=ENC_LIMIT-e->received;
        pthread_mutex_unlock(&e->lock);
        if(stop)break;
        if(job.pixels && (!active || memcmp(config+1,job.args+1,5*sizeof(uint32_t)))){
            if(active)encoder_uv_stop(&session);
            active=false;
            if(encoder_uv_start(&session,e,job.args)){failed=true;break;}
            active=true;memcpy(config,job.args,sizeof(config));
            fprintf(stderr,"glhost: async encoder %u/%u %ux%u %u fps (%s)\n",
                    e->owner,e->id,config[1],config[2],config[3],encoder_codec());
        }
        if(active){
            if(room && !session.reading){
                int error=uv_read_start((uv_stream_t *)&session.output,encoder_uv_alloc,encoder_uv_read);
                if(error){failed=true;break;}session.reading=true;
            }
            if(job.pixels && !session.writing && session.sent<job.size){
                size_t amount=job.size-session.sent;if(amount>65536)amount=65536;
                uv_buf_t buffer=uv_buf_init((char *)job.pixels+session.sent,(unsigned)amount);
                session.write_size=amount;session.writing=true;
                int error=uv_write(&session.write,(uv_stream_t *)&session.input,&buffer,1,encoder_uv_written);
                if(error){session.writing=false;failed=true;break;}
            }
            uv_run(&session.loop,UV_RUN_ONCE);
            if(session.failed){failed=true;break;}
            if(job.pixels && !session.writing && session.sent==job.size){
                free(job.pixels);job.pixels=NULL;session.sent=0;
                pthread_mutex_lock(&e->lock);e->completed++;pthread_mutex_unlock(&e->lock);
            }
            if(job.pixels && encoder_now()-session.progress>5){failed=true;break;}
        }
        if(!active)uv_sleep(1);
    }
    if(active)encoder_uv_stop(&session);
    free(job.pixels);
    pthread_mutex_lock(&e->lock);
    for(unsigned i=0;i<e->count;i++)free(e->jobs[(e->head+i)%2].pixels);
    e->count=0;e->failed=failed || !e->stop;e->done=true;
    fprintf(stderr,"glhost: encoder %u/%u submitted=%u delivered-to-codec=%u dropped=%u failed=%d\n",
            e->owner,e->id,e->submitted,e->completed,e->dropped,e->failed);
    pthread_mutex_unlock(&e->lock);return NULL;
}

