#include "TelemetrySender.h"
#include <sstream>
#include <iomanip>
#include <iostream>

#pragma comment(lib, "ws2_32.lib")

TelemetrySender::TelemetrySender() : udpSocket(INVALID_SOCKET), isConnected(false), lastPacketId(-1) {
}

TelemetrySender::~TelemetrySender() {
    Cleanup();
}

bool TelemetrySender::Init(const char* ip, int port) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return false;
    
    udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSocket == INVALID_SOCKET) {
        WSACleanup();
        return false;
    }
    
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.s_addr = inet_addr(ip);
    
    isConnected = true;
    return true;
}

std::string TelemetrySender::WstringToUtf8(const std::wstring& str) {
    if (str.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &str[0], (int)str.size(), &strTo[0], size_needed, NULL, NULL);
    strTo.erase(std::find(strTo.begin(), strTo.end(), '\0'), strTo.end());
    return strTo;
}

void TelemetrySender::SendData(SPageFilePhysics* physics, SPageFileGraphics* graphics, SPageFileStatic* statics) {
    if (!isConnected || !physics || !graphics || !statics) return;

    if (graphics->status != AC_LIVE) return;
    
    if (physics->packetId != lastPacketId) {
        lastPacketId = physics->packetId;

        std::string carModel = WstringToUtf8(statics->carModel);
        std::string tb = WstringToUtf8(statics->track);
        std::string tc = WstringToUtf8(statics->trackConfiguration);
        std::string trackName = tc.empty() ? tb : tb + "-" + tc;
        std::string playerName = WstringToUtf8(statics->playerName);
        std::string tyreCompound = WstringToUtf8(graphics->tyreCompound);

        float car_x = physics->tyreContactPoint[0][0];
        float car_z = physics->tyreContactPoint[0][2];

        std::stringstream ss;
        ss << std::fixed << std::setprecision(4);
        ss << "{";
        ss << "\"carModel\":\"" << carModel << "\",";
        ss << "\"trackName\":\"" << trackName << "\",";
        ss << "\"playerName\":\"" << playerName << "\",";
        ss << "\"setupName\":\"Default\",";
        ss << "\"tyreCompound\":\"" << tyreCompound << "\",";
        ss << "\"lap\":" << graphics->completedLaps << ",";
        ss << "\"sector\":" << graphics->currentSectorIndex << ",";
        ss << "\"lastSectorTime\":" << graphics->lastSectorTime << ",";
        ss << "\"currentTime\":" << graphics->iCurrentTime << ",";
        ss << "\"progress\":" << (graphics->normalizedCarPosition * 100.0f) << ",";
        ss << "\"position\":" << graphics->position << ",";
        ss << "\"speed\":" << physics->speedKmh << ",";
        ss << "\"rpm\":" << physics->rpms << ",";
        ss << "\"gear\":" << physics->gear - 1 << ",";
        ss << "\"gas\":" << physics->gas * 100.0f << ",";
        ss << "\"brake\":" << physics->brake * 100.0f << ",";
        ss << "\"steer\":" << physics->steerAngle << ",";
        ss << "\"coords\":[" << car_x << "," << car_z << "],";
        
        ss << "\"tyrePressures\":{\"FL\":" << physics->wheelsPressure[0] << ",\"FR\":" << physics->wheelsPressure[1] << ",\"RL\":" << physics->wheelsPressure[2] << ",\"RR\":" << physics->wheelsPressure[3] << "},";
        ss << "\"tyreTemps\":{\"FL\":" << physics->tyreCoreTemperature[0] << ",\"FR\":" << physics->tyreCoreTemperature[1] << ",\"RL\":" << physics->tyreCoreTemperature[2] << ",\"RR\":" << physics->tyreCoreTemperature[3] << "},";
        ss << "\"brakeTemps\":{\"FL\":" << physics->brakeTemp[0] << ",\"FR\":" << physics->brakeTemp[1] << ",\"RL\":" << physics->brakeTemp[2] << ",\"RR\":" << physics->brakeTemp[3] << "},";
        ss << "\"suspension\":{\"FL\":" << physics->suspensionTravel[0] << ",\"FR\":" << physics->suspensionTravel[1] << ",\"RL\":" << physics->suspensionTravel[2] << ",\"RR\":" << physics->suspensionTravel[3] << "},";
        ss << "\"weather\":{\"airTemp\":" << physics->airTemp << ",\"roadTemp\":" << physics->roadTemp << ",\"surfaceGrip\":" << (graphics->surfaceGrip > 0.01f ? graphics->surfaceGrip * 100.0f : 100.0f) << ",\"windSpeed\":" << graphics->windSpeed << "},";
        ss << "\"fuel\":" << physics->fuel << ",";
        ss << "\"rideHeight\":{\"F\":" << physics->rideHeight[0] << ",\"R\":" << physics->rideHeight[1] << "},";
        ss << "\"brakeBias\":" << physics->brakeBias << ",";
        ss << "\"accG\":[" << physics->accG[0] << "," << physics->accG[1] << "," << physics->accG[2] << "],";
        ss << "\"wheelSlip\":[" << physics->wheelSlip[0] << "," << physics->wheelSlip[1] << "," << physics->wheelSlip[2] << "," << physics->wheelSlip[3] << "],";
        ss << "\"tyreWear\":[" << physics->tyreWear[0] << "," << physics->tyreWear[1] << "," << physics->tyreWear[2] << "," << physics->tyreWear[3] << "],";
        ss << "\"carDamage\":[" << physics->carDamage[0] << "," << physics->carDamage[1] << "," << physics->carDamage[2] << "," << physics->carDamage[3] << "," << physics->carDamage[4] << "],";
        ss << "\"suspensionDamage\":[" << physics->suspensionDamage[0] << "," << physics->suspensionDamage[1] << "," << physics->suspensionDamage[2] << "," << physics->suspensionDamage[3] << "],";
        ss << "\"wheelAngularSpeed\":[" << physics->wheelAngularSpeed[0] << "," << physics->wheelAngularSpeed[1] << "," << physics->wheelAngularSpeed[2] << "," << physics->wheelAngularSpeed[3] << "],";
        
        ss << "\"session\":" << graphics->session << ",";
        ss << "\"numberOfLaps\":" << graphics->numberOfLaps << ",";
        ss << "\"bestTime\":" << graphics->iBestTime << ",";
        ss << "\"lastTime\":" << graphics->iLastTime << ",";
        ss << "\"delta\":" << physics->performanceMeter;
        ss << "}";

        std::string jsonStr = ss.str();
        sendto(udpSocket, jsonStr.c_str(), jsonStr.length(), 0, (SOCKADDR*)&serverAddr, sizeof(serverAddr));
    }
}

void TelemetrySender::Cleanup() {
    if (isConnected) {
        closesocket(udpSocket);
        WSACleanup();
        isConnected = false;
    }
}
