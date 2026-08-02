#pragma once
#include "ACTelemetry.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include <string>
#include <cmath>
#include <cstring>
#include <wchar.h>
#include <thread>
#include <mutex>

#include "json.hpp"
using json = nlohmann::json;

class IDataSource {
public:
    virtual ~IDataSource() = default;
    virtual void Update(float deltaTime) = 0;
    virtual bool IsConnected() const = 0;
    
    virtual SPageFilePhysics* GetPhysics() = 0;
    virtual SPageFileGraphics* GetGraphics() = 0;
    virtual SPageFileStatic* GetStatic() = 0;
    
    virtual const char* GetSourceName() const = 0;
};

class LocalMemorySource : public IDataSource {
private:
    void* pPhys = nullptr;
    void* pGraph = nullptr;
    void* pStat = nullptr;
    bool connected = false;
    int lastPacketId = -1;
    ULONGLONG lastPacketTick = 0;

    void* InitSharedMemory(const char* name) {
        HANDLE hMapFile = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
        if (hMapFile == NULL) return nullptr;
        return MapViewOfFile(hMapFile, FILE_MAP_READ, 0, 0, 0);
    }

public:
    LocalMemorySource() {}
    ~LocalMemorySource() override {
        if (pPhys) UnmapViewOfFile(pPhys);
        if (pGraph) UnmapViewOfFile(pGraph);
        if (pStat) UnmapViewOfFile(pStat);
    }

    void Update(float /*deltaTime*/) override {
        if (!connected) {
            pPhys = InitSharedMemory("Local\\acpmf_physics");
            pGraph = InitSharedMemory("Local\\acpmf_graphics");
            pStat = InitSharedMemory("Local\\acpmf_static");
            if (pPhys && pGraph && pStat) {
                connected = true;
            }
        }
        
        // Status check to handle disconnections
        if (connected && pGraph) {
            auto* g = static_cast<SPageFileGraphics*>(pGraph);
            if (g->packetId != lastPacketId) {
                lastPacketId = g->packetId;
                lastPacketTick = GetTickCount64();
            }
        }
    }

    bool IsConnected() const override {
        if (!connected || !pGraph) return false;
        const auto* graphics = static_cast<const SPageFileGraphics*>(pGraph);
        const bool packetIsFresh = lastPacketTick != 0 && (GetTickCount64() - lastPacketTick) < 1500;
        return graphics->status == AC_LIVE && packetIsFresh;
    }
    
    SPageFilePhysics* GetPhysics() override { return static_cast<SPageFilePhysics*>(pPhys); }
    SPageFileGraphics* GetGraphics() override { return static_cast<SPageFileGraphics*>(pGraph); }
    SPageFileStatic* GetStatic() override { return static_cast<SPageFileStatic*>(pStat); }
    
    const char* GetSourceName() const override { return "VIRTUAL SIGNAL: ASSETTO CORSA LIVE"; }
};

class DemoSource : public IDataSource {
private:
    SPageFilePhysics physics = {};
    SPageFileGraphics graphics = {};
    SPageFileStatic statics = {};
    float demoTime = 0.0f;

public:
    DemoSource() {
        physics.gear = 4;
        graphics.status = AC_LIVE;
        graphics.currentSectorIndex = 0;
        graphics.lastSectorTime = 0;
        wcscpy_s(graphics.bestTime, L"1:23.456");
    }

    void Update(float deltaTime) override {
        demoTime += deltaTime;
        float cycle = (sinf(demoTime * 2.0f) * 0.5f + 0.5f);
        
        physics.rpms = 1000 + static_cast<int>(cycle * 7000);
        physics.speedKmh = 100 + cycle * 80;
        physics.tyreContactPoint[0][0] = sinf(demoTime) * 500.0f;
        physics.tyreContactPoint[0][2] = cosf(demoTime) * 500.0f;
        
        for(int i=0; i<4; i++) {
            physics.tyreCoreTemperature[i] = 80.0f + (cycle * 20.0f);
            physics.wheelsPressure[i] = 22.0f + (cycle * 5.0f);
            physics.brakeTemp[i] = 400.0f + (cycle * 200.0f);
        }
    }

    bool IsConnected() const override { return true; }
    
    SPageFilePhysics* GetPhysics() override { return &physics; }
    SPageFileGraphics* GetGraphics() override { return &graphics; }
    SPageFileStatic* GetStatic() override { return &statics; }
    
