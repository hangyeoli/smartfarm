/* SmartFarm STM32 NUCLEO-F411RE
 * Based on bt_nucleo_f411re_uart2_printf_uart6_clcd_dht11/Core/Src/main.c
 * UART2 : printf debug
 * UART6 : Bluetooth module to Raspberry Pi /dev/rfcomm0 bridge
 * LCD   : current time + CDS/TEMP/HUMI (I2C3 PA8=SCL, PC9=SDA)
 * FND   : 3461AS 4-digit 7-segment WATERLEVEL display
 * LD2   : WATERLEVEL low warning LED
 * PB9   : login alarm buzzer
 */
#include "main.h"

/* Private includes ----------------------------------------------------------*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <clcd.h>

#ifdef __GNUC__
#define PUTCHAR_PROTOTYPE int __io_putchar(int ch)
#else
#define PUTCHAR_PROTOTYPE int fputc(int ch, FILE *f)
#endif

#define ARR_CNT 8
#define CMD_SIZE 160

/* 3461AS 4-digit FND. Common cathode = 0, Common anode = 1 */
#define FND_COMMON_ANODE 0

#define FND_GPIO_Port GPIOB
#define FND_A_Pin GPIO_PIN_0
#define FND_B_Pin GPIO_PIN_1
#define FND_C_Pin GPIO_PIN_2
#define FND_D_Pin GPIO_PIN_4
#define FND_E_Pin GPIO_PIN_5
#define FND_F_Pin GPIO_PIN_6
#define FND_G_Pin GPIO_PIN_7
#define FND_DP_Pin GPIO_PIN_8
#define FND_USE_DP 1
#if FND_USE_DP
#define FND_SEGMENT_PINS (FND_A_Pin|FND_B_Pin|FND_C_Pin|FND_D_Pin|FND_E_Pin|FND_F_Pin|FND_G_Pin|FND_DP_Pin)
#else
#define FND_SEGMENT_PINS (FND_A_Pin|FND_B_Pin|FND_C_Pin|FND_D_Pin|FND_E_Pin|FND_F_Pin|FND_G_Pin)
#endif

#define FND_DIGIT_COUNT 4

#define FND_DIG1_GPIO_Port GPIOA
#define FND_DIG1_Pin GPIO_PIN_6

#define FND_DIG2_GPIO_Port GPIOC
#define FND_DIG2_Pin GPIO_PIN_0

#define FND_DIG3_GPIO_Port GPIOC
#define FND_DIG3_Pin GPIO_PIN_1

#define FND_DIG4_GPIO_Port GPIOB
#define FND_DIG4_Pin GPIO_PIN_10

#define FND_SEG_ON     (FND_COMMON_ANODE ? GPIO_PIN_RESET : GPIO_PIN_SET)
#define FND_SEG_OFF    (FND_COMMON_ANODE ? GPIO_PIN_SET : GPIO_PIN_RESET)
#define FND_DIGIT_ON   (FND_COMMON_ANODE ? GPIO_PIN_SET : GPIO_PIN_RESET)
#define FND_DIGIT_OFF  (FND_COMMON_ANODE ? GPIO_PIN_RESET : GPIO_PIN_SET)

#define BUZZER_GPIO_Port GPIOB
#define BUZZER_Pin GPIO_PIN_9
#define BUZZER_ENABLED 1
#define WATER_LOW_TH 30
#define LCD_DIRTY_TIME   0x01
#define LCD_DIRTY_SENSOR 0x02
#define BT_DEBUG_PERIOD_MS 500

/* Private variables ---------------------------------------------------------*/
I2C_HandleTypeDef hi2c3;
UART_HandleTypeDef huart2;
UART_HandleTypeDef huart6;

uint8_t rx2char;
volatile unsigned char rx2Flag = 0;
volatile char rx2Data[CMD_SIZE];
volatile unsigned char btFlag = 0;
uint8_t btRxChar;
char btData[CMD_SIZE];

