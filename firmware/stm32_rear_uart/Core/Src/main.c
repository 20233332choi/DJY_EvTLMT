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
#include "electronic_diff.h"
#include "safety_monitor.h"
#include "sd_logger.h"
#include "can_messages.h"
#include "control_settings.h"
#include "telemetry_uart.h"
#include "../../../../shared/include/djy_watchdog.h"
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
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
CAN_HandleTypeDef hcan1;

DAC_HandleTypeDef hdac;

SPI_HandleTypeDef hspi2;

TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim6;

UART_HandleTypeDef huart2;
UART_HandleTypeDef huart3;
DMA_HandleTypeDef hdma_usart3_rx;

/* USER CODE BEGIN PV */
static TV_t tv;
static volatile bool s_dbg_print_flag = false;   /* USART2로 RPM 출력 트리거 */

/* 입력 필터 상태 (100Hz 제어 ISR 전용 — 다른 컨텍스트에서 건드리지 말 것) */
static Deglitch_t s_tps_dg,  s_sas_dg;
static LPF1_t     s_tps_lpf, s_sas_lpf;

/* 페달 정지 위치 — 부팅 시 TPS_LearnIdle()이 실측값으로 갱신한다.
 * 학습이 거부되면 벤치 실측 기본값이 그대로 남는다. */
