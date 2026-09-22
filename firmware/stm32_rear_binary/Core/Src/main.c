/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "fatfs.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "board_config.h"
#include "vehicle_params.h"
#include "common_types.h"
#include "filters.h"
#include "can_comm.h"
#include "imu_sensor.h"
#include "rpm_sensor.h"
#include "dac_output.h"
#include "torque_vectoring.h"
#include "esp_link.h"
#include "control_settings.h"
#include "electronic_diff.h"
#include "safety_monitor.h"
#include "sd_logger.h"
#include "can_messages.h"
#include "vehicle_clock.h"
#include "board_time_sync.h"
#include "timing_diag.h"
#include "djy_telemetry_protocol.h"
#include <string.h>
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* ★★ 벤치 테스트 모드 ★★
 * 1 = 모터를 안 돌리고 책상에서 시험할 때. RPM 센서 신호가 없으므로 TPS에
 *     비례한 가짜 RPM을 만들고 RPM freshness 검사를 우회한다.
 * 0 = 실차. 반드시 0으로 두고 차에 올릴 것.
 *
 * ★실차에 1로 올리면 매우 위험하다: 차속이 실제 속도가 아니라 페달 위치에서
 *  계산되므로, 정지 상태에서 페달만 밟아도 시스템이 "3000RPM으로 달리는 중"
 *  이라고 믿고 TV(폐루프)를 붙여버린다.
 *  예전 코드에는 이 우회 로직이 상수로 하드코딩돼 있었다 — 스위치로 뺐다. */
#define BENCH_TEST_MODE   0

/* ★★ 컨트롤러 CAN H/L 스니핑 모드 ★★
 * 1 = 컨트롤러의 CAN 비트레이트/ID 레이아웃을 모르는 상태에서 탐색할 때.
 *     켜면 main()이 CAN_SniffSweep()에서 리턴하지 않는다 — 평소 동작
 *     (IMU/RPM/DAC/TV/로깅)이 전혀 안 돈다. 결과는 USART2(115200)로 출력.
 * 0 = 평소. 반드시 0으로 두고 차에 올릴 것.
 *
 * ★★ 반드시 Board A와 물리적으로 분리한 상태에서만 켤 것 — CAN1을 재초기화
 * 하며 Board A와의 정상 통신을 깬다. 컨트롤러 CAN H/L은 STM32 PA11/PA12가
 * 아니라 Board A/B가 쓰는 것과 같은 CAN 트랜시버 모듈의 CANH/CANL 단자에
 * 물릴 것(트랜시버는 그대로 두고 버스 배선만 바꿔 물림). 자세한 내용은
 * can_comm.c의 CAN_SniffSweep() 주석 참고. */
#define CAN_SNIFF_MODE    0

/* ★★ DAC 셀프테스트 모드 ★★
 * 1 = DAC 좌우 채널을 단독으로 검증할 때. Board A/CAN/SD 전혀 필요 없다.
 *     좌우에 동일한 코드를 단계별로 써 넣으며 USART2(115200)로 출력한다.
 *     각 단계에서 PA4/PA5를 재보면 어느 채널이 명령을 안 따르는지 바로 보인다.
 *     켜면 main()이 DAC_SelfTest()에서 리턴하지 않는다.
 * 0 = 평소. 반드시 0으로 두고 차에 올릴 것. */
#define DAC_SELFTEST_MODE 0
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
CAN_HandleTypeDef hcan1;

SPI_HandleTypeDef hspi3;

SPI_HandleTypeDef hspi2;

#if DAC_USE_INTERNAL
DAC_HandleTypeDef hdac;   /* ★임시 백엔드 전용 — vehicle_params.h 참고 */
#endif

TIM_HandleTypeDef htim3;
TIM_HandleTypeDef htim6;

UART_HandleTypeDef huart1;   /* ESP32 링크 */
UART_HandleTypeDef huart2;
UART_HandleTypeDef huart4;
DMA_HandleTypeDef hdma_uart4_rx;

IWDG_HandleTypeDef hiwdg;

/* USER CODE BEGIN PV */
static TV_t tv;
static volatile bool s_dbg_print_flag = false;   /* USART2로 RPM 출력 트리거 */
static volatile uint32_t s_telemetry_sequence = 0; /* Includes skipped 100 Hz requests. */

/* 입력 필터 상태 (100Hz 제어 ISR 전용 — 다른 컨텍스트에서 건드리지 말 것) */
static Deglitch_t s_tps_dg,  s_sas_dg;
static LPF1_t     s_tps_lpf, s_sas_lpf;

/* 페달 정지 위치 — 부팅 시 TPS_LearnIdle()이 실측값으로 갱신한다.
 * 학습이 거부되면 벤치 실측 기본값이 그대로 남는다. */
static uint16_t   s_tps_idle = TPS_PEDAL_IDLE;

/* ── 워치독 상태 ──────────────────────────────────────────────────────
 * ★"양쪽 생존 확인" 방식. 제어 ISR이 이 플래그를 세우고, 메인 루프는
 *  플래그가 서 있을 때만 IWDG를 갱신한다. 한쪽만 확인하면 구멍이 생긴다:
 *    - ISR에서만 갱신 → 메인 루프(SD/IMU 파싱)가 멎어도 계속 갱신됨
 *    - 메인에서만 갱신 → 제어 ISR이 멎어 DAC가 얼어붙어도 계속 갱신됨
 *  둘 다 돌아야 갱신되므로 어느 쪽이 멎어도 리셋된다. */
