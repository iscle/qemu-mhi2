#!/usr/bin/env python3
"""Exercise native queue ownership/bounds and host PCM packet handling."""
import io
import subprocess
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import Mock, patch
from audio_host import AudioEndpoint


class Checks(unittest.TestCase):
    def test_host_payload_and_microphone_fragments(self):
        audio = AudioEndpoint()
        audio.log = io.BytesIO()
        audio.player = Mock(); audio.player.poll.return_value = None
        audio.recorder = Mock(); audio.recorder.poll.return_value = None
        header = b'AUD0'+bytes([1, 11, 0, 0])+bytes(4)
        data = bytes(range(256))*4
        with patch('audio_host.os.write') as write:
            audio.process(header+data)
            self.assertEqual(audio.log.getvalue(), data)
            write.assert_called_once_with(audio.player.stdin.fileno(), data)
        request = b'AUD0'+bytes([2, 24, 0, 0])+b'abcd'
        with patch('audio_host.os.read', return_value=data*2):
            self.assertEqual(audio.process(request)[12:], data)
        with patch('audio_host.os.read', side_effect=BlockingIOError):
            self.assertEqual(audio.process(request)[12:], data)
            self.assertEqual(audio.process(request)[12:], bytes(1024))
        for bad in (b'', header, header+data+b'x', request+b'x', b'BAD0'+request[4:]):
            self.assertIsNone(audio.process(bad))
        # A failed microphone must not suppress otherwise available speakers.
        audio.player = None
        audio.retry['recorder'] = time.monotonic()+60
        player = Mock(); player.poll.return_value = None
        with patch.object(audio, 'start', return_value=player) as start, patch('audio_host.os.write'):
            audio.process(header+data)
            start.assert_called_once_with('pw-play', playback=True)

    def test_original_queue_layout(self):
        source = Path(__file__).with_name('audio')/'pcm_endpoint.c'
        harness = '''
#define _start endpoint_start
#include "SOURCE"
int pthread_mutex_lock(void *p){return 0;}
int pthread_mutex_unlock(void *p){return 0;}
int pthread_cond_signal(void *p){return 0;}
static unsigned char storage[4096],a[1024],b[1024];
int main(void){
 unsigned char *q=storage; queues[10]=q; q[1]=1;
 *word(q,32)=3;*word(q,36)=1024;*word(q,20)=2;
 *word(q,24)=*word(q,28)=~0u;
 for(unsigned i=0;i<3;i++)*word(q+(i+1)*40,8)=i-1;
 for(unsigned i=0;i<3;i++){memset(a,i+1,1024);if(!transfer(10,a,1))return 1;}
 if(transfer(10,a,1))return 2;
 for(unsigned i=0;i<3;i++){
  if(!transfer(10,b,0))return 3;
  for(unsigned j=0;j<1024;j++)if(b[j]!=i+1)return 4;
 }
 if(transfer(10,b,0))return 5;
 q[1]=0;if(transfer(10,a,1))return 6;q[1]=1;
 *word(q,24)=0;*word(q,28)=100;
 unsigned old=*word(q,20);
 if(transfer(10,a,1)||*word(q,20)!=old)return 7;
 *word(q,24)=100;if(transfer(10,b,0))return 8;
 return 0;
}
'''.replace('SOURCE', str(source))
        with tempfile.TemporaryDirectory(prefix='mhi2-audio-test-') as tmp:
            src=Path(tmp)/'queue.c'; exe=Path(tmp)/'queue';src.write_text(harness)
            subprocess.run(['cc','-O2','-fno-builtin','-ffunction-sections','-Wl,--gc-sections',
                            str(src),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)


if __name__ == '__main__':
    unittest.main()
