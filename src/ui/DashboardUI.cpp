#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <commdlg.h>

#include "DashboardUI.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr ImGuiTableFlags kTableFlags =
    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame;

void RenderSignalStatus(bool online, bool valid, float ageMs) {
    if (online && valid) {
        ImGui::TextColored(COLOR_OK_GREEN, "수신 중");
    } else if (online) {
        ImGui::TextColored(COLOR_ALERT_RED, "수신됨 · 값 오류");
    } else if (ageMs < 0.0f) {
        ImGui::TextColored(COLOR_MUTED, "미수신");
    } else {
        ImGui::TextColored(COLOR_WARN_AMBER, "오래됨 · %.1f초", ageMs / 1000.0f);
    }
}

void RenderMissing(float ageMs) {
    if (ageMs < 0.0f) ImGui::TextColored(COLOR_MUTED, "-- · 미수신");
    else ImGui::TextColored(COLOR_WARN_AMBER, "-- · 오래됨 %.1f초", ageMs / 1000.0f);
}

std::string WideFieldToUtf8(const wchar_t* value, size_t capacity) {
    size_t length = 0;
    while (length < capacity && value[length] != L'\0') ++length;
    if (length == 0) return "--";
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value,
                                          static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return "경로 변환 오류";
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value,
                        static_cast<int>(length), result.data(), bytes, nullptr, nullptr);
    return result;
}

void BeginMetric(const char* label) {
    ImGui::TextDisabled("%s", label);
    ImGui::SetWindowFontScale(1.35f);
}

void EndMetric() { ImGui::SetWindowFontScale(1.0f); }

}  // namespace

