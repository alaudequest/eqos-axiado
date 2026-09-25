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
#include "string.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "lan8742.h"
#include <stddef.h>
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
#define ETH_APP_RX_BUFFER_SIZE 1536U
#define ETH_APP_RX_POOL_SIZE (2U * ETH_RX_DESC_CNT)

typedef struct {
    ETH_BufferTypeDef AppBuff;
    uint8_t in_use;
    uint8_t buffer[ETH_APP_RX_BUFFER_SIZE] __ALIGNED(32);
} ETH_AppBuff;


typedef struct {
    uint8_t dest_mac[6];
    uint8_t src_mac[6];
    uint8_t type[2];
    uint8_t payload[100];
} ethernet_frame_t;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* DUAL_CORE_BOOT_SYNC_SEQUENCE: Define for dual core boot synchronization    */
/*                             demonstration code based on hardware semaphore */
/* This define is present in both CM7/CM4 projects                            */
/* To comment when developping/debugging on a single core                     */
#define DUAL_CORE_BOOT_SYNC_SEQUENCE

#if defined(DUAL_CORE_BOOT_SYNC_SEQUENCE)
#ifndef HSEM_ID_0
#define HSEM_ID_0 (0U) /* HW semaphore 0*/
#endif
#endif /* DUAL_CORE_BOOT_SYNC_SEQUENCE */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
#if defined ( __ICCARM__ ) /*!< IAR Compiler */
#pragma location=0x30000000
ETH_DMADescTypeDef  DMARxDscrTab[ETH_RX_DESC_CNT]; /* Ethernet Rx DMA Descriptors */
#pragma location=0x30000080
ETH_DMADescTypeDef  DMATxDscrTab[ETH_TX_DESC_CNT]; /* Ethernet Tx DMA Descriptors */

#elif defined ( __CC_ARM )  /* MDK ARM Compiler */

__attribute__((at(0x30000000))) ETH_DMADescTypeDef  DMARxDscrTab[ETH_RX_DESC_CNT]; /* Ethernet Rx DMA Descriptors */
__attribute__((at(0x30000080))) ETH_DMADescTypeDef  DMATxDscrTab[ETH_TX_DESC_CNT]; /* Ethernet Tx DMA Descriptors */

#elif defined ( __GNUC__ ) /* GNU Compiler */

ETH_DMADescTypeDef DMARxDscrTab[ETH_RX_DESC_CNT] __attribute__((section(".RxDescripSection"))); /* Ethernet Rx DMA Descriptors */
ETH_DMADescTypeDef DMATxDscrTab[ETH_TX_DESC_CNT] __attribute__((section(".TxDescripSection")));   /* Ethernet Tx DMA Descriptors */
#endif

ETH_TxPacketConfig TxConfig;

COM_InitTypeDef BspCOMInit;

ETH_HandleTypeDef heth;

/* USER CODE BEGIN PV */
uint8_t payload[] = "Hello world hehe lmao";
/* The current linker puts .bss in DMA-accessible RAM_D1; D-cache is disabled. */
static ETH_AppBuff rx_pool[ETH_APP_RX_POOL_SIZE];
static volatile uint8_t rx_pending;
static volatile uint8_t tx_complete;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_ETH_Init(void);
/* USER CODE BEGIN PFP */
int32_t ETH_PHY_INTERFACE_Init(void);
int32_t ETH_PHY_INTERFACE_DeInit (void);
int32_t ETH_PHY_INTERFACE_ReadReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t *pRegVal);
int32_t ETH_PHY_INTERFACE_WriteReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t RegVal);
int32_t ETH_PHY_INTERFACE_GetTick(void);
void ETH_StartLink();
void ETH_ConstructEthernetFrame(ethernet_frame_t *frame, uint8_t *dest_mac,
								uint8_t *src_mac, uint8_t *type, uint8_t *payload,
								uint16_t payload_len);




void ETH_DumpRegisters(void);

void ETH_DumpMAC(void);
void ETH_DumpMTL(void);
void ETH_DumpDMA(void);
void ETH_DumpPHY(void);

void ETH_DumpRCC(void);
void ETH_DumpGPIO(void);

