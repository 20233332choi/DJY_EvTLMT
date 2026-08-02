#include "DashboardUI.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cfloat>
#include <string>
#include <windows.h>
#include <commdlg.h>

static std::string WideFieldToUtf8(const wchar_t* value, size_t capacity) {
    size_t length = 0;
    while (length < capacity && value[length] != L'\0') ++length;
    if (length == 0) return "--:--.---";
    int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value,
                                    static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return "INVALID TIME";
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value,
                        static_cast<int>(length), &result[0], bytes, nullptr, nullptr);
    return result;
}

DashboardUI::DashboardUI() {
    ApplyF1Theme();
}

void DashboardUI::ApplyF1Theme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 6.0f;
    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(9.0f, 6.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                   = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_TextDisabled]           = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_WindowBg]               = ImVec4(0.06f, 0.06f, 0.06f, 0.94f);
    colors[ImGuiCol_ChildBg]                = ImVec4(0.10f, 0.10f, 0.10f, 0.94f);
    colors[ImGuiCol_PopupBg]                = ImVec4(0.08f, 0.08f, 0.08f, 0.94f);
    colors[ImGuiCol_Border]                 = ImVec4(0.20f, 0.20f, 0.20f, 0.50f);
    colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]                = ImVec4(0.16f, 0.16f, 0.16f, 0.54f);
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.26f, 0.26f, 0.26f, 0.40f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.36f, 0.36f, 0.36f, 0.67f);
    colors[ImGuiCol_TitleBg]                = ImVec4(0.04f, 0.04f, 0.04f, 1.00f);
    colors[ImGuiCol_TitleBgActive]          = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);
    colors[ImGuiCol_MenuBarBg]              = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
    colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.51f, 0.51f, 0.51f, 1.00f);
    colors[ImGuiCol_CheckMark]              = COLOR_FERRARI_RED;
    colors[ImGuiCol_SliderGrab]             = COLOR_FERRARI_RED;
    colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.98f, 0.26f, 0.26f, 1.00f);
    colors[ImGuiCol_Button]                 = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_ButtonHovered]          = COLOR_FERRARI_RED;
    colors[ImGuiCol_ButtonActive]           = ImVec4(0.98f, 0.26f, 0.26f, 1.00f);
    colors[ImGuiCol_Header]                 = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_HeaderHovered]          = COLOR_FERRARI_RED;
    colors[ImGuiCol_HeaderActive]           = ImVec4(0.98f, 0.26f, 0.26f, 1.00f);
    colors[ImGuiCol_Separator]              = ImVec4(0.30f, 0.30f, 0.30f, 0.50f);
    colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.10f, 0.40f, 0.75f, 0.78f);
    colors[ImGuiCol_SeparatorActive]        = ImVec4(0.10f, 0.40f, 0.75f, 1.00f);
}

ImVec4 DashboardUI::GetTempColor(float temp) {
    if (temp < 70.0f) return COLOR_F1_CYAN;
    if (temp < 95.0f) return COLOR_F1_GREEN;
    if (temp < 110.0f) return COLOR_F1_YELLOW;
    return COLOR_FERRARI_RED;
}

void DashboardUI::UpdateSectorTiming(int sectorIndex, int lastSectorTime) {
    if (lastSectorTime <= 0 || sectorIndex < 0 || sectorIndex > 2) return;
    float timeS = (float)lastSectorTime / 1000.0f;
    
    // lastSectorTime은 방금 통과한 "이전" 섹터의 기록입니다.
    // 현재 섹터가 1(S2)이라면, 방금 통과한 섹터는 0(S1)입니다.
    int prevSector = (sectorIndex == 0) ? 2 : (sectorIndex - 1);

    if (timeS == sectorTiming.lastTime[prevSector]) return;
    
    sectorTiming.lastTime[prevSector] = timeS;
    if (timeS < sectorTiming.sessionBest[prevSector]) { 
        sectorTiming.sessionBest[prevSector] = timeS; 
        sectorTiming.personalBest[prevSector] = timeS; 
        sectorTiming.colors[prevSector] = COLOR_F1_PURPLE; 
    }
    else if (timeS < sectorTiming.personalBest[prevSector]) { 
        sectorTiming.personalBest[prevSector] = timeS; 
        sectorTiming.colors[prevSector] = COLOR_F1_GREEN; 
    }
    else {
        sectorTiming.colors[prevSector] = COLOR_F1_YELLOW;
    }
}

