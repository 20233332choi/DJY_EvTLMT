#ifndef ELECTRONIC_DIFF_H
#define ELECTRONIC_DIFF_H
#include <stdint.h>
#include <stdbool.h>

/* =====================================================================
 *  전자식 디퍼런셜 (Electronic Differential, ED)
 * ---------------------------------------------------------------------
 *  토크벡터링(TV)이 꺼져 있을 때만 동작하는 "최소한의 차동" 로직.
 *  TV가 실제로 개입 중일 때는 반드시 꺼진다 (torque_vectoring.c에서 배타 제어).
 *
 *  TV와의 차이:
 *    TV = 폐루프. IMU 요레이트 피드백 + PID. 차를 "돌린다".
 *    ED = 개루프. 조향각 + 애커만 기구학만. 안쪽 바퀴가 "끌리지 않게" 한다.
 *
 *  ★IMU를 전혀 쓰지 않는다. 따라서
 *    - IMU 고장/미장착(SAFE_ACTION_DISABLE_TV)에도 차동이 살아있고
 *    - 요레이트 캘리브레이션 실패와 무관하며
 *    - MIN_SPEED_FOR_TV 아래(정지~저속)에서도 정상 동작한다.
 *
 *  자세한 수식 유도는 vehicle_params.h의 ED 섹션 주석 참고.
 * ===================================================================== */

void  ED_Init(void);
void  ED_Reset(void);

/* 조향각과 운전자 요구전력으로 제로섬 차동전력 ΔP를 계산한다.
 *   반환값 > 0 → 우측 증가 / 좌측 감소 (좌회전 보조)
 *   TV가 개입 중이거나 ED를 쓰지 않는 사이클에는 P_demand_kW=0으로 호출할 것.
 *   그래야 내부 필터가 0으로 수렴해 있어서 ED로 복귀할 때 튀지 않는다.
 * @param delta_rad     조향각 [rad], +가 좌회전 (필터 통과값)
 * @param P_demand_kW   운전자 요구 총전력 [kW]
 * @param v_mps         차속 [m/s] — 고속 페이드에만 사용, 차동 계산 자체엔 불필요 */
float ED_ComputeDeltaPower(float delta_rad, float P_demand_kW, float v_mps);

#endif /* ELECTRONIC_DIFF_H */