void ETH_DumpTxDescriptors(void);
void ETH_DumpRxDescriptors(void);
void ETH_DumpRxPacket(ETH_BufferTypeDef *frame);
static void ETH_CompareRxPacket(const ETH_BufferTypeDef *rx,
                                const ETH_BufferTypeDef *tx);
static void ETH_FreeRxPacket(ETH_BufferTypeDef *frame);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
lan8742_Object_t LAN8742;
lan8742_IOCtx_t  LAN8742_IOCtx = {ETH_PHY_INTERFACE_Init,
                                  ETH_PHY_INTERFACE_DeInit,
                                  ETH_PHY_INTERFACE_WriteReg,
                                  ETH_PHY_INTERFACE_ReadReg,
                                  ETH_PHY_INTERFACE_GetTick};
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */
/* USER CODE BEGIN Boot_Mode_Sequence_0 */
//#if defined(DUAL_CORE_BOOT_SYNC_SEQUENCE)
//  int32_t timeout;
//#endif /* DUAL_CORE_BOOT_SYNC_SEQUENCE */
/* USER CODE END Boot_Mode_Sequence_0 */

/* USER CODE BEGIN Boot_Mode_Sequence_1 */
//#if defined(DUAL_CORE_BOOT_SYNC_SEQUENCE)
//  /* Wait until CPU2 boots and enters in stop mode or timeout*/
//  timeout = 0xFFFF;
//  while((__HAL_RCC_GET_FLAG(RCC_FLAG_D2CKRDY) != RESET) && (timeout-- > 0));
//  if ( timeout < 0 )
//  {
//  Error_Handler();
//  }
//#endif /* DUAL_CORE_BOOT_SYNC_SEQUENCE */
/* USER CODE END Boot_Mode_Sequence_1 */
  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */
ETH_BufferTypeDef TxBuffer;
ethernet_frame_t frame;
uint8_t dest_mac[] = {0x6C, 0x1F, 0xF7, 0xCA, 0x2B, 0xFD};
uint8_t src_mac[] =  {0x00, 0x80, 0xE1, 0x00, 0x00, 0x00};
uint8_t type[] = 	 {0x08, 0x00};

//uint8_t payload[] =  {0x41, 0x68, 0x6c, 0x61, 0x2c, 0x20, 0x57, 0x65, 0x6e, 0x74, 0x69, 0x3f, 0x41, 0x68, 0x6c, 0x61, 0x2c, 0x20, 0x57, 0x65, 0x6e, 0x74, 0x69, 0x3f, 0x41, 0x68, 0x6c, 0x61, 0x2c, 0x20, 0x57, 0x65, 0x6e, 0x74, 0x69, 0x3f, 0x41, 0x68, 0x6c, 0x61, 0x2c, 0x20, 0x57, 0x65, 0x6e, 0x74, 0x69, 0x3f};
uint16_t payload_len = sizeof(payload);
  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();
/* USER CODE BEGIN Boot_Mode_Sequence_2 */
//#if defined(DUAL_CORE_BOOT_SYNC_SEQUENCE)
///* When system initialization is finished, Cortex-M7 will release Cortex-M4 by means of
//HSEM notification */
///*HW semaphore Clock enable*/
//__HAL_RCC_HSEM_CLK_ENABLE();
///*Take HSEM */
//HAL_HSEM_FastTake(HSEM_ID_0);
///*Release HSEM in order to notify the CPU2(CM4)*/
//HAL_HSEM_Release(HSEM_ID_0,0);
///* wait until CPU2 wakes up from stop mode */
//timeout = 0xFFFF;
//while((__HAL_RCC_GET_FLAG(RCC_FLAG_D2CKRDY) == RESET) && (timeout-- > 0));
//if ( timeout < 0 )
//{
//Error_Handler();
//}
//#endif /* DUAL_CORE_BOOT_SYNC_SEQUENCE */
/* USER CODE END Boot_Mode_Sequence_2 */

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_ETH_Init();
  /* USER CODE BEGIN 2 */

  //Set bit LM (Loopback mode) thanh ghi MAC Configure