static int g_cds = 0;
static float g_temp = 0.0f;
static float g_humi = 0.0f;
static int g_water = 0;
static char g_time[17] = "00/00 00:00:00";
static uint8_t g_fndDigits[FND_DIGIT_COUNT] = {0};
static volatile uint8_t g_fndReady = 0;
static uint8_t g_lcdDirty = LCD_DIRTY_TIME | LCD_DIRTY_SENSOR;
static uint32_t g_lastBtDebugTick = 0;
static volatile uint32_t g_uart2Error = 0;
static volatile uint32_t g_uart6Error = 0;

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_USART6_UART_Init(void);
static void MX_I2C3_Init(void);
static void I2C3_BusRecover(void);
void bluetooth_Event(void);
static void Time_SetDisplay(const char *timeText);
static void LCD_Update(uint8_t dirty);
static void FND_SetWater(int water);
static void FND_Refresh(void);
static void FND_AllDigitsOff(void);
static void FND_WriteSegments(uint8_t pattern);
static void Login_Alarm(void);
static void Play_Tone(uint32_t freq, uint32_t duration_ms);
static HAL_StatusTypeDef BT_Send(const char *msg);
static void BT_WaitRx(uint32_t waitMs);
static void UART2_EnsureRx(void);

int main(void)
{
  HAL_Init();
  SystemClock_Config();

  MX_GPIO_Init();
  MX_USART2_UART_Init();
  MX_USART6_UART_Init();
  I2C3_BusRecover();
  printf("I2C3 REC SCL=%d SDA=%d\r\n",
         HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_8),
         HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_9));
  MX_I2C3_Init();

  UART2_EnsureRx();
  HAL_UART_Receive_IT(&huart6, &btRxChar, 1);

  LCD_init(&hi2c3);
  printf("LCD I2C %s 0x%02X SCL=%d SDA=%d\r\n",
         LCD_isReady() ? "OK" : "FAIL",
         LCD_getAddress(),
         HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_8),
         HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_9));
  printf("DWT BYPASS\r\n");
  printf("UART6 RX IT\r\n");
  LCD_writeStringXY(0,0,"SmartFarm Ready ");
  LCD_writeStringXY(1,0,"C000 T00 H00    ");
  FND_SetWater(g_water);
  g_fndReady = 1;
  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_SET);
  printf("SmartFarm STM32 start\r\n");

  Login_Alarm();
  
  // 부팅 후 바로 시간 요청 (초기값 대신 실제 시간 표시)
  char reqTime[] = "[GETTIME]TIME\n";
  HAL_StatusTypeDef btTxStatus;
  printf("main loop start\r\n");
  btTxStatus = BT_Send(reqTime);
  BT_WaitRx(200);
  printf("UART6 TX GETTIME %s\r\n", (btTxStatus == HAL_OK) ? "OK" : "FAIL");
  
  LCD_Update(LCD_DIRTY_TIME | LCD_DIRTY_SENSOR);
  g_lcdDirty = 0;

  uint32_t prevLcdTick = HAL_GetTick();
  uint32_t prevTimeReqTick = HAL_GetTick() - 2000;  // 2초 후 다시 요청
  

  while (1)
  {
    UART2_EnsureRx();
    // BT_PollRx();

    if(g_uart2Error)
    {
      uint32_t err = g_uart2Error;
      g_uart2Error = 0;
      printf("UART2 ERR 0x%08lX\r\n", (unsigned long)err);
    }

    if(g_uart6Error)
    {
      uint32_t err = g_uart6Error;
      g_uart6Error = 0;
      printf("UART6 ERR 0x%08lX\r\n", (unsigned long)err);
    }

    if(rx2Flag)
    {
      printf("recv2 : %s\r\n", rx2Data);
      
      char reqBuf[CMD_SIZE + 20];
      sprintf(reqBuf, "[SF_SQL]%s\n", rx2Data);
      btTxStatus = BT_Send(reqBuf);
      BT_WaitRx(200);
      printf("UART6 TX CMD %s\r\n", (btTxStatus == HAL_OK) ? "OK" : "FAIL");
      
      rx2Flag = 0;
    }

    if(btFlag)
    {
      btFlag = 0;
      bluetooth_Event();
    }

    if(g_lcdDirty && HAL_GetTick() - prevLcdTick >= 300)
    {
      uint8_t dirty = g_lcdDirty;
      prevLcdTick = HAL_GetTick();
      g_lcdDirty = 0;
      LCD_Update(dirty);
    }

    if(HAL_GetTick() - prevTimeReqTick >= 3000)  // 3초마다 시간 요청 (자동 시간 동기화)
    {
      prevTimeReqTick = HAL_GetTick();
      btTxStatus = BT_Send(reqTime);
      BT_WaitRx(200);
      printf("UART6 TX GETTIME %s\r\n", (btTxStatus == HAL_OK) ? "OK" : "FAIL");
    }
  }
}

