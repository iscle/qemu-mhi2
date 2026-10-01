/* K3342 companion audio endpoint. Attaches the original driver's queues;
 * the speech executable, nvaudio library and speech engine remain original.
 * Queue layout recovered from libnvaudio.so, functions 0x32d8/0x3490.
 * Speech streams are 512 frames, mono S16LE, 48 kHz. SSE_REC has six slots.
 */
typedef unsigned u32;
extern int open(const char*,int,...),close(int),write(int,const void*,unsigned);
extern int snprintf(char*,unsigned,const char*,...);
extern void *mmap64(void*,unsigned,int,int,int,unsigned long long);
extern int pthread_mutex_lock(void*),pthread_mutex_unlock(void*),pthread_cond_signal(void*);
extern void *memcpy(void*,const void*,unsigned),*memset(void*,int,unsigned);
extern int socket(int,int,int),connect(int,const void*,unsigned),send(int,const void*,unsigned,int),recv(int,void*,unsigned,int),usleep(unsigned);
extern void exit(int);
static unsigned char *queues[29];
static unsigned char mic[1024],sse[1024],interleaved[6144],block[1024],packet[1040];
static u32 sequence;
static volatile u32 *hardware;
static u32 *word(unsigned char *p,unsigned off){return (u32*)(p+off);}
/* Copy and move an entry under the driver's process-shared mutex. No blocking
 * queue waits: unavailable/full queues are retried on the next audio tick. */
static int transfer(unsigned id,void *data,int produce){
 unsigned char *q=queues[id];if(!q)return 0;
 if(pthread_mutex_lock(q+4))return 0;
 u32 count=*word(q,32),size=*word(q,36),index=*word(q,produce?20:24);
 int ok=0;
 if(q[1] && count>=2 && count<=16 && size==(id==28?6144:1024) && (count+1)*40+count*size<=65536 && index<count){
  unsigned char *entry=q+(index+1)*40,*samples=q+(count+1)*40+size*index;
  if(produce){
   if(*word(q,24)!=~0u && *word(q,28)>=count)goto done;
   *word(q,20)=*word(entry,8);memcpy(samples,data,size);
   *word(entry,0)=*word(entry,4)=0;*word(entry,8)=~0u;
   if(*word(q,24)==~0u)*word(q,24)=index;
   else {u32 tail=*word(q,28);if(tail>=count)goto done;*word(q+(tail+1)*40,8)=index;}
   *word(q,28)=index;
  }else{
   memcpy(data,samples,size);
   if(index==*word(q,28))*word(q,24)=*word(q,28)=~0u;
   else *word(q,24)=*word(entry,8);
   *word(entry,8)=*word(q,20);*word(q,20)=index;
  }
  pthread_cond_signal(q+12);ok=1;
 }
 done:pthread_mutex_unlock(q+4);return ok;
}
static void header(unsigned kind,unsigned stream){
 memcpy(packet,"AUD0",4);packet[4]=kind;packet[5]=stream;packet[6]=packet[7]=0;
 *word(packet,8)=sequence++;
}
void _start(void){
 char path[80],line[100];
 int memfd=open("/dev/mem",2);
 if(memfd>=0){
  void *mapping=mmap64(0,4096,0x700,1,memfd,0x70080000ull);close(memfd);
  if(mapping!=(void*)-1){
   volatile u32 *mailbox=(volatile u32*)((unsigned char*)mapping+0xe00);
   if(mailbox[0]==0x4d484941)hardware=mailbox;
  }
 }
 for(unsigned i=10;i<29;i++){
  if(i>19&&i<24)continue;
  snprintf(path,sizeof(path),"/dev/shmem/NVshm-tdm-%u",i);
  int fd=open(path,2);if(fd<0)continue;
  unsigned char *q=mmap64(0,65536,0x300,1,fd,0);close(fd);
  if(q==(void*)-1)continue;
  u32 count=*word(q,32),size=*word(q,36);
  if(count<2||count>16||size!=(i==28?6144:1024)||(count+1)*40+count*size>65536)continue;
  queues[i]=q;
 }
 int fd=socket(2,2,0);
 /* QNX sockaddr_in: length, family, network-order port and IPv4. */
 unsigned char address[16]={16,2,0xc3,0x50,10,0,0,16};
 if(fd<0||connect(fd,address,16)){write(2,"PCM endpoint socket failed\n",27);exit(1);}
 if(hardware)write(1,"PCM endpoint: native DMA mailbox active\n",sizeof("PCM endpoint: native DMA mailbox active\n")-1);
 write(1,"PCM endpoint ready: original NVIDIA speech queues\n",sizeof("PCM endpoint ready: original NVIDIA speech queues\n")-1);
 unsigned ticks=0,played=0,recorded=0;
 for(;;){
  int active=queues[24]&&queues[24][1];
  if(active){
   header(2,24);send(fd,packet,12,0);
   memset(mic,0,sizeof(mic));
   /* MSG_DONTWAIT = 0x80 on QNX. Responses are one mono PCM block. */
   int n;
   while((n=recv(fd,packet,sizeof(packet),0x80))>0)
    if(n==1036&&packet[0]=='A'&&packet[1]=='U'&&packet[2]=='D'&&packet[3]=='0'&&packet[4]==3)
     memcpy(mic,packet+12,1024);
   if(hardware){
    if(hardware[3]>=512)for(unsigned i=0;i<512;i++){
     hardware[4]=(u32)mic[2*i]|((u32)mic[2*i+1]<<8);
    }
    recorded++;
   }else for(unsigned i=24;i<28;i++)recorded+=transfer(i,mic,1);
  }
  if(!hardware && transfer(19,sse,0)){
   memset(interleaved,0,sizeof(interleaved));
   for(unsigned i=0;i<512;i++)memcpy(interleaved+i*12,sse+i*2,2);
   transfer(28,interleaved,1);
  }
  if(hardware){
   /* Tap the native TDM output after DMA; do not steal the driver's queues. */
   while(hardware[1]>=512){
    for(unsigned i=0;i<512;i++){u32 sample=hardware[2];block[2*i]=sample;block[2*i+1]=sample>>8;}
    header(1,16);memcpy(packet+12,block,1024);send(fd,packet,1036,0);played++;
   }
  }
  for(unsigned i=10;!hardware && i<19;i++)if(transfer(i,block,0)){
   header(1,i);memcpy(packet+12,block,1024);send(fd,packet,1036,0);played++;
  }
  if(++ticks%1000==0){int n=snprintf(line,sizeof(line),"PCM endpoint blocks: playback=%u microphone=%u\n",played,recorded);write(1,line,n);}
  usleep(10667);
 }
}