//  ETH->MACCR |= ETH_MACCR_LM_Msk;

  //Set bit PR (Promiscuous mode) cho phep MAC nhan tat ca Packet bypass qua tat ca filter.
  SET_BIT(heth.Instance->MACPFR, ETH_MACPFR_PR);

  //Build packet chuan bi gui ra
  ETH_ConstructEthernetFrame(&frame, dest_mac, src_mac, type, payload, payload_len);

  //Config cac gia tri cho ETH_Buffertypedef TxBuffer
  TxBuffer.buffer = (uint8_t *)&frame;
  TxBuffer.len = sizeof(dest_mac) + sizeof(src_mac) + sizeof(type) + payload_len;
  TxBuffer.next = NULL;
  TxConfig.TxBuffer = &TxBuffer;
  TxConfig.Length = TxBuffer.len;
  TxConfig.pData = &frame;
  /* This is raw test data, not an IPv4 packet to checksum. */
  TxConfig.Attributes = ETH_TX_PACKETS_FEATURES_CRCPAD;
  /* USER CODE END 2 */

  /* Initialize USER push-button, will be used to trigger an interrupt each time it's pressed.*/
  BSP_PB_Init(BUTTON_USER, BUTTON_MODE_EXTI);

  /* Initialize COM1 port (115200, 8 bits (7-bit data + 1 stop bit), no parity */
  BspCOMInit.BaudRate   = 115200;
  BspCOMInit.WordLength = COM_WORDLENGTH_8B;
  BspCOMInit.StopBits   = COM_STOPBITS_1;
  BspCOMInit.Parity     = COM_PARITY_NONE;
  BspCOMInit.HwFlowCtl  = COM_HWCONTROL_NONE;
  if (BSP_COM_Init(COM1, &BspCOMInit) != BSP_ERROR_NONE)
  {
    Error_Handler();
  }

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  printf("Program started!\n\r");
  fflush(0);

  //Check xem MAC Loopbackmode bit da SET
  printf("Loopbackmode LM bit - MAC config reg         %lu \r \n", (ETH->MACCR >> ETH_MACCR_LM_Pos)&1u);

  //Check xem thanh ghi MAC Packet filter da set bit PR (Promiscuous mode)
  printf("MAC Packet filter reg		0x%08lX \r \n", ETH->MACPFR);

  //Dump data TX de compare voi RX
  printf("TX bytes (before MAC padding/FCS):\r\n");
  for (uint32_t i = 0; i < TxBuffer.len; ++i)
  {
    printf("%02X%c", (unsigned int)TxBuffer.buffer[i],
           ((i + 1U) % 16U == 0U) ? '\n' : ' ');
  }
  printf("\r\n");

  //Bat dau TX packet dau tien
  if (HAL_ETH_Transmit(&heth, &TxConfig, HAL_MAX_DELAY) != HAL_OK)
  {
    //Neu truyen Packet failed se thong bao check status PHY link
	  printf("ETH transmit failed; check that the PHY link has started.\r\n");
  }

  while (1)
  {
	//Neu flag TX packet duoc set o ham TxCpItCallback -> Packet da truyen thanh cong
    if (tx_complete != 0U)
    {
    	//Reset Flag de cho lan nhan tiep theo
      tx_complete = 0U;

      //Release transmitted Tx Packets.
      HAL_ETH_ReleaseTxPacket(&heth);
      printf("Packet transmitted\r\n");
    }

    //Tao con tro void de chua Packet nhan ve
    void *packet = NULL;

    //ReadData thanh cong -> dump data ra de compare
    while (HAL_ETH_ReadData(&heth, &packet) == HAL_OK){

    	//Ep kieu du lieu ETH_BufferTypedef chua RxBuffer address va Packet length
    	ETH_BufferTypeDef *rx = (ETH_BufferTypeDef *)packet;

    	printf("============= RX Packet ================ \r \n");
    	printf("Packet address: %p \r \n", rx->buffer);
    	printf("Packet length: %lu \r \n", rx->len);
    	for(uint32_t i = 0; i < rx->len; i++){
    		printf("%02X ", rx->buffer[i]);
    		if ((i + 1U) % 16U == 0U){
    			printf("\r \n");
    		}
    	}


//    if (rx_pending != 0U)
//    {
//      void *packet = NULL;
//      /* Clear before draining so an interrupt during processing is retained. */
//      rx_pending = 0U;
//      while (HAL_ETH_ReadData(&heth, &packet) == HAL_OK)
//      {
//        ETH_BufferTypeDef *rx = packet;
//        ETH_DumpRxPacket(rx);
//        ETH_CompareRxPacket(rx, &TxBuffer);
//        ETH_FreeRxPacket(rx);
//        packet = NULL;
//      }

    }

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
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

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_DIRECT_SMPS_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
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
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 28;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 5;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
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
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ETH Initialization Function
  * @param None
  * @retval None
  */
static void MX_ETH_Init(void)
{

  /* USER CODE BEGIN ETH_Init 0 */

  /* USER CODE END ETH_Init 0 */

   static uint8_t MACAddr[6];

  /* USER CODE BEGIN ETH_Init 1 */

  /* USER CODE END ETH_Init 1 */
  heth.Instance = ETH;
  MACAddr[0] = 0x00;
  MACAddr[1] = 0x80;
  MACAddr[2] = 0xE1;
  MACAddr[3] = 0x00;
  MACAddr[4] = 0x00;
  MACAddr[5] = 0x00;
  heth.Init.MACAddr = &MACAddr[0];
  heth.Init.MediaInterface = HAL_ETH_RMII_MODE;
  heth.Init.TxDesc = DMATxDscrTab;
  heth.Init.RxDesc = DMARxDscrTab;
  heth.Init.RxBuffLen = ETH_APP_RX_BUFFER_SIZE;

  /* USER CODE BEGIN MACADDRESS */

  /* USER CODE END MACADDRESS */

  if (HAL_ETH_Init(&heth) != HAL_OK)
  {
    Error_Handler();
  }

  memset(&TxConfig, 0 , sizeof(ETH_TxPacketConfig));
  TxConfig.Attributes = ETH_TX_PACKETS_FEATURES_CSUM | ETH_TX_PACKETS_FEATURES_CRCPAD;
  TxConfig.ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC;
  TxConfig.CRCPadCtrl = ETH_CRC_PAD_INSERT;
  /* USER CODE BEGIN ETH_Init 2 */


  /* Set PHY IO functions */
    LAN8742_RegisterBusIO(&LAN8742, &LAN8742_IOCtx);

    /* Initialize the LAN8742 ETH PHY */
    LAN8742_Init(&LAN8742);

    /* Initialize link speed negotiation and start Ethernet peripheral */
    ETH_StartLink();
  /* USER CODE END ETH_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
/*===============================PHY Functions===============================*/

/* ============================================================
 * MAC
 * ============================================================ */
static void ETH_Dump_MAC(void)
{
    printf("\r\n");
    printf("========================================\r\n");
    printf("                 MTL\r\n");
    printf("========================================\r\n");

    printf("MTLOMR        = 0x%08lX\r\n", ETH->MTLOMR);
    printf("MTLISR        = 0x%08lX\r\n", ETH->MTLISR);

    printf("\r\n--- TX Queue 0 ---\r\n");

    printf("MTLTQOMR      = 0x%08lX\r\n", ETH->MTLTQOMR);
    printf("MTLTQUR       = 0x%08lX\r\n", ETH->MTLTQUR);
    printf("MTLTQDR       = 0x%08lX\r\n", ETH->MTLTQDR);

    printf("\r\n--- RX Queue 0 ---\r\n");

    printf("MTLRQOMR      = 0x%08lX\r\n", ETH->MTLRQOMR);
    printf("MTLRQMPOCR    = 0x%08lX\r\n", ETH->MTLRQMPOCR);
    printf("MTLRQDR       = 0x%08lX\r\n", ETH->MTLRQDR);
}


/* ============================================================
 * MTL
 * ============================================================ */
void ETH_Dump_MTL(void)
{
    printf("\r\n");
    printf("========================================\r\n");
    printf("                 MTL\r\n");
    printf("========================================\r\n");

    printf("MTLOMR        = 0x%08lX\r\n", ETH->MTLOMR);
    printf("MTLISR        = 0x%08lX\r\n", ETH->MTLISR);

    printf("\r\n--- TX Queue 0 ---\r\n");

    printf("MTLTQOMR      = 0x%08lX\r\n", ETH->MTLTQOMR);
    printf("MTLTQUR       = 0x%08lX\r\n", ETH->MTLTQUR);
    printf("MTLTQDR       = 0x%08lX\r\n", ETH->MTLTQDR);

    printf("\r\n--- RX Queue 0 ---\r\n");

    printf("MTLRQOMR      = 0x%08lX\r\n", ETH->MTLRQOMR);
    printf("MTLRQMPOCR    = 0x%08lX\r\n", ETH->MTLRQMPOCR);
    printf("MTLRQDR       = 0x%08lX\r\n", ETH->MTLRQDR);
}

/* ============================================================
 * DMA
 * ============================================================ */
static void ETH_Dump_DMA(void)
{
    printf("\r\n");
    printf("====================================================\r\n");
    printf("                    ETH DMA\r\n");
    printf("====================================================\r\n");

    printf("DMAMR          = 0x%08lX\r\n", ETH->DMAMR);
    printf("DMASBMR        = 0x%08lX\r\n", ETH->DMASBMR);
    printf("DMAISR         = 0x%08lX\r\n", ETH->DMAISR);
    printf("DMADSR         = 0x%08lX\r\n", ETH->DMADSR);

    /* ---------------- TX DMA ---------------- */

    printf("\r\n--- TX DMA ---\r\n");

    printf("DMACTCR        = 0x%08lX\r\n", ETH->DMACTCR);
    printf("DMACTDLAR      = 0x%08lX\r\n", ETH->DMACTDLAR);
    printf("DMACTDRLR      = 0x%08lX\r\n", ETH->DMACTDRLR);
    printf("DMACTDTPR      = 0x%08lX\r\n", ETH->DMACTDTPR);

    /* ---------------- RX DMA ---------------- */

    printf("\r\n--- RX DMA ---\r\n");

    printf("DMACRCR        = 0x%08lX\r\n", ETH->DMACRCR);
    printf("DMACRDLAR      = 0x%08lX\r\n", ETH->DMACRDLAR);
    printf("DMACRDRLR      = 0x%08lX\r\n", ETH->DMACRDRLR);
    printf("DMACRDTPR      = 0x%08lX\r\n", ETH->DMACRDTPR);

    /* Interrupt */
    printf("\r\n--- DMA Interrupt ---\r\n");

    printf("DMACIER        = 0x%08lX\r\n", ETH->DMACIER);
}


/* ============================================================
 * PHY
 * ============================================================ */

static void ETH_Dump_PHY(void)
{
    uint32_t reg;

    printf("\r\n");
    printf("====================================================\r\n");
    printf("                    PHY\r\n");
    printf("====================================================\r\n");

    for (uint32_t i = 0; i <= 31; i++)
    {
        if (HAL_ETH_ReadPHYRegister(&heth, 0, i, &reg) == HAL_OK)
        {
            printf("PHY[%02lu]       = 0x%04lX\r\n",
                   i, reg & 0xFFFFU);
        }
        else
        {
            printf("PHY[%02lu]       = READ ERROR\r\n", i);
        }
    }
}


/* ============================================================
 * COMPLETE ETH DUMP
 * ============================================================ */

void ETH_DumpRegisters(void)
{
    printf("\r\n\r\n");
    printf("####################################################\r\n");
    printf("#              STM32H755 ETH DUMP                 #\r\n");
    printf("####################################################\r\n");

    printf("ETH BASE       = 0x%08lX\r\n", (uint32_t)ETH);
    printf("ETH Handle     = 0x%08lX\r\n", (uint32_t)&heth);
    printf("MediaInterface = %lu\r\n",
           (uint32_t)heth.Init.MediaInterface);

    ETH_Dump_MAC();
    ETH_Dump_MTL();
    ETH_Dump_DMA();
    ETH_Dump_PHY();

    printf("\r\n");
    printf("####################################################\r\n");
    printf("#                  END ETH DUMP                    #\r\n");
    printf("####################################################\r\n");
}


int32_t ETH_PHY_INTERFACE_Init(void)
{
  /* Configure the MDIO Clock */
  HAL_ETH_SetMDIOClockRange(&heth);
  return 0;
}

int32_t ETH_PHY_INTERFACE_DeInit (void)
{
  return 0;
}

int32_t ETH_PHY_INTERFACE_ReadReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t *pRegVal)
{
  if(HAL_ETH_ReadPHYRegister(&heth, DevAddr, RegAddr, pRegVal) != HAL_OK)
  {
    return -1;
  }

  return 0;
}
int32_t ETH_PHY_INTERFACE_WriteReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t RegVal)
{
  if(HAL_ETH_WritePHYRegister(&heth, DevAddr, RegAddr, RegVal) != HAL_OK)
  {
    return -1;
  }

  return 0;
}