static volatile bool s_ctrl_isr_alive = false;
/* 직전 리셋이 워치독 때문이었는지 — 부팅 시 1회 판정 */
static bool          s_iwdg_reset     = false;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_CAN1_Init(void);
static void MX_SPI3_Init(void);
#if DAC_USE_INTERNAL
static void MX_DAC_Init(void);
#endif
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_UART4_Init(void);
static void MX_SPI2_Init(void);
static void MX_TIM6_Init(void);
static void MX_TIM3_Init(void);
static void MX_IWDG_Init(void);
/* USER CODE BEGIN PFP */
static float SAS_to_SteeringAngle(float raw);
static float TPS_to_Fraction(float raw);
static void  TPS_LearnIdle(uint32_t duration_ms);
static void  Telemetry_Publish(uint32_t sequence);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */

	HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
  VehicleClock_Init();
  /* ★직전 리셋이 워치독이었는지 판정 — 반드시 플래그를 지우기 전에 읽는다.
   * 주행 중 워치독이 걸렸다면 차가 움직이는 상태로 부팅하는 것이므로,
   * 아래에서 자이로 0점 캘리브레이션을 건너뛰고 TV를 막는다. */
  s_iwdg_reset = (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) != RESET);
  __HAL_RCC_CLEAR_RESET_FLAGS();
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_CAN1_Init();
  MX_SPI3_Init();
#if DAC_USE_INTERNAL
  MX_DAC_Init();
#endif
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();
  HAL_NVIC_SetPriority(USART2_IRQn,3,0);
  HAL_NVIC_EnableIRQ(USART2_IRQn);
  MX_UART4_Init();
  MX_SPI2_Init();
  MX_TIM6_Init();
  MX_FATFS_Init();
  MX_TIM3_Init();
  /* USER CODE BEGIN 2 */
#if CAN_SNIFF_MODE
  /* ★TEMP: 리턴하지 않는다. 아래 정상 초기화/제어루프는 전혀 안 돈다.
   * 확인 끝나면 CAN_SNIFF_MODE를 0으로 되돌릴 것. */
  CAN_SniffSweep();
#endif
  CAN_Init();
   IMU_Init();
   RPM_Init();
   DAC_Output_Init();
#if DAC_SELFTEST_MODE
   /* ★TEMP: 리턴하지 않는다. 아래 초기화/제어루프는 전혀 안 돈다.
    * 확인 끝나면 DAC_SELFTEST_MODE를 0으로 되돌릴 것. */
   DAC_SelfTest();