void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_I2C3_Init(void)
{
  hi2c3.Instance = I2C3;
  hi2c3.Init.ClockSpeed = 10000;
  hi2c3.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c3.Init.OwnAddress1 = 0;
  hi2c3.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c3.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c3.Init.OwnAddress2 = 0;
  hi2c3.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c3.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c3) != HAL_OK)
  {
    Error_Handler();
  }
}

static void I2C3_BusRecover(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_SET);
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_9, GPIO_PIN_SET);

  GPIO_InitStruct.Pin = GPIO_PIN_8;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = GPIO_PIN_9;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  HAL_Delay(1);

  for(uint8_t i = 0; i < 9 && HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_9) == GPIO_PIN_RESET; i++)
  {
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);
    HAL_Delay(1);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_SET);
    HAL_Delay(1);
  }

  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_9, GPIO_PIN_RESET);
  HAL_Delay(1);
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_SET);
  HAL_Delay(1);
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_9, GPIO_PIN_SET);
  HAL_Delay(1);
}

static void MX_USART2_UART_Init(void)
{
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 9600;
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
}

static void MX_USART6_UART_Init(void)
{
  huart6.Instance = USART6;
  huart6.Init.BaudRate = 9600;
  huart6.Init.WordLength = UART_WORDLENGTH_8B;
  huart6.Init.StopBits = UART_STOPBITS_1;
  huart6.Init.Parity = UART_PARITY_NONE;
  huart6.Init.Mode = UART_MODE_TX_RX;
  huart6.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart6.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart6) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(FND_GPIO_Port, FND_SEGMENT_PINS, FND_SEG_OFF);
  HAL_GPIO_WritePin(FND_DIG1_GPIO_Port, FND_DIG1_Pin, FND_DIGIT_OFF);
  HAL_GPIO_WritePin(FND_DIG2_GPIO_Port, FND_DIG2_Pin, FND_DIGIT_OFF);
  HAL_GPIO_WritePin(FND_DIG3_GPIO_Port, FND_DIG3_Pin, FND_DIGIT_OFF);
  HAL_GPIO_WritePin(FND_DIG4_GPIO_Port, FND_DIG4_Pin, FND_DIGIT_OFF);
#if BUZZER_ENABLED
  HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin, GPIO_PIN_RESET);
#endif

  GPIO_InitStruct.Pin = B1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(B1_GPIO_Port, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = LD2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LD2_GPIO_Port, &GPIO_InitStruct);

  /* FND_DIG1 = PA6 */
  GPIO_InitStruct.Pin = FND_DIG1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(FND_DIG1_GPIO_Port, &GPIO_InitStruct);

  /* FND_DIG2 = PC0, FND_DIG3 = PC1 */
  GPIO_InitStruct.Pin = FND_DIG2_Pin|FND_DIG3_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /* FND segments PB0-PB8, BUZZER PB9, FND_DIG4 PB10 */
  GPIO_InitStruct.Pin = FND_SEGMENT_PINS|FND_DIG4_Pin;