int32_t ETH_PHY_INTERFACE_GetTick(void)
{
  return HAL_GetTick();
}

/*===============================ETH Functions===============================*/

void ETH_StartLink()
{
  ETH_MACConfigTypeDef MACConf = {0};
  int32_t PHYLinkState = 0U;
  uint32_t linkchanged = 0U, speed = 0U, duplex =0U;

  PHYLinkState = LAN8742_GetLinkState(&LAN8742);

  if(PHYLinkState <= LAN8742_STATUS_LINK_DOWN)
  {
    HAL_ETH_Stop(&heth);
  }
  else if(PHYLinkState > LAN8742_STATUS_LINK_DOWN)
  {
    switch (PHYLinkState)
    {
    case LAN8742_STATUS_100MBITS_FULLDUPLEX:
      duplex = ETH_FULLDUPLEX_MODE;
      speed = ETH_SPEED_100M;
      linkchanged = 1;
      break;
    case LAN8742_STATUS_100MBITS_HALFDUPLEX:
      duplex = ETH_HALFDUPLEX_MODE;
      speed = ETH_SPEED_100M;
      linkchanged = 1;
      break;
    case LAN8742_STATUS_10MBITS_FULLDUPLEX:
      duplex = ETH_FULLDUPLEX_MODE;
      speed = ETH_SPEED_10M;
      linkchanged = 1;
      break;
    case LAN8742_STATUS_10MBITS_HALFDUPLEX:
      duplex = ETH_HALFDUPLEX_MODE;
      speed = ETH_SPEED_10M;
      linkchanged = 1;
      break;
    default:
      break;
    }

    if(linkchanged)
    {
      /* Get MAC Config MAC */
      HAL_ETH_GetMACConfig(&heth, &MACConf);
      MACConf.DuplexMode = duplex;
      MACConf.Speed = speed;
      MACConf.DropTCPIPChecksumErrorPacket = DISABLE; //changed
      MACConf.ForwardRxErrorPacket = ENABLE;   //FEP Bit
      MACConf.ForwardRxUndersizedGoodPacket = ENABLE; //FUP Bit
      MACConf.LoopbackMode= DISABLE;
      HAL_ETH_SetMACConfig(&heth, &MACConf);
      HAL_ETH_Start(&heth);
    }
  }
}

