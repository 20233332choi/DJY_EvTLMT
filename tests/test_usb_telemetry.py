import binascii
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch
from host_compiler import compiler
import test_esp_bms_packet

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'gateway'))
from usb_serial import decode_usb_line, usb_lines
from ev_gateway import EVGateway


class UsbTelemetryTests(unittest.TestCase):
    def test_actual_formatter_over_3mbaud_and_lost_ack(self):
        formatted = test_esp_bms_packet.EspBmsPacketTests('test_complete_bms_datagram').format_datagram_rows()
        columns = json.loads(formatted[7]) # 48 cells + wide diagnostics
        row = formatted[8].decode()
        # Preserve the formatter's exact decimal widths when measuring bandwidth.
        spans, cursor, decoder = [], 1, json.JSONDecoder()
        for _ in columns:
            _, end = decoder.raw_decode(row, cursor)
            spans.append((cursor, end)); cursor = end + 1
        for index, token in sorted(((columns.index('seq'), 'SEQ_TOKEN'),
                                    (columns.index('timestamp_ms'), 'TIME_TOKEN')), reverse=True):
            start, end = spans[index]
            row = row[:start] + json.dumps(token) + row[end:]
        seq_prefix = row.split('"SEQ_TOKEN"')[0]
        harness = r'''
#include "usb_telemetry.h"
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
static const std::string schema=SCHEMA, base=ROW, seqPrefix=SEQ_PREFIX;
struct Uart {
    std::string tx, received;
    size_t shortWrite=4096;
    int availableForWrite() { return 4096-static_cast<int>(tx.size()); }
    size_t write(const uint8_t* p,size_t n) {
        assert(n<=static_cast<size_t>(availableForWrite()));
        n=std::min(n,shortWrite);tx.append(reinterpret_cast<const char*>(p),n);return n;
    }
};
void replace(std::string& row,const std::string& token,const std::string& value) {
    const auto p=row.find(token);assert(p!=std::string::npos);row.replace(p,token.size(),value);
}
int main(int argc,char**) {
    const bool failure=argc>1;
    UsbTelemetry usb; Uart uart; usb.start();
    if(failure) uart.shortWrite=7; // Partial writes must preserve bytes and CRC.
    unsigned produced=0, frames=0;
    uint32_t ackAt=0, pendingAck=0;
    for(uint32_t now=0;now<14000;++now) {
        if(pendingAck && now>=ackAt) { usb.acknowledge(pendingAck);pendingAck=0; }
        if((!failure && now<10000 && now%5==0) || (failure && (now==0 || now==5 || now==500))) {
            auto row=base;
            replace(row,"\"SEQ_TOKEN\"",std::to_string(++produced));
            replace(row,"\"TIME_TOKEN\"",std::to_string(now));
            usb.push(produced,row.c_str());
        }
        for(unsigned i=0;i<4;++i)usb.poll(uart,now,schema.c_str(),"0123456789abcdef","EV");
        // Physical UART budget: 3,000,000 / 10 bits = 300 bytes per millisecond.
        const auto sent=std::min<size_t>(300,uart.tx.size());
        uart.received.append(uart.tx,0,sent);uart.tx.erase(0,sent);
        auto newline=uart.received.find('\n');
        if(newline!=std::string::npos) {
            auto frame=uart.received.substr(0,newline+1);uart.received.erase(0,newline+1);
            std::cout<<frame; ++frames;
            const auto p=frame.rfind(seqPrefix);assert(p!=std::string::npos);
            const uint32_t last=std::strtoul(frame.c_str()+p+seqPrefix.size(),nullptr,10);
            usb.acknowledge(last+1);assert(usb.queued()>0); // Wrong ACK never consumes.
            if(!failure || frames>1) { pendingAck=last;ackAt=now+10; } // PC processing/ACK latency.
        }
    }
    assert(usb.queued()==0 && usb.dropped()==0 && uart.tx.empty());
    assert(produced==(failure?3u:2000u));
}
'''.replace('SCHEMA', json.dumps(json.dumps(columns, separators=(',', ':')))).replace('ROW', json.dumps(row)).replace('SEQ_PREFIX', json.dumps(seq_prefix))
        with tempfile.TemporaryDirectory() as folder:
            source, exe = Path(folder)/'usb.cpp', Path(folder)/'usb'
            source.write_text(harness)
            build = subprocess.run(compiler(True)+['-std=c++11', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT/'firmware/esp32_ev_gateway/include'), str(source), '-o', str(exe)], capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr.decode())
            for fault, count in ((False, 2000), (True, 3)):
                run = subprocess.run([str(exe)] + (['lost-ack'] if fault else []), capture_output=True)
                self.assertEqual(run.returncode, 0, run.stderr.decode())
                packets = [decode_usb_line(line) for line in run.stdout.splitlines()]
                schema = None
                with patch('ev_gateway.socket.socket', return_value=Mock()):
                    gateway = EVGateway('127.0.0.1', 0, '127.0.0.1', 19004, database_path=Path(folder)/f'{fault}.db')
                try:
                    gateway.database.start()
                    for packet in packets:
                        schema = packet.get('columns', schema)
                        packet['columns'] = schema
                        result = gateway.telemetry_exchange(packet, 'USB')
                        self.assertEqual(result['ack_seq'], packet['samples'][-1][columns.index('seq')])
                    self.assertEqual(gateway.database.status()['recording_sample_count'], count)
                    self.assertEqual(len(gateway.tuning.read()['samples']), count)
                    self.assertEqual(len(gateway.battery.read()['samples']), count)
                    self.assertEqual(gateway.snapshot()['seq'], count)
                finally:
                    gateway.database.close()
                self.assertTrue(packets[0]['columns'])
                self.assertLess(len(run.stdout) / (10 if not fault else 14), 300_000)

    def test_fragmented_corrupted_and_oversized_usb_lines(self):
        payload = b'{"seq":1,"samples":[]}'
        frame = payload + b'\t%04x\n' % binascii.crc_hqx(payload, 0xffff)
        self.assertEqual(decode_usb_line(frame)['seq'], 1)
        for corrupted in (frame.replace(b'1', b'2', 1), frame[:-6] + b'FFFF\n', payload):
            with self.assertRaises(ValueError): decode_usb_line(corrupted)
        stop = threading.Event()
        class Input:
            def __init__(self): self.data = bytearray(b'# boot\n' + b'x'*70_000 + b'\n' + frame)
            @property
            def in_waiting(self): return min(7, len(self.data))
            def read(self, count):
                result = bytes(self.data[:count]); del self.data[:count]
                if not self.data: stop.set()
                return result
        received = list(usb_lines(Input(), stop))
        self.assertEqual(received, [b'# boot\n', frame])

    def test_pc_serial_loop_retries_failed_db_batch_and_reuses_schema(self):
        def frame(data):
            payload = json.dumps(data, separators=(',', ':')).encode()
            return payload + b'\t%04x\n' % binascii.crc_hqx(payload, 0xffff)
        first = {'stream_id': '0123456789abcdef', 'sent_ms': 20,
                 'columns': ['seq', 'timestamp_ms'], 'samples': [[1,10],[2,20]]}
        next_batch = {'stream_id': first['stream_id'], 'sent_ms': 30, 'samples': [[3,30]]}
        with tempfile.TemporaryDirectory() as folder, patch('ev_gateway.socket.socket', return_value=Mock()):
            gateway = EVGateway('127.0.0.1', 0, '127.0.0.1', 19004, database_path=Path(folder)/'usb.db')
            db = gateway.database
            db.start()
            db._connection.execute("CREATE TRIGGER fail_batch BEFORE UPDATE ON measurement_sessions BEGIN SELECT RAISE(ABORT, 'disk write failure'); END")
            writes = []
            class Serial:
                def __init__(self, **settings):
                    self.frames = [bytearray(frame(first).replace(b'20',b'21',1)),
                                   bytearray(frame(first)), bytearray(frame(first)), bytearray(frame(next_batch))]
                    self.index = 0
                    self.settings = settings
                def open(self):
                    assert self.settings['baudrate'] == 3000000 and not self.dtr and not self.rts
                def __enter__(self): return self
                def __exit__(self, *args): pass
                @property
                def in_waiting(self): return 7
                def read(self, count):
                    if self.index == len(self.frames):
                        gateway.stop_event.set(); return b''
                    if not self.frames[self.index]:
                        self.index += 1
                        if self.index == 2:
                            assert db.status()['recording_error']
                            assert not gateway.tuning.read()['samples']
                            assert len(writes) == 1 # Only usb_start; neither bad batch was ACKed.
                            db._connection.execute('DROP TRIGGER fail_batch')
                        return self.read(count)
                    data = self.frames[self.index]
                    chunk = bytes(data[:count]); del data[:count]; return chunk
                def write(self, data): writes.append(json.loads(data)); return len(data)
            try:
                with patch.dict(sys.modules, {'serial': SimpleNamespace(Serial=Serial, SerialException=OSError)}):
                    gateway.serial_loop('TEST', 3000000)
                self.assertEqual(writes[0], {'type': 'usb_start'})
                self.assertEqual([w['ack_seq'] for w in writes[1:]], [2,3])
                self.assertEqual(db.status()['recording_sample_count'], 3)
                self.assertEqual(len(gateway.tuning.read()['samples']), 3)
                self.assertEqual(len(gateway.battery.read()['samples']), 3)
                self.assertIsNone(gateway.serial_stream)
            finally:
                db.close()


if __name__ == '__main__': unittest.main()
