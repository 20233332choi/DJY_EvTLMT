#include "VirtualLogRecorder.h"
#include "ACTelemetry.h"
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace {
constexpr uint8_t MAGIC = 0xAA;
constexpr uint8_t TYPE_HEADER = 0;
constexpr uint8_t TYPE_RECORD = 1;
constexpr uint32_t SYNTHETIC_UID[3] = {0x45564552, 0x474D5431, 0x46454445};

template <size_t N> void PutU16(std::array<uint8_t, N>& b, size_t o, uint16_t v) { b[o]=uint8_t(v); b[o+1]=uint8_t(v>>8); }
template <size_t N> void PutU32(std::array<uint8_t, N>& b, size_t o, uint32_t v) { PutU16(b,o,uint16_t(v)); PutU16(b,o+2,uint16_t(v>>16)); }
template <size_t N> void FinishChecksum(std::array<uint8_t, N>& b) { uint16_t c=0; for(size_t i=0;i<N;i+=2)c^=uint16_t(b[i]|(b[i+1]<<8)); PutU16(b,2,c); }
int16_t Quantize(float value, float scale) { const float q=std::round(value*scale); return static_cast<int16_t>(std::clamp(q,-32768.0f,32767.0f)); }

void* MapAcPage(const wchar_t* name, size_t size) {
    HANDLE handle=OpenFileMappingW(FILE_MAP_READ,FALSE,name); if(!handle)return nullptr;
    void* view=MapViewOfFile(handle,FILE_MAP_READ,0,0,size); CloseHandle(handle); return view;
}

std::filesystem::path DummyDirectory() {
    wchar_t module[MAX_PATH]{}; GetModuleFileNameW(nullptr,module,MAX_PATH);
    return std::filesystem::path(module).parent_path().parent_path()/L"data"/L"dummy";
}

std::array<uint8_t,32> MakeHeader() {
    std::array<uint8_t,32> b{}; b[0]=MAGIC; b[1]=TYPE_HEADER; PutU32(b,4,550);
    for(int i=0;i<3;++i)PutU32(b,8+i*4,SYNTHETIC_UID[i]);
    SYSTEMTIME t{}; GetLocalTime(&t); b[24]=uint8_t(t.wYear-2000); b[25]=uint8_t(t.wMonth); b[26]=uint8_t(t.wDay);
    b[27]=uint8_t(t.wHour); b[28]=uint8_t(t.wMinute); b[29]=uint8_t(t.wSecond); PutU16(b,30,t.wMilliseconds); FinishChecksum(b); return b;
}

std::array<uint8_t,16> MakeRecord(const SPageFilePhysics& p,uint32_t timestampMs,float elapsedSeconds) {
    const float gas=std::clamp(std::isfinite(p.gas)?p.gas:0.0f,0.0f,1.0f);
    const float brake=std::clamp(std::isfinite(p.brake)?p.brake:0.0f,0.0f,1.0f);
    const float speed=std::max(std::isfinite(p.speedKmh)?p.speedKmh:0.0f,0.0f);
    const float speedFactor=std::clamp(speed/90.0f,0.0f,1.0f);
    const float powerKW=350.0f*gas-350.0f*brake*speedFactor+2.2f+0.006f*speed;
    const float voltage=std::clamp(600.0f-(powerKW>0?powerKW*0.095f:powerKW*0.035f),545.0f,615.0f);
    const float current=std::clamp(powerKW*1000.0f/voltage,-749.0f,749.0f);
    const float lv=13.72f-0.12f*gas;
    const float temp=31.0f+0.035f*elapsedSeconds+2.4f*gas+0.8f*brake;
    std::array<uint8_t,16>b{}; b[0]=MAGIC;b[1]=TYPE_RECORD;PutU32(b,4,timestampMs);
    PutU16(b,8,uint16_t(Quantize(voltage,10)));PutU16(b,10,uint16_t(Quantize(current,10)));
    PutU16(b,12,uint16_t(Quantize(lv,100)));PutU16(b,14,uint16_t(Quantize(temp,100)));FinishChecksum(b);return b;
}
}