void HAL_ETH_RxAllocateCallback(uint8_t **buff)
{
    *buff = NULL;
    for (uint32_t i = 0; i < ETH_APP_RX_POOL_SIZE; ++i)
    {
        if (rx_pool[i].in_use == 0U)
        {
            rx_pool[i].in_use = 1U;
            rx_pool[i].AppBuff.buffer = rx_pool[i].buffer;
            rx_pool[i].AppBuff.len = 0U;
            rx_pool[i].AppBuff.next = NULL;
            *buff = rx_pool[i].buffer;
            return;
        }
    }
}

void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd, uint8_t *buff, uint16_t Length)
{
    ETH_AppBuff *app = (ETH_AppBuff *)(buff - offsetof(ETH_AppBuff, buffer));
    ETH_BufferTypeDef *p = &app->AppBuff;
    p->buffer = buff;
    p->len = Length;
    p->next = NULL;

    if (*pStart == NULL)
    {
        *pStart = p;
    }
    else
    {
        ((ETH_BufferTypeDef *)*pEnd)->next = p;
    }
    *pEnd = p;
}

static void ETH_FreeRxPacket(ETH_BufferTypeDef *frame)
{
    while (frame != NULL)
    {
        ETH_BufferTypeDef *next = frame->next;
        /* AppBuff is the first member; allocation and release run in main. */
        ((ETH_AppBuff *)frame)->in_use = 0U;
        frame = next;
    }
}

