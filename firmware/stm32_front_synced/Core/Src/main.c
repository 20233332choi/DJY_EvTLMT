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

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "board_config.h"
#include "sas_sensor.h"
#include "tps_sensor.h"
#include "can_comm.h"
#include "can_messages.h"
#include "front_timing.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* ★★ CAN 배선 검증 모드 ★★
 * 1 = 실제 TPS/SAS 대신 합성값(톱니파)을 CAN으로 내보낸다. 센서가 차량에
 *     장착돼 있어 손으로 움직일 수 없을 때, "CAN 배선이 되는가"와
 *     "센서가 되는가"를 분리해서 확인하는 용도.
 *     Board B의 USART2 출력에서 tps= 값이 계속 변하면 CAN 경로 정상.
 * 0 = 평소. 반드시 0으로 두고 차에 올릴 것.
 *
 * ★실차에 1로 올리면 매우 위험하다: 페달을 안 밟아도 TPS가 풀스케일까지
 *  올라가므로 시스템이 전력을 요구한다. 반드시 구동계 분리 상태에서만 쓸 것. */
#define CAN_TEST_MODE     0

/* 테스트 파형 상수 (CAN_TEST_MODE 전용)
 * 부팅 후 HOLD_TICKS 동안은 IDLE_VAL로 고정해 Board B의 TPS_LearnIdle()이
 * 성공하게 한다 — 그래야 부팅 시점의 CAN 수신까지 함께 검증된다.
 * ★IDLE_VAL을 기본값 868이 아닌 900으로 둔 이유: Board B가 "TPS idle: 900
 *   (learned)"로 찍으면 기본값 폴백이 아니라 진짜 수신했다는 증거가 된다. */
#define TEST_IDLE_VAL     900u
#define TEST_HOLD_TICKS   300u   /* 100Hz 기준 3초 */
#define TEST_TPS_STEP     8u     /* 톱니파 상승폭/틱 → 약 2.8초에 풀스윙 */
#define TEST_SAS_STEP     40u    /* SAS(14비트)는 더 크게 — 약 4초에 한 바퀴 */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;

CAN_HandleTypeDef hcan1;

SPI_HandleTypeDef hspi1;

TIM_HandleTypeDef htim6;

UART_HandleTypeDef huart2;