static uint16_t   s_tps_idle = TPS_PEDAL_IDLE;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_CAN1_Init(void);
static void MX_DAC_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_USART3_UART_Init(void);
static void MX_SPI2_Init(void);
static void MX_TIM6_Init(void);
static void MX_TIM2_Init(void);
/* USER CODE BEGIN PFP */
static float SAS_to_SteeringAngle(float raw);
static float TPS_to_Fraction(float raw);
static void  TPS_LearnIdle(uint32_t duration_ms);
static bool  TV_Switch_Debounced(void);
static void  Debug_PrintRPM(void);

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

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_CAN1_Init();
  MX_DAC_Init();
  MX_USART2_UART_Init();
  (void)TelemetryUart_Init();
  MX_USART3_UART_Init();
  MX_SPI2_Init();
  MX_TIM6_Init();
  MX_FATFS_Init();
  MX_TIM2_Init();
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
   SD_Logger_Init();
   Safety_Init();
   TV_Init();          /* TV + ED(전자식 디퍼런셜) 동시 초기화 */
   ControlSettings_Init();
   DAC_SetSafeState();

   /* 입력 필터 초기화 — 첫 샘플이 그대로 통과하므로 부팅 시 튐 없음 */
   Deglitch_Reset(&s_tps_dg);  Deglitch_Reset(&s_sas_dg);
   LPF1_Reset(&s_tps_lpf);     LPF1_Reset(&s_sas_lpf);

   /* ★차량 완전 정지 상태에서 1초간 자이로 0점 캘리브레이션. 여기서 흔들리거나
    * 움직이면 캘리브레이션이 거부되거나(바이어스 이상치) 부정확해질 수 있다. */
   IMU_Calibrate(1000);
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

   DjyWatchdog_Init();
   HAL_TIM_Base_Start_IT(&htim6);   /* 100Hz 제어 루프 */
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
	  DjyWatchdog_Kick();
	  IMU_ProcessData();   /* UART DMA 버퍼 파싱 */
	     IMU_Watchdog();      /* UART 에러로 DMA가 멎으면 자동 복구 */
	     SD_Logger_Flush();   /* SD 기록 */
	     if (s_dbg_print_flag) {
	         s_dbg_print_flag = false;
	         Debug_PrintRPM();
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
  * @brief DAC Initialization Function
  * @param None
  * @retval None
  */
static void MX_DAC_Init(void)
{

  /* USER CODE BEGIN DAC_Init 0 */

  /* USER CODE END DAC_Init 0 */

  DAC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN DAC_Init 1 */

  /* USER CODE END DAC_Init 1 */

  /** DAC Initialization
  */
  hdac.Instance = DAC;
  if (HAL_DAC_Init(&hdac) != HAL_OK)
  {
    Error_Handler();
  }

  /** DAC channel OUT1 config
  */
  sConfig.DAC_Trigger = DAC_TRIGGER_NONE;
  sConfig.DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE;
  if (HAL_DAC_ConfigChannel(&hdac, &sConfig, DAC_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }

  /** DAC channel OUT2 config
  */
  if (HAL_DAC_ConfigChannel(&hdac, &sConfig, DAC_CHANNEL_2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN DAC_Init 2 */

  /* USER CODE END DAC_Init 2 */

}

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
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_IC_InitTypeDef sConfigIC = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 839;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 4294967295;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_IC_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigIC.ICPolarity = TIM_INPUTCHANNELPOLARITY_RISING;
  sConfigIC.ICSelection = TIM_ICSELECTION_DIRECTTI;
  sConfigIC.ICPrescaler = TIM_ICPSC_DIV1;
  sConfigIC.ICFilter = 8;
  if (HAL_TIM_IC_ConfigChannel(&htim2, &sConfigIC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_IC_ConfigChannel(&htim2, &sConfigIC, TIM_CHANNEL_2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */

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
static void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */

  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 115200;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA1_Stream1_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Stream1_IRQn, 2, 0);
  HAL_NVIC_EnableIRQ(DMA1_Stream1_IRQn);

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

  /* USER CODE BEGIN MX_GPIO_Init_2 */
  /* ★CubeMX가 이 프로젝트에서 타이머 Input Capture 핀의 GPIO 대체기능 설정을
   * 자동생성 안 해주는 문제가 있어 수동으로 추가함(중복 안전장치 — 정상적으로는
   * stm32f4xx_hal_msp.c의 HAL_TIM_IC_MspInit()에서 생성된다).
   * .ioc를 다시 Generate Code 해도 이 USER CODE 블록은 보존되니 안전함. */
  GPIO_InitStruct.Pin       = GPIO_PIN_0 | GPIO_PIN_1;   /* PA0=CH1(좌), PA1=CH2(우) */
  GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull      = GPIO_PULLUP;   /* SPD 오픈컬렉터 대비 풀업 */
  GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.Alternate = GPIO_AF1_TIM2;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
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

/* 토글 스위치 디바운스 — 기계식 접점 채터링과 배선에 유도된 노이즈로
 * TV가 깜빡깜빡 켜졌다 꺼지는 걸 막는다. SWITCH_DEBOUNCE_TICKS(50ms)
 * 연속으로 같은 레벨이어야 상태를 바꾼다. 100Hz ISR에서만 호출. */
static bool TV_Switch_Debounced(void) {
    static bool    stable = false;
    static uint8_t count  = 0;

    bool raw = (HAL_GPIO_ReadPin(TV_SWITCH_PORT, TV_SWITCH_PIN)
                == TV_SWITCH_ON_STATE);
    if (raw == stable) { count = 0; }
    else if (++count >= SWITCH_DEBOUNCE_TICKS) { stable = raw; count = 0; }
    return stable;
}

/* 메인 루프에서 200ms마다 호출 — USART2(ST-Link 가상COM, 115200 8N1)로 출력.
 * 정수(uint16_t)만 찍으므로 nano.specs의 printf 부동소수점 미지원과 무관하다. */
static void Debug_PrintRPM(void) {
    /* tv.rpm_left/right가 아니라 RPM_GetLeft/Right()를 직접 찍는다 —
     * tv는 Safety_Update()가 Board A(CAN) 신호 없으면 STOP으로 매 사이클
     * memset(0)해버려서, Board B 단독 테스트 시 RPM 센서가 멀쩡해도 항상
     * 0으로 보이기 때문. Safety와 무관한 원본값으로 RPM만 따로 검증한다. */
    SensorData_t s = CAN_GetSensorData();   /* ★TEMP: TPS 재캘리브레이션용 raw 확인 */
    TV_t tv_log = tv;
    DjyUartLiveTv esp_command = {0};
    bool esp_fresh = TelemetryUart_GetLiveTv(&esp_command);
    char line[512];
    /* glt = 노이즈로 폐기한 캡처 수. 실차에서 이 값이 계속 오르면 SPD 배선
     * 문제이므로 RC 필터(1kΩ+10nF)/실드선/접지 분리를 검토할 것. */
    /* tps= raw값 / pct= 데드밴드까지 반영한 최종 출력의지[%].
     * 페달 링키지 조정할 때 이 두 개를 같이 봐야 한다:
     *   발 뗐을 때  pct=0  이 아니면 → TPS_DEADBAND_RAW 를 늘린다
     *   끝까지 밟아 pct=100이 아니면 → TPS_FULL_MARGIN_RAW 를 늘린다 */
    int n = snprintf(line, sizeof(line),
                      "L=%u R=%u cap=%lu/%lu glt=%lu/%lu tps=%u pct=%d idle=%u imu=%u sas=%u "
                      "yaw=%ld lat=%ld lon=%ld ax=%ld ay=%ld az=%ld "
                      "vs=%ld dy=%ld ye=%ld dp=%ld pl=%ld pr=%ld tva=%u eda=%u tr=%ld "
                      "ctl=%u/%u req=%u lim=%u app=%u tv=%u ed=%u fault=%u "
                      "dac=%u/%u sas=%u imu=%u urx=%lu uerr=%lu "
                      "ipk=%lu bad=%lu rs=%lu rv=%u/%u\r\n",
                      RPM_GetLeft(), RPM_GetRight(),
                      (unsigned long)g_rpm_cap_count_l,
                      (unsigned long)g_rpm_cap_count_r,
                      (unsigned long)g_rpm_glitch_l,
                      (unsigned long)g_rpm_glitch_r,
                      s.tps_raw,
                      (int)(TPS_to_Fraction((float)s.tps_raw) * 100.0f),
                      s_tps_idle,
                      (unsigned)(IMU_IsValid() ? 1u : 0u),
                      (unsigned)(s.sas_angle & 0x3fffu),
                      (long)(IMU_GetYawRate() * 1000.0f),
                      (long)(IMU_GetLateralAcc() * 1000.0f),
                      (long)(IMU_GetLongitudinalAcc() * 1000.0f),
                      (long)(IMU_GetRawAx() * 1000.0f),
                      (long)(IMU_GetRawAy() * 1000.0f),
                      (long)(IMU_GetRawAz() * 1000.0f),
                      (long)(tv_log.vehicle_speed * 1000.0f),
                      (long)(tv_log.desired_yaw * 1000.0f),
                      (long)(tv_log.yaw_error * 1000.0f),
                      (long)(tv_log.delta_power * 1000.0f),
                      (long)(tv_log.power_left * 1000.0f),
                      (long)(tv_log.power_right * 1000.0f),
                      (unsigned)(tv_log.tv_active ? 1u : 0u),
                      (unsigned)(tv_log.ed_active ? 1u : 0u),
                      (long)(tv_log.traction_scale * 1000.0f),
                      (unsigned)(esp_fresh ? 1u : 0u),
                      (unsigned)esp_command.sequence,
                      (unsigned)esp_command.strength_percent,
                      (unsigned)ControlSettings_GetTvLimitPercent(),
                      (unsigned)ControlSettings_GetTvAppliedPercent(),
                      (unsigned)(tv_log.tv_active ? 1u : 0u),
                      (unsigned)(tv_log.ed_active ? 1u : 0u),
                      (unsigned)Safety_GetFaultCode(),
                      (unsigned)tv_log.dac_left,
                      (unsigned)tv_log.dac_right,
                      (unsigned)(s.sas_angle & 0x3fffu),
                      (unsigned)(IMU_IsValid() ? 1u : 0u),
                      (unsigned long)TelemetryUart_GetRxCount(),
                      (unsigned long)TelemetryUart_GetCrcErrorCount(),
                      (unsigned long)IMU_GetPacketOkCount(),
                      (unsigned long)IMU_GetPacketBadCount(),
                      (unsigned long)IMU_GetResyncCount(),
                      (unsigned)RPM_IsLeftFresh(),
                      (unsigned)RPM_IsRightFresh());
    if (n > 0 && n < (int)sizeof(line)) {
        (void)TelemetryUart_Send((const uint8_t *)line, (uint16_t)n);
        HAL_UART_Transmit(&huart2, (uint8_t *)line, (uint16_t)n, 50);
    }
}

/* 100Hz 실시간 제어 루프 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim) {
    if (htim->Instance != TIM6) return;

    /* 0. 디버그 출력 트리거 — 100Hz/20 = 5Hz(200ms 간격) */
    static uint8_t dbg_div = 0;
    if (++dbg_div >= 20) { dbg_div = 0; s_dbg_print_flag = true; }

    /* 1. 토글 스위치 (PC13, 내부 풀업, 눌림=RESET=ON) — 디바운스 적용 */
    bool sw_on = TV_Switch_Debounced();

    /* 2. 센서 필터 갱신 — 반드시 Safety_Update()보다 먼저.
     *    안전 판정이 IMU_IsValid()/RPM_IsFresh()를 보기 때문이다. */
    IMU_Update();
    RPM_Update();

    /* 3. 입력 수집 (RPM은 CAN이 아니라 TIM3 Input Capture 직접 측정) */
    SensorData_t s = CAN_GetSensorData();

    /* Pit configuration is volatile and accepted only with a fresh front
     * sensor frame, released accelerator, and both motors stopped. The ESP32
     * additionally requires its physical PIT ENABLE input, so a Wi-Fi packet
     * alone cannot change these limits while driving. */
    {
        static uint8_t last_pit_sequence = 0xffu;
        DjyPitConfig pit_config;
        if (CAN_GetPendingPitConfig(&pit_config, last_pit_sequence) &&
            CAN_IsSensorFresh() && TPS_to_Fraction((float)s.tps_raw) <= 0.02f &&
            RPM_GetLeft() <= 30u && RPM_GetRight() <= 30u &&
            !RPM_HasRecentActivity()) { /* Rejected noise is not a stopped motor. */
            ControlSettings_ApplyPitConfig(&pit_config);
            last_pit_sequence = pit_config.sequence;
            (void)CAN_SendPitConfigAck(&pit_config);
        }
    }

    /* ESP 명령이 500ms 이내로 계속 들어올 때만 강도 입력원으로 사용한다.
     * 끊기면 기존 Front/CAN 입력원으로 즉시 복귀한다. TV 계산식, 안전 판정,
     * 물리 TV 스위치와 출력 슬루는 기존 경로를 그대로 통과한다. */
    DjyUartLiveTv esp_control = {0};
    bool esp_control_fresh = TelemetryUart_GetLiveTv(&esp_control);
    if (esp_control_fresh) {
        ControlSettings_UpdateEsp10ms(
            esp_control.strength_percent,
            esp_control.limit_percent,
            (esp_control.flags & DJY_UART_TV_FLAG_ENABLE) != 0u);
    } else {
        DjyDriverControl driver_control = CAN_GetDriverControl();
        ControlSettings_Update10ms(&driver_control, CAN_IsDriverControlFresh());
    }
    TV_SetStrength((float)ControlSettings_GetTvAppliedPercent() * 0.01f);

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
        bool command_fresh = ControlSettings_IsFresh();
        bool tv_requested = (ControlSettings_GetFlags() &
                             DJY_CONTROL_FLAG_TV_ENABLE) != 0u;
        TV_SetTVEnabled(sw_on && command_fresh && tv_requested &&
                        (action == SAFE_ACTION_NONE));
        TV_SetEDEnabled(command_fresh &&
                        (action != SAFE_ACTION_DISABLE_DIFF));

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

    /* 20Hz acknowledgement/status for the front board and ESP32 gateway. */
    {
        static uint8_t status_div = 0u;
        static uint8_t status_sequence = 0u;
        if (++status_div >= 5u) {
            status_div = 0u;
            DjyRearStatus status = {0};
            status.tv_applied_percent = ControlSettings_GetTvAppliedPercent();
            status.regen_applied_percent = ControlSettings_GetRegenAppliedPercent();
            status.mode = ControlSettings_GetMode();
            if (ControlSettings_IsFresh()) status.status_flags |= DJY_REAR_STATUS_CONTROL_FRESH;
            if (tv.tv_active) status.status_flags |= DJY_REAR_STATUS_TV_ACTIVE;
            if (tv.ed_active) status.status_flags |= DJY_REAR_STATUS_ED_ACTIVE;
            if (action != SAFE_ACTION_NONE) status.status_flags |= DJY_REAR_STATUS_FAULT;
            status.fault_code = (uint8_t)Safety_GetFaultCode();
            status.sequence = status_sequence++;
            (void)CAN_SendRearStatus(&status);
            (void)CAN_SendRearDrivetrain(tv.rpm_left, tv.rpm_right,
                                         tv.dac_left, tv.dac_right);
        }
    }
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* If DAC was already initialized, remove propulsion before stopping. */
  if (hdac.Instance == DAC && __HAL_RCC_DAC_IS_CLK_ENABLED()) {
    DAC_SetSafeState();
  }
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
