#pragma once
#include "imgui.h"
#include "DataSource.h"
#include "EnergyLog.h"
#include "VirtualLogRecorder.h"
#include <deque>

// --- Constants & Themes ---
const ImVec4 COLOR_FERRARI_RED = ImVec4(0.89f, 0.15f, 0.21f, 1.0f);
const ImVec4 COLOR_F1_YELLOW   = ImVec4(1.00f, 0.95f, 0.00f, 1.0f);
const ImVec4 COLOR_F1_CYAN     = ImVec4(0.00f, 0.82f, 0.75f, 1.0f);
const ImVec4 COLOR_F1_GREEN    = ImVec4(0.00f, 1.00f, 0.00f, 1.0f);
const ImVec4 COLOR_F1_PURPLE   = ImVec4(0.72f, 0.00f, 0.72f, 1.0f);

struct SectorData {
    float lastTime[3] = {0, 0, 0};
    float personalBest[3] = {999, 999, 999};
    float sessionBest[3] = {999, 999, 999};
    ImVec4 colors[3] = {ImVec4(0.5f, 0.5f, 0.5f, 1), ImVec4(0.5f, 0.5f, 0.5f, 1), ImVec4(0.5f, 0.5f, 0.5f, 1)};
};

class DashboardUI {
private:
    bool showTyreWin = true;
    bool showMapWin = false;
    bool showTimingWin = true;
    bool showDriverInputWin = true;
    bool showVirtualEnergyWin = true;
    bool showActualEnergyWin = true;
    bool showRecordedLogWin = false;
    SectorData sectorTiming;
    std::deque<ImVec2> trackTrail;
    EnergyLog recordedLog;
    int selectedLogSample = 0;
    VirtualLogRecorder* virtualRecorder = nullptr;

    void UpdateSectorTiming(int sectorIndex, int lastSectorTime);
    ImVec4 GetTempColor(float temp);
    
    void ApplyF1Theme();

public:
    bool enableWebBroadcast = false;

    DashboardUI();
    ~DashboardUI() = default;

    void Render(IDataSource* dataSource, bool& outDemoMode);
    
    void RenderCommander(IDataSource* dataSource, bool& outDemoMode);
    void RenderTyreMonitor(SPageFilePhysics* physics);
    void RenderTiming(SPageFileGraphics* graphics);
    void RenderTrackMap(SPageFilePhysics* physics);
    void RenderEnergyMeter(SPageFilePhysics* physics, const char* sourceName);
    void RenderActualEnergyMeter();
    void RenderDriverInputs(SPageFilePhysics* physics, bool isVirtual);
    void RenderRecordedLog();
    void SetVirtualRecorder(VirtualLogRecorder* recorder) { virtualRecorder = recorder; }
};