void DashboardUI::RenderImuGeometry(const EVTelemetry& t) {
    ImGui::SeparatorText("IMU G-미터 (F1 스타일)");
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float size = std::clamp(std::min(available.x, 190.0f), 150.0f, 190.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 center(origin.x + size * 0.5f, origin.y + size * 0.5f);
    const float radius = size * 0.36f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const bool valid = t.imuOnline && t.imuOk;
    float displayLateral = t.lateralAccelMS2;
    float displayLongitudinal = t.longitudinalAccelMS2;
    const bool rawAxesAvailable = std::fabs(t.imuRawAxMS2) + std::fabs(t.imuRawAyMS2) +
                                  std::fabs(t.imuRawAzMS2) > 0.5f;
    const char* axisMapping = "STM 기준 횡/종축";
    // 현재 센서에서는 Ay가 중력(-9.8 m/s²)을 받고 있어 화면 횡축으로 쓰면
    // 점이 한쪽에 고정된다. Ay가 수직축으로 감지될 때만 X/Z를 화면축으로
    // 사용한다. 제어용 lateral/longitudinal 값은 변경하지 않는다.
    if (rawAxesAvailable && std::fabs(t.imuRawAyMS2) >= 6.0f &&
        std::fabs(t.imuRawAyMS2) >= std::fabs(t.imuRawAxMS2) &&
        std::fabs(t.imuRawAyMS2) >= std::fabs(t.imuRawAzMS2)) {
        displayLateral = t.imuRawAxMS2;
        displayLongitudinal = t.imuRawAzMS2;
        axisMapping = "Ay 수직 감지 · 화면 X/Z";
    }
    const ImU32 ring = valid ? IM_COL32(42, 188, 220, 255) : IM_COL32(110, 116, 128, 180);
    draw->AddCircle(center, radius, ring, 64, 2.0f);
    draw->AddLine(ImVec2(center.x - radius, center.y), ImVec2(center.x + radius, center.y), IM_COL32(80, 90, 105, 150), 1.0f);
    draw->AddLine(ImVec2(center.x, center.y - radius), ImVec2(center.x, center.y + radius), IM_COL32(80, 90, 105, 150), 1.0f);
    draw->AddText(ImVec2(center.x - 5.0f, center.y - radius - 18.0f), IM_COL32(180, 190, 205, 255), "앞");
    draw->AddText(ImVec2(center.x + radius + 7.0f, center.y - 7.0f), IM_COL32(180, 190, 205, 255), "우");
    draw->AddText(ImVec2(center.x - 7.0f, center.y + radius + 5.0f), IM_COL32(145, 155, 170, 255), "뒤");
    draw->AddText(ImVec2(center.x - radius - 18.0f, center.y - 7.0f), IM_COL32(145, 155, 170, 255), "좌");
    if (valid) {
        // 표시용 영점을 뺀 가속도 벡터. 정지 상태는 중앙에 표시한다.
        const float lateral = displayLateral - imuCenterLateralMS2_;
        const float longitudinal = displayLongitudinal - imuCenterLongitudinalMS2_;
        const float x = std::clamp(lateral / 9.80665f, -1.0f, 1.0f);
        const float y = std::clamp(longitudinal / 9.80665f, -1.0f, 1.0f);
        const ImVec2 dot(center.x + x * radius * 0.82f,
                         center.y - y * radius * 0.82f);
        draw->AddLine(center, dot, IM_COL32(255, 190, 64, 190), 3.0f);
        draw->AddCircleFilled(dot, 7.0f, IM_COL32(255, 190, 64, 255));
        draw->AddCircle(dot, 10.0f, IM_COL32(255, 235, 150, 220), 24, 1.5f);
    }
    ImGui::Dummy(ImVec2(size, size));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextColored(valid ? COLOR_OK_GREEN : COLOR_ALERT_RED, "%s", valid ? "IMU 수신 정상" : "IMU 미수신/무효");
    if (valid) {
        ImGui::Text("Yaw rate  %+.3f rad/s", t.yawRateRadS - imuCenterYawRateRadS_);
        ImGui::Text("횡가속도 %+.3f m/s²", displayLateral - imuCenterLateralMS2_);
        ImGui::Text("종가속도 %+.3f m/s²", displayLongitudinal - imuCenterLongitudinalMS2_);
        ImGui::TextDisabled("축 매핑: %s", axisMapping);
        if (std::fabs(t.yawRateRadS - imuCenterYawRateRadS_) > 0.5f ||
            std::fabs(displayLateral - imuCenterLateralMS2_) > 3.0f ||
            std::fabs(displayLongitudinal - imuCenterLongitudinalMS2_) > 3.0f) {
            ImGui::TextColored(COLOR_WARN_AMBER, "정지 상태 값 확인 필요");
        }
    } else {
        ImGui::TextDisabled("센서 패킷을 기다리는 중");
    }
    if (ImGui::Button("현재값 중앙 고정")) {
        if (valid) {
            imuCenterLateralMS2_ = displayLateral;
            imuCenterLongitudinalMS2_ = displayLongitudinal;
            imuCenterYawRateRadS_ = t.yawRateRadS;
            imuCenterCaptured_ = true;
        } else {
            imuCenterLateralMS2_ = 0.0f;
            imuCenterLongitudinalMS2_ = 0.0f;
            imuCenterYawRateRadS_ = 0.0f;
            imuCenterCaptured_ = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("중앙 초기화")) {
        imuCenterLateralMS2_ = 0.0f;
        imuCenterLongitudinalMS2_ = 0.0f;
        imuCenterYawRateRadS_ = 0.0f;
        imuCenterCaptured_ = false;
    }
    ImGui::TextDisabled("%s · 화면 표시용 영점(제어값 변경 없음)",
                       imuCenterCaptured_ ? "현재값 기준" : "기본 0 기준");
    ImGui::EndGroup();
}

DashboardUI::DashboardUI() { ApplyTheme(); }

DashboardUI::~DashboardUI() {
    if (liveCommandArmed_) SendEVLiveControlRequest(true);
}

void DashboardUI::ApplyTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 7.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.ScrollbarRounding = 5.0f;
    style.GrabRounding = 4.0f;
    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(9.0f, 6.0f);
    style.ItemSpacing = ImVec2(9.0f, 7.0f);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(0.93f, 0.95f, 0.97f, 1.0f);
    colors[ImGuiCol_TextDisabled] = COLOR_MUTED;
    colors[ImGuiCol_WindowBg] = ImVec4(0.035f, 0.047f, 0.065f, 0.98f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.055f, 0.070f, 0.095f, 1.0f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.045f, 0.055f, 0.075f, 1.0f);
    colors[ImGuiCol_Border] = ImVec4(0.16f, 0.22f, 0.29f, 0.9f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.09f, 0.12f, 0.16f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.12f, 0.18f, 0.24f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.15f, 0.23f, 0.31f, 1.0f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.025f, 0.035f, 0.050f, 1.0f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.07f, 0.11f, 0.15f, 1.0f);
    colors[ImGuiCol_CheckMark] = COLOR_INFO_CYAN;
    colors[ImGuiCol_SliderGrab] = COLOR_INFO_CYAN;
    colors[ImGuiCol_Button] = ImVec4(0.10f, 0.15f, 0.20f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.15f, 0.28f, 0.36f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.18f, 0.36f, 0.46f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.10f, 0.16f, 0.21f, 1.0f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.14f, 0.25f, 0.32f, 1.0f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.18f, 0.34f, 0.43f, 1.0f);
}

void DashboardUI::Render(IDataSource* dataSource) {
    if (!dataSource) return;
    const EVTelemetry* telemetry = dataSource->GetEVTelemetry();
    if (!telemetry) return;

    const bool connected = dataSource->IsConnected();
    UpdateGnssTrail(*telemetry);
    RenderSystemStatus(dataSource, *telemetry);
    if (showVehicleWindow_) RenderVehicle(*telemetry);
    if (showBatteryWindow_) RenderBattery(*telemetry, connected);
    if (showGnssWindow_) RenderGnss(*telemetry, connected);
    if (showControlWindow_) RenderControl(*telemetry, connected && telemetry->vehicleOnline);
    if (showRecordedLogWindow_) RenderRecordedLog();
}

void DashboardUI::RenderSystemStatus(IDataSource* source, const EVTelemetry& t) {
    const bool connected = source->IsConnected();
    ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(360, 430), ImGuiCond_Once);
    ImGui::Begin("시스템 상태", nullptr, ImGuiWindowFlags_NoCollapse);

    ImGui::SetWindowFontScale(1.20f);
    ImGui::TextColored(connected ? COLOR_OK_GREEN : COLOR_ALERT_RED,
                       connected ? "Gateway UDP 수신 중" : "Gateway UDP 미수신");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("입력: %s", source->GetSourceName());
    ImGui::TextDisabled("SEQ %u  |  지연 %.2f초  |  %.1f Hz", t.sequence, t.ageSeconds, t.receiveRateHz);
    ImGui::SeparatorText("통신 상태");

    if (ImGui::BeginTable("SystemLinks", 2, kTableFlags)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("차량 STM");
        RenderSignalStatus(t.vehicleOnline, t.stmOnline, t.vehicleAgeMs);
        ImGui::TableNextColumn(); ImGui::TextDisabled("좌 / 우 RPM");
        ImGui::TextColored(t.rpmLeftOnline ? COLOR_OK_GREEN : COLOR_WARN_AMBER, "좌 %s", t.rpmLeftOnline ? "수신" : "미수신");
        ImGui::SameLine(); ImGui::TextColored(t.rpmRightOnline ? COLOR_OK_GREEN : COLOR_WARN_AMBER,
                                              "· 우 %s", t.rpmRightOnline ? "수신" : "미수신");
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("TPS / SAS");
        ImGui::TextColored(t.tpsOnline ? COLOR_OK_GREEN : COLOR_WARN_AMBER, "TPS %s", t.tpsOnline ? "수신" : "미수신");
        ImGui::SameLine(); ImGui::TextColored(t.sasOnline ? COLOR_OK_GREEN : COLOR_WARN_AMBER,
                                              "· SAS %s", t.sasOnline ? "수신" : "미수신");
        ImGui::TableNextColumn(); ImGui::TextDisabled("IMU / Rear 출력");
        ImGui::TextColored(t.imuOnline ? COLOR_OK_GREEN : COLOR_WARN_AMBER, "IMU %s", t.imuOnline ? "수신" : "미수신");
        ImGui::SameLine(); ImGui::TextColored(t.rearOutputOnline ? COLOR_OK_GREEN : COLOR_WARN_AMBER,
                                              "· 출력 %s", t.rearOutputOnline ? "수신" : "미수신");
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("BMS");
        ImGui::TextColored(t.bmsOnline && !t.bmsFault ? COLOR_OK_GREEN : COLOR_WARN_AMBER,
                           "%s", t.bmsOnline ? (t.bmsFault ? "경고 발생" : "정상") : "수신 대기");
        ImGui::TableNextColumn(); ImGui::TextDisabled("GNSS");
        ImGui::TextColored(t.gnssOnline ? COLOR_OK_GREEN : COLOR_WARN_AMBER,
                           "%s", t.gnssOnline ? "위치 수신" : "수신 대기");
        ImGui::EndTable();
    }

    ImGui::SeparatorText("측정 기록");
    ImGui::TextColored(t.recordingActive ? COLOR_ALERT_RED : COLOR_MUTED,
                       "%s", t.recordingActive ? "● 기록 중" : "기록 대기");
    if (t.recordingActive) {
        ImGui::SameLine();
        ImGui::Text("세션 %u · %llu개 · %.1f초", t.recordingSessionId,
                    static_cast<unsigned long long>(t.recordingSampleCount),
                    t.recordingElapsedS);
    }
    const char* recordButton = t.recordingActive ? "측정 종료" : "측정 시작";
    if (ImGui::Button(recordButton, ImVec2(150, 32))) {
        SendEVRecordingRequest(!t.recordingActive);
    }
    ImGui::TextDisabled("조회: http://127.0.0.1:8766/records");
    if (t.databasePath[0]) ImGui::TextDisabled("DB: %s", t.databasePath);

    ImGui::SeparatorText("표시 창");
    if (ImGui::BeginTable("WindowToggles", 2)) {
        ImGui::TableNextColumn(); ImGui::Checkbox("차량 센서", &showVehicleWindow_);
        ImGui::TableNextColumn(); ImGui::Checkbox("배터리/BMS", &showBatteryWindow_);
        ImGui::TableNextColumn(); ImGui::Checkbox("GNSS 트랙맵", &showGnssWindow_);
        ImGui::TableNextColumn(); ImGui::Checkbox("TQV/회생 설정", &showControlWindow_);
        ImGui::TableNextColumn(); ImGui::Checkbox("실측 에너지 로그", &showRecordedLogWindow_);
        ImGui::EndTable();
    }
    ImGui::TextDisabled("가상값과 데모값은 표시하지 않습니다.");
    ImGui::End();
}