#if BUZZER_ENABLED
  GPIO_InitStruct.Pin |= BUZZER_Pin;
#endif
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
}

static void LCD_Update(uint8_t dirty)
{
  char line0[17];
  char line1[17];
  char sensor[17];

  if(dirty & LCD_DIRTY_TIME)
  {
    // 시간 표시 (MM/DD HH:MM:SS 형식, 왼쪽 정렬)
    snprintf(line0, sizeof(line0), "%-16s", g_time);
    LCD_writeStringXY(0,0,line0);
  }

  if(dirty & LCD_DIRTY_SENSOR)
  {
    // 센서 데이터 표시
    snprintf(sensor, sizeof(sensor), "C%03d T%02.0f H%02.0f", g_cds, g_temp, g_humi);
    snprintf(line1, sizeof(line1), "%-16s", sensor);
    LCD_writeStringXY(1,0,line1);
  }
}

static void Time_SetDisplay(const char *timeText)
{
  int year, mon, day, hour, min, sec;

  if(sscanf(timeText, "%d.%d.%d %d:%d:%d", &year, &mon, &day, &hour, &min, &sec) == 6)
  {
    snprintf(g_time, sizeof(g_time), "%02d/%02d %02d:%02d:%02d", mon, day, hour, min, sec);
  }
  else
  {
    strncpy(g_time, timeText, sizeof(g_time)-1);
    g_time[sizeof(g_time)-1] = '\0';
  }
}

static void FND_SetDigit(uint8_t digit, GPIO_PinState state)
{
  static GPIO_TypeDef * const digitPorts[FND_DIGIT_COUNT] = {
    FND_DIG1_GPIO_Port, FND_DIG2_GPIO_Port, FND_DIG3_GPIO_Port, FND_DIG4_GPIO_Port
  };
  static const uint16_t digitPins[FND_DIGIT_COUNT] = {
    FND_DIG1_Pin, FND_DIG2_Pin, FND_DIG3_Pin, FND_DIG4_Pin
  };

  if(digit < FND_DIGIT_COUNT)
  {
    HAL_GPIO_WritePin(digitPorts[digit], digitPins[digit], state);
  }
}

static void FND_AllDigitsOff(void)
{
  for(uint8_t i=0; i<FND_DIGIT_COUNT; i++)
  {
    FND_SetDigit(i, FND_DIGIT_OFF);
  }
}

static void FND_WriteSegments(uint8_t pattern)
{
  HAL_GPIO_WritePin(FND_GPIO_Port, FND_A_Pin,  (pattern & 0x01) ? FND_SEG_ON : FND_SEG_OFF);
  HAL_GPIO_WritePin(FND_GPIO_Port, FND_B_Pin,  (pattern & 0x02) ? FND_SEG_ON : FND_SEG_OFF);
  HAL_GPIO_WritePin(FND_GPIO_Port, FND_C_Pin,  (pattern & 0x04) ? FND_SEG_ON : FND_SEG_OFF);
  HAL_GPIO_WritePin(FND_GPIO_Port, FND_D_Pin,  (pattern & 0x08) ? FND_SEG_ON : FND_SEG_OFF);
  HAL_GPIO_WritePin(FND_GPIO_Port, FND_E_Pin,  (pattern & 0x10) ? FND_SEG_ON : FND_SEG_OFF);
  HAL_GPIO_WritePin(FND_GPIO_Port, FND_F_Pin,  (pattern & 0x20) ? FND_SEG_ON : FND_SEG_OFF);
  HAL_GPIO_WritePin(FND_GPIO_Port, FND_G_Pin,  (pattern & 0x40) ? FND_SEG_ON : FND_SEG_OFF);
#if FND_USE_DP
  HAL_GPIO_WritePin(FND_GPIO_Port, FND_DP_Pin, (pattern & 0x80) ? FND_SEG_ON : FND_SEG_OFF);
#endif
}

