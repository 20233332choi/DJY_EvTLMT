from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from host_compiler import compiler

class FrontTimingTests(unittest.TestCase):
    def test_async_queue_full_wrap_and_buffer_lifetime(self):
        front=Path.home()/'Documents/TV/stm_front/Core'
        with tempfile.TemporaryDirectory() as tmp:
            folder=Path(tmp)
            for name in ('front_timing.c',):shutil.copyfile(front/'Src'/name,folder/name)
            shutil.copyfile(front/'Inc/front_timing.h',folder/'front_timing.h')
            (folder/'main.h').write_text('/* mock definitions supplied by harness */')
            (folder/'test.c').write_text(r'''
#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef struct {int gState;} UART_HandleTypeDef;
UART_HandleTypeDef huart2;
#define HAL_UART_STATE_READY 0
#define HAL_OK 0
#define __DMB() ((void)0)
static char *active;
static int HAL_UART_Transmit_IT(UART_HandleTypeDef *h,uint8_t *data,uint16_t n){
    (void)n;assert(h->gState==0);h->gState=1;active=(char *)data;return HAL_OK;
}
#include "front_timing.c"
int main(void){
    FrontTimingSample s={0};
    for(unsigned i=0;i<300;i++){s.seq=i;FrontTiming_Record(&s);}
    assert(dropped==44 && head==256 && tail==0);
    for(unsigned i=0;i<256;i++){
        FrontTiming_Poll();assert(huart2.gState==1);
        char saved[200];strcpy(saved,active);FrontTiming_Poll();assert(!strcmp(saved,active));
        unsigned seq;assert(sscanf(active,"F1,%u,",&seq)==1 && seq==i);
        huart2.gState=0;
    }
    assert(head==tail);FrontTiming_Poll();assert(huart2.gState==0);
    for(unsigned i=300;i<900;i++){
        s.seq=i;FrontTiming_Record(&s);FrontTiming_Poll();
        unsigned seq;assert(sscanf(active,"F1,%u,",&seq)==1 && seq==i);huart2.gState=0;
    }
    assert(dropped==44);return 0;
}
''')
            exe=folder/'test.exe'
            run=subprocess.run(compiler()+['-std=gnu11','-O2','-Wall','-Werror','-I',str(folder),str(folder/'test.c'),'-o',str(exe)],capture_output=True,timeout=180)
            self.assertEqual(run.returncode,0,run.stderr.decode(errors='replace'))
            run=subprocess.run([str(exe)],capture_output=True,timeout=10)
            self.assertEqual(run.returncode,0,run.stderr.decode(errors='replace'))

if __name__=='__main__':unittest.main()