void DashboardUI::RenderVehicle(const EVTelemetry& t) {
    ImGui::SetNextWindowPos(ImVec2(392, 16), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(990, 410), ImGuiCond_Once);
    ImGui::Begin("차량 실시간 센서", &showVehicleWindow_, ImGuiWindowFlags_NoCollapse);

    ImGui::TextColored(t.vehicleOnline ? COLOR_OK_GREEN : COLOR_ALERT_RED,
                       t.vehicleOnline ? "차량 패킷 수신 중" : "차량 패킷 미수신");
    ImGui::SameLine();
    ImGui::TextDisabled("패킷 %llu / 누락 %llu  |  전송경로 %s",
                        static_cast<unsigned long long>(t.receivedPackets),
                        static_cast<unsigned long long>(t.lostPackets),
                        t.telemetryTransport[0] ? t.telemetryTransport : "--");

    if (ImGui::BeginTable("VehiclePrimary", 3, kTableFlags)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); BeginMetric("차량 속도");
        const bool speedLive = t.speedOnline;
        if (speedLive) ImGui::TextColored(COLOR_INFO_CYAN, "%.1f km/h", t.speedKmh); else RenderMissing(t.speedAgeMs); EndMetric();
        ImGui::TableNextColumn(); BeginMetric("좌측 모터");
        if (t.rpmLeftOnline && t.motorLeftOk) ImGui::Text("%u RPM", t.rpmLeft); else if (t.rpmLeftOnline) ImGui::TextColored(COLOR_ALERT_RED, "-- · 값 오류"); else RenderMissing(t.rpmLeftAgeMs); EndMetric();
        ImGui::TableNextColumn(); BeginMetric("우측 모터");
        if (t.rpmRightOnline && t.motorRightOk) ImGui::Text("%u RPM", t.rpmRight); else if (t.rpmRightOnline) ImGui::TextColored(COLOR_ALERT_RED, "-- · 값 오류"); else RenderMissing(t.rpmRightAgeMs); EndMetric();
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("가속 페달 (TPS)  |  RAW %s", t.tpsOnline ? std::to_string(t.tpsRaw).c_str() : "--");
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, t.tpsOnline && t.tpsOk ? COLOR_OK_GREEN : COLOR_ALERT_RED);
    char throttleText[48] = "미수신";
    if (t.tpsOnline && t.tpsOk) snprintf(throttleText, sizeof(throttleText), "%.1f %%", t.tpsPercent);
    else if (t.tpsOnline) snprintf(throttleText, sizeof(throttleText), "값 오류");
    else if (t.tpsAgeMs >= 0.0f) snprintf(throttleText, sizeof(throttleText), "오래됨 %.1f초", t.tpsAgeMs / 1000.0f);
    ImGui::ProgressBar(t.tpsOnline && t.tpsOk ? std::clamp(t.tpsPercent / 100.0f, 0.0f, 1.0f) : 0.0f,
                       ImVec2(-1.0f, 24.0f), throttleText);
    ImGui::PopStyleColor();

    ImGui::SeparatorText("조향 및 차체 센서");
    if (ImGui::BeginTable("VehicleSensors", 4, kTableFlags)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("조향각");
        if (t.sasOnline && t.sasOk) ImGui::TextColored(COLOR_INFO_CYAN, "%+.2f deg", t.steeringDeg); else if (t.sasOnline) ImGui::TextColored(COLOR_ALERT_RED, "-- · 값 오류"); else RenderMissing(t.sasAgeMs);
        if (t.sasOnline) ImGui::TextDisabled("SAS 상대 %+.2f deg", t.sasRelativeDeg);
        ImGui::TableNextColumn(); ImGui::TextDisabled("SAS 원시값");
        if (t.sasOnline) ImGui::Text("%u / 16383", t.sasRaw); else RenderMissing(t.sasAgeMs);
        if (t.sasOnline) ImGui::TextDisabled("중앙값 %u", t.sasCenterRaw);
        ImGui::TableNextColumn(); ImGui::TextDisabled("Yaw rate");
        if (t.imuOnline && t.imuOk) ImGui::Text("%.3f rad/s", t.yawRateRadS); else if (t.imuOnline) ImGui::TextColored(COLOR_ALERT_RED, "-- · 값 오류"); else RenderMissing(t.imuAgeMs);
        ImGui::TextDisabled("Rear IMU 상태");
        ImGui::TableNextColumn(); ImGui::TextDisabled("횡가속도");
        if (t.imuOnline && t.imuOk) ImGui::Text("%.2f m/s²", t.lateralAccelMS2); else if (t.imuOnline) ImGui::TextColored(COLOR_ALERT_RED, "-- · 값 오류"); else RenderMissing(t.imuAgeMs);
        if (t.rpmLeftOnline || t.rpmRightOnline) ImGui::TextDisabled("펄스 L/R %u / %u", t.captureLeft, t.captureRight);
        ImGui::EndTable();
    }
    RenderImuGeometry(t);

    ImGui::SeparatorText("TQV 및 회생 상태");
    if (ImGui::BeginTable("VehicleOutput", 4, kTableFlags)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("좌측 출력");
        if (t.rearOutputOnline) ImGui::Text("DAC %u  |  %.2f kW", t.dacLeft, t.powerLeftKw); else RenderMissing(t.rearOutputAgeMs);
        ImGui::TableNextColumn(); ImGui::TextDisabled("우측 출력");
        if (t.rearOutputOnline) ImGui::Text("DAC %u  |  %.2f kW", t.dacRight, t.powerRightKw); else RenderMissing(t.rearOutputAgeMs);
        ImGui::TableNextColumn(); ImGui::TextDisabled("TQV 요청 / 적용");
        if (t.rearOutputOnline) ImGui::Text("%.0f%% / %.0f%%", t.tvRequestedPercent, t.tvAppliedPercent); else RenderMissing(t.rearOutputAgeMs);
        if (t.rearOutputOnline) ImGui::TextColored(t.tvLimited ? COLOR_WARN_AMBER : COLOR_OK_GREEN, "%s", t.tvLimited ? "제한됨" : (t.tvActive ? "작동" : "대기"));
        ImGui::TableNextColumn(); ImGui::TextDisabled("회생 요청 / 적용");
        if (t.rearOutputOnline) ImGui::Text("%.0f%% / %.0f%%", t.regenRequestedPercent, t.regenAppliedPercent); else RenderMissing(t.rearOutputAgeMs);
        if (t.rearOutputOnline) ImGui::TextColored(t.regenReady ? COLOR_OK_GREEN : COLOR_WARN_AMBER, "%s", t.regenReady ? "준비" : "검증 대기");
        ImGui::EndTable();
    }

    if (t.tqvInternalOnline && ImGui::BeginTable("TqvInternal", 4, kTableFlags)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("TQV 차량속도"); ImGui::Text("%.3f m/s", t.vehicleSpeedMS);
        ImGui::TableNextColumn(); ImGui::TextDisabled("목표 / 오차 Yaw"); ImGui::Text("%+.3f / %+.3f rad/s", t.desiredYawRadS, t.yawErrorRadS);
        ImGui::TableNextColumn(); ImGui::TextDisabled("차동전력 L / R / Δ"); ImGui::Text("%.2f / %.2f / %+.2f kW", t.powerLeftKw, t.powerRightKw, t.deltaPowerKw);
        ImGui::TableNextColumn(); ImGui::TextDisabled("TV / ED / Traction");
        if (t.tvActive && t.edActive) ImGui::TextColored(COLOR_ALERT_RED, "배타 조건 위반");
        else ImGui::Text("%s / %s / %.3f", t.tvActive ? "ON" : "OFF", t.edActive ? "ON" : "OFF", t.tractionScale);
        ImGui::EndTable();
    }
    if (t.rearOutputOnline && !t.tqvInternalOnline) {
        ImGui::TextColored(COLOR_WARN_AMBER, "TQV 내부 연산값 미수신");
    }

    ImGui::TextDisabled("주행모드 %s  |  제한 사유 %s  |  Front 출처 %s",
                        EVDriveModeName(t.driveMode), t.limitReason, t.frontSource);
    ImGui::TextColored(t.faultCode == 0 ? COLOR_OK_GREEN : COLOR_ALERT_RED,
                       "차량 오류: %u · %s", t.faultCode, EVFaultName(t.faultCode));
    ImGui::End();
}