#endif
   SD_Logger_Init();
   Safety_Init();
   TV_Init();          /* TV + ED(전자식 디퍼런셜) 동시 초기화 */
   ControlSettings_Init();
   EspLink_Init(&huart1);   /* ESP32 → TV 강도 명령 (10Hz, 9바이트 CRC 프레임) */
   DAC_SetSafeState();

   /* 입력 필터 초기화 — 첫 샘플이 그대로 통과하므로 부팅 시 튐 없음 */
   Deglitch_Reset(&s_tps_dg);  Deglitch_Reset(&s_sas_dg);
   LPF1_Reset(&s_tps_lpf);     LPF1_Reset(&s_sas_lpf);

   /* ★차량 완전 정지 상태에서 1초간 자이로 0점 캘리브레이션. 여기서 흔들리거나
    * 움직이면 캘리브레이션이 거부되거나(바이어스 이상치) 부정확해질 수 있다.
    *
    * ★워치독 리셋 직후에는 건너뛴다. 그 경우 차가 주행 중일 가능성이 높아서
    *  ① 움직이는 차에서 0점을 잡으면 바이어스가 통째로 틀어지고
    *  ② 1초를 블로킹하는 동안 출력이 계속 0이라 복귀가 늦어진다.
    *  대신 바이어스 0으로 두고 TV를 막는다(아래) — ED는 IMU를 안 쓰므로
    *  개루프 디퍼런셜로 정상 동작한다. */
   if (!s_iwdg_reset) IMU_Calibrate(1000);
   else {
       const char *m = "!! IWDG RESET — skip IMU calib, TV disabled (ED only)\r\n";
       HAL_UART_Transmit(&huart2, (uint8_t *)m, (uint16_t)strlen(m), 50);
       TV_SetTVEnabled(false);
   }
   {
       char cal_msg[96];
       /* 캘리브레이션 직후라 정지 상태 그대로면 두 값 모두 0 근처여야 정상.
        * lat는 보정 후 잔차 — 보정 전에는 IMU 기울기(1도당 약 171mm/s^2)와
        * 센서 zero-g offset 때문에 보통 100~500mm/s^2 나온다. */
       int  residual_mrad = (int)(IMU_GetYawRate() * 1000.0f);
       int  lat_mm        = (int)(IMU_GetLateralAcc() * 1000.0f);
       int  n = snprintf(cal_msg, sizeof(cal_msg),
                          "IMU calib: %s (yaw=%dmrad/s lat=%dmm/s2)\r\n",
                          IMU_IsCalibrated() ? "OK" : "FAIL", residual_mrad, lat_mm);
       if (n > 0) HAL_UART_Transmit(&huart2, (uint8_t *)cal_msg, (uint16_t)n, 50);
   }

   /* ★페달에서 발을 뗀 상태로 부팅해야 한다. IMU 캘리브레이션이 이미 1초를
    * 블로킹했으므로 이 시점에는 Board A의 CAN 데이터가 들어와 있다. */
   TPS_LearnIdle(TPS_IDLE_LEARN_MS);
   {
       char msg[80];
       int n = snprintf(msg, sizeof(msg),
                        "TPS idle: %u (%s) -> 0%%=%u 100%%=%u\r\n",
                        s_tps_idle,
                        (s_tps_idle == TPS_PEDAL_IDLE) ? "default" : "learned",
                        (unsigned)(s_tps_idle + TPS_DEADBAND_RAW),
                        (unsigned)(TPS_PEDAL_FULL - TPS_FULL_MARGIN_RAW));
       if (n > 0) HAL_UART_Transmit(&huart2, (uint8_t *)msg, (uint16_t)n, 50);
   }

   /* ★워치독은 여기서 시작한다 — IMU_Calibrate(1초)/TPS_LearnIdle(300ms) 같은
    * 긴 블로킹 초기화가 모두 끝난 뒤여야 한다. 초기화 중에 켜면 부팅이
    * 리셋 루프에 빠진다. IWDG는 한 번 켜면 끌 수 없다. */
   MX_IWDG_Init();
   Timing_Start(); /* Exclude boot/calibration from running-control timing. */
   HAL_TIM_Base_Start_IT(&htim6);   /* 100Hz 제어 루프 */
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
	  IMU_ProcessData();   /* UART DMA 버퍼 파싱 */
	     IMU_Watchdog();      /* UART 에러로 DMA가 멎으면 자동 복구 */
	     EspLink_Watchdog();  /* ESP32 수신이 죽으면 되살림 */
	     BoardTimeSync_Poll();
	     SD_Logger_Flush();   /* SD 기록 */
	     if (s_dbg_print_flag) {
	         uint32_t mask = __get_PRIMASK();
	         __disable_irq();
	         uint32_t sequence = s_telemetry_sequence;
	         s_dbg_print_flag = false;
	         __set_PRIMASK(mask);
	         Telemetry_Publish(sequence);
	     }
	     Timing_Publish(); /* Service diagnostics after async status TX completes. */
	     /* ★IWDG 갱신 — 제어 ISR이 플래그를 세웠을 때만. 여기까지 왔다는 건
	      * 메인 루프가 살아있다는 뜻이고, 플래그가 서 있다는 건 제어 ISR도
	      * 살아있다는 뜻이다. 둘 다 확인돼야 갱신한다(PV 블록 주석 참고). */
	     if (s_ctrl_isr_alive) {
	         s_ctrl_isr_alive = false;
	         HAL_IWDG_Refresh(&hiwdg);
	     }
}
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief CAN1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_CAN1_Init(void)
{

  /* USER CODE BEGIN CAN1_Init 0 */

  /* USER CODE END CAN1_Init 0 */

  /* USER CODE BEGIN CAN1_Init 1 */

  /* USER CODE END CAN1_Init 1 */
  hcan1.Instance = CAN1;
  hcan1.Init.Prescaler = 6;
  hcan1.Init.Mode = CAN_MODE_NORMAL;
  hcan1.Init.SyncJumpWidth = CAN_SJW_1TQ;
  hcan1.Init.TimeSeg1 = CAN_BS1_10TQ;
  hcan1.Init.TimeSeg2 = CAN_BS2_3TQ;
  hcan1.Init.TimeTriggeredMode = DISABLE;
  hcan1.Init.AutoBusOff = DISABLE;
  hcan1.Init.AutoWakeUp = DISABLE;
  hcan1.Init.AutoRetransmission = DISABLE;
  hcan1.Init.ReceiveFifoLocked = DISABLE;
  hcan1.Init.TransmitFifoPriority = DISABLE;
  if (HAL_CAN_Init(&hcan1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN CAN1_Init 2 */

  /* USER CODE END CAN1_Init 2 */

}

/**
  * @brief SPI3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI3_Init(void)
{

  /* USER CODE BEGIN SPI3_Init 0 */

  /* USER CODE END SPI3_Init 0 */

  /* USER CODE BEGIN SPI3_Init 1 */

  /* USER CODE END SPI3_Init 1 */
  /* SPI3 parameter configuration — 외부 MCP4822 DAC 전용 (쓰기 전용, MISO 미사용)
   * MCP4822는 SPI 모드 0,0 또는 1,1을 지원. 여기선 모드 0(CPOL=0, CPHA=1Edge).
   * 프리스케일러 32 → 42MHz/32 ≈ 1.3MHz. 16비트 전송이 약 12us라 100Hz 제어
   * 루프에서 무시할 수준이고, 아이솔레이터/배선을 거쳐도 여유 있는 속도다. */
  hspi3.Instance = SPI3;
  hspi3.Init.Mode = SPI_MODE_MASTER;
  hspi3.Init.Direction = SPI_DIRECTION_2LINES;
  hspi3.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi3.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi3.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi3.Init.NSS = SPI_NSS_SOFT;
  hspi3.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_32;
  hspi3.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi3.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi3.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi3.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI3_Init 2 */

  /* USER CODE END SPI3_Init 2 */
}

#if DAC_USE_INTERNAL
/**
  * @brief DAC Initialization Function (★임시 백엔드 전용)
  * @param None
  * @retval None
  */
static void MX_DAC_Init(void)
{
  DAC_ChannelConfTypeDef sConfig = {0};

  hdac.Instance = DAC;
  if (HAL_DAC_Init(&hdac) != HAL_OK)
  {
    Error_Handler();
  }

  sConfig.DAC_Trigger = DAC_TRIGGER_NONE;
  sConfig.DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE;
  if (HAL_DAC_ConfigChannel(&hdac, &sConfig, DAC_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_DAC_ConfigChannel(&hdac, &sConfig, DAC_CHANNEL_2) != HAL_OK)
  {
    Error_Handler();
  }
}
#endif

/**
  * @brief SPI2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI2_Init(void)
{

  /* USER CODE BEGIN SPI2_Init 0 */

  /* USER CODE END SPI2_Init 0 */

  /* USER CODE BEGIN SPI2_Init 1 */

  /* USER CODE END SPI2_Init 1 */
  /* SPI2 parameter configuration*/
  hspi2.Instance = SPI2;
  hspi2.Init.Mode = SPI_MODE_MASTER;
  hspi2.Init.Direction = SPI_DIRECTION_2LINES;
  hspi2.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi2.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi2.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi2.Init.NSS = SPI_NSS_SOFT;
  hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4;
  hspi2.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi2.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi2.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi2.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI2_Init 2 */

  /* USER CODE END SPI2_Init 2 */

}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
/**
  * @brief IWDG Initialization Function (독립 워치독)
  * ★LSI(약 32kHz)로 동작하므로 시스템 클럭이 죽어도 살아있다. 한 번 시작하면
  *  소프트웨어로 끌 수 없으니, 긴 블로킹 초기화가 모두 끝난 뒤에 호출할 것.
  *  타임아웃 계산은 vehicle_params.h의 IWDG_* 주석 참고.
  * @retval None
  */
static void MX_IWDG_Init(void)
{
  hiwdg.Instance       = IWDG;
  hiwdg.Init.Prescaler = IWDG_PRESCALER_DIV;
  hiwdg.Init.Reload    = IWDG_RELOAD_COUNT;
  if (HAL_IWDG_Init(&hiwdg) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_TIM3_Init(void)
{

  /* USER CODE BEGIN TIM3_Init 0 */

  /* USER CODE END TIM3_Init 0 */

  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_IC_InitTypeDef sConfigIC = {0};

  /* USER CODE BEGIN TIM3_Init 1 */

  /* USER CODE END TIM3_Init 1 */
  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 839;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 65535;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_IC_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigIC.ICPolarity = TIM_INPUTCHANNELPOLARITY_RISING;
  sConfigIC.ICSelection = TIM_ICSELECTION_DIRECTTI;
  sConfigIC.ICPrescaler = TIM_ICPSC_DIV1;
  sConfigIC.ICFilter = 8;
  if (HAL_TIM_IC_ConfigChannel(&htim3, &sConfigIC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_IC_ConfigChannel(&htim3, &sConfigIC, TIM_CHANNEL_2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM3_Init 2 */

  /* USER CODE END TIM3_Init 2 */

}

/**
  * @brief TIM6 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM6_Init(void)
{

  /* USER CODE BEGIN TIM6_Init 0 */

  /* USER CODE END TIM6_Init 0 */

  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM6_Init 1 */

  /* USER CODE END TIM6_Init 1 */
  htim6.Instance = TIM6;
  htim6.Init.Prescaler = 839;
  htim6.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim6.Init.Period = 999;
  htim6.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim6) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim6, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM6_Init 2 */

  /* USER CODE END TIM6_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
/**
  * @brief USART1 Initialization Function — ESP32 전용 링크
  * PA9=TX / PA10=RX (CN5-1 / CN9-3), DJY_TELEMETRY_BAUD (460800) 8N1.
  * ESP32가 10Hz로 9바이트 TV 강도 명령(djy_uart_protocol.h)을 보내온다.
  * 수신은 바이트 단위 인터럽트(esp_link.c) — DMA는 IMU가 이미 쓰고 있어 안 겹친다.
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{
  huart1.Instance = USART1;
  huart1.Init.BaudRate = DJY_TELEMETRY_BAUD;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief USART3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_UART4_Init(void)
{

  /* USER CODE BEGIN UART4_Init 0 */

  /* USER CODE END UART4_Init 0 */

  /* USER CODE BEGIN UART4_Init 1 */

  /* USER CODE END UART4_Init 1 */
  huart4.Instance = UART4;
  huart4.Init.BaudRate = 115200;
  huart4.Init.WordLength = UART_WORDLENGTH_8B;
  huart4.Init.StopBits = UART_STOPBITS_1;
  huart4.Init.Parity = UART_PARITY_NONE;
  huart4.Init.Mode = UART_MODE_TX_RX;
  huart4.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart4.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart4) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN UART4_Init 2 */

  /* USER CODE END UART4_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA1_Stream2_IRQn interrupt configuration — UART4_RX (IMU) */
  HAL_NVIC_SetPriority(DMA1_Stream2_IRQn, 2, 0);
  HAL_NVIC_EnableIRQ(DMA1_Stream2_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, LED_Pin|SD_CS_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level — ★DAC_CS는 반드시 High(비선택)로 시작.
   * Low인 채로 SPI가 돌면 MCP4822에 쓰레기 데이터가 들어간다. */
  HAL_GPIO_WritePin(DAC_CS_GPIO_Port, DAC_CS_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin : TV_SWITCH_Pin */
  GPIO_InitStruct.Pin = TV_SWITCH_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(TV_SWITCH_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : LED_Pin */
  GPIO_InitStruct.Pin = LED_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : SD_CS_Pin */
  GPIO_InitStruct.Pin = SD_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_MEDIUM;
  HAL_GPIO_Init(SD_CS_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : DAC_CS_Pin (외부 MCP4822 칩셀렉트) */
  GPIO_InitStruct.Pin = DAC_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_MEDIUM;
  HAL_GPIO_Init(DAC_CS_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */
  /* ★CubeMX가 이 프로젝트에서 타이머 Input Capture 핀의 GPIO 대체기능 설정을
   * 자동생성 안 해주는 문제가 있어 수동으로 추가함(중복 안전장치 — 정상적으로는
   * stm32f4xx_hal_msp.c의 HAL_TIM_IC_MspInit()에서 생성된다).
   * .ioc를 다시 Generate Code 해도 이 USER CODE 블록은 보존되니 안전함. */
  GPIO_InitStruct.Pin       = GPIO_PIN_4 | GPIO_PIN_5;   /* PB4=CH1(좌), PB5=CH2(우) */
  GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull      = GPIO_PULLUP;   /* SPD 오픈컬렉터 대비 풀업 */
  GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.Alternate = GPIO_AF2_TIM3;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
static float SAS_to_SteeringAngle(float raw) {
    float a = (raw - (float)SAS_CENTER_RAW) * SAS_RAW_TO_RAD
              * SAS_TO_STEERING_RATIO;
    return CLAMP(a, -MAX_STEERING_ANGLE_RAD, MAX_STEERING_ANGLE_RAD);
}

/* 페달 유격(데드밴드)과 끝단 여유를 반영한 매핑.
 *   raw ≤ idle+DEADBAND        → 0%   (유격 구간, 출력 안 나감)
 *   raw ≥ FULL-FULL_MARGIN     → 100% (스토퍼가 먼저 닿아도 풀파워 보장)
 * 그 사이만 선형. idle은 부팅 시 학습된 s_tps_idle을 쓴다. */
static float TPS_to_Fraction(float raw) {
    float lo = (float)s_tps_idle + (float)TPS_DEADBAND_RAW;
    float hi = (float)TPS_PEDAL_FULL - (float)TPS_FULL_MARGIN_RAW;

    /* 안전망: 어떤 이유로든 구간이 무너지면 매핑이 폭주하므로 기본값으로 되돌린다 */
    if ((hi - lo) < 200.0f) {
        lo = (float)TPS_PEDAL_IDLE + (float)TPS_DEADBAND_RAW;
        hi = (float)TPS_PEDAL_FULL - (float)TPS_FULL_MARGIN_RAW;
    }

    if (raw <= lo) return 0.0f;
    if (raw >= hi) return 1.0f;
    return (raw - lo) / (hi - lo);
}

/* 부팅 시 페달 정지 위치 학습 — 반드시 페달에서 발을 뗀 상태로 부팅할 것.
 * Board A의 CAN 데이터를 duration_ms 동안 모아 평균낸다. 블로킹 함수이므로
 * 제어 루프(TIM6) 시작 전에 호출한다. 거부되면 기본값(TPS_PEDAL_IDLE) 유지. */
static void TPS_LearnIdle(uint32_t duration_ms) {
    uint32_t start = HAL_GetTick();
    uint32_t sum = 0, n = 0;
    uint16_t mn = 0xFFFFu, mx = 0;

    while ((HAL_GetTick() - start) < duration_ms) {
        if (CAN_IsSensorFresh()) {
            uint16_t r = CAN_GetSensorData().tps_raw;
            sum += r; n++;
            if (r < mn) mn = r;
            if (r > mx) mx = r;
        }
        HAL_Delay(10);   /* Board A 송신 주기(100Hz)에 맞춤 */
    }

    if (n < 10) return;                                  /* CAN 데이터 부족 */
    if ((int)mx - (int)mn > TPS_IDLE_LEARN_SPREAD) return;   /* 페달이 움직였음 */

    int avg = (int)(sum / n);
    if (avg < (TPS_PEDAL_IDLE - TPS_IDLE_LEARN_WINDOW)) return;  /* 너무 낮음 → 거부 */
    if (avg > (TPS_PEDAL_IDLE + TPS_IDLE_LEARN_WINDOW)) return;  /* 밟은 채 부팅 → 거부 */

    s_tps_idle = (uint16_t)avg;
}

/* ★TV 토글 스위치 디바운스 함수는 Board A로 옮겼다(stm_front/Core/Src/main.c).
 * 스위치가 조종석에 있어 앞 보드 배선이 훨씬 짧고, 앞 보드는 이미 100Hz로
 * CAN을 보내고 있어서 추가 프레임 없이 바이트 하나만 실으면 된다.
 * Board B는 CAN_IsTVSwitchOn()으로 읽는다. */

/* 100 Hz main-loop snapshots on UART1; USB ASCII remains at most 5 Hz.
 * UART1 owns frame until READY. USB owns line independently. Neither blocks
 * control; CRC, packing, formatting and TX run with interrupts enabled. */
static void Telemetry_Publish(uint32_t sequence) {
    static uint8_t frame[DJY_TELEMETRY_SIZE];
    static char line[768];
    static uint32_t sent_count;
    static uint64_t usb_last_us;
    static bool usb_sent;
    if (huart1.gState != HAL_UART_STATE_READY) return;
    DjyTelemetry packet = {0};
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    TV_t t = tv;
    SensorData_t s = CAN_GetSensorData();
    DjyUartLiveTv live = {0};
    bool fresh = EspLink_GetLiveTv(&live);
    bool sas_valid = CAN_IsSensorFresh() && CAN_IsHeartbeatFresh() &&
                     !(CAN_GetHeartbeatStatus() & HB_STATUS_SAS_ERR);
    bool imu_valid = IMU_IsTelemetryFresh();
    bool left_valid = RPM_IsLeftFresh(), right_valid = RPM_IsRightFresh();
    unsigned rpm_l = RPM_GetLeft(), rpm_r = RPM_GetRight();
    unsigned applied = (unsigned)(TV_GetStrength() * 100.0f + 0.5f);
    unsigned fault = (unsigned)Safety_GetFaultCode();
    unsigned dac_l = t.dac_left, dac_r = t.dac_right;
#if DAC_USE_INTERNAL
    dac_l = (unsigned)DAC->DOR1;
    dac_r = (unsigned)DAC->DOR2;
#endif
    float yaw = IMU_GetYawRate(), lat = IMU_GetLateralAcc();
    packet.sequence = sequence;
    packet.snapshot_us = VehicleClock_NowUs();
    packet.tx_skipped = sequence - sent_count - 1u;
    packet.rpm_left = (uint16_t)rpm_l; packet.rpm_right = (uint16_t)rpm_r;
    packet.tps_raw = s.tps_raw; packet.tps_idle = s_tps_idle;
    packet.sas_raw = s.sas_angle; packet.sas_center = SAS_CENTER_RAW;
    packet.dac_left = (uint16_t)dac_l; packet.dac_right = (uint16_t)dac_r;
    packet.traction_milli = (uint16_t)(t.traction_scale * 1000.0f);
    packet.tps_pct = (uint8_t)(TPS_to_Fraction((float)s.tps_raw) * 100.0f);
    packet.flags = (imu_valid ? DJY_TM_IMU_VALID : 0u) |
        (sas_valid ? DJY_TM_SAS_VALID : 0u) |
        (left_valid ? DJY_TM_RPM_LEFT_VALID : 0u) |
        (right_valid ? DJY_TM_RPM_RIGHT_VALID : 0u) |
        (fresh ? DJY_TM_CONTROL_FRESH : 0u) |
        (t.tv_active ? DJY_TM_TV_ACTIVE : 0u) | (t.ed_active ? DJY_TM_ED_ACTIVE : 0u);
    packet.fault = (uint8_t)fault; packet.command_sequence = live.sequence;
    packet.requested = fresh ? live.strength_percent : (uint8_t)applied;
    packet.limit = fresh ? live.limit_percent : 100u; packet.applied = (uint8_t)applied;
    packet.yaw_milli = (int32_t)(yaw * 1000.0f);
    packet.lat_milli = (int32_t)(lat * 1000.0f);
    packet.ax_milli = (int32_t)(IMU_GetAccelerationX() * 1000.0f);
    packet.ay_milli = (int32_t)(IMU_GetAccelerationY() * 1000.0f);
    packet.az_milli = (int32_t)(IMU_GetAccelerationZ() * 1000.0f);
    packet.speed_milli = (int32_t)(t.vehicle_speed * 1000.0f);
    packet.desired_yaw_milli = (int32_t)(t.desired_yaw * 1000.0f);
    packet.yaw_error_milli = (int32_t)(t.yaw_error * 1000.0f);
    packet.delta_power_milli = (int32_t)(t.delta_power * 1000.0f);
    packet.power_left_milli = (int32_t)(t.power_left * 1000.0f);
    packet.power_right_milli = (int32_t)(t.power_right * 1000.0f);
    packet.steer_milli = (int32_t)(SAS_to_SteeringAngle((float)s.sas_angle) * 1000.0f);
    packet.kp_milli = (int32_t)(PID_KP * 1000.0f);
    packet.ki_milli = (int32_t)(PID_KI * 1000.0f);
    packet.kd_milli = (int32_t)(PID_KD * 1000.0f);
    packet.capture_left = g_rpm_cap_count_l; packet.capture_right = g_rpm_cap_count_r;
    packet.glitch_left = g_rpm_glitch_l; packet.glitch_right = g_rpm_glitch_r;
    packet.command_rx = g_esp_rx_count; packet.command_errors = g_esp_crc_err + g_esp_uart_err;
    packet.can_rx = g_can_rx_count; packet.can_errors = g_can_err_count;
    packet.can_status = g_can_last_esr;
    packet.imu_gyro_ok = g_imu_gyro_ok; packet.imu_pkt_bad = g_imu_pkt_bad;
    packet.imu_resync = g_imu_resync; packet.imu_dma_restart = g_imu_dma_restart;
    __set_PRIMASK(mask);
    packet.timing_valid = Timing_ReadRelay(packet.timing);
    djy_telemetry_pack(frame, &packet);
    if (HAL_UART_Transmit_IT(&huart1, frame, sizeof(frame)) == HAL_OK) ++sent_count;
    if (huart2.gState != HAL_UART_STATE_READY) return;
    if (usb_sent && packet.snapshot_us - usb_last_us < 200000u) return;
    int n = snprintf(line, sizeof(line),
        "L=%u R=%u cap=%lu/%lu glt=%lu/%lu tps=%u pct=%d idle=%u "
        "imu=%u sas=%u yaw=%ld lat=%ld lon=%ld ax=%ld ay=%ld az=%ld "
        "vs=%ld dy=%ld ye=%ld dp=%ld pl=%ld pr=%ld tva=%u eda=%u tr=%ld "
        "ctl=%u/%u req=%u lim=%u app=%u tv=%u ed=%u fault=%u dac=%u/%u sas=%u imu=%u urx=%lu uerr=%lu "
        "rv=%u/%u steer=%ld sv=%u sc=%u can=%lu/%lu/%08lx idg=%lu/%lu/%lu/%lu "
        "kp=%ld ki=%ld kd=%ld\r\n",
        rpm_l, rpm_r, (unsigned long)g_rpm_cap_count_l, (unsigned long)g_rpm_cap_count_r,
        (unsigned long)g_rpm_glitch_l, (unsigned long)g_rpm_glitch_r,
        (unsigned)s.tps_raw, (int)(TPS_to_Fraction((float)s.tps_raw) * 100.0f), (unsigned)s_tps_idle,
        (unsigned)imu_valid, (unsigned)s.sas_angle, (long)(yaw * 1000.0f), (long)(lat * 1000.0f),
        (long)(IMU_GetAccelerationX() * 1000.0f), (long)(IMU_GetAccelerationX() * 1000.0f),
        (long)(IMU_GetAccelerationY() * 1000.0f), (long)(IMU_GetAccelerationZ() * 1000.0f),
        (long)(t.vehicle_speed * 1000.0f), (long)(t.desired_yaw * 1000.0f),
        (long)(t.yaw_error * 1000.0f), (long)(t.delta_power * 1000.0f),
        (long)(t.power_left * 1000.0f), (long)(t.power_right * 1000.0f),
        (unsigned)t.tv_active, (unsigned)t.ed_active, (long)(t.traction_scale * 1000.0f),
        (unsigned)fresh, (unsigned)live.sequence, fresh ? (unsigned)live.strength_percent : applied,
        fresh ? (unsigned)live.limit_percent : 100u, applied, (unsigned)t.tv_active,
        (unsigned)t.ed_active, fault, dac_l, dac_r, (unsigned)s.sas_angle, (unsigned)imu_valid,
        (unsigned long)g_esp_rx_count, (unsigned long)(g_esp_crc_err + g_esp_uart_err),
        (unsigned)left_valid, (unsigned)right_valid,
        (long)(SAS_to_SteeringAngle((float)s.sas_angle) * 1000.0f),
        (unsigned)sas_valid, (unsigned)SAS_CENTER_RAW,
        (unsigned long)g_can_rx_count, (unsigned long)g_can_err_count, (unsigned long)g_can_last_esr,
        (unsigned long)g_imu_gyro_ok, (unsigned long)g_imu_pkt_bad,
        (unsigned long)g_imu_resync, (unsigned long)g_imu_dma_restart,
        (long)(PID_KP * 1000.0f), (long)(PID_KI * 1000.0f), (long)(PID_KD * 1000.0f));
    if (n <= 0 || (size_t)n >= sizeof(line)) return;
    if (HAL_UART_Transmit_IT(&huart2, (uint8_t *)line, (uint16_t)n) == HAL_OK) {
        usb_last_us = packet.snapshot_us;
        usb_sent = true;
    }
}

/* 100Hz 실시간 제어 루프 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim) {
    if (htim->Instance != TIM6) return;
    uint32_t timing_start = VehicleClock_Us32();
    Timing_ControlBegin(timing_start);

    /* ★워치독 생존 신호 — 실제 갱신은 메인 루프가 한다. 여기서 직접
     * HAL_IWDG_Refresh()를 부르면 메인 루프가 멎어도 워치독이 계속 먹여져서
     * 아무것도 못 잡는다. */
    s_ctrl_isr_alive = true;

    /* 1. 토글 스위치 — ★Board A로 이전됨(PC13 → 앞 보드 PC13 → CAN 0x100 바이트4).
     *    스위치가 조종석에 있어 앞 보드에서 읽는 게 배선상 짧다. 디바운스는
     *    송신측에서 끝내서 오고, CAN이 끊기면 false(=ED 폴백)로 떨어진다. */
    bool sw_on = CAN_IsTVSwitchOn();

    /* 1-b. ESP32 TV 강도 명령 — 10Hz로 들어오고 200%/s로 램프된다.
     *  ★링크가 없거나 끊기면 TV_STRENGTH_NO_ESP로 되돌아간다. 지금 구성은
     *   1.0이라 ESP32를 안 달아도 기존과 똑같이 동작한다. 핏에서 강도를
     *   쥐게 하려면 vehicle_params.h에서 0.0으로 바꿀 것. */
    {
        DjyUartLiveTv live;
        if (EspLink_GetLiveTv(&live)) {
            ControlSettings_UpdateEsp10ms(live.strength_percent,
                                          live.limit_percent,
                                          (live.flags & DJY_UART_TV_FLAG_ENABLE) != 0u);
            TV_SetStrength((float)ControlSettings_GetTvAppliedPercent() * 0.01f);
        } else {
            ControlSettings_UpdateEsp10ms(0u, 100u, false);   /* 램프 상태 유지 */
            TV_SetStrength(TV_STRENGTH_NO_ESP);
        }
    }

    /* 2. 센서 필터 갱신 — 반드시 Safety_Update()보다 먼저.
     *    안전 판정이 IMU_IsValid()/RPM_IsFresh()를 보기 때문이다. */
    IMU_Update();
    RPM_Update();

    /* 3. 입력 수집 (RPM은 CAN이 아니라 TIM3 Input Capture 직접 측정) */
    SensorData_t s = CAN_GetSensorData();

    /* 4. 안전 판정 */
    Safety_Update();
    SafeAction_t action = Safety_GetAction();

    if (action == SAFE_ACTION_STOP) {
        DAC_SetSafeState();
        TV_SetTVEnabled(false);
        TV_SetEDEnabled(false);
        /* ★PID 적분·슬루·ED 필터 상태를 명시적으로 비운다. 예전에는 STOP일 때
         * TV_Update()를 아예 안 불러서 적분값이 그대로 남았고, 폴트가 풀리는
         * 순간 쌓여있던 적분항이 한꺼번에 튀어나왔다(windup). */
        TV_Reset();
        memset(&tv, 0, sizeof(tv));   /* 로그 일관성 */
    } else {
        /* ── 입력 필터 ───────────────────────────────────────────────
         * Deglitch는 정상 구간에서 지연 0(입력을 그대로 통과)이고, 물리적으로
         * 불가능한 점프만 최대 2틱 무시한다. 그 뒤 1차 IIR로 잔여 노이즈 제거.
         * SAS 6.4ms / TPS 10.6ms 군지연 — 100Hz 루프에서 체감되지 않는 수준. */
        float sas_f = Deglitch_Update(&s_sas_dg, (float)s.sas_angle,
                                      SAS_MAX_STEP_RAW, DEGLITCH_MAX_REJECT);
        sas_f = LPF1_Update(&s_sas_lpf, sas_f, LPF1_Alpha(SAS_LPF_FC_HZ, CONTROL_DT));

        float tps_f = Deglitch_Update(&s_tps_dg, (float)s.tps_raw,
                                      TPS_MAX_STEP_RAW, DEGLITCH_MAX_REJECT);
        tps_f = LPF1_Update(&s_tps_lpf, tps_f, LPF1_Alpha(TPS_LPF_FC_HZ, CONTROL_DT));

        tv.steering_angle_rad = SAS_to_SteeringAngle(sas_f);
        tv.tps_fraction       = TPS_to_Fraction(tps_f);
#if BENCH_TEST_MODE
        /* 모터 안 돌리고 책상에서 테스트 — TPS에 비례한 가짜 RPM(0~3000) */
        tv.rpm_left           = (uint16_t)(tv.tps_fraction * 3000.0f);
        tv.rpm_right          = tv.rpm_left;
#else
        tv.rpm_left           = RPM_GetLeft();
        tv.rpm_right          = RPM_GetRight();
#endif
        tv.imu_yaw_rate       = IMU_GetYawRate();

        /* ── TV / ED 배타 활성화 ────────────────────────────────────
         *   NONE         : TV 허용(스위치 ON일 때). ED는 TV_Update()가 알아서 뺀다.
         *   DISABLE_TV   : IMU/RPM 문제 → TV만 끄고 ED로 폴백 (ED는 IMU 불필요)
         *   DISABLE_DIFF : SAS 문제 → 조향각을 못 믿으므로 차동 자체를 포기, 50:50
         * ED 자체를 켜고 끄는 조건은 "조향각을 믿을 수 있는가" 하나뿐이다. */
        TV_SetTVEnabled(sw_on && (action == SAFE_ACTION_NONE));
        TV_SetEDEnabled(action != SAFE_ACTION_DISABLE_DIFF);

        TV_Update(&tv);
        DAC_SetLeftThrottle(tv.dac_left);
        DAC_SetRightThrottle(tv.dac_right);
    }

    /* 5. 상태 LED — 정상 점멸 2.5Hz / TV 개입 중 10Hz / 폴트 시 소등 */
    {
        static uint8_t led_div = 0;
        if (action == SAFE_ACTION_STOP) {
            HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET);
            led_div = 0;
        } else if (++led_div >= (tv.tv_active ? 5 : 20)) {
            led_div = 0;
            HAL_GPIO_TogglePin(LED_PORT, LED_PIN);
        }
    }

    /* 6. 로깅 */
    SD_Logger_Write(&tv, action);
    /* One telemetry request per completed control tick. A stalled/busy main
     * loop is visible as a source sequence gap, never a fabricated sample. */
    ++s_telemetry_sequence;
    s_dbg_print_flag = true;
    Timing_ControlEnd(timing_start);
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