VirtualLogRecorder::~VirtualLogRecorder(){Stop();}
void VirtualLogRecorder::Start(){if(running.exchange(true))return;worker=std::thread(&VirtualLogRecorder::Worker,this);}
void VirtualLogRecorder::Stop(){running=false;if(worker.joinable())worker.join();}
std::wstring VirtualLogRecorder::CurrentFilePath()const{std::lock_guard<std::mutex>l(stateMutex);return currentFilePath;}
std::string VirtualLogRecorder::LastError()const{std::lock_guard<std::mutex>l(stateMutex);return lastError;}
void VirtualLogRecorder::SetError(const std::string&m){std::lock_guard<std::mutex>l(stateMutex);lastError=m;}

void VirtualLogRecorder::Worker(){
    using clock=std::chrono::steady_clock; void* physView=nullptr;void* graphView=nullptr;std::ofstream file;
    int lastPacket=-1;auto lastFresh=clock::time_point{};auto inactiveSince=clock::time_point{};auto sessionStart=clock::now();auto nextSample=clock::now();int flushCounter=0;
    while(running){
        if(!physView)physView=MapAcPage(L"Local\\acpmf_physics",sizeof(SPageFilePhysics));
        if(!graphView)graphView=MapAcPage(L"Local\\acpmf_graphics",sizeof(SPageFileGraphics));
        const auto now=clock::now();
        if(!physView||!graphView){std::this_thread::sleep_for(std::chrono::milliseconds(250));continue;}
        auto* phys=static_cast<SPageFilePhysics*>(physView);auto* graph=static_cast<SPageFileGraphics*>(graphView);
        if(phys->packetId!=lastPacket){lastPacket=phys->packetId;lastFresh=now;}
        const bool fresh=lastFresh!=clock::time_point{}&&now-lastFresh<std::chrono::milliseconds(1500);
        const bool live=graph->status==AC_LIVE&&fresh;
        if(live&&!file.is_open()){
            try{const auto dir=DummyDirectory();std::filesystem::create_directories(dir);SYSTEMTIME t{};GetLocalTime(&t);wchar_t name[96]{};
                swprintf_s(name,L"VIRTUAL_AC_%04u%02u%02u_%02u%02u%02u.log",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
                const auto path=dir/name;file.open(path,std::ios::binary|std::ios::trunc);if(!file){SetError("Cannot create dummy log file.");}
                else{const auto h=MakeHeader();file.write(reinterpret_cast<const char*>(h.data()),h.size());file.flush();sessionStart=now;nextSample=now;flushCounter=0;recordCount=0;recording=true;inactiveSince={};std::lock_guard<std::mutex>l(stateMutex);currentFilePath=path.wstring();lastError.clear();}}
            catch(const std::exception&e){SetError(e.what());}
        }
        if(live&&file.is_open()&&now>=nextSample){
            const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(now-sessionStart);const auto packet=MakeRecord(*phys,uint32_t(elapsed.count()+550),elapsed.count()/1000.0f);
            file.write(reinterpret_cast<const char*>(packet.data()),packet.size());++recordCount;if(++flushCounter>=10){file.flush();flushCounter=0;}nextSample+=std::chrono::milliseconds(10);if(nextSample<now-std::chrono::milliseconds(10))nextSample=now+std::chrono::milliseconds(10);
        }
        if(!live&&file.is_open()){
            if(inactiveSince==clock::time_point{})inactiveSince=now;
            if(now-inactiveSince>=std::chrono::seconds(3)){file.flush();file.close();recording=false;inactiveSince={};UnmapViewOfFile(physView);UnmapViewOfFile(graphView);physView=graphView=nullptr;lastPacket=-1;lastFresh={};}
        }else if(!live&&!file.is_open()){
            if(inactiveSince==clock::time_point{})inactiveSince=now;
            if(now-inactiveSince>=std::chrono::seconds(3)){UnmapViewOfFile(physView);UnmapViewOfFile(graphView);physView=graphView=nullptr;lastPacket=-1;lastFresh={};inactiveSince={};}
        }else if(live)inactiveSince={};
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if(file.is_open()){file.flush();file.close();}recording=false;if(physView)UnmapViewOfFile(physView);if(graphView)UnmapViewOfFile(graphView);
}