void DashboardUI::RenderBattery(const EVTelemetry& t, bool connected) {
    ImGui::SetNextWindowPos(ImVec2(392, 442), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(990, 420), ImGuiCond_Once);
    ImGui::Begin("배터리 / DALY BMS", &showBatteryWindow_, ImGuiWindowFlags_NoCollapse);

    const bool live = connected && t.bmsOnline;
    ImGui::TextColored(live && !t.bmsFault ? COLOR_OK_GREEN : (t.bmsFault ? COLOR_ALERT_RED : COLOR_WARN_AMBER),
                       live ? (t.bmsFault ? "BMS 경고 발생" : "BMS 실제값 수신 중") : "BMS 데이터 수신 대기");
    ImGui::SameLine();
    ImGui::TextDisabled("출처 %s  |  지연 %.0f ms  |  %s", t.bmsSource, t.bmsAgeMs,
                        t.bmsModel[0] ? t.bmsModel : "모델 미확인");

    if (ImGui::BeginTable("BatteryPrimary", 4, kTableFlags)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); BeginMetric("충전 상태");
        if (live) ImGui::TextColored(COLOR_INFO_CYAN, "%.1f %%", t.batterySocPercent); else ImGui::Text("--"); EndMetric();
        ImGui::TableNextColumn(); BeginMetric("팩 전압");
        if (live) ImGui::Text("%.1f V", t.batteryPackVoltageV); else ImGui::Text("--"); EndMetric();
        ImGui::TableNextColumn(); BeginMetric("팩 전류");
        if (live) ImGui::Text("%+.1f A", t.batteryCurrentA); else ImGui::Text("--"); EndMetric();
        ImGui::TableNextColumn(); BeginMetric("팩 전력");
        if (live) ImGui::TextColored(COLOR_WARN_AMBER, "%+.2f kW", t.batteryPowerKw); else ImGui::Text("--"); EndMetric();
        ImGui::EndTable();
    }

    if (ImGui::BeginTable("BatteryHealth", 4, kTableFlags)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("최고 / 최저 셀");
        if (live) ImGui::Text("%.3f V (#%u) / %.3f V (#%u)", t.bmsMaxCellVoltageV, t.bmsMaxCellNumber,
                              t.bmsMinCellVoltageV, t.bmsMinCellNumber); else ImGui::Text("--");
        ImGui::TableNextColumn(); ImGui::TextDisabled("셀 편차");
        if (live) ImGui::TextColored(t.bmsCellDeltaMv <= 20.0f ? COLOR_OK_GREEN :
                                     (t.bmsCellDeltaMv <= 50.0f ? COLOR_WARN_AMBER : COLOR_ALERT_RED),
                                     "%.0f mV", t.bmsCellDeltaMv); else ImGui::Text("--");
        ImGui::TableNextColumn(); ImGui::TextDisabled("온도 범위");
        if (live) ImGui::Text("%.0f ~ %.0f °C", t.bmsTempMinC, t.bmsTempMaxC); else ImGui::Text("--");
        ImGui::TableNextColumn(); ImGui::TextDisabled("MOS / 밸런싱");
        if (live) ImGui::Text("충전 %s · 방전 %s · %s", t.bmsChargeMosOn ? "ON" : "OFF",
                              t.bmsDischargeMosOn ? "ON" : "OFF", t.bmsBalancing ? "밸런싱" : "대기");
        ImGui::EndTable();
    }

    ImGui::SeparatorText("셀별 전압 · 팩 내부 상대 비교");
    const std::uint8_t count = live ? t.bmsCellVoltageCount : 0;
    if (count == 0) {
        ImGui::TextDisabled("셀 전압 프레임을 기다리고 있습니다. 값이 없을 때 임의값을 표시하지 않습니다.");
        ImGui::End();
        return;
    }

    float minimum = FLT_MAX;
    float maximum = -FLT_MAX;
    float sum = 0.0f;
    std::uint8_t validCount = 0;
    for (std::uint8_t i = 0; i < count; ++i) {
        const float value = t.bmsCellVoltagesV[i];
        if (!std::isfinite(value) || value <= 0.0f) continue;
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
        sum += value;
        ++validCount;
    }
    if (validCount == 0) {
        ImGui::TextColored(COLOR_ALERT_RED, "유효한 셀 전압이 없습니다.");
        ImGui::End();
        return;
    }

    const float average = sum / validCount;
    const float visualMin = minimum - 0.010f;
    const float visualSpan = std::max(0.020f, maximum - minimum + 0.020f);
    const int columns = std::min<int>(7, count);
    if (ImGui::BeginTable("BatteryCells", columns, kTableFlags)) {
        for (std::uint8_t i = 0; i < count; ++i) {
            const float value = t.bmsCellVoltagesV[i];
            const float deviationMv = (value - average) * 1000.0f;
            const float absoluteDeviation = std::fabs(deviationMv);
            const ImVec4 color = absoluteDeviation <= 10.0f ? COLOR_OK_GREEN :
                                 (absoluteDeviation <= 25.0f ? COLOR_WARN_AMBER : COLOR_ALERT_RED);
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(i));
            ImGui::TextDisabled("셀 %02u", static_cast<unsigned>(i + 1));
            ImGui::TextColored(color, "%.3f V", value);
            char overlay[24];
            snprintf(overlay, sizeof(overlay), "%+.0f mV", deviationMv);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
            ImGui::ProgressBar(std::clamp((value - visualMin) / visualSpan, 0.0f, 1.0f), ImVec2(-1.0f, 17.0f), overlay);
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("평균 %.3f V  |  그래프는 현재 팩의 최저~최고 셀 상대 범위이며 절대 SOC 눈금이 아닙니다.", average);
    ImGui::TextColored(t.bmsFault ? COLOR_ALERT_RED : COLOR_OK_GREEN, "BMS 알람: %s", t.bmsAlarmSummary);
    ImGui::End();
}

