#pragma once

#include "DataSource.h"
#include "EnergyLog.h"
#include "EVTelemetry.h"
#include "imgui.h"

#include <cstdint>
#include <deque>

inline const ImVec4 COLOR_ALERT_RED  = ImVec4(0.94f, 0.22f, 0.25f, 1.0f);
inline const ImVec4 COLOR_WARN_AMBER = ImVec4(1.00f, 0.70f, 0.12f, 1.0f);
inline const ImVec4 COLOR_INFO_CYAN  = ImVec4(0.15f, 0.78f, 0.88f, 1.0f);
inline const ImVec4 COLOR_OK_GREEN   = ImVec4(0.20f, 0.84f, 0.47f, 1.0f);
inline const ImVec4 COLOR_MUTED      = ImVec4(0.58f, 0.62f, 0.68f, 1.0f);

class DashboardUI {
public:
    DashboardUI();
    ~DashboardUI();

    void Render(IDataSource* dataSource);

private:
    bool showVehicleWindow_ = true;
    bool showBatteryWindow_ = true;
    bool showGnssWindow_ = true;
    bool showControlWindow_ = false;
    bool showRecordedLogWindow_ = false;

    bool evControlInitialized_ = false;
    float stagedTvStrength_ = 0.0f;
    bool stagedLiveTvEnable_ = false;
    bool liveCommandArmed_ = false;
    float activeLiveTvStrength_ = 0.0f;
    float activeLiveTvLimit_ = 100.0f;
    double lastLiveHeartbeatTime_ = 0.0;
    float stagedTvLimit_ = 100.0f;
    float stagedRegenLimit_ = 0.0f;
    int stagedTvRamp_ = 200;
    int stagedRegenRamp_ = 50;
    bool stagedTvEnable_ = true;
    bool stagedRegenEnable_ = false;
    std::uint32_t controlRequestId_ = 0;
    char localControlStatus_[128] = "전송한 명령 없음";

    bool gnssOriginValid_ = false;
    double gnssOriginLatitude_ = 0.0;
    double gnssOriginLongitude_ = 0.0;
    std::deque<ImVec2> gnssTrailMeters_;

    // IMU G-미터 표시용 영점. 실제 센서/제어 값에는 적용하지 않는다.
    float imuCenterLateralMS2_ = 0.0f;
    float imuCenterLongitudinalMS2_ = 0.0f;
    float imuCenterYawRateRadS_ = 0.0f;
    bool imuCenterCaptured_ = false;

    EnergyLog recordedLog_;
    int selectedLogSample_ = 0;

    void ApplyTheme();
    void UpdateGnssTrail(const EVTelemetry& telemetry);
    void RenderSystemStatus(IDataSource* source, const EVTelemetry& telemetry);
    void RenderVehicle(const EVTelemetry& telemetry);
    void RenderImuGeometry(const EVTelemetry& telemetry);
    void RenderBattery(const EVTelemetry& telemetry, bool connected);
    void RenderGnss(const EVTelemetry& telemetry, bool connected);
    void RenderControl(const EVTelemetry& telemetry, bool connected);
    void RenderRecordedLog();

    bool SendEVRelayPing();
    bool SendEVRecordingRequest(bool start);
    bool SendEVControlRequest();
    bool SendEVLiveControlRequest(bool neutral, bool heartbeat = false);
};