static void FND_SetWater(int water)
{
  static const uint8_t seg[10] = {
    0x3F, /* 0 */ 0x06, /* 1 */ 0x5B, /* 2 */ 0x4F, /* 3 */ 0x66, /* 4 */
    0x6D, /* 5 */ 0x7D, /* 6 */ 0x07, /* 7 */ 0x7F, /* 8 */ 0x6F  /* 9 */
  };

  if(water < 0) water = 0;
  if(water > 99) water = 99;

  g_fndDigits[0] = 0x00;                    /* blank */
  g_fndDigits[1] = 0x00;                    /* blank */
  g_fndDigits[2] = seg[(water / 10) % 10];  /* tens  */
  g_fndDigits[3] = seg[water % 10];         /* ones  */
}

static void FND_Refresh(void)
{
  static uint8_t digit = 2;          /* 2=tens, 3=ones (only right 2 digits used) */
  uint8_t next = (digit == 2) ? 3 : 2;

  FND_SetDigit(digit, FND_DIGIT_OFF);    /* turn off current digit only  */
  FND_WriteSegments(g_fndDigits[next]);  /* set segments for next digit  */
  FND_SetDigit(next, FND_DIGIT_ON);     /* turn on next digit           */

  digit = next;
}

void FND_RefreshTick(void)
{
  if(g_fndReady)
  {
    FND_Refresh();
  }
}

void bluetooth_Event(void)
{
  int i = 0;
  char *pToken;
  char *pArray[ARR_CNT] = {0};
  char recvBuf[CMD_SIZE] = {0};
  char *cmd;
  int base = 0;
  uint32_t now;

  strcpy(recvBuf, btData);
  now = HAL_GetTick();
  if(now - g_lastBtDebugTick >= BT_DEBUG_PERIOD_MS)
  {
    g_lastBtDebugTick = now;
    printf("btData : %s\r\n", btData);
  }

  pToken = strtok(recvBuf, "[@]");
  while(pToken != NULL)
  {
    pArray[i] = pToken;
    if(++i >= ARR_CNT)
      break;
    pToken = strtok(NULL, "[@]");
  }
  if(i < 2)
    return;

  if(pArray[1] && (!strncmp(pArray[1], " New conn", 9) || !strncmp(pArray[1], " Already log", 12)))
  {
    return;
  }

  cmd = pArray[0];
  if(pArray[1] && (!strcmp(pArray[1], "SENSOR") || !strcmp(pArray[1], "WATERLED") || !strcmp(pArray[1], "GETDB")))
  {
    cmd = pArray[1];
    base = 1;
  }

  if(!strcmp(pArray[0], "GETTIME"))
  {
    Time_SetDisplay(pArray[1]);
    g_lcdDirty |= LCD_DIRTY_TIME;
  }
  else if(!strcmp(cmd, "SENSOR") && i >= base + 5)
  {
    g_cds = atoi(pArray[base + 1]);
    g_temp = atof(pArray[base + 2]);
    g_humi = atof(pArray[base + 3]);
    g_water = atoi(pArray[base + 4]);
    FND_SetWater(g_water);
    g_lcdDirty |= LCD_DIRTY_SENSOR;
    HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, (g_water <= WATER_LOW_TH) ? GPIO_PIN_SET : GPIO_PIN_RESET);
  }
  else if(!strcmp(cmd, "WATERLED") && i >= base + 2)
  {
    HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, (!strcmp(pArray[base + 1],"ON")) ? GPIO_PIN_SET : GPIO_PIN_RESET);
  }
  else if(!strcmp(cmd, "GETDB") && i >= base + 2)
  {
    printf("GETDB %s : %s\r\n", pArray[base+1], pArray[base+2] ? pArray[base+2] : "NULL");
    if(!strcmp(pArray[base+1], "WATERLEVEL") && pArray[base+2])
    {
      g_water = atoi(pArray[base+2]);
      FND_SetWater(g_water);
      HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, (g_water <= WATER_LOW_TH) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
  }
}