void DashboardUI::UpdateGnssTrail(const EVTelemetry& t) {
    if (!t.gnssOnline || t.gnssAgeMs > 2000.0f) return;
    if (!gnssOriginValid_) {
        gnssOriginLatitude_ = t.gnssLatitudeDeg;
        gnssOriginLongitude_ = t.gnssLongitudeDeg;
        gnssOriginValid_ = true;
    }
    constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
    const double x = (t.gnssLongitudeDeg - gnssOriginLongitude_) * 111320.0 *
                     std::cos(gnssOriginLatitude_ * kDegToRad);
    const double y = (t.gnssLatitudeDeg - gnssOriginLatitude_) * 110540.0;
    const ImVec2 point(static_cast<float>(x), static_cast<float>(y));
    if (gnssTrailMeters_.empty() || std::hypot(point.x - gnssTrailMeters_.back().x,
                                               point.y - gnssTrailMeters_.back().y) >= 1.0f) {
        gnssTrailMeters_.push_back(point);
        if (gnssTrailMeters_.size() > 5000) gnssTrailMeters_.pop_front();
    }
}

void DashboardUI::RenderGnss(const EVTelemetry& t, bool connected) {
    ImGui::SetNextWindowPos(ImVec2(16, 462), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(360, 400), ImGuiCond_Once);
    ImGui::Begin("GNSS 트랙맵", &showGnssWindow_, ImGuiWindowFlags_NoCollapse);

    const bool live = connected && t.gnssOnline && t.gnssAgeMs < 2000.0f;
    ImGui::TextColored(live ? COLOR_OK_GREEN : COLOR_WARN_AMBER,
                       live ? "휴대폰 GNSS 실제 위치 수신 중" : "휴대폰 GNSS 연결 대기");
    ImGui::SameLine();
    ImGui::TextDisabled("출처 %s", live ? t.gnssSource : "--");
    if (ImGui::Button("주행 궤적 초기화")) {
        gnssTrailMeters_.clear();
        gnssOriginValid_ = false;
    }

    if (ImGui::BeginTable("GnssInfo", 2, kTableFlags)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("Fix / 위성");
        if (live) ImGui::Text("%u / %u개", t.gnssFixType, t.gnssSatellites); else ImGui::Text("--");
        ImGui::TableNextColumn(); ImGui::TextDisabled("정확도 / 지연");
        if (live && t.gnssAccuracyM > 0.0f) ImGui::Text("%.1f m / %.0f ms", t.gnssAccuracyM, t.gnssAgeMs);
        else if (live) ImGui::Text("HDOP %.2f / %.0f ms", t.gnssHdop, t.gnssAgeMs);
        else ImGui::Text("--");
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("위도");
        if (live) ImGui::Text("%.7f°", t.gnssLatitudeDeg); else ImGui::Text("--");
        ImGui::TableNextColumn(); ImGui::TextDisabled("경도");
        if (live) ImGui::Text("%.7f°", t.gnssLongitudeDeg); else ImGui::Text("--");
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("고도");
        if (live) ImGui::Text("%.1f m", t.gnssAltitudeM); else ImGui::Text("--");
        ImGui::TableNextColumn(); ImGui::TextDisabled("진행 방향");
        if (live) ImGui::Text("%.1f°", t.gnssHeadingDeg); else ImGui::Text("--");
        ImGui::EndTable();
    }

    ImGui::SeparatorText("주행 궤적");
    const ImVec2 canvasPosition = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    canvasSize.y = std::max(250.0f, canvasSize.y);
    ImGui::InvisibleButton("GnssCanvas", canvasSize);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 canvasEnd(canvasPosition.x + canvasSize.x, canvasPosition.y + canvasSize.y);
    draw->AddRectFilled(canvasPosition, canvasEnd, IM_COL32(10, 18, 25, 255));
    draw->AddRect(canvasPosition, canvasEnd, IM_COL32(55, 80, 100, 255));
    for (int i = 1; i < 5; ++i) {
        const float x = canvasPosition.x + canvasSize.x * i / 5.0f;
        const float y = canvasPosition.y + canvasSize.y * i / 5.0f;
        draw->AddLine(ImVec2(x, canvasPosition.y), ImVec2(x, canvasEnd.y), IM_COL32(28, 45, 58, 255));
        draw->AddLine(ImVec2(canvasPosition.x, y), ImVec2(canvasEnd.x, y), IM_COL32(28, 45, 58, 255));
    }

    if (!live || gnssTrailMeters_.empty()) {
        const char* waiting = "GNSS 좌표 수신 시 실제 주행 궤적을 표시합니다";
        const ImVec2 textSize = ImGui::CalcTextSize(waiting);
        draw->AddText(ImVec2(canvasPosition.x + (canvasSize.x - textSize.x) * 0.5f,
                             canvasPosition.y + canvasSize.y * 0.5f),
                      ImGui::ColorConvertFloat4ToU32(COLOR_MUTED), waiting);
        ImGui::End();
        return;
    }

    float minX = gnssTrailMeters_.front().x, maxX = minX;
    float minY = gnssTrailMeters_.front().y, maxY = minY;
    for (const ImVec2& point : gnssTrailMeters_) {
        minX = std::min(minX, point.x); maxX = std::max(maxX, point.x);
        minY = std::min(minY, point.y); maxY = std::max(maxY, point.y);
    }
    const float spanX = std::max(20.0f, maxX - minX);
    const float spanY = std::max(20.0f, maxY - minY);
    const float scale = std::min((canvasSize.x - 32.0f) / spanX, (canvasSize.y - 32.0f) / spanY);
    const float centerX = (minX + maxX) * 0.5f;
    const float centerY = (minY + maxY) * 0.5f;
    auto toScreen = [&](const ImVec2& point) {
        return ImVec2(canvasPosition.x + canvasSize.x * 0.5f + (point.x - centerX) * scale,
                      canvasPosition.y + canvasSize.y * 0.5f - (point.y - centerY) * scale);
    };
    for (size_t i = 1; i < gnssTrailMeters_.size(); ++i) {
        draw->AddLine(toScreen(gnssTrailMeters_[i - 1]), toScreen(gnssTrailMeters_[i]),
                      IM_COL32(38, 199, 224, 220), 2.5f);
    }
    const ImVec2 vehicle = toScreen(gnssTrailMeters_.back());
    draw->AddCircleFilled(vehicle, 6.0f, IM_COL32(51, 214, 120, 255));
    const float headingRad = (t.gnssHeadingDeg - 90.0f) * 3.14159265f / 180.0f;
    draw->AddLine(vehicle, ImVec2(vehicle.x + std::cos(headingRad) * 18.0f,
                                  vehicle.y + std::sin(headingRad) * 18.0f),
                  IM_COL32(255, 190, 45, 255), 3.0f);
    ImGui::End();
}