/* USER CODE BEGIN PV */
static volatile uint8_t hb_counter = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_ADC1_Init(void);
static void MX_CAN1_Init(void);
static void MX_SPI1_Init(void);
static void MX_TIM6_Init(void);
static void MX_USART2_UART_Init(void);
/* USER CODE BEGIN PFP */

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

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_ADC1_Init();
  MX_CAN1_Init();
  MX_SPI1_Init();
  MX_TIM6_Init();
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */
  HAL_NVIC_SetPriority(USART2_IRQn, 3, 0);
  HAL_NVIC_EnableIRQ(USART2_IRQn);
  SAS_Init();
     TPS_Init();
     CAN_Init();
     HAL_TIM_Base_Start_IT(&htim6);   /* 100Hz */
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    FrontTiming_Poll();
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
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_0;
  sConfig.Rank = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_84CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

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
  hcan1.Init.AutoBusOff = ENABLE;
  hcan1.Init.AutoWakeUp = DISABLE;
  hcan1.Init.AutoRetransmission = ENABLE;
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
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_16BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_2EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_64;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

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
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;
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
  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(SAS_CS_GPIO_Port, SAS_CS_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin : B1_Pin */
  GPIO_InitStruct.Pin = B1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(B1_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : LD2_Pin */
  GPIO_InitStruct.Pin = LD2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LD2_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : SAS_CS_Pin */
  GPIO_InitStruct.Pin = SAS_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_MEDIUM;
  HAL_GPIO_Init(SAS_CS_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */
  /* ★TV 토글 스위치(PC13)를 EXTI가 아닌 일반 입력(풀업)으로 다시 잡는다.
   * 위 생성 코드가 B1_Pin을 GPIO_MODE_IT_FALLING으로 설정하는데, 우리는
   * 인터럽트가 아니라 100Hz ISR에서 폴링+디바운스로 읽는다. 이 블록이
   * 생성 코드 뒤에 실행되므로 CubeMX 재생성에도 덮이지 않는다. */
  {
    GPIO_InitTypeDef sw = {0};
    sw.Pin  = TV_SWITCH_PIN;
    sw.Mode = GPIO_MODE_INPUT;
    sw.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(TV_SWITCH_PORT, &sw);
  }
  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
/* 토크벡터링 토글 스위치 디바운스 — 기계식 접점 채터링과 배선에 유도된
 * 노이즈로 TV가 깜빡깜빡 켜졌다 꺼지는 걸 막는다. SWITCH_DEBOUNCE_TICKS
 * (50ms) 연속으로 같은 레벨이어야 상태를 바꾼다. 100Hz ISR에서만 호출.
 * ★Board B에 있던 것과 동일한 로직 — 스위치를 앞 보드로 옮기면서 같이 왔다. */
static bool TV_Switch_Debounced(void) {
    static bool    stable = false;
    static uint8_t count  = 0;

    bool raw = (HAL_GPIO_ReadPin(TV_SWITCH_PORT, TV_SWITCH_PIN)
                == TV_SWITCH_ON_STATE);
    if (raw == stable) { count = 0; }
    else if (++count >= SWITCH_DEBOUNCE_TICKS) { stable = raw; count = 0; }
    return stable;
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim) {
    if (htim->Instance == TIM6) {
        uint32_t sample_start = VehicleClock_Us32();
        uint16_t sas = SAS_ReadAngle();
        uint32_t sas_end = VehicleClock_Us32();
        uint16_t tps = TPS_ReadRaw();
        uint32_t sample_end = VehicleClock_Us32();
        uint8_t  flags = TV_Switch_Debounced() ? SENSOR_FLAG_TV_SW : 0u;

        uint8_t status = HB_STATUS_OK;
        if (SAS_HasError())    status |= HB_STATUS_SAS_ERR;
        if (!TPS_IsValid(tps)) status |= HB_STATUS_TPS_ERR;

#if CAN_TEST_MODE
        /* ★TEMP: 실제 센서값을 버리고 합성 톱니파로 덮어쓴다.
         * 상태 비트도 OK로 강제한다 — 차량에 장착된 실제 센서가 범위를
         * 벗어나 에러를 띄우면 Board B의 세이프티가 걸려서, CAN이 멀쩡한데도
         * 안 되는 것처럼 보이기 때문이다. */
        {
            static uint32_t t     = 0;
            static uint16_t tps_w = TEST_IDLE_VAL;
            static uint16_t sas_w = 8192u;   /* 14비트 중앙 */
            static int16_t  sas_d = TEST_SAS_STEP;

            if (t < TEST_HOLD_TICKS) {
                t++;                      /* 부팅 직후: idle 고정 구간 */
            } else {
                tps_w += TEST_TPS_STEP;
                if (tps_w > TPS_ADC_MAX) tps_w = TEST_IDLE_VAL;   /* 톱니파 */
                /* ★SAS는 톱니파가 아니라 삼각파로 만든다. 0↔16383을 감싸면
                 * Board B의 Deglitch가 그 순간을 이상치로 걸러서, 멀쩡한
                 * 통신인데 값이 튀는 것처럼 보이기 때문이다. */
                sas_w = (uint16_t)((int32_t)sas_w + sas_d);
                if (sas_w > 12288u || sas_w < 4096u) sas_d = (int16_t)(-sas_d);
            }
            tps    = tps_w;
            sas    = sas_w;
            status = HB_STATUS_OK;
        }
#endif

        uint32_t span = sample_end - sample_start;
        // Midpoint of the acquisition interval, not simultaneous SPI/ADC sampling.
        bool enqueued=BoardTimeSync_SendSensor(sas, tps, flags, sample_start + span/2u,
                                      (uint16_t)(span > 65535u ? 65535u : span));
        uint32_t enqueue_end=VehicleClock_Us32();

        if (++hb_counter >= HEARTBEAT_DIV) {
            CAN_SendHeartbeat(status);
            hb_counter = 0;
        }
        HAL_GPIO_TogglePin(LED_PORT, LED_PIN);
        static uint32_t timing_seq,previous_start;
        FrontTimingSample timing={0};
        timing.seq=++timing_seq;timing.started=sample_start;
        timing.period=timing_seq>1?sample_start-previous_start:0;
        previous_start=sample_start;
        timing.sas_us=sas_end-sample_start;
        timing.tps_us=sample_end-sas_end;timing.acquire_us=span;
        timing.enqueue_us=enqueue_end-sample_start;
        timing.sas=sas;timing.tps=tps;
        timing.flags=(SAS_HasError()?1u:0u)|(!TPS_IsValid(tps)?2u:0u)|
                     (!SAS_LastIOOk()?4u:0u)|(!TPS_LastIOOk()?8u:0u)|(!enqueued?16u:0u);
        timing.esr=hcan1.Instance->ESR;
        timing.work_us=VehicleClock_Us32()-sample_start;
        FrontTiming_Record(&timing);
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