static void Play_Tone(uint32_t freq, uint32_t duration_ms)
{
#if !BUZZER_ENABLED
  (void)freq;
  (void)duration_ms;
  return;
#else
  uint32_t halfPeriodMs;
  uint32_t start;

  if(freq == 0) {
    HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin, GPIO_PIN_RESET);
    HAL_Delay(duration_ms);
    return;
  }

  halfPeriodMs = 500U / freq;
  if(halfPeriodMs == 0)
  {
    halfPeriodMs = 1;
  }

  start = HAL_GetTick();
  while(HAL_GetTick() - start < duration_ms)
  {
    HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin, GPIO_PIN_SET);
    HAL_Delay(halfPeriodMs);
    HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin, GPIO_PIN_RESET);
    HAL_Delay(halfPeriodMs);
  }
  HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin, GPIO_PIN_RESET);
#endif
}

static void Login_Alarm(void)
{
  /* 약 3초 login alarm. 짧고 부드러운 상승/하강 멜로디 */
  Play_Tone(659, 220);
  Play_Tone(784, 220);
  Play_Tone(988, 260);
  Play_Tone(1047, 360);
  Play_Tone(988, 220);
  Play_Tone(784, 220);
  Play_Tone(659, 260);
  Play_Tone(523, 280);
  Play_Tone(587, 220);
  Play_Tone(659, 220);
  Play_Tone(0, 560);
}

PUTCHAR_PROTOTYPE
{
  HAL_UART_Transmit(&huart2, (uint8_t *)&ch, 1, 0xFFFF);
  return ch;
}

static HAL_StatusTypeDef BT_Send(const char *msg)
{
  return HAL_UART_Transmit(&huart6, (uint8_t *)msg, strlen(msg), 100);
}

static void UART2_EnsureRx(void)
{
  if(huart2.RxState == HAL_UART_STATE_READY)
  {
    HAL_UART_Receive_IT(&huart2, &rx2char, 1);
  }
}

static void BT_WaitRx(uint32_t waitMs)
{
  uint32_t start = HAL_GetTick();

  while(HAL_GetTick() - start < waitMs)
  {
    if(btFlag)
    {
      break;
    }
  }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if(huart->Instance == USART2)
  {
    static int i = 0;

    rx2Data[i] = rx2char;

    if(rx2Data[i] == '\r' || rx2Data[i] == '\n')
    {
      rx2Data[i] = '\0';
      rx2Flag = 1;
      i = 0;
    }
    else
    {
      if(i < CMD_SIZE - 2) i++;
      else i = 0;
    }

    HAL_UART_Receive_IT(&huart2, &rx2char, 1);
  }

  if(huart->Instance == USART6)
  {
    static int j = 0;

    if(btRxChar == '\r' || btRxChar == '\n')
    {
      btData[j] = '\0';

      if(j > 0)
      {
        btFlag = 1;
        printf("UART6 RX : %s\r\n", btData);
      }

      j = 0;
    }
    else
    {
      if(j < CMD_SIZE - 2)
      {
        btData[j++] = (char)btRxChar;
      }
      else
      {
        j = 0;
      }
    }

    HAL_UART_Receive_IT(&huart6, &btRxChar, 1);
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if(huart->Instance == USART2)
  {
    g_uart2Error = huart->ErrorCode;
    __HAL_UART_CLEAR_OREFLAG(huart);
    __HAL_UART_CLEAR_FEFLAG(huart);
    __HAL_UART_CLEAR_NEFLAG(huart);
    UART2_EnsureRx();
  }

  if(huart->Instance == USART6)
  {
    g_uart6Error = huart->ErrorCode;
    __HAL_UART_CLEAR_OREFLAG(huart);
    __HAL_UART_CLEAR_FEFLAG(huart);
    __HAL_UART_CLEAR_NEFLAG(huart);

    HAL_UART_Receive_IT(&huart6, &btRxChar, 1);
  }
}

void Error_Handler(void)
{
  __disable_irq();
  while (1)
  {
  }
}


#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
}
#endif