bool DashboardUI::SendEVRelayPing() {
    SOCKET commandSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (commandSocket == INVALID_SOCKET) {
        strcpy_s(localControlStatus_, "UDP 소켓 생성 실패");
        return false;
    }
    sockaddr_in target = {};
    target.sin_family = AF_INET;
    target.sin_port = htons(9005);
    inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);
    ++controlRequestId_;
    char payload[128] = {};
    const int length = snprintf(payload, sizeof(payload),
                                "{\"type\":\"relay_ping\",\"request_id\":%u}", controlRequestId_);
    const int sent = sendto(commandSocket, payload, length, 0,
                            reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    closesocket(commandSocket);
    if (sent != length) {
        strcpy_s(localControlStatus_, "링크 시험 전송 실패");
        return false;
    }
    snprintf(localControlStatus_, sizeof(localControlStatus_), "링크 시험 %u 전송, ESP 확인 대기", controlRequestId_);
    return true;
}

bool DashboardUI::SendEVRecordingRequest(bool start) {
    SOCKET commandSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (commandSocket == INVALID_SOCKET) {
        strcpy_s(localControlStatus_, "UDP 소켓 생성 실패");
        return false;
    }
    sockaddr_in target = {};
    target.sin_family = AF_INET;
    target.sin_port = htons(9005);
    inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);
    const char* payload = start ? "{\"type\":\"recording_start\"}" :
                                  "{\"type\":\"recording_stop\"}";
    const int length = static_cast<int>(std::strlen(payload));
    const int sent = sendto(commandSocket, payload, length, 0,
                            reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    closesocket(commandSocket);
    if (sent != length) {
        strcpy_s(localControlStatus_, "측정 기록 명령 전송 실패");
        return false;
    }
    strcpy_s(localControlStatus_, start ? "측정 기록 시작 요청 전송" : "측정 기록 종료 요청 전송");
    return true;
}

bool DashboardUI::SendEVControlRequest() {
    SOCKET commandSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (commandSocket == INVALID_SOCKET) {
        strcpy_s(localControlStatus_, "UDP 소켓 생성 실패");
        return false;
    }
    sockaddr_in target = {};
    target.sin_family = AF_INET;
    target.sin_port = htons(9005);
    inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);
    ++controlRequestId_;
    char payload[512] = {};
    const int length = snprintf(
        payload, sizeof(payload),
        "{\"type\":\"pit_config\",\"request_id\":%u,\"tv_limit_pct\":%.0f,"
        "\"regen_limit_pct\":%.0f,\"tv_ramp_pct_s\":%d,\"regen_ramp_pct_s\":%d,"
        "\"tv_enable\":%s,\"regen_enable\":%s}",
        controlRequestId_, stagedTvLimit_, stagedRegenLimit_, stagedTvRamp_, stagedRegenRamp_,
        stagedTvEnable_ ? "true" : "false", stagedRegenEnable_ ? "true" : "false");
    const int sent = sendto(commandSocket, payload, length, 0,
                            reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    closesocket(commandSocket);
    if (sent != length) {
        strcpy_s(localControlStatus_, "피트 설정 전송 실패");
        return false;
    }
    snprintf(localControlStatus_, sizeof(localControlStatus_), "설정 %u 전송, Gateway/STM 응답 대기", controlRequestId_);
    return true;
}

bool DashboardUI::SendEVLiveControlRequest(bool neutral, bool heartbeat) {
    SOCKET commandSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (commandSocket == INVALID_SOCKET) {
        strcpy_s(localControlStatus_, "UDP 소켓 생성 실패");
        return false;
    }
    sockaddr_in target = {};
    target.sin_family = AF_INET;
    target.sin_port = htons(9005);
    inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);
    if (!heartbeat) ++controlRequestId_;
    const float strength = neutral ? 0.0f : (heartbeat ? activeLiveTvStrength_ : std::round(stagedTvStrength_ / 5.0f) * 5.0f);
    const float limit = heartbeat ? activeLiveTvLimit_ : std::round(stagedTvLimit_ / 5.0f) * 5.0f;
    const bool enabled = neutral ? false : (heartbeat ? liveCommandArmed_ : stagedLiveTvEnable_);
    char payload[256] = {};
    const int length = snprintf(payload, sizeof(payload),
                                "{\"type\":\"live_tv\",\"request_id\":%u,\"strength_pct\":%.0f,"
                                "\"limit_pct\":%.0f,\"tv_enable\":%s}",
                                controlRequestId_, strength, limit, enabled ? "true" : "false");
    const int sent = sendto(commandSocket, payload, length, 0,
                            reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    closesocket(commandSocket);
    if (sent != length) {
        strcpy_s(localControlStatus_, "실시간 TQV 전송 실패");
        return false;
    }
    if (neutral || !enabled) liveCommandArmed_ = false;
    else if (!heartbeat) {
        activeLiveTvStrength_ = strength;
        activeLiveTvLimit_ = limit;
        liveCommandArmed_ = true;
    }
    lastLiveHeartbeatTime_ = static_cast<double>(GetTickCount64()) / 1000.0;
    if (!heartbeat) snprintf(localControlStatus_, sizeof(localControlStatus_), "실시간 요청 %u 전송, Rear 응답 대기", controlRequestId_);
    return true;
}

