import pathlib
import subprocess
import tempfile
import unittest
from host_compiler import compiler

ROOT = pathlib.Path(__file__).resolve().parents[1]


class QueueTests(unittest.TestCase):
    def test_fifo_retries_overflow_and_partial_ack(self):
        firmware = (ROOT/'firmware/esp32_ev_gateway/src/main.cpp').read_text()
        body = firmware[firmware.index('class RelayBody :'):firmware.index('void stageRelayTelemetry(')]
        source = r'''
#include "telemetry_queue.h"
#include <cassert>
#include <cstring>
#include <string>
#include "telemetry_columns.h"
struct Stream {
    virtual ~Stream() {}
    virtual int available()=0;
    virtual int read()=0;
    virtual int peek()=0;
    virtual size_t readBytes(char*,size_t)=0;
    virtual size_t write(uint8_t)=0;
};
TelemetryQueue<1536,256,96*1024> relaySamples;
''' + body + r'''
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

    // Byte budget fills before entry count; wrapped rows survive ACK/retry.
    TelemetryQueue<32,10,40> packed;
    assert(packed.push(1,"1234567890123456789012345"));
    assert(packed.push(2,"abcdefghij"));
    assert(!packed.push(3,"123456") && packed.bytes()==35);
    packed.acknowledge(1);
    assert(packed.push(4,"ABCDEFGHIJKLMNOPQRST"));
    assert(packed.batch(a,sizeof(a),10,last)==2);
    assert(!strcmp(a,"[abcdefghij,ABCDEFGHIJKLMNOPQRST]"));
    packed.acknowledge(4); assert(packed.bytes()==0);

    char keys[256], values[256];
    assert(telemetryColumns("{\"text\":\"a,\\\"b}\",\"array\":[1,[2,3]],\"zero\":0}",keys,sizeof(keys),values,sizeof(values)));
    assert(!strcmp(keys,"[\"text\",\"array\",\"zero\"]"));
    assert(!strcmp(values,"[\"a,\\\"b}\",[1,[2,3]],0]"));
    assert(!telemetryColumns("{\"x\":[1,2",keys,sizeof(keys),values,sizeof(values)));
    assert(!telemetryColumns("{\"long_key\":1}",keys,5,values,sizeof(values)));

    // Real HTTP Stream adapter: prefix, wrapped FIFO data, commas, closing
    // brace, EOF, and appends during an in-flight frozen batch.
    assert(relaySamples.push(1,"[1]")); assert(relaySamples.push(2,"[2]"));
    size_t arrayLength=0;
    const auto rows=relaySamples.batchInfo(65536,64,last,arrayLength);
    RelayBody body("{\"samples\":",rows,arrayLength);
    assert(body.peek()=='{' && body.available()==static_cast<int>(strlen("{\"samples\":[[1],[2]]}")));
    std::string streamed;
    while(body.available()) {
        char part[3]; const auto n=body.readBytes(part,sizeof(part)); assert(n);
        streamed.append(part,n);
        if(streamed.size()==3) assert(relaySamples.push(3,"[3]"));
    }
    assert(streamed=="{\"samples\":[[1],[2]]}");
    assert(body.peek()==-1 && body.read()==-1);

    // 200 events/s: FIFO is retained during a lost ACK and subsequent retry.
    TelemetryQueue<1536,256,96*1024> fast;
    char output[48*1024]; uint32_t produced=0, accepted=0, ack=0;
    for(unsigned ms=0;ms<10000;ms+=5) {
        const auto row="["+std::to_string(++produced)+",\""+std::string(700,'x')+"\"]";
        assert(fast.push(produced,row.c_str()));
        if(ms%200==0) {
            const size_t count=fast.batch(output,sizeof(output),64,ack);
            assert(count && ack>=accepted);
            if(ms==1000)continue; // HTTP failed; don't acknowledge or consume.
            accepted=ack; fast.acknowledge(ack);
        }
    }
    while(fast.count()) { assert(fast.batch(output,sizeof(output),64,ack)); accepted=ack; fast.acknowledge(ack); }
    assert(accepted==produced && produced==2000 && fast.dropped()==0);
}
'''
        with tempfile.TemporaryDirectory() as folder:
            cpp = pathlib.Path(folder)/'queue.cpp'; cpp.write_text(source)
            exe = pathlib.Path(folder)/'queue.exe'
            subprocess.run(compiler(True)+['-std=c++11','-I',str(ROOT/'firmware/esp32_ev_gateway/include'),str(cpp),'-o',str(exe)],check=True,timeout=180,capture_output=True)
            subprocess.run([str(exe)],check=True,timeout=10)


if __name__ == '__main__': unittest.main()