void DashboardUI::Render(IDataSource* dataSource, bool& outDemoMode) {
    if (!dataSource) return;

    SPageFilePhysics* pPhys = dataSource->GetPhysics();
    SPageFileGraphics* pGraph = dataSource->GetGraphics();

    if (pGraph) {
        UpdateSectorTiming(pGraph->currentSectorIndex, pGraph->lastSectorTime);
    }

    RenderCommander(dataSource, outDemoMode);
    if (showActualEnergyWin) RenderActualEnergyMeter();
    if (showRecordedLogWin) RenderRecordedLog();

    if (dataSource->IsConnected() && pPhys && pGraph) {
        if (showDriverInputWin) RenderDriverInputs(pPhys, true);
        if (showVirtualEnergyWin) RenderEnergyMeter(pPhys, dataSource->GetSourceName());
        if (showTyreWin) RenderTyreMonitor(pPhys);
        if (showTimingWin) RenderTiming(pGraph);
        if (showMapWin) RenderTrackMap(pPhys);
    }
}

void DashboardUI::RenderEnergyMeter(SPageFilePhysics* p, const char* sourceName) {
    static double netEnergyKWh = 0.0;
    static float previousPowerKW = 0.0f;

    // Assetto Corsa does not expose real HV pack voltage/current. These values
    // deliberately use the same estimator as EV_ErgMt and are always labelled virtual.
    float throttle = p->gas < 0.0f ? 0.0f : (p->gas > 1.0f ? 1.0f : p->gas);
    float brake = p->brake < 0.0f ? 0.0f : (p->brake > 1.0f ? 1.0f : p->brake);
    float speedFactor = p->speedKmh / 90.0f;
    if (speedFactor < 0.0f) speedFactor = 0.0f;
    if (speedFactor > 1.0f) speedFactor = 1.0f;
    float powerKW = 350.0f * throttle - 350.0f * brake * speedFactor + 2.2f + 0.006f * p->speedKmh;
    float voltage = 600.0f - (powerKW > 0.0f ? powerKW * 0.095f : powerKW * 0.035f);
    if (voltage < 545.0f) voltage = 545.0f;
    if (voltage > 615.0f) voltage = 615.0f;
    float current = powerKW * 1000.0f / voltage;
    if (current < -749.0f) current = -749.0f;
    if (current > 749.0f) current = 749.0f;
    float lv = 13.72f - 0.12f * throttle;
    float temp = 31.0f + 2.4f * throttle + 0.8f * brake;
    float dt = ImGui::GetIO().DeltaTime;
    if (dt > 0.0f && dt < 0.2f) netEnergyKWh += (previousPowerKW + powerKW) * 0.5 * dt / 3600.0;
    previousPowerKW = powerKW;

    ImGui::SetNextWindowPos(ImVec2(840, 20), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(410, 330), ImGuiCond_Once);
    ImGui::Begin("VIRTUAL SIGNALS · ASSETTO CORSA", &showVirtualEnergyWin, ImGuiWindowFlags_NoCollapse);
    ImGui::TextColored(COLOR_F1_YELLOW, "VIRTUAL DATA ONLY · NOT A CIRCUIT MEASUREMENT");
    ImGui::TextDisabled("%s", sourceName);
    ImGui::Separator();
    if (ImGui::BeginTable("EnergyValues", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("HV BUS VOLTAGE [VIRTUAL]"); ImGui::TableNextColumn(); ImGui::TextColored(COLOR_F1_CYAN, "%.1f V", voltage);
        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("HV BUS CURRENT [VIRTUAL]"); ImGui::TableNextColumn(); ImGui::TextColored(COLOR_F1_GREEN, "%.1f A", current);
        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("DC POWER [VIRTUAL]"); ImGui::TableNextColumn(); ImGui::TextColored(powerKW >= 0 ? COLOR_F1_YELLOW : COLOR_F1_CYAN, "%.1f kW", powerKW);
        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("NET ENERGY [VIRTUAL]"); ImGui::TableNextColumn(); ImGui::Text("%.4f kWh", netEnergyKWh);
        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("LV SUPPLY [VIRTUAL]"); ImGui::TableNextColumn(); ImGui::Text("%.2f V", lv);
        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("MCU TEMP [VIRTUAL]"); ImGui::TableNextColumn(); ImGui::Text("%.2f C", temp);
        ImGui::EndTable();
    }
    ImGui::Separator();
    ImGui::TextDisabled("HV+/HV- -> isolated AFE -> ADC HV");
    ImGui::TextDisabled("M6 busbar -> Hall sensor -> ADC CURRENT");
    ImGui::TextDisabled("STM32F401 -> 16x average -> 100 Hz -> SD log");
    ImGui::End();
}

void DashboardUI::RenderActualEnergyMeter() {
    ImGui::SetNextWindowPos(ImVec2(840, 365), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(410, 285), ImGuiCond_Once);
    ImGui::Begin("REAL HARDWARE SIGNALS · ENERGY METER", &showActualEnergyWin, ImGuiWindowFlags_NoCollapse);
    ImGui::TextColored(COLOR_FERRARI_RED, "OFFLINE · NO HARDWARE DATA");
    ImGui::TextDisabled("Waiting for isolated CAN/UART receiver");
    ImGui::Separator();
    if (ImGui::BeginTable("RealEnergyValues", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
        const char* labels[] = {"HV BUS VOLTAGE", "HV BUS CURRENT", "DC POWER", "NET ENERGY", "LV SUPPLY", "MCU TEMPERATURE"};
        const char* routes[] = {"HV+/HV- ADC", "HALL CURRENT ADC", "CALCULATED", "INTEGRATED", "LV ADC", "INTERNAL ADC"};
        for (int i = 0; i < 6; ++i) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(labels[i]);
            ImGui::TableNextColumn(); ImGui::TextDisabled("--  [%s]", routes[i]);
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    ImGui::TextWrapped("This window accepts measured values only. Simulator estimates are never copied into this panel.");
    ImGui::End();
}

void DashboardUI::RenderCommander(IDataSource* dataSource, bool& outDemoMode) {
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::Begin("F1 PIT WALL CONTROL", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);
    
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "SYS_STATUS:");
    ImGui::SameLine();
    if (dataSource->IsConnected()) {
        ImGui::TextColored(COLOR_F1_GREEN, "ONLINE (%s)", dataSource->GetSourceName());
    } else {
        ImGui::TextColored(COLOR_FERRARI_RED, "OFFLINE (WAITING AC)");
    }
    
    ImGui::Separator();
    
    ImGui::Checkbox("DEMO_MODE [VIRTUAL]", &outDemoMode);
    ImGui::SameLine(150);
    ImGui::Checkbox("WEB_BROADCAST (UDP) [VIRTUAL SOURCE]", &enableWebBroadcast);
    
    ImGui::Separator();
    ImGui::Text("PANEL TOGGLES (ONE CHECKBOX PER WINDOW)");
    if (ImGui::BeginTable("PanelToggleTable", 2)) {
        ImGui::TableNextColumn(); ImGui::Checkbox("DRIVER INPUTS [VIRTUAL]", &showDriverInputWin);
        ImGui::TableNextColumn(); ImGui::Checkbox("EV ENERGY [VIRTUAL]", &showVirtualEnergyWin);
        ImGui::TableNextColumn(); ImGui::Checkbox("TYRE & BRAKE [VIRTUAL]", &showTyreWin);
        ImGui::TableNextColumn(); ImGui::Checkbox("TIMING [VIRTUAL]", &showTimingWin);
        ImGui::TableNextColumn(); ImGui::Checkbox("TRACK MAP [VIRTUAL]", &showMapWin);
        ImGui::TableNextColumn(); ImGui::Checkbox("REAL HARDWARE ENERGY", &showActualEnergyWin);
        ImGui::TableNextColumn(); ImGui::Checkbox("RECORDED ENERGY LOG", &showRecordedLogWin);
        ImGui::EndTable();
    }
    ImGui::Separator();
    if (virtualRecorder) {
        if (virtualRecorder->IsRecording()) {
            ImGui::TextColored(COLOR_F1_GREEN, "VIRTUAL RECORDER: RECORDING · %llu packets",
                               static_cast<unsigned long long>(virtualRecorder->RecordCount()));
        } else {
            ImGui::TextDisabled("VIRTUAL RECORDER: WAITING FOR AC_LIVE");
        }
        const std::string recorderError = virtualRecorder->LastError();
        if (!recorderError.empty()) ImGui::TextColored(COLOR_FERRARI_RED, "RECORDER ERROR: %s", recorderError.c_str());
    }
    
    ImGui::End();
}

void DashboardUI::RenderRecordedLog() {
    ImGui::SetNextWindowPos(ImVec2(270, 70), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(860, 740), ImGuiCond_Once);
    ImGui::Begin("RECORDED ENERGY LOG", &showRecordedLogWin, ImGuiWindowFlags_NoCollapse);

    if (ImGui::Button("OPEN FSK-EEM LOG", ImVec2(190, 34))) {
        wchar_t path[4096] = {};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.lpstrFilter = L"FSK Energy Meter Log (*.log)\0*.log\0All Files (*.*)\0*.*\0";
        dialog.lpstrFile = path;
        dialog.nMaxFile = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&dialog)) {
            recordedLog.Load(path);
            selectedLogSample = 0;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("CLEAR", ImVec2(90, 34))) { recordedLog.Clear(); selectedLogSample = 0; }
    if (virtualRecorder) {
        const std::wstring latestPath = virtualRecorder->CurrentFilePath();
        if (!latestPath.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("OPEN LATEST DUMMY", ImVec2(180, 34))) {
                recordedLog.Load(latestPath.c_str());
                selectedLogSample = 0;
            }
        }
    }
    ImGui::Separator();

    if (!recordedLog.IsLoaded()) {
        ImGui::Dummy(ImVec2(0, 25));
        ImGui::SetWindowFontScale(1.35f);
        ImGui::TextColored(COLOR_F1_CYAN, "NO RECORDED LOG LOADED");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Spacing();
        ImGui::TextWrapped("Copy an FSK-EEM .log file from the SD drive and select OPEN FSK-EEM LOG.");
        if (!recordedLog.error.empty()) ImGui::TextColored(COLOR_FERRARI_RED, "%s", recordedLog.error.c_str());
        ImGui::End(); return;
    }

    if (recordedLog.IsVirtual())
        ImGui::TextColored(COLOR_F1_YELLOW, "VIRTUAL RECORDED LOG | SYNTHETIC UID");
    else
        ImGui::TextColored(COLOR_F1_GREEN, "MEASURED HARDWARE LOG | DEVICE UID");
    const std::string utf8Path = WideFieldToUtf8(recordedLog.filePath.c_str(), recordedLog.filePath.size());
    ImGui::TextDisabled("FILE  %s", utf8Path.c_str());
    ImGui::Separator();

    ImGui::TextColored(COLOR_F1_CYAN, "LOG SUMMARY");
    if (ImGui::BeginTable("LogSummary", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("DURATION"); ImGui::Text("%.2f s", recordedLog.durationSeconds);
        ImGui::TableNextColumn(); ImGui::TextDisabled("SAMPLES / RATE"); ImGui::Text("%zu / %.2f Hz", recordedLog.samples.size(), recordedLog.averageRateHz);
        ImGui::TableNextColumn(); ImGui::TextDisabled("VALIDATION"); ImGui::TextColored(recordedLog.invalidPackets ? COLOR_FERRARI_RED : COLOR_F1_GREEN, "%zu invalid", recordedLog.invalidPackets);
        ImGui::TableNextColumn(); ImGui::TextDisabled("VOLTAGE RANGE"); ImGui::Text("%.1f - %.1f V", recordedLog.minVoltage, recordedLog.maxVoltage);
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("DRIVE ENERGY"); ImGui::TextColored(COLOR_F1_YELLOW, "%.5f kWh", recordedLog.driveEnergyKWh);
        ImGui::TableNextColumn(); ImGui::TextDisabled("REGEN ENERGY"); ImGui::TextColored(COLOR_F1_CYAN, "%.5f kWh", recordedLog.regenEnergyKWh);
        ImGui::TableNextColumn(); ImGui::TextDisabled("NET ENERGY"); ImGui::Text("%.5f kWh", recordedLog.netEnergyKWh);
        ImGui::TableNextColumn(); ImGui::TextDisabled("CURRENT RANGE"); ImGui::Text("%.1f - %.1f A", recordedLog.minCurrent, recordedLog.maxCurrent);
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("LOG TIMELINE");

    const int maxIndex = static_cast<int>(recordedLog.samples.size()) - 1;
    if (selectedLogSample > maxIndex) selectedLogSample = maxIndex;
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderInt("##LogTimeline", &selectedLogSample, 0, maxIndex, "Sample %d");
    const auto& sample = recordedLog.samples[static_cast<size_t>(selectedLogSample)];
    ImGui::Text("POSITION  %d / %d", selectedLogSample + 1, maxIndex + 1);
    ImGui::SameLine(250); ImGui::TextColored(COLOR_F1_CYAN, "TIME  %.3f s", sample.timestampMs / 1000.0f);

    if (ImGui::BeginTable("SelectedSample", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("HV VOLTAGE"); ImGui::Text("%.1f V", sample.hvVoltage);
        ImGui::TableNextColumn(); ImGui::TextDisabled("HV CURRENT"); ImGui::Text("%.1f A", sample.hvCurrent);
        ImGui::TableNextColumn(); ImGui::TextDisabled("DC POWER"); ImGui::TextColored(sample.powerKW >= 0 ? COLOR_F1_YELLOW : COLOR_F1_CYAN, "%.1f kW", sample.powerKW);
        ImGui::TableNextColumn(); ImGui::TextDisabled("LV SUPPLY"); ImGui::Text("%.2f V", sample.lvVoltage);
        ImGui::TableNextColumn(); ImGui::TextDisabled("MCU TEMP"); ImGui::Text("%.2f C", sample.temperature);
        ImGui::EndTable();
    }

    auto voltageGetter = [](void* data, int index) { return static_cast<EnergyLog*>(data)->samples[static_cast<size_t>(index)].hvVoltage; };
    auto currentGetter = [](void* data, int index) { return static_cast<EnergyLog*>(data)->samples[static_cast<size_t>(index)].hvCurrent; };
    auto powerGetter = [](void* data, int index) { return static_cast<EnergyLog*>(data)->samples[static_cast<size_t>(index)].powerKW; };
    const int plotCount = static_cast<int>(recordedLog.samples.size());
    ImGui::Spacing();
    ImGui::SeparatorText("SYNCHRONIZED SIGNALS");
    char voltageOverlay[64]; sprintf(voltageOverlay, "HV VOLTAGE | selected %.1f V", sample.hvVoltage);
    char currentOverlay[64]; sprintf(currentOverlay, "HV CURRENT | selected %.1f A", sample.hvCurrent);
    char powerOverlay[64]; sprintf(powerOverlay, "DC POWER | selected %.1f kW", sample.powerKW);
    auto drawTimelineCursor = [&]() {
        const ImVec2 plotMin = ImGui::GetItemRectMin();
        const ImVec2 plotMax = ImGui::GetItemRectMax();
        const float ratio = maxIndex > 0 ? static_cast<float>(selectedLogSample) / static_cast<float>(maxIndex) : 0.0f;
        const float cursorX = plotMin.x + (plotMax.x - plotMin.x) * ratio;
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImU32 cursorColor = IM_COL32(255, 242, 0, 255);
        drawList->AddLine(ImVec2(cursorX, plotMin.y + 2.0f), ImVec2(cursorX, plotMax.y - 2.0f), cursorColor, 2.0f);
        drawList->AddTriangleFilled(
            ImVec2(cursorX, plotMin.y + 2.0f),
            ImVec2(cursorX - 5.0f, plotMin.y + 10.0f),
            ImVec2(cursorX + 5.0f, plotMin.y + 10.0f),
            cursorColor);
    };
    ImGui::PlotLines("##VoltagePlot", voltageGetter, &recordedLog, plotCount, 0, voltageOverlay, FLT_MAX, FLT_MAX, ImVec2(-1, 92));
    drawTimelineCursor();
    ImGui::PlotLines("##CurrentPlot", currentGetter, &recordedLog, plotCount, 0, currentOverlay, FLT_MAX, FLT_MAX, ImVec2(-1, 92));
    drawTimelineCursor();
    ImGui::PlotLines("##PowerPlot", powerGetter, &recordedLog, plotCount, 0, powerOverlay, FLT_MAX, FLT_MAX, ImVec2(-1, 92));
    drawTimelineCursor();
    ImGui::End();
}

void DashboardUI::RenderDriverInputs(SPageFilePhysics* p, bool isVirtual) {
    ImGui::SetNextWindowPos(ImVec2(20, 220), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(380, 220), ImGuiCond_Once);
    ImGui::Begin("DRIVER INPUTS & DRIVETRAIN", &showDriverInputWin, ImGuiWindowFlags_NoCollapse);

    if (isVirtual) {
        ImGui::TextColored(COLOR_F1_YELLOW, "VIRTUAL SIGNAL · ASSETTO CORSA");
    } else {
        ImGui::TextColored(COLOR_F1_GREEN, "MEASURED VEHICLE SIGNAL");
    }
    ImGui::Separator();

    auto safePedal = [](float value) {
        if (!std::isfinite(value)) return 0.0f;
        if (value < 0.0f) return 0.0f;
        if (value > 1.0f) return 1.0f;
        return value;
    };
    const float gas = safePedal(p->gas);
    const float brake = safePedal(p->brake);
    // Assetto Corsa reports clutch engagement (1 = pedal released), while the
    // dashboard shows pedal travel (0 = released, 100 = fully pressed).
    const float clutch = 1.0f - safePedal(p->clutch);

    ImGui::TextDisabled("THROTTLE [VIRTUAL]");
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, COLOR_F1_GREEN);
    char throttleText[32]; sprintf(throttleText, "%.1f %%", gas * 100.0f);
    ImGui::ProgressBar(gas, ImVec2(-1.0f, 18.0f), throttleText);
    ImGui::PopStyleColor();

    ImGui::TextDisabled("BRAKE [VIRTUAL]");
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, COLOR_FERRARI_RED);
    char brakeText[32]; sprintf(brakeText, "%.1f %%", brake * 100.0f);
    ImGui::ProgressBar(brake, ImVec2(-1.0f, 18.0f), brakeText);
    ImGui::PopStyleColor();

    ImGui::TextDisabled("CLUTCH [VIRTUAL]");
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, COLOR_F1_CYAN);
    char clutchText[32]; sprintf(clutchText, "%.1f %%", clutch * 100.0f);
    ImGui::ProgressBar(clutch, ImVec2(-1.0f, 18.0f), clutchText);
    ImGui::PopStyleColor();

    int gear = p->gear - 1;
    char gearText[8];
    if (gear == 0) strcpy(gearText, "N");
    else if (gear < 0) strcpy(gearText, "R");
    else sprintf(gearText, "%d", gear);

    ImGui::Separator();
    ImGui::SetWindowFontScale(1.65f);
    ImGui::TextColored(COLOR_F1_YELLOW, "GEAR %s", gearText);
    ImGui::SameLine(145.0f);
    ImGui::TextColored(COLOR_F1_CYAN, "%d KM/H", static_cast<int>(p->speedKmh));
    ImGui::SetWindowFontScale(1.0f);

    float maxRpm = p->currentMaxRpm > 0 ? static_cast<float>(p->currentMaxRpm) : 12000.0f;
    float rpmRatio = static_cast<float>(p->rpms) / maxRpm;
    if (rpmRatio < 0.0f) rpmRatio = 0.0f;
    if (rpmRatio > 1.0f) rpmRatio = 1.0f;
    ImVec4 rpmColor = rpmRatio > 0.85f ? COLOR_FERRARI_RED : (rpmRatio > 0.65f ? COLOR_F1_YELLOW : COLOR_F1_GREEN);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, rpmColor);
    char rpmText[32]; sprintf(rpmText, "RPM %d / %.0f", p->rpms, maxRpm);
    ImGui::ProgressBar(rpmRatio, ImVec2(-1.0f, 20.0f), rpmText);
    ImGui::PopStyleColor();
    ImGui::End();
}