void DashboardUI::RenderControl(const EVTelemetry& t, bool connected) {
    ImGui::SetNextWindowPos(ImVec2(300, 70), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(800, 760), ImGuiCond_Once);
    ImGui::Begin("TQV / 회생제동 피트 설정", &showControlWindow_, ImGuiWindowFlags_NoCollapse);

    if (!evControlInitialized_ && connected) {
        stagedTvStrength_ = t.tvRequestedPercent;
        stagedTvLimit_ = t.tvLimitPercent;
        stagedRegenLimit_ = t.regenLimitPercent;
        stagedTvRamp_ = static_cast<int>(t.tvRampPercentPerSecond);
        stagedRegenRamp_ = static_cast<int>(t.regenRampPercentPerSecond);
        evControlInitialized_ = true;
    }

    ImGui::TextColored(t.gatewayControlEnabled ? COLOR_WARN_AMBER : COLOR_OK_GREEN,
                       "Gateway: %s", t.gatewayControlEnabled ? "제어 허용 모드" : "읽기 전용 모드");
    ImGui::TextWrapped("모든 설정은 Rear STM 안전 조건을 우회하지 않습니다. 실차 출력 전에는 PIT ENABLE, 정차, TPS/RPM 및 오류 조건을 다시 확인합니다.");

    ImGui::SeparatorText("통신 링크 시험");
    const bool mayPing = connected && t.gatewayControlEnabled && t.relayLinkOnline;
    if (!mayPing) ImGui::BeginDisabled();
    if (ImGui::Button("ESP 링크 시험 (STM 출력 없음)", ImVec2(280, 34))) SendEVRelayPing();
    if (!mayPing) ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::Text("요청 %u", t.commandRequestId);

    if (ImGui::BeginTable("ControlState", 3, kTableFlags)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("휠 요청"); ImGui::Text("TQV %.0f%% · 회생 %.0f%%", t.tvRequestedPercent, t.regenRequestedPercent);
        ImGui::Text("SEQ %u · %s", t.driverControlSequence, t.driverControlFresh ? "최신" : "오래됨");
        ImGui::TableNextColumn(); ImGui::TextDisabled("Rear 적용"); ImGui::Text("TQV %.0f%% · 회생 %.0f%%", t.tvAppliedPercent, t.regenAppliedPercent);
        ImGui::Text("SEQ %u · %s", t.rearStatusSequence, t.limitReason);
        ImGui::TableNextColumn(); ImGui::TextDisabled("피트 제한"); ImGui::Text("TQV %.0f%% · 회생 %.0f%%", t.tvLimitPercent, t.regenLimitPercent);
        ImGui::Text("Ramp %.0f%%/s", t.tvRampPercentPerSecond);
        ImGui::EndTable();
    }

    ImGui::SeparatorText("실시간 TQV 요청");
    ImGui::SliderFloat("TQV 강도", &stagedTvStrength_, 0.0f, 100.0f, "%.0f %%");
    stagedTvStrength_ = std::round(stagedTvStrength_ / 5.0f) * 5.0f;
    ImGui::Checkbox("실시간 TQV 허용", &stagedLiveTvEnable_);
    const bool mayLiveSend = connected && t.gatewayControlEnabled && t.stmOnline && t.faultCode == 0;
    if (liveCommandArmed_) {
        if (!mayLiveSend) liveCommandArmed_ = false;
        else if (static_cast<double>(GetTickCount64()) / 1000.0 - lastLiveHeartbeatTime_ >= 0.25 &&
                 !SendEVLiveControlRequest(false, true)) liveCommandArmed_ = false;
    }
    if (!mayLiveSend) ImGui::BeginDisabled();
    if (ImGui::Button("실시간 TQV 전송", ImVec2(210, 34))) SendEVLiveControlRequest(false);
    if (!mayLiveSend) ImGui::EndDisabled();
    ImGui::SameLine();
    const bool mayNeutral = connected && t.gatewayControlEnabled && t.stmOnline;
    if (!mayNeutral) ImGui::BeginDisabled();
    if (ImGui::Button("50:50 중립 요청", ImVec2(210, 34))) {
        stagedTvStrength_ = 0.0f;
        stagedLiveTvEnable_ = false;
        SendEVLiveControlRequest(true);
    }
    if (!mayNeutral) ImGui::EndDisabled();
    ImGui::TextColored(liveCommandArmed_ ? COLOR_OK_GREEN : COLOR_MUTED,
                       "CMake heartbeat: %s", liveCommandArmed_ ? "작동" : "대기");

    ImGui::SeparatorText("정차 피트 설정");
    ImGui::SliderFloat("TQV 최대값", &stagedTvLimit_, 0.0f, 100.0f, "%.0f %%");
    ImGui::SliderFloat("회생 최대값", &stagedRegenLimit_, 0.0f, 100.0f, "%.0f %%");
    ImGui::SliderInt("TQV 변화율", &stagedTvRamp_, 10, 250, "%d %%/s");
    ImGui::SliderInt("회생 변화율", &stagedRegenRamp_, 5, 100, "%d %%/s");
    ImGui::Checkbox("TQV 허용", &stagedTvEnable_);
    ImGui::SameLine(); ImGui::Checkbox("회생 허용", &stagedRegenEnable_);
    const bool maySend = connected && t.gatewayControlEnabled && t.pitAdjustAllowed;
    if (!maySend) ImGui::BeginDisabled();
    if (ImGui::Button("STM에 임시 적용", ImVec2(220, 34))) SendEVControlRequest();
    if (!maySend) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("현재 제한값 복사", ImVec2(180, 34))) {
        stagedTvLimit_ = t.tvLimitPercent;
        stagedRegenLimit_ = t.regenLimitPercent;
        stagedTvRamp_ = static_cast<int>(t.tvRampPercentPerSecond);
        stagedRegenRamp_ = static_cast<int>(t.regenRampPercentPerSecond);
    }
    ImGui::Text("로컬: %s", localControlStatus_);
    const bool commandFailed = strcmp(t.commandStatus, "REJECTED") == 0 ||
                               strcmp(t.commandStatus, "FAILSAFE") == 0 ||
                               strcmp(t.commandStatus, "TIMEOUT") == 0;
    ImGui::TextColored(commandFailed ? COLOR_ALERT_RED :
                       (strcmp(t.commandStatus, "ACKED") == 0 ? COLOR_OK_GREEN : COLOR_INFO_CYAN),
                       "Gateway/STM: %s · %s", t.commandStatus, t.commandMessage);
    ImGui::TextDisabled("회생 적용값은 BMS 제한, 브레이크 입력 타당성, 모터 컨트롤러 검증 전까지 0으로 유지합니다.");
    ImGui::End();
}

