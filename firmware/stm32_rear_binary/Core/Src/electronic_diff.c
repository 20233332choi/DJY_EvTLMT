#include "electronic_diff.h"
#include "vehicle_params.h"
#include "common_types.h"
#include "filters.h"
#include <math.h>

/* ΔP 출력 평활 — 조향 입력 노이즈가 그대로 모터 지령으로 새는 걸 막는다.
 * 조향은 물리적으로 저주파(수 Hz)라 fc=5Hz로 충분하고, ED는 폐루프가 아니라
 * 위상 여유를 신경 쓸 필요가 없어서 여기선 지연이 문제되지 않는다. */
static LPF1_t s_dp_lpf;

void ED_Init(void)  { ED_Reset(); }
void ED_Reset(void) { LPF1_Reset(&s_dp_lpf); }

float ED_ComputeDeltaPower(float delta_rad, float P_demand_kW, float v_mps)
{
    const float alpha = LPF1_Alpha(ED_LPF_FC_HZ, CONTROL_DT);

    /* 스로틀을 안 밟았거나 TV가 개입 중(P_demand=0으로 호출됨)이면 0으로 수렴 */
    if (P_demand_kW <= P_OFF_EPS_KW) {
        return LPF1_Update(&s_dp_lpf, 0.0f, alpha);
    }

    /* 애커만 기구학상 좌우 바퀴 속도비:
     *   v_left = v(1-k), v_right = v(1+k),  k = (T/2L)·tanδ   (δ>0 = 좌회전)
     * 오픈 디퍼렌셜은 좌우 토크가 같고 속도만 다르므로 전력비 = 속도비.
     *   P_left/P_right = (1-k)/(1+k),  P_left+P_right = P_demand
     *   →  P_left  = (P_demand/2)(1-k)
     *      P_right = (P_demand/2)(1+k)
     *      ΔP      = P_right - P_left = P_demand·k     (완전한 제로섬)
     * v가 소거되므로 차속을 몰라도 되고, 정지 상태에서도 유효하다. */
    float k = (TRACK_WIDTH / (2.0f * WHEELBASE)) * tanf(delta_rad);
    k = CLAMP(k, -ED_K_MAX, ED_K_MAX);   /* 조향각 이상치 대비 안전망 */

    /* 고속 페이드: 순수 기구학은 타이어 슬립각이 0이라는 전제라, 고속에서
     * 그대로 적용하면 차동을 과하게 준다. 선형으로 줄인다. */
    float fade = 1.0f;
    if (v_mps > ED_FADE_START_MPS) {
        float t = (v_mps - ED_FADE_START_MPS) / (ED_FADE_END_MPS - ED_FADE_START_MPS);
        fade = 1.0f - t * (1.0f - ED_FADE_MIN_GAIN);
        fade = CLAMP(fade, ED_FADE_MIN_GAIN, 1.0f);
    }

    float dP = P_demand_kW * k * ED_GAIN * fade;
    dP = CLAMP(dP, -ED_DELTA_MAX_KW, ED_DELTA_MAX_KW);

    /* ★TV와 달리 ΔF=ΔP/v 기반 저속 제한을 걸지 않는다. ED의 ΔP는 절대량이
     * 아니라 P_demand에 대한 "비율" 배분(기계식 오픈 디퍼렌셜과 동일한 동작)
     * 이라, 저속에서도 좌우 지령이 같은 비율로 줄어들 뿐 과대 개입이 아니다.
     * 반대로 TV의 ΔP는 PID가 만든 절대량이라 반드시 속도 제한이 필요하다. */
    return LPF1_Update(&s_dp_lpf, dP, alpha);
}
