import pathlib
import subprocess
import tempfile
import unittest
from host_compiler import compiler

ROOT = pathlib.Path(__file__).resolve().parents[1]


class QueueTests(unittest.TestCase):
    def test_fifo_retries_overflow_and_partial_ack(self):
        source = r'''
#include "telemetry_queue.h"
#include <cassert>
#include <cstring>
int main() {
    TelemetryQueue<64,4> q;
    assert(q.push(1,"{\"seq\":1}")); assert(q.push(2,"{\"seq\":2}"));
    char a[512],b[512]; uint32_t last=0;
    assert(q.batch(a,sizeof(a),2,last)==2 && last==2);
    assert(q.batch(b,sizeof(b),2,last)==2 && !strcmp(a,b)); // failed HTTP retry
    assert(q.push(3,"{\"seq\":3}")); assert(q.push(4,"{\"seq\":4}"));
    assert(!q.push(5,"{\"seq\":5}") && q.dropped()==1 && q.count()==4);
    q.acknowledge(2); assert(q.count()==2);
    assert(q.push(6,"{\"seq\":6}")); // wrap ring, sequence gap exposes drop
    assert(q.batch(a,sizeof(a),4,last)==3 && last==6);
    assert(!strcmp(a,"[{\"seq\":3},{\"seq\":4},{\"seq\":6}]"));
    q.acknowledge(2); assert(q.count()==3); // duplicate ACK
    q.acknowledge(6); assert(q.count()==0);
    assert(q.batch(a,sizeof(a),4,last)==0 && !strcmp(a,"[]"));
    assert(q.push(7,"{\"seq\":7}"));
    assert(q.batch(a,3,4,last)==0 && q.count()==1); // small output never consumes
}
'''
        with tempfile.TemporaryDirectory() as folder:
            cpp = pathlib.Path(folder)/'queue.cpp'; cpp.write_text(source)
            exe = pathlib.Path(folder)/'queue.exe'
            subprocess.run(compiler(True)+['-std=c++11','-I',str(ROOT/'firmware/esp32_ev_gateway/include'),str(cpp),'-o',str(exe)],check=True,timeout=180,capture_output=True)
            subprocess.run([str(exe)],check=True,timeout=10)


if __name__ == '__main__': unittest.main()