void HAL_ETH_TxCpltCallback(ETH_HandleTypeDef *heth)
{
    UNUSED(heth);
    //Flag TX Packet da truyen thanh cong duoc SET
    tx_complete = 1U;
}

void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *heth)
{
    UNUSED(heth);
    /* UART printf and HAL_ETH_ReadData are handled in the main loop. */
    rx_pending = 1U;
}

static void ETH_CompareRxPacket(const ETH_BufferTypeDef *rx,
                                const ETH_BufferTypeDef *tx)
{
    uint32_t offset = 0U;
    uint32_t mismatches = 0U;
    const ETH_BufferTypeDef *p = rx;

    while (p != NULL)
    {
        if ((p->buffer == NULL) || (p->len > ETH_APP_RX_BUFFER_SIZE))
        {
            printf("TX/RX comparison: invalid RX buffer\r\n");
            return;
        }
        for (uint32_t i = 0; i < p->len; ++i, ++offset)
        {
            if ((offset < tx->len) && (p->buffer[i] != tx->buffer[offset]))
            {
                if (mismatches == 0U)
                {
                    printf("First mismatch at byte %lu: TX=%02X RX=%02X\r\n",
                           (unsigned long)offset,
                           (unsigned int)tx->buffer[offset],
                           (unsigned int)p->buffer[i]);
                }
                ++mismatches;
            }
        }
        p = p->next;
    }
    /* Compare submitted bytes only: the MAC adds padding and possibly RX FCS. */
    printf("TX/RX submitted bytes: %s (TX=%lu RX=%lu, mismatches=%lu)\r\n",
           ((offset >= tx->len) && (mismatches == 0U)) ? "MATCH" : "FAIL",
           (unsigned long)tx->len, (unsigned long)offset,
           (unsigned long)mismatches);
    if (offset > tx->len)
    {
        printf("RX has %lu trailing bytes (MAC padding/FCS are not compared).\r\n",
               (unsigned long)(offset - tx->len));
    }
}

