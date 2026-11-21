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

/* USER CODE BEGIN Includes */
#include "includes/eth_printf.h"
#include "includes/eth_functions_utils.h"
#include "includes/eth_function_stm32.h"
#include "includes/eth_flow_utils.h"
/* USER CODE END Includes */

/* USER CODE BEGIN PD */
/*
 * L0_MK_RMII_MAC_LOOPBACK on the Nucleo-H755ZI-Q (STM32H755, Cortex-M7).
 * main() only starts the board (the "boot" of the Kirkwood framework), then calls do_test():
 *   src/L0_MK_RMII_MAC_LOOPBACK.c   test options (eth_run_test_ops) and do_test()
 *   includes/eth_function_stm32.c   test flow eth_rmii_mac_lb(), STM32 clocks/pins/delays/UART
 *   includes/eth_functions.c        EQoS MAC: reset, DMA, rings, MTL, MAC, egress/ingress, traffic test
 *   includes/eth_filter_functions.c MAC address filter test (eth_run_test_ops = 1)
 *   includes/eth_burst_functions.c  DMA burst test, DMA_SysBus_Mode.FB / AAL (eth_run_test_ops = 2)
 *   includes/eth_ptp_functions.c    PTP timestamp test, MAC_Timestamp_Control / TDES2.TTSE (eth_run_test_ops = 3)
 *   includes/eth_vlan_functions.c   VLAN insertion test, TX context descriptor (eth_run_test_ops = 4), QinQ test (5)
 *   includes/eth_tso_functions.c    TSO test, MSS context descriptor, TDES3.TSE (eth_run_test_ops = 6)
 *   includes/eth_coe_functions.c    checksum offload test, TDES3.CIC / MAC_Configuration.IPC / RDES1 (eth_run_test_ops = 7)
 *   includes/eth_vlan_rx_functions.c VLAN filter test (eth_run_test_ops = 8), VLAN strip test (9): MAC_VLAN_Tag on RX
 *   includes/eth_sa_functions.c     SA insertion / replacement + CRC-pad test, TDES3 SAIC / CPC, MAC_Configuration.SARC (eth_run_test_ops = 10)
 *   includes/eth_addr_functions.c   MAC address filter test, MAC_Packet_Filter / MAC_Address1..3_High/Low / hash table (eth_run_test_ops = 11)
 *   includes/eth_istat_functions.c  interrupt status test, DMA_CH0_Status / DMA_Interrupt_Status / MTL_Q0_Interrupt_Control_Status / MAC_Interrupt_Status, no enable (eth_run_test_ops = 12)
 *   includes/eth_flow_functions.c   FLOW log of each step [1]..[11] (FLOW_LOG in eth_flow_utils.h)
 *   includes/eth_registers.h        base address, ring and buffer sizes, MMC counters
 *   includes/mac_defines.h          register offsets (R_*), bits and field values, descriptor layout
 *   includes/eth_traffic_test.h     test frame: size, count, addresses, timeouts
 */
/* USER CODE END PD */

/* USER CODE BEGIN PV */
// BSP UART configuration (COM1 = ST-LINK virtual COM port)
COM_InitTypeDef BspCOMInit;
// USER button: run do_test() again. volatile: written by the EXTI interrupt
static volatile uint8_t test_requested;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* MCU Configuration--------------------------------------------------------*/
  // HAL: SysTick 1 ms tick, NVIC priority grouping
  HAL_Init();
  // CPU/HCLK 224 MHz from HSI 64 MHz (see SystemClock_Config)
  SystemClock_Config();

  /* USER CODE BEGIN 2 */
  // USER button -> EXTI interrupt -> BSP_PB_Callback()
  BSP_PB_Init(BUTTON_USER, BUTTON_MODE_EXTI);
  // UART COM1 115200-8N1, no flow control: printf / eth_printf output
  BspCOMInit.BaudRate = 115200;
  BspCOMInit.WordLength = COM_WORDLENGTH_8B;
  BspCOMInit.StopBits = COM_STOPBITS_1;
  BspCOMInit.Parity = COM_PARITY_NONE;
  BspCOMInit.HwFlowCtl = COM_HWCONTROL_NONE;
  if (BSP_COM_Init(COM1, &BspCOMInit) != BSP_ERROR_NONE)
  {
    Error_Handler();
  }
  eth_printf("\n============ L0_MK_RMII_MAC_LOOPBACK ============\n");
  eth_printf("Press USER to run the test again\n");
  FLOW_STEP("[1]", "Board: HAL, clock, UART, USER button");
  FLOW("[1] HAL + SystemClock_Config done: SystemCoreClock = %lu Hz, HCLK = %lu Hz\n",
       (unsigned long)SystemCoreClock, (unsigned long)HAL_RCC_GetHCLKFreq());
  FLOW("[1] UART COM1 %lu-8N1, USER button = EXTI interrupt\n", (unsigned long)BspCOMInit.BaudRate);
  eth_stm32_print_chip_info();

  do_test();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    if (test_requested != 0U)
    {
      test_requested = 0U;
      do_test();
    }
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * HSI 64 MHz / PLLM 4 = 16 MHz; VCO = 16 x PLLN 28 = 448 MHz; PLLP 2 -> SYSCLK 224 MHz.
  * SYSCLK and AHB dividers /1 -> CPU = HCLK = 224 MHz (HCLK is the Ethernet CSR clock).
  * The PHY supplies the separate 50 MHz RMII REF_CLK. Recheck MAC_1US_Tic_Counter and
  * the DWT delays (eth_function_stm32.c) if these dividers change.
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  // Direct SMPS supply: must match the board power circuit (RM0399 Table 33)
  HAL_PWREx_ConfigSupply(PWR_DIRECT_SMPS_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  // Voltage scale 2 (RM0399 7.6.2)
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 4;                       // 64 / 4 = 16 MHz PLL input
  RCC_OscInitStruct.PLL.PLLN = 28;                      // 16 x 28 = 448 MHz VCO
  RCC_OscInitStruct.PLL.PLLP = 2;                       // 448 / 2 = 224 MHz SYSCLK
  RCC_OscInitStruct.PLL.PLLQ = 5;                       // 89.6 MHz, not used
  RCC_OscInitStruct.PLL.PLLR = 2;                       // 224 MHz, not used
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;    // PLL input range 8-16 MHz
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;    // wide VCO range
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;    // CPU 224 MHz
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV1;      // HCLK 224 MHz
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;     // 112 MHz
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV4;     // 56 MHz
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;     // 112 MHz
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;     // 112 MHz

  // 3 flash wait states (RM0399 Table 16)
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
/* Weak in the BSP. Called from the EXTI interrupt: only set the flag, the test runs in main() */
void BSP_PB_Callback(Button_TypeDef Button)
{
  if (Button == BUTTON_USER)
  {
    test_requested = 1U;
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
  printf("\r\n[!] Error_Handler(): program stopped\r\n");
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
