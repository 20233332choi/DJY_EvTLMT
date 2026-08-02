#pragma once
#include "ACTelemetry.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>

class TelemetrySender {
private:
    SOCKET udpSocket;
    sockaddr_in serverAddr;
    bool isConnected;
    int lastPacketId;
    
    std::string WstringToUtf8(const std::wstring& str);

public:
    TelemetrySender();
    ~TelemetrySender();

    bool Init(const char* ip = "127.0.0.1", int port = 8001);
    void SendData(SPageFilePhysics* physics, SPageFileGraphics* graphics, SPageFileStatic* statics);
    void Cleanup();
};