void DashboardUI::RenderRecordedLog() {
    ImGui::SetNextWindowPos(ImVec2(230, 70), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(920, 760), ImGuiCond_Once);
    ImGui::Begin("실측 에너지 로그", &showRecordedLogWindow_, ImGuiWindowFlags_NoCollapse);

    if (ImGui::Button("FSK-EEM 로그 열기", ImVec2(190, 34))) {
        wchar_t path[4096] = {};
        OPENFILENAMEW dialog = {};
        dialog.lStructSize = sizeof(dialog);
        dialog.lpstrFilter = L"FSK Energy Meter Log (*.log)\0*.log\0All Files (*.*)\0*.*\0";
        dialog.lpstrFile = path;
        dialog.nMaxFile = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&dialog)) {
            recordedLog_.Load(path);
            selectedLogSample_ = 0;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("닫기", ImVec2(80, 34))) {
        recordedLog_.Clear();
        selectedLogSample_ = 0;
    }
    ImGui::Separator();

    if (!recordedLog_.IsLoaded()) {
        ImGui::TextColored(COLOR_INFO_CYAN, "실측 로그 파일을 선택하세요.");
        ImGui::TextDisabled("지원 형식: FSK-EEM .log");
        if (!recordedLog_.error.empty()) ImGui::TextColored(COLOR_ALERT_RED, "%s", recordedLog_.error.c_str());
        ImGui::End();
        return;
    }
    if (recordedLog_.IsVirtual()) {
        ImGui::TextColored(COLOR_ALERT_RED, "이 파일은 합성 데이터로 표시되어 실차 화면에서 열지 않습니다.");
        ImGui::End();
        return;
    }

    const std::string path = WideFieldToUtf8(recordedLog_.filePath.c_str(), recordedLog_.filePath.size());
    ImGui::TextColored(COLOR_OK_GREEN, "실측 장치 로그");
    ImGui::TextDisabled("파일 %s", path.c_str());
    if (ImGui::BeginTable("LogSummary", 4, kTableFlags)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("기록 시간"); ImGui::Text("%.2f s", recordedLog_.durationSeconds);
        ImGui::TableNextColumn(); ImGui::TextDisabled("샘플 / 주기"); ImGui::Text("%zu / %.2f Hz", recordedLog_.samples.size(), recordedLog_.averageRateHz);
        ImGui::TableNextColumn(); ImGui::TextDisabled("유효성"); ImGui::TextColored(recordedLog_.invalidPackets ? COLOR_ALERT_RED : COLOR_OK_GREEN, "%zu 오류", recordedLog_.invalidPackets);
        ImGui::TableNextColumn(); ImGui::TextDisabled("전압 범위"); ImGui::Text("%.1f ~ %.1f V", recordedLog_.minVoltage, recordedLog_.maxVoltage);
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("구동 에너지"); ImGui::Text("%.5f kWh", recordedLog_.driveEnergyKWh);
        ImGui::TableNextColumn(); ImGui::TextDisabled("회생 에너지"); ImGui::Text("%.5f kWh", recordedLog_.regenEnergyKWh);
        ImGui::TableNextColumn(); ImGui::TextDisabled("순 에너지"); ImGui::Text("%.5f kWh", recordedLog_.netEnergyKWh);
        ImGui::TableNextColumn(); ImGui::TextDisabled("전류 범위"); ImGui::Text("%.1f ~ %.1f A", recordedLog_.minCurrent, recordedLog_.maxCurrent);
        ImGui::EndTable();
    }

    const int maxIndex = static_cast<int>(recordedLog_.samples.size()) - 1;
    selectedLogSample_ = std::clamp(selectedLogSample_, 0, std::max(0, maxIndex));
    ImGui::SliderInt("##LogTimeline", &selectedLogSample_, 0, std::max(0, maxIndex), "샘플 %d");
    if (maxIndex < 0) {
        ImGui::TextColored(COLOR_ALERT_RED, "표시할 샘플이 없습니다.");
        ImGui::End();
        return;
    }
    const auto& sample = recordedLog_.samples[static_cast<size_t>(selectedLogSample_)];
    if (ImGui::BeginTable("LogSample", 5, kTableFlags)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("시간"); ImGui::Text("%.3f s", sample.timestampMs / 1000.0f);
        ImGui::TableNextColumn(); ImGui::TextDisabled("HV 전압"); ImGui::Text("%.1f V", sample.hvVoltage);
        ImGui::TableNextColumn(); ImGui::TextDisabled("HV 전류"); ImGui::Text("%.1f A", sample.hvCurrent);
        ImGui::TableNextColumn(); ImGui::TextDisabled("DC 전력"); ImGui::Text("%.1f kW", sample.powerKW);
        ImGui::TableNextColumn(); ImGui::TextDisabled("MCU 온도"); ImGui::Text("%.2f °C", sample.temperature);
        ImGui::EndTable();
    }
    auto voltage = [](void* data, int i) { return static_cast<EnergyLog*>(data)->samples[static_cast<size_t>(i)].hvVoltage; };
    auto current = [](void* data, int i) { return static_cast<EnergyLog*>(data)->samples[static_cast<size_t>(i)].hvCurrent; };
    auto power = [](void* data, int i) { return static_cast<EnergyLog*>(data)->samples[static_cast<size_t>(i)].powerKW; };
    const int count = static_cast<int>(recordedLog_.samples.size());
    ImGui::PlotLines("HV 전압", voltage, &recordedLog_, count, 0, nullptr, FLT_MAX, FLT_MAX, ImVec2(-1, 120));
    ImGui::PlotLines("HV 전류", current, &recordedLog_, count, 0, nullptr, FLT_MAX, FLT_MAX, ImVec2(-1, 120));
    ImGui::PlotLines("DC 전력", power, &recordedLog_, count, 0, nullptr, FLT_MAX, FLT_MAX, ImVec2(-1, 120));
    ImGui::End();
}