void DashboardUI::RenderTyreMonitor(SPageFilePhysics* p) {
    ImGui::SetNextWindowPos(ImVec2(20, 455), ImGuiCond_FirstUseEver);
    ImGui::Begin("TYRE & BRAKE TELEMETRY", &showTyreWin, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextColored(COLOR_F1_YELLOW, "VIRTUAL SIGNAL · ASSETTO CORSA");
    ImGui::Separator();
    
    if (ImGui::BeginTable("TyreTable", 2)) {
        const char* labels[] = {"FRONT LEFT", "FRONT RIGHT", "REAR LEFT", "REAR RIGHT"};
        for(int i = 0; i < 4; i++) {
            ImGui::TableNextColumn();
            ImGui::BeginChild(labels[i], ImVec2(180, 160), true);
            ImGui::TextColored(COLOR_F1_CYAN, "%s", labels[i]); 
            ImGui::Separator();
            
            float coreTemp = p->tyreCoreTemperature[i];
            ImGui::Text("Core: "); ImGui::SameLine();
            ImGui::TextColored(GetTempColor(coreTemp), "%.1f C", coreTemp);
            
            ImGui::Text("I/M/O: %.0f / %.0f / %.0f", p->tyreTempI[i], p->tyreTempM[i], p->tyreTempO[i]);
            ImGui::Text("Press: %.1f psi", p->wheelsPressure[i]);
            
            ImGui::Separator();
            float brakeTemp = p->brakeTemp[i];
            ImGui::Text("Brake: "); ImGui::SameLine();
            ImGui::TextColored(GetTempColor(brakeTemp), "%.0f C", brakeTemp);
            
            ImGui::EndChild(); 
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void DashboardUI::RenderTiming(SPageFileGraphics* curG) {
    ImGui::SetNextWindowPos(ImVec2(420, 20), ImGuiCond_FirstUseEver);
    ImGui::Begin("TIMING & DELTA", &showTimingWin, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextColored(COLOR_F1_YELLOW, "VIRTUAL SIGNAL · ASSETTO CORSA");
    ImGui::Separator();
    
    ImGui::SetWindowFontScale(1.5f);
    const std::string best = WideFieldToUtf8(curG->bestTime, 15);
    const std::string last = WideFieldToUtf8(curG->lastTime, 15);
    ImGui::TextColored(COLOR_F1_PURPLE, "BEST: %s", best.c_str());
    ImGui::Text("LAST: %s", last.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    
    if (ImGui::BeginTable("SectorTable", 3)) {
        for(int i = 0; i < 3; i++) { 
            ImGui::TableNextColumn();
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "SECTOR %d", i + 1); 
            ImGui::SetWindowFontScale(1.8f);
            ImGui::TextColored(sectorTiming.colors[i], "%.3f", sectorTiming.lastTime[i]); 
            ImGui::SetWindowFontScale(1.0f);
        }
        ImGui::EndTable();
    }
    
    ImGui::Separator();
    ImGui::Text("LAPS: %d", curG->completedLaps);
    ImGui::SameLine(120);
    ImGui::Text("POS: %d", curG->position);
    
    ImGui::End();
}

void DashboardUI::RenderTrackMap(SPageFilePhysics* p) {
    ImGui::SetNextWindowPos(ImVec2(420, 200), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 350), ImGuiCond_FirstUseEver);
    ImGui::Begin("TRACK MAP GPS", &showMapWin);
    ImGui::TextColored(COLOR_F1_YELLOW, "VIRTUAL SIGNAL · ASSETTO CORSA");
    
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos(); 
    ImVec2 sz = ImGui::GetContentRegionAvail();
    
    dl->AddRectFilled(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), IM_COL32(15, 15, 15, 255));
    
    ImVec2 cp = ImVec2(p->tyreContactPoint[0][0], p->tyreContactPoint[0][2]);
    
    if (trackTrail.empty() || hypotf(trackTrail.back().x - cp.x, trackTrail.back().y - cp.y) > 5.0f) { 
        trackTrail.push_back(cp); 
        if(trackTrail.size() > 1500) trackTrail.pop_front(); 
    }
    
    ImVec2 center = ImVec2(p0.x + sz.x * 0.5f, p0.y + sz.y * 0.5f); 
    float sc = 0.12f;
    
    for(size_t i = 0; i + 1 < trackTrail.size(); i++) {
        dl->AddLine(
            ImVec2(center.x + trackTrail[i].x * sc, center.y + trackTrail[i].y * sc), 
            ImVec2(center.x + trackTrail[i+1].x * sc, center.y + trackTrail[i+1].y * sc), 
            IM_COL32(150, 150, 150, 150), 2.0f
        );
    }
    
    dl->AddCircleFilled(ImVec2(center.x + cp.x * sc, center.y + cp.y * sc), 6.0f, IM_COL32(227, 38, 54, 255));
    ImGui::End();
}