    const char* GetSourceName() const override { return "VIRTUAL SIGNAL: BUILT-IN DEMO"; }
};

class NetworkDataSource : public IDataSource {
private:
    SPageFilePhysics physics = {};
    SPageFileGraphics graphics = {};
    SPageFileStatic statics = {};
    
    SOCKET udpSocket = INVALID_SOCKET;
    std::thread listenThread;
    std::mutex dataMutex;
    bool running = false;
    bool connected = false;

    void ListenLoop() {
        char buffer[4096];
        while (running) {
            sockaddr_in senderAddr;
            int senderAddrSize = sizeof(senderAddr);
            int bytesReceived = recvfrom(udpSocket, buffer, sizeof(buffer) - 1, 0, (SOCKADDR*)&senderAddr, &senderAddrSize);
            
            if (bytesReceived > 0) {
                buffer[bytesReceived] = '\0';
                try {
                    auto j = json::parse(buffer);
                    std::lock_guard<std::mutex> lock(dataMutex);
                    
                    connected = true;
                    graphics.status = AC_LIVE;
                    
                    if (j.contains("speed")) physics.speedKmh = j["speed"];
                    if (j.contains("rpm")) physics.rpms = j["rpm"];
                    if (j.contains("gear")) physics.gear = j["gear"] + 1; // Sender subtracted 1
                    if (j.contains("gas")) physics.gas = j["gas"] / 100.0f;
                    if (j.contains("brake")) physics.brake = j["brake"] / 100.0f;
                    if (j.contains("steer")) physics.steerAngle = j["steer"];
                    if (j.contains("fuel")) physics.fuel = j["fuel"];
                    
                    if (j.contains("lap")) graphics.completedLaps = j["lap"];
                    if (j.contains("sector")) graphics.currentSectorIndex = j["sector"];
                    if (j.contains("lastSectorTime")) graphics.lastSectorTime = j["lastSectorTime"];
                    if (j.contains("progress")) graphics.normalizedCarPosition = j["progress"] / 100.0f;
                    
                    if (j.contains("tyrePressures")) {
                        physics.wheelsPressure[0] = j["tyrePressures"]["FL"];
                        physics.wheelsPressure[1] = j["tyrePressures"]["FR"];
                        physics.wheelsPressure[2] = j["tyrePressures"]["RL"];
                        physics.wheelsPressure[3] = j["tyrePressures"]["RR"];
                    }
                    if (j.contains("tyreTemps")) {
                        physics.tyreCoreTemperature[0] = j["tyreTemps"]["FL"];
                        physics.tyreCoreTemperature[1] = j["tyreTemps"]["FR"];
                        physics.tyreCoreTemperature[2] = j["tyreTemps"]["RL"];
                        physics.tyreCoreTemperature[3] = j["tyreTemps"]["RR"];
                    }
                    if (j.contains("suspension")) {
                        physics.suspensionTravel[0] = j["suspension"]["FL"];
                        physics.suspensionTravel[1] = j["suspension"]["FR"];
                        physics.suspensionTravel[2] = j["suspension"]["RL"];
                        physics.suspensionTravel[3] = j["suspension"]["RR"];
                    }
                } catch (...) {
                    // Ignore parsing errors
                }
            }
        }
    }

public:
    NetworkDataSource() {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
        
        udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(8002);
        serverAddr.sin_addr.s_addr = INADDR_ANY;
        
        bind(udpSocket, (SOCKADDR*)&serverAddr, sizeof(serverAddr));
        
        // Set non-blocking timeout just in case
        DWORD timeout = 100;
        setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        
        running = true;
        listenThread = std::thread(&NetworkDataSource::ListenLoop, this);
    }
    
    ~NetworkDataSource() override {
        running = false;
        if (udpSocket != INVALID_SOCKET) closesocket(udpSocket);
        if (listenThread.joinable()) listenThread.join();
        WSACleanup();
    }

    void Update(float /*deltaTime*/) override {
        // Data is updated asynchronously by the ListenLoop thread
    }

    bool IsConnected() const override { return connected; }
    
    SPageFilePhysics* GetPhysics() override { return &physics; }
    SPageFileGraphics* GetGraphics() override { return &graphics; }
    SPageFileStatic* GetStatic() override { return &statics; }
    
    const char* GetSourceName() const override { return "Mode: NETWORK (Pit Wall)"; }
};