void ETH_ConstructEthernetFrame(ethernet_frame_t *frame, uint8_t *dest_mac, uint8_t *src_mac, uint8_t *type, uint8_t *payload, uint16_t payload_len)
{
    // Copy the destination MAC address
    memcpy(frame->dest_mac, dest_mac, 6);
    // Copy the source MAC address
    memcpy(frame->src_mac, src_mac, 6);
    // Set the Ethernet type field
    memcpy(frame->type, type, 2);
    // Copy the payload data
    memcpy(frame->payload, payload, payload_len);
}

void BSP_PB_Callback(Button_TypeDef Button)
{
  if (Button == BUTTON_USER)
  {
	  printf("Button press\r\n");
//      HAL_ETH_Transmit_IT(&heth, &TxConfig);
//      HAL_ETH_ReleaseTxPacket(&heth);
  } else {
      __NOP();
  }
}

void ETH_DumpRxPacket(ETH_BufferTypeDef *frame)
{
    ETH_BufferTypeDef *p = frame;
    uint32_t total = 0;

    printf("\r\n===== RX PACKET =====\r\n");

    while (p != NULL)
    {
        if ((p->buffer == NULL) || (p->len > ETH_APP_RX_BUFFER_SIZE))
        {
            printf("Invalid RX buffer or length\r\n");
            return;
        }
        printf("Buffer address : %p\r\n", (void *)p->buffer);
        printf("Buffer length  : %lu\r\n",
               (unsigned long)p->len);

        for (uint32_t i = 0; i < p->len; i++)
        {
            printf("%02X ", p->buffer[i]);

            if (((i + 1U) % 16U) == 0U)
            {
                printf("\r\n");
            }
        }

        if ((p->len % 16U) != 0U)
        {
            printf("\r\n");
        }

        total += p->len;
        p = p->next;
    }

    printf("Total RX length: %lu bytes\r\n",
           (unsigned long)total);

    printf("=====================\r\n");
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
