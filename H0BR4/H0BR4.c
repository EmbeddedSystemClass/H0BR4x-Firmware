/*
    BitzOS (BOS) V0.2.1 - Copyright (C) 2017-2020 Hexabitz
    All rights reserved

    File Name     : H0BR4.c
    Description   : Source code for module H0BR4.
										IMU (ST LSM6DS3TR) + Digital Compass (ST LSM303AGRTR)
		
		Required MCU resources : 
		
			>> USARTs 1,2,3,4,5,6 for module ports.
			>> I2C2 for LSM6DS3TR and LSM303AGRTR communication.
			>> GPIOB 12, GPIOA 6 for LSM6DS3TR IMU_INT1 and IMU_INT2.
			>> GPIOB 1, GPIOB 0 for LSM303AGRTR XL_INT1 and XL_INT2.
			>> GPIOA 7 for LSM303AGRTR MAG_INT.
			
*/
	
/* Includes ------------------------------------------------------------------*/
#include "BOS.h"

#include "LSM6DS3.h"
#include "LSM303AGR_ACC.h"
#include "LSM303AGR_MAG.h"

#include <math.h>

#define LSM303AGR_MAG_SENSITIVITY_FOR_FS_50G  1.5  /**< Sensitivity value for 16 gauss full scale [mgauss/LSB] */

#define MIN_MEMS_PERIOD_MS				200
#define MAX_MEMS_TIMEOUT_MS				0xFFFFFFFF


/* Define UART variables */
UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;
UART_HandleTypeDef huart3;
UART_HandleTypeDef huart4;
UART_HandleTypeDef huart5;
UART_HandleTypeDef huart6;

/* Module exported parameters ------------------------------------------------*/
float H0BR4_gyroX=0.0f;
float H0BR4_gyroY=0.0f;
float H0BR4_gyroZ=0.0f;
float H0BR4_accX=0.0f;
float H0BR4_accY=0.0f;
float H0BR4_accZ=0.0f;
int H0BR4_magX=0.0f;
int H0BR4_magY=0.0f;
int H0BR4_magZ=0.0f;
float H0BR4_temp=0.0f;

module_param_t modParam[NUM_MODULE_PARAMS] = {{.paramPtr=&H0BR4_gyroX, .paramFormat=FMT_FLOAT, .paramName="gyroX"},
{.paramPtr=&H0BR4_gyroY, .paramFormat=FMT_FLOAT, .paramName="gyroY"},
{.paramPtr=&H0BR4_gyroZ, .paramFormat=FMT_FLOAT, .paramName="gyroZ"},
{.paramPtr=&H0BR4_accX, .paramFormat=FMT_FLOAT, .paramName="accX"},
{.paramPtr=&H0BR4_accY, .paramFormat=FMT_FLOAT, .paramName="accY"},
{.paramPtr=&H0BR4_accZ, .paramFormat=FMT_FLOAT, .paramName="accZ"},
{.paramPtr=&H0BR4_magX, .paramFormat=FMT_INT32, .paramName="magX"},
{.paramPtr=&H0BR4_magY, .paramFormat=FMT_INT32, .paramName="magY"},
{.paramPtr=&H0BR4_magZ, .paramFormat=FMT_INT32, .paramName="magZ"},
{.paramPtr=&H0BR4_temp, .paramFormat=FMT_FLOAT, .paramName="temp"},
};

typedef Module_Status (*SampleMemsToPort)(uint8_t, uint8_t);
typedef Module_Status (*SampleMemsToString)(char *, size_t);
typedef Module_Status (*SampleMemsToBuffer)(float *buffer);

/* Private variables ---------------------------------------------------------*/
static bool stopStream = false;


/* Private function prototypes -----------------------------------------------*/
static Module_Status LSM6DS3Init(void);
//static Module_Status LSM303AccInit(void);
static Module_Status LSM303MagInit(void);

static Module_Status LSM6DS3SampleGyroMDPS(int *gyroX, int *gyroY, int *gyroZ);
static Module_Status LSM6DS3SampleGyroRaw(int16_t *gyroX, int16_t *gyroY, int16_t *gyroZ);

static Module_Status LSM6DS3SampleAccMG(int *accX, int *accY, int *accZ);
static Module_Status LSM6DS3SampleAccRaw(int16_t *accX, int16_t *accY, int16_t *accZ);

static Module_Status LSM6DS3SampleTempCelsius(float *temp);
static Module_Status LSM6DS3SampleTempFahrenheit(float *temp);

static Module_Status LSM303SampleMagMGauss(int *magX, int *magY, int *magZ);
static Module_Status LSM303SampleMagRaw(int16_t *magX, int16_t *magY, int16_t *magZ);

static Module_Status StreamMemsToPort(uint8_t port, uint8_t module, uint32_t period, uint32_t timeout, SampleMemsToPort function);
static Module_Status StreamMemsToCLI(uint32_t period, uint32_t timeout, SampleMemsToString function);
static Module_Status StreamMemsToBuf(float *buffer, uint32_t numDatapoints, uint32_t period, uint32_t timeout, 
																																						SampleMemsToBuffer function);



/* Create CLI commands --------------------------------------------------------*/
static portBASE_TYPE SampleSensorCommand(int8_t *pcWriteBuffer, size_t xWriteBufferLen, const int8_t *pcCommandString);
static portBASE_TYPE StreamSensorCommand(int8_t *pcWriteBuffer, size_t xWriteBufferLen, const int8_t *pcCommandString);
static portBASE_TYPE StopStreamCommand(int8_t *pcWriteBuffer, size_t xWriteBufferLen, const int8_t *pcCommandString);

const CLI_Command_Definition_t SampleCommandDefinition = {
	(const int8_t *) "sample",
	(const int8_t *) "sample:\r\n Syntax: sample [gyro]/[acc]/[mag]/[temp]\r\n \
\tGet filtered and calibrated Gyro, Acc, Mag or Temp values in \
dps, g, mguass or celsius units respectively.\r\n\r\n",
	SampleSensorCommand,
	1
};

const CLI_Command_Definition_t StreamCommandDefinition = {
	(const int8_t *) "stream",
	(const int8_t *) "stream:\r\n Syntax: stream [gyro]/[acc]/[mag]/[temp] (period in ms) (time in ms) [port]/[buffer] [module]\r\n \
\tGet stream of  filtered and calibrated Gyro, Acc, Mag or Temp values in \
dps, g, mguass or celsius units respectively. Press ENTER to stop the stream.\r\n\r\n",
	StreamSensorCommand,
	-1
};

const CLI_Command_Definition_t StopCommandDefinition = {
	(const int8_t *) "stop",
	(const int8_t *) "stop:\r\n Syntax: stop\r\n \
\tStop the current streaming of MEMS values. r\n\r\n",
	StopStreamCommand,
	0
};



/* -----------------------------------------------------------------------
	|												 Private Functions	 														|
   ----------------------------------------------------------------------- 
*/

/* --- H0BR4 module initialization. 
*/
void Module_Init(void)
{
	/* Peripheral clock enable */

	/* Array ports */
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();
  MX_USART3_UART_Init();
  MX_USART4_UART_Init();
  MX_USART5_UART_Init();
  MX_USART6_UART_Init();
	
	// TODO: Initialize I2C
	MX_I2C_Init();
	
	LSM6DS3Init();
	LSM303MagInit();
	
	// Disabling Accelerometer of LSM303AGR
	// LSM303AccInit();

}

/*-----------------------------------------------------------*/

/* --- H0BR4 message processing task. 
*/
Module_Status Module_MessagingTask(uint16_t code, uint8_t port, uint8_t src, uint8_t dst, uint8_t shift)
{
	Module_Status result = H0BR4_OK;
	uint32_t period = 0, timeout = 0;
	
	switch (code)
	{
		case CODE_H0BR4_GET_GYRO:
		{
			SampleGyroDPS(&H0BR4_gyroX, &H0BR4_gyroY, &H0BR4_gyroZ);
			
			break;
		}
		case CODE_H0BR4_GET_ACC:
		{
			SampleAccG(&H0BR4_accX, &H0BR4_accY, &H0BR4_accZ);
			
			break;
		}
		case CODE_H0BR4_GET_MAG:
		{
			SampleMagMGauss(&H0BR4_magX, &H0BR4_magY, &H0BR4_magZ);

			
			break;
		}
		case CODE_H0BR4_GET_TEMP:
		{
			SampleTempCelsius(&H0BR4_temp);
			
			break;
		}
		
		case CODE_H0BR4_STREAM_GYRO:
		{
			period = ( (uint32_t) cMessage[port-1][shift] << 24 ) + ( (uint32_t) cMessage[port-1][1+shift] << 16 ) + ( (uint32_t) cMessage[port-1][2+shift] << 8 ) + cMessage[port-1][3+shift];
			timeout = ( (uint32_t) cMessage[port-1][4+shift] << 24 ) + ( (uint32_t) cMessage[port-1][5+shift] << 16 ) + ( (uint32_t) cMessage[port-1][6+shift] << 8 ) + cMessage[port-1][7+shift];
			if ((result = StreamGyroDPSToPort(port, dst, period, timeout)) != H0BR4_OK)
				break;
			
			break;
		}

		case CODE_H0BR4_STREAM_ACC:
		{
			period = ( (uint32_t) cMessage[port-1][shift] << 24 ) + ( (uint32_t) cMessage[port-1][1+shift] << 16 ) + ( (uint32_t) cMessage[port-1][2+shift] << 8 ) + cMessage[port-1][3+shift];
			timeout = ( (uint32_t) cMessage[port-1][4+shift] << 24 ) + ( (uint32_t) cMessage[port-1][5+shift] << 16 ) + ( (uint32_t) cMessage[port-1][6+shift] << 8 ) + cMessage[port-1][7+shift];
			if ((result = StreamAccGToPort(port, dst, period, timeout)) != H0BR4_OK)
				break;
			
			break;
		}
		case CODE_H0BR4_STREAM_MAG:
		{
			period = ( (uint32_t) cMessage[port-1][shift] << 24 ) + ( (uint32_t) cMessage[port-1][1+shift] << 16 ) + ( (uint32_t) cMessage[port-1][2+shift] << 8 ) + cMessage[port-1][3+shift];
			timeout = ( (uint32_t) cMessage[port-1][4+shift] << 24 ) + ( (uint32_t) cMessage[port-1][5+shift] << 16 ) + ( (uint32_t) cMessage[port-1][6+shift] << 8 ) + cMessage[port-1][7+shift];
			if ((result = StreamMagMGaussToPort(port, dst, period, timeout)) != H0BR4_OK)
				break;
			
			break;
		}
		case CODE_H0BR4_STREAM_TEMP:
		{
			period = ( (uint32_t) cMessage[port-1][shift] << 24 ) + ( (uint32_t) cMessage[port-1][1+shift] << 16 ) + ( (uint32_t) cMessage[port-1][2+shift] << 8 ) + cMessage[port-1][3+shift];
			timeout = ( (uint32_t) cMessage[port-1][4+shift] << 24 ) + ( (uint32_t) cMessage[port-1][5+shift] << 16 ) + ( (uint32_t) cMessage[port-1][6+shift] << 8 ) + cMessage[port-1][7+shift];
			if ((result = StreamTempCToPort(port, dst, period, timeout)) != H0BR4_OK)
				break;
			
			break;
		}	
		case CODE_H0BR4_STREAM_STOP:
		{
			stopStreamMems();
			result = H0BR4_OK;
			break;
		}
		
		default:
			result = H0BR4_ERR_UnknownMessage;
			break;
	}			

	return result;	
}

/*-----------------------------------------------------------*/

/* --- Register this module CLI Commands 
*/
void RegisterModuleCLICommands(void)
{
	FreeRTOS_CLIRegisterCommand(&SampleCommandDefinition);
	FreeRTOS_CLIRegisterCommand(&StreamCommandDefinition);
	FreeRTOS_CLIRegisterCommand(&StopCommandDefinition);
}

/*-----------------------------------------------------------*/

/* --- Get the port for a given UART. 
*/
uint8_t GetPort(UART_HandleTypeDef *huart)
{
	if (huart->Instance == USART4)
		return P1;
	else if (huart->Instance == USART2)
		return P2;
	else if (huart->Instance == USART6)
		return P3;
	else if (huart->Instance == USART3)
		return P4;
	else if (huart->Instance == USART1)
		return P5;
	else if (huart->Instance == USART5)
		return P6;

	return 0;
}

/*-----------------------------------------------------------*/

static Module_Status LSM6D3Enable(void)
{
	// Check WHO_AM_I Register	
	uint8_t who_am_i = 0;
	if (LSM6DS3_ACC_GYRO_R_WHO_AM_I(&hi2c2, &who_am_i) != MEMS_SUCCESS)
		return H0BR4_ERR_LSM6DS3;
	
	if (who_am_i != LSM6DS3_ACC_GYRO_WHO_AM_I)
		return H0BR4_ERR_LSM6DS3;
	
	// Enable register address automatically incremented during a multiple byte access with a serial interface 
	if (LSM6DS3_ACC_GYRO_W_IF_Addr_Incr(&hi2c2, LSM6DS3_ACC_GYRO_IF_INC_ENABLED) != MEMS_SUCCESS)
		return H0BR4_ERR_LSM6DS3;
	
	// Bypass Mode
	if (LSM6DS3_ACC_GYRO_W_FIFO_MODE(&hi2c2, LSM6DS3_ACC_GYRO_FIFO_MODE_BYPASS) != MEMS_SUCCESS)
		return H0BR4_ERR_LSM6DS3;
	
	return H0BR4_OK;
}

static Module_Status LSM6D3SetupGyro(void)
{
	// Gyroscope ODR Init
	if (LSM6DS3_ACC_GYRO_W_ODR_G(&hi2c2, LSM6DS3_ACC_GYRO_ODR_G_13Hz) != MEMS_SUCCESS)
		return H0BR4_ERR_LSM303;
	
	// Gyroscope FS Init
	if (LSM6DS3_ACC_GYRO_W_FS_G(&hi2c2, LSM6DS3_ACC_GYRO_FS_G_2000dps) != MEMS_SUCCESS)
		return H0BR4_ERR_LSM6DS3;
	
	// Gyroscope Axes Status Init
	if (LSM6DS3_ACC_GYRO_W_XEN_G(&hi2c2, LSM6DS3_ACC_GYRO_XEN_G_ENABLED) != MEMS_SUCCESS)
		return H0BR4_ERR_LSM6DS3;
	
	if (LSM6DS3_ACC_GYRO_W_YEN_G(&hi2c2, LSM6DS3_ACC_GYRO_YEN_G_ENABLED) != MEMS_SUCCESS)
		return H0BR4_ERR_LSM6DS3;
	
	if (LSM6DS3_ACC_GYRO_W_ZEN_G(&hi2c2, LSM6DS3_ACC_GYRO_ZEN_G_ENABLED) != MEMS_SUCCESS)
		return H0BR4_ERR_LSM6DS3;
	
	return H0BR4_OK;
}

static Module_Status LSM6D3SetupAcc(void)
{
	// Accelerometer ODR Init
	if (LSM6DS3_ACC_GYRO_W_ODR_XL(&hi2c2, LSM6DS3_ACC_GYRO_ODR_XL_104Hz) != MEMS_SUCCESS)
		return H0BR4_ERR_LSM6DS3;
	
	// Bandwidth Selection
	// Selection of bandwidth and ODR should be in accordance of Nyquist Sampling theorem!
	if (LSM6DS3_ACC_GYRO_W_BW_XL(&hi2c2, LSM6DS3_ACC_GYRO_BW_XL_50Hz) != MEMS_SUCCESS)
		return H0BR4_ERR_LSM6DS3;
	
	// Accelerometer FS Init
	if (LSM6DS3_ACC_GYRO_W_FS_XL(&hi2c2, LSM6DS3_ACC_GYRO_FS_XL_16g) != MEMS_SUCCESS)
		return H0BR4_ERR_LSM6DS3;
	
	// Accelerometer Axes Status Init
	if (LSM6DS3_ACC_GYRO_W_XEN_XL(&hi2c2, LSM6DS3_ACC_GYRO_XEN_XL_ENABLED) != MEMS_SUCCESS)
		return H0BR4_ERR_LSM6DS3;
	
	if (LSM6DS3_ACC_GYRO_W_YEN_XL(&hi2c2, LSM6DS3_ACC_GYRO_YEN_XL_ENABLED) != MEMS_SUCCESS)
		return H0BR4_ERR_LSM6DS3;
		
	if (LSM6DS3_ACC_GYRO_W_ZEN_XL(&hi2c2, LSM6DS3_ACC_GYRO_ZEN_XL_ENABLED) != MEMS_SUCCESS)
		return H0BR4_ERR_LSM6DS3;
	
	// Enable Bandwidth Scaling
	if (LSM6DS3_ACC_GYRO_W_BW_Fixed_By_ODR(&hi2c2, LSM6DS3_ACC_GYRO_BW_SCAL_ODR_ENABLED) != MEMS_ERROR)
		return H0BR4_ERR_LSM6DS3;
	
	return H0BR4_OK;
}



static Module_Status LSM6DS3SampleGyroRaw(int16_t *gyroX, int16_t *gyroY, int16_t *gyroZ)
{
	uint8_t temp[6];
	if (LSM6DS3_ACC_GYRO_GetRawGyroData(&hi2c2, temp) != MEMS_SUCCESS)
		return H0BR4_ERR_LSM6DS3;
	
	*gyroX = concatBytes(temp[1], temp[0]);
	*gyroY = concatBytes(temp[3], temp[2]);
	*gyroZ = concatBytes(temp[5], temp[4]);
	
	return H0BR4_OK;
}

static Module_Status LSM6DS3SampleGyroMDPS(int *gyroX, int *gyroY, int *gyroZ)
{
	int buff[3];
	if (LSM6DS3_ACC_Get_AngularRate(&hi2c2, buff, 0) != MEMS_SUCCESS)
		return H0BR4_ERR_LSM6DS3;
	
	*gyroX = buff[0];
	*gyroY = buff[1];
	*gyroZ = buff[2];
	
	return H0BR4_OK;
}

static Module_Status LSM6DS3SampleAccRaw(int16_t *accX, int16_t *accY, int16_t *accZ)
{
	uint8_t temp[6];
	if (LSM6DS3_ACC_GYRO_GetRawAccData(&hi2c2, temp) != MEMS_SUCCESS)
		return H0BR4_ERR_LSM6DS3;
	
	*accX = concatBytes(temp[1], temp[0]);
	*accY = concatBytes(temp[3], temp[2]);
	*accZ = concatBytes(temp[5], temp[4]);
	
	return H0BR4_OK;
}

static Module_Status LSM6DS3SampleAccMG(int *accX, int *accY, int *accZ)
{
	int buff[3];
	if (LSM6DS3_ACC_Get_Acceleration(&hi2c2, buff, 0) != MEMS_SUCCESS)
		return H0BR4_ERR_LSM6DS3;
	
	*accX = buff[0];
	*accY = buff[1];
	*accZ = buff[2];
	
	return H0BR4_OK;
}

static Module_Status LSM6DS3SampleTempCelsius(float *temp)
{
	uint8_t buff[2];
	if (LSM6DS3_ACC_GYRO_ReadReg(&hi2c2, LSM6DS3_ACC_GYRO_OUT_TEMP_L, buff, 2) != MEMS_SUCCESS)
		return H0BR4_ERR_LSM6DS3;
	
	int16_t rawTemp = concatBytes(buff[0], buff[1]);
	*temp = (((float)rawTemp)/16) + 25;
	
	return H0BR4_OK;
}

static Module_Status LSM6DS3SampleTempFahrenheit(float *temp)
{
	Module_Status status = H0BR4_OK;
	float celsius = 0;
	
	if ((status = LSM6DS3SampleTempCelsius(&celsius)) != H0BR4_OK)
		return status;
	
	*temp = celsiusToFahrenheit(celsius);
	return H0BR4_OK;
}

static Module_Status LSM6DS3Init(void)
{
	// Common Init
	Module_Status status = H0BR4_OK;
	if ((status = LSM6D3Enable()) != H0BR4_OK)
		return status;
	
	if ((status = LSM6D3SetupGyro()) != H0BR4_OK)
		return status;
	
	if ((status = LSM6D3SetupAcc()) != H0BR4_OK)
		return status;
	
	// TODO: Configure Interrupt Lines
	
	return status;
}


//static Module_Status LSM303AccInit(void)
//{
//	// Check WhoAmI
//	uint8_t who_am_i;
//	if (LSM303AGR_ACC_R_WHO_AM_I(&hi2c2, &who_am_i ) != MEMS_SUCCESS)
//    return H0BR4_ERR_LSM303;
//
//	if (who_am_i != LSM303AGR_ACC_WHO_AM_I)
//		return H0BR4_ERR_LSM303;
//	
//	// Enable Block Data update
//	if (LSM303AGR_ACC_W_BlockDataUpdate(&hi2c2, LSM303AGR_ACC_BDU_ENABLED) != MEMS_SUCCESS)
//    return H0BR4_ERR_LSM303;
//	
//	// FIFO Mode: ByPass
//	if (LSM303AGR_ACC_W_FifoMode(&hi2c2, LSM303AGR_ACC_FM_BYPASS) != MEMS_SUCCESS)
//    return H0BR4_ERR_LSM303;
//	
//	// ODR: Power Down
//	if (LSM303AGR_ACC_W_ODR(&hi2c2, LSM303AGR_ACC_ODR_DO_PWR_DOWN) != MEMS_SUCCESS)
//    return H0BR4_ERR_LSM303;
//	
//	// Operating mode Selection: High Resolution
//	if (LSM303AGR_ACC_W_HiRes(&hi2c2, LSM303AGR_ACC_HR_ENABLED) != MEMS_SUCCESS)
//    return H0BR4_ERR_LSM303;
//	
//	// Operating mode Selection: Low Power Disable
//	if (LSM303AGR_ACC_W_LOWPWR_EN(&hi2c2, LSM303AGR_ACC_LPEN_DISABLED) != MEMS_SUCCESS)
//    return H0BR4_ERR_LSM303;
//	
//	// Full Scale Selection
//	if (LSM303AGR_ACC_W_FullScale(&hi2c2, LSM303AGR_ACC_FS_8G) != MEMS_SUCCESS)
//    return H0BR4_ERR_LSM303;
//	
//	// Enable Axes: X, Y, Z
//	if (LSM303AGR_ACC_W_XEN(&hi2c2, LSM303AGR_ACC_XEN_ENABLED) != MEMS_SUCCESS)
//    return H0BR4_ERR_LSM303;
//	
//	if (LSM303AGR_ACC_W_YEN(&hi2c2, LSM303AGR_ACC_YEN_ENABLED) != MEMS_SUCCESS)
//    return H0BR4_ERR_LSM303;
//	
//	if (LSM303AGR_ACC_W_ZEN(&hi2c2, LSM303AGR_ACC_ZEN_ENABLED) != MEMS_SUCCESS)
//    return H0BR4_ERR_LSM303;
//	
//	return H0BR4_OK;
//}

//static Module_Status LSM303AccDeInit(void)
//{
//	// Check WhoAmI
//	uint8_t who_am_i;
//	if ( LSM303AGR_ACC_R_WHO_AM_I(&hi2c2, &who_am_i ) != MEMS_SUCCESS)
//    return H0BR4_ERR_LSM303;
//  
//	if (who_am_i != LSM303AGR_ACC_WHO_AM_I)
//		return H0BR4_ERR_LSM303;
//	
//	// ODR: Power Down
//	if (LSM303AGR_ACC_W_ODR(&hi2c2, LSM303AGR_ACC_ODR_DO_PWR_DOWN) != MEMS_SUCCESS)
//    return H0BR4_ERR_LSM303;
//	
//	return H0BR4_OK;
//}

//static Module_Status LSM303AccGetAxis(int *accX, int *accY, int *accZ)
//{
//	int data[3];
//  /* Read data from LSM303AGR. */
//  if (LSM303AGR_ACC_Get_Acceleration(&hi2c2, data) != MEMS_SUCCESS)
//    return H0BR4_ERR_LSM303;
//	
//  /* Calculate the data. */
//  *accX = data[0];
//  *accX = data[1];
//  *accZ = data[2];
//
//  return H0BR4_OK;
//}

//static Module_Status LSM303AccGetAxesRaw(int16_t *accX, int16_t *accY, int16_t *accZ)
//{
//  Type3Axis16bit_U raw_data_tmp;
//  uint8_t shift = 0;
//  LSM303AGR_ACC_LPEN_t lp;
//  LSM303AGR_ACC_HR_t hr;
//
//  /* Determine which operational mode the acc is set */
//  if (LSM303AGR_ACC_R_HiRes(&hi2c2, &hr) != MEMS_SUCCESS)
//    return H0BR4_ERR_LSM303;
//
//  if (LSM303AGR_ACC_R_LOWPWR_EN(&hi2c2, &lp) != MEMS_SUCCESS)
//    return H0BR4_ERR_LSM303;
//
//  if ((lp == LSM303AGR_ACC_LPEN_ENABLED) && (hr == LSM303AGR_ACC_HR_DISABLED)) {
//    /* op mode is LP 8-bit */
//    shift = 8;
//  } else if ((lp == LSM303AGR_ACC_LPEN_DISABLED) && (hr == LSM303AGR_ACC_HR_DISABLED)) {
//    /* op mode is Normal 10-bit */
//    shift = 6;
//  } else if ((lp == LSM303AGR_ACC_LPEN_DISABLED) && (hr == LSM303AGR_ACC_HR_ENABLED)) {
//    /* op mode is HR 12-bit */
//    shift = 4;
//  } else {
//    return H0BR4_ERR_LSM303;
//  }
//
//  /* Read output registers from LSM303AGR_ACC_GYRO_OUTX_L_XL to LSM303AGR_ACC_GYRO_OUTZ_H_XL. */
//  if (LSM303AGR_ACC_Get_Raw_Acceleration(&hi2c2, raw_data_tmp.u8bit) != MEMS_SUCCESS)
//    return H0BR4_ERR_LSM303;
//
//  /* Format the data. */
//  *accX = (raw_data_tmp.i16bit[0] >> shift);
//  *accY = (raw_data_tmp.i16bit[1] >> shift);
//  *accZ = (raw_data_tmp.i16bit[2] >> shift);
//
//  return H0BR4_OK;
//}

//static Module_Status LSM303AccGetDRDYStatus(bool *status)
//{
//  LSM303AGR_ACC_XDA_t status_raw;
//  if (LSM303AGR_ACC_R_XDataAvail(&hi2c2, &status_raw ) != MEMS_SUCCESS)
//    return H0BR4_ERR_LSM303;
//  
//  switch (status_raw) {
//    case LSM303AGR_ACC_XDA_AVAILABLE:
//      *status = true;
//      break;
//    case LSM303AGR_ACC_XDA_NOT_AVAILABLE:
//      *status = false;
//      break;
//    default:
//      return H0BR4_ERR_LSM303;
//  }
//  return H0BR4_OK;
//}

//static Module_Status LSM303AccClearDRDY(void)
//{
//  uint8_t regValue[6];
//	
//	if (LSM303AGR_ACC_ReadReg(&hi2c2, LSM303AGR_ACC_OUT_X_L, regValue, 2) != MEMS_SUCCESS)
//    return H0BR4_ERR_LSM303;
//  
//	
//	if (LSM303AGR_ACC_ReadReg(&hi2c2, LSM303AGR_ACC_OUT_Y_L, regValue, 2) != MEMS_SUCCESS)
//    return H0BR4_ERR_LSM303;
//	
//	if (LSM303AGR_ACC_ReadReg(&hi2c2, LSM303AGR_ACC_OUT_Z_L, regValue, 2) != MEMS_SUCCESS)
//    return H0BR4_ERR_LSM303;
//	
//  return H0BR4_OK;
//}

static Module_Status LSM303MagEnable(void)
{
	if (LSM303AGR_MAG_W_MD(&hi2c2, LSM303AGR_MAG_MD_CONTINUOS_MODE) != MEMS_SUCCESS)
    return H0BR4_ERR_LSM303;
	
	return H0BR4_OK;
}

//static Module_Status LSM303MagDisable(void)
//{
//	if (LSM303AGR_MAG_W_MD(&hi2c2, LSM303AGR_MAG_MD_IDLE1_MODE) != MEMS_SUCCESS)
//    return H0BR4_ERR_LSM303;
//	
//	return H0BR4_OK;
//}

static Module_Status LSM303MagInit(void)
{
	// Check the Sensor
	uint8_t who_am_i = 0x00;
  if (LSM303AGR_MAG_R_WHO_AM_I(&hi2c2, &who_am_i ) != MEMS_SUCCESS)
    return H0BR4_ERR_LSM303;
  
  if (who_am_i != LSM303AGR_MAG_WHO_AM_I)
    return H0BR4_ERR_LSM303;
	
	// Operating Mode: Power Down
	if (LSM303AGR_MAG_W_MD(&hi2c2, LSM303AGR_MAG_MD_IDLE1_MODE) != MEMS_SUCCESS)
    return H0BR4_ERR_LSM303;
	
	// Enable Block Data Update
  if (LSM303AGR_MAG_W_BDU(&hi2c2, LSM303AGR_MAG_BDU_ENABLED ) != MEMS_SUCCESS)
    return H0BR4_ERR_LSM303;
	
	// TODO: Change the default ODR
	if (LSM303AGR_MAG_W_ODR(&hi2c2, LSM303AGR_MAG_ODR_10Hz) != MEMS_SUCCESS)
    return H0BR4_ERR_LSM303;

	// Self Test Disabled
	if (LSM303AGR_MAG_W_ST(&hi2c2, LSM303AGR_MAG_ST_DISABLED) != MEMS_SUCCESS)
    return H0BR4_ERR_LSM303;

  return LSM303MagEnable();
}

//static Module_Status LSM303MagDeInit(void)
//{
//	// Check the Sensor
//	uint8_t who_am_i = 0x00;
//  if (LSM303AGR_MAG_R_WHO_AM_I(&hi2c2, &who_am_i ) != MEMS_SUCCESS)
//    return H0BR4_ERR_LSM303;
//	
//  if (who_am_i != LSM303AGR_MAG_WHO_AM_I)
//    return H0BR4_ERR_LSM303;
//	
//	return LSM303MagDisable();
//}

static Module_Status LSM303SampleMagRaw(int16_t *magX, int16_t *magY, int16_t *magZ)
{
	int16_t *pData;
	uint8_t data[6];
	
	memset(data, 0, sizeof(data));
	
	if (LSM303AGR_MAG_Get_Raw_Magnetic(&hi2c2, data) != MEMS_SUCCESS)
		return H0BR4_ERR_LSM303;
	
	pData = (int16_t *)data;
	*magX = pData[0];
	*magY = pData[1];
	*magZ = pData[2];
	
	return H0BR4_OK;
}

static Module_Status LSM303SampleMagMGauss(int *magX, int *magY, int *magZ)
{
	Module_Status status = H0BR4_OK;
  int16_t rawMagX, rawMagY, rawMagZ;

  /* Read raw data from LSM303AGR output register. */
  if ((status = LSM303SampleMagRaw(&rawMagX, &rawMagY, &rawMagZ)) != H0BR4_OK)
    return status;

  /* Set the raw data. */
  *magX = rawMagX * (float)LSM303AGR_MAG_SENSITIVITY_FOR_FS_50G;
  *magY = rawMagY * (float)LSM303AGR_MAG_SENSITIVITY_FOR_FS_50G;
  *magZ = rawMagZ * (float)LSM303AGR_MAG_SENSITIVITY_FOR_FS_50G;
  return status;
}

//static Module_Status LSM303MagGetDRDYStatus(bool *status)
//{
//  LSM303AGR_MAG_ZYXDA_t status_raw;
//
//  if (LSM303AGR_MAG_R_ZYXDA(&hi2c2, &status_raw ) != MEMS_SUCCESS)
//    return H0BR4_ERR_LSM303;
//
//  switch (status_raw) {
//    case LSM303AGR_MAG_ZYXDA_EV_ON:
//      *status = true;
//      break;
//    case LSM303AGR_MAG_ZYXDA_EV_OFF:
//      *status = false;
//      break;
//    default:
//      return H0BR4_ERR_LSM303;
//  }
//  return H0BR4_OK;
//}

static Module_Status PollingSleepCLISafe(uint32_t period)
{
	const unsigned DELTA_SLEEP_MS = 100; // milliseconds
	long numDeltaDelay =  period / DELTA_SLEEP_MS;
	unsigned lastDelayMS = period % DELTA_SLEEP_MS;
	
	while (numDeltaDelay-- > 0) {
		vTaskDelay(pdMS_TO_TICKS(DELTA_SLEEP_MS));
		
		// Look for ENTER key to stop the stream
		for (uint8_t chr=0 ; chr<MSG_RX_BUF_SIZE ; chr++)
		{
			if (UARTRxBuf[PcPort-1][chr] == '\r') {
				UARTRxBuf[PcPort-1][chr] = 0;
				return H0BR4_ERR_TERMINATED;
			}
		}
		
		if (stopStream)
			return H0BR4_ERR_TERMINATED;
	}
	
	vTaskDelay(pdMS_TO_TICKS(lastDelayMS));
	return H0BR4_OK;
}


static Module_Status StreamMemsToPort(uint8_t port, uint8_t module, uint32_t period, uint32_t timeout, SampleMemsToPort function)
{
	Module_Status status = H0BR4_OK;
	
	if (period < MIN_MEMS_PERIOD_MS)
		return H0BR4_ERR_WrongParams;
	if (port == 0)
		return H0BR4_ERR_WrongParams;
	if (port == PcPort) // Check if CLI is not enabled at that port!
		return H0BR4_ERR_BUSY;
	
	if (period > timeout)
		timeout = period;
	
	long numTimes = timeout / period;
	stopStream = false;
	
	while ((numTimes-- > 0) || (timeout >= MAX_MEMS_TIMEOUT_MS)) {
		if ((status = function(port, module)) != H0BR4_OK)
			break;
		
		vTaskDelay(pdMS_TO_TICKS(period));
		if (stopStream) {
			status = H0BR4_ERR_TERMINATED;
			break;
		}
	}
	return status;
}

static Module_Status StreamMemsToCLI(uint32_t period, uint32_t timeout, SampleMemsToString function)
{
	Module_Status status = H0BR4_OK;
	int8_t *pcOutputString = NULL;
	
	if (period < MIN_MEMS_PERIOD_MS)
		return H0BR4_ERR_WrongParams;

	// TODO: Check if CLI is enable or not
	
	if (period > timeout)
		timeout = period;
	
	long numTimes = timeout / period;
	stopStream = false;
	
	while ((numTimes-- > 0) || (timeout >= MAX_MEMS_TIMEOUT_MS)) {
		pcOutputString = FreeRTOS_CLIGetOutputBuffer();
		if ((status = function((char *)pcOutputString, 100)) != H0BR4_OK)
			break;
		
		writePxMutex(PcPort, (char *)pcOutputString, strlen((char *)pcOutputString), cmd500ms, HAL_MAX_DELAY);
		if (PollingSleepCLISafe(period) != H0BR4_OK)
			break;
	}

	memset((char *) pcOutputString, 0, configCOMMAND_INT_MAX_OUTPUT_SIZE);
  sprintf((char *)pcOutputString, "\r\n");
	return status;
}

static Module_Status StreamMemsToBuf( float *buffer, uint32_t numDatapoints, uint32_t period, uint32_t timeout, 
																																						SampleMemsToBuffer function)
{
	Module_Status status = H0BR4_OK;
	
	if (period < MIN_MEMS_PERIOD_MS)
		return H0BR4_ERR_WrongParams;

	// TODO: Check if CLI is enable or not
	
	if (period > timeout)
		timeout = period;
	
	long numTimes = timeout / period;
	stopStream = false;
	
	while ((numTimes-- > 0) || (timeout >= MAX_MEMS_TIMEOUT_MS)) {
		if ((status = function(buffer)) != H0BR4_OK)
			break;
		
		buffer += numDatapoints;
		
		vTaskDelay(pdMS_TO_TICKS(period));
		if (stopStream) {
			status = H0BR4_ERR_TERMINATED;
			break;
		}
	}
	return status;
}




/* -----------------------------------------------------------------------
	|																APIs	 																 	|
   ----------------------------------------------------------------------- 
*/

Module_Status SampleGyroMDPS(int *gyroX, int *gyroY, int *gyroZ)
{
	return LSM6DS3SampleGyroMDPS(gyroX, gyroY, gyroZ);
}

Module_Status SampleGyroRaw(int16_t *gyroX, int16_t *gyroY, int16_t *gyroZ)
{
	return LSM6DS3SampleGyroRaw(gyroX, gyroY, gyroZ);
}


Module_Status SampleGyroDPSToPort(uint8_t port, uint8_t module)
{
	float buffer[3]; // Three Samples X, Y, Z
	static uint8_t temp[12];
	Module_Status status = H0BR4_OK;
	
	if ((status = SampleGyroDPSToBuf(buffer)) != H0BR4_OK)
		return status;
	
	/*memcpy(messageParams, buffer, sizeof(buffer));
	if (SendMessageFromPort(port, myID, module, CODE_H0BR4_RESULT_GYRO, sizeof(buffer)) != BOS_OK)
		status = H0BR4_ERR_IO;*/
	
	if (module==myID){
						temp[0] = *((__IO uint8_t *)(&buffer[0])+3);  temp[1] = *((__IO uint8_t *)(&buffer[0])+2);
						temp[2] = *((__IO uint8_t *)(&buffer[0])+1);  temp[3] = *((__IO uint8_t *)(&buffer[0])+0);
		
		        temp[4] = *((__IO uint8_t *)(&buffer[1])+3);  temp[5] = *((__IO uint8_t *)(&buffer[1])+2);
						temp[6] = *((__IO uint8_t *)(&buffer[1])+1);  temp[7] = *((__IO uint8_t *)(&buffer[1])+0);

		        temp[8] = *((__IO uint8_t *)(&buffer[2])+3);  temp[9] = *((__IO uint8_t *)(&buffer[2])+2);
						temp[10] = *((__IO uint8_t *)(&buffer[2])+1); temp[11] = *((__IO uint8_t *)(&buffer[2])+0);

						writePxITMutex(port, (char *)&temp[0], 12*sizeof(uint8_t), 10);
						//memset(temp,0,12*sizeof(uint8_t));
				}
			else{
						messageParams[0]=port;
					  messageParams[1] = *((__IO uint8_t *)(&buffer[0])+3);  messageParams[2] = *((__IO uint8_t *)(&buffer[0])+2);
						messageParams[3] = *((__IO uint8_t *)(&buffer[0])+1);  messageParams[4] = *((__IO uint8_t *)(&buffer[0])+0);
				
					  messageParams[5] = *((__IO uint8_t *)(&buffer[1])+3);  messageParams[6] = *((__IO uint8_t *)(&buffer[1])+2);
						messageParams[7] = *((__IO uint8_t *)(&buffer[1])+1);  messageParams[8] = *((__IO uint8_t *)(&buffer[1])+0);
				
					  messageParams[9] = *((__IO uint8_t *)(&buffer[2])+3);  messageParams[10] = *((__IO uint8_t *)(&buffer[2])+2);
						messageParams[11] = *((__IO uint8_t *)(&buffer[2])+1); messageParams[12] = *((__IO uint8_t *)(&buffer[2])+0);
						SendMessageToModule(module, CODE_PORT_FORWARD, (sizeof(float)*3)+1);
					}
	
	return status;
}

Module_Status SampleGyroDPSToString(char *cstring, size_t maxLen)
{
	Module_Status status = H0BR4_OK;
	float x = 0, y = 0, z = 0;
	
	if ((status = SampleGyroDPS(&x, &y, &z)) != H0BR4_OK)
		return status;
	
	snprintf(cstring, maxLen, "Gyro(DPS) | X: %.2f, Y: %.2f, Z: %.2f\r\n", x, y, z);
	return status;
}

Module_Status SampleGyroDPS(float *x, float *y, float *z)
{
	Module_Status status = H0BR4_OK;
	int xInMDPS = 0, yInMDPS = 0, zInMDPS = 0;
	
	if ((status = LSM6DS3SampleGyroMDPS(&xInMDPS, &yInMDPS, &zInMDPS)) != H0BR4_OK)
		return status;
	
	*x = ((float)xInMDPS) / 1000;
	*y = ((float)yInMDPS) / 1000;
	*z = ((float)zInMDPS) / 1000;
	
	return status;
}

Module_Status SampleGyroDPSToBuf(float *buffer)
{
	return SampleGyroDPS(buffer, buffer + 1, buffer + 2);
}

Module_Status SampleAccMG(int *accX, int *accY, int *accZ)
{
	return LSM6DS3SampleAccMG(accX, accY, accZ);
}

Module_Status SampleAccRaw(int16_t *accX, int16_t *accY, int16_t *accZ)
{
	return LSM6DS3SampleAccRaw(accX, accY, accZ);
}

Module_Status SampleAccGToPort(uint8_t port, uint8_t module)
{
	float buffer[3]; // Three Samples X, Y, Z
	static uint8_t temp[12];
	Module_Status status = H0BR4_OK;
	
	if ((status = SampleAccGToBuf(buffer)) != H0BR4_OK)
		return status;
	
	/*memcpy(messageParams, buffer, sizeof(buffer));
	if (SendMessageFromPort(port, myID, module, CODE_H0BR4_RESULT_ACC, sizeof(buffer)) != BOS_OK)
		status = H0BR4_ERR_IO;*/
	
	if (module==myID){
						temp[0] = *((__IO uint8_t *)(&buffer[0])+3);  temp[1] = *((__IO uint8_t *)(&buffer[0])+2);
						temp[2] = *((__IO uint8_t *)(&buffer[0])+1);  temp[3] = *((__IO uint8_t *)(&buffer[0])+0);
		
		        temp[4] = *((__IO uint8_t *)(&buffer[1])+3);  temp[5] = *((__IO uint8_t *)(&buffer[1])+2);
						temp[6] = *((__IO uint8_t *)(&buffer[1])+1);  temp[7] = *((__IO uint8_t *)(&buffer[1])+0);

		        temp[8] = *((__IO uint8_t *)(&buffer[2])+3);  temp[9] = *((__IO uint8_t *)(&buffer[2])+2);
						temp[10] = *((__IO uint8_t *)(&buffer[2])+1); temp[11] = *((__IO uint8_t *)(&buffer[2])+0);
		
						writePxITMutex(port, (char *)&temp[0], 12*sizeof(uint8_t), 10);
						//memset(temp,0,12*sizeof(uint8_t));
				}
			else{
						messageParams[0]=port;
					  messageParams[1] = *((__IO uint8_t *)(&buffer[0])+3);  messageParams[2] = *((__IO uint8_t *)(&buffer[0])+2);
						messageParams[3] = *((__IO uint8_t *)(&buffer[0])+1);  messageParams[4] = *((__IO uint8_t *)(&buffer[0])+0);
				
					  messageParams[5] = *((__IO uint8_t *)(&buffer[1])+3);  messageParams[6] = *((__IO uint8_t *)(&buffer[1])+2);
						messageParams[7] = *((__IO uint8_t *)(&buffer[1])+1);  messageParams[8] = *((__IO uint8_t *)(&buffer[1])+0);
				
					  messageParams[9] = *((__IO uint8_t *)(&buffer[2])+3);  messageParams[10] = *((__IO uint8_t *)(&buffer[2])+2);
						messageParams[11] = *((__IO uint8_t *)(&buffer[2])+1); messageParams[12] = *((__IO uint8_t *)(&buffer[2])+0);
						SendMessageToModule(module, CODE_PORT_FORWARD, (sizeof(float)*3)+1);
					}
	return status;
}

Module_Status SampleAccGToString(char *cstring, size_t maxLen)
{
	Module_Status status = H0BR4_OK;
	float x = 0, y = 0, z = 0;
	
	if ((status = SampleAccG(&x, &y, &z)) != H0BR4_OK)
		return status;
	
	snprintf(cstring, maxLen, "Acc(G) | X: %.2f, Y: %.2f, Z: %.2f\r\n", x, y, z);
	return status;
}

Module_Status SampleAccG(float *x, float *y, float *z)
{
	Module_Status status = H0BR4_OK;
	int xInMG = 0, yInMG = 0, zInMG = 0;
	
	if ((status = LSM6DS3SampleAccMG(&xInMG, &yInMG, &zInMG)) != H0BR4_OK)
		return status;
	
	*x = ((float)xInMG) / 1000;
	*y = ((float)yInMG) / 1000;
	*z = ((float)zInMG) / 1000;
	
	return status;
}

Module_Status SampleAccGToBuf(float *buffer)
{
	return SampleAccG(buffer, buffer + 1, buffer + 2);
}

Module_Status SampleMagMGauss(int *magX, int *magY, int *magZ)
{
	return LSM303SampleMagMGauss(magX, magY, magZ);
}

Module_Status SampleMagRaw(int16_t *magX, int16_t *magY, int16_t *magZ)
{
	return LSM303SampleMagRaw(magX, magY, magZ);
}

Module_Status SampleMagMGaussToPort(uint8_t port, uint8_t module)
{
	float buffer[3]; // Three Samples X, Y, Z
	static uint8_t temp[12];
	Module_Status status = H0BR4_OK;
	
	if ((status = SampleMagMGaussToBuf(buffer)) != H0BR4_OK)
		return status;
	
	/*memcpy(messageParams, buffer, sizeof(buffer));
	if (SendMessageFromPort(port, myID, module, CODE_H0BR4_RESULT_MAG, sizeof(buffer)) != BOS_OK)
		status = H0BR4_ERR_TIMEOUT;*/
	
	if (module==myID){
						temp[0] = *((__IO uint8_t *)(&buffer[0])+3);  temp[1] = *((__IO uint8_t *)(&buffer[0])+2);
						temp[2] = *((__IO uint8_t *)(&buffer[0])+1);  temp[3] = *((__IO uint8_t *)(&buffer[0])+0);
		
		        temp[4] = *((__IO uint8_t *)(&buffer[1])+3);  temp[5] = *((__IO uint8_t *)(&buffer[1])+2);
						temp[6] = *((__IO uint8_t *)(&buffer[1])+1);  temp[7] = *((__IO uint8_t *)(&buffer[1])+0);

		        temp[8] = *((__IO uint8_t *)(&buffer[2])+3);  temp[9] = *((__IO uint8_t *)(&buffer[2])+2);
						temp[10] = *((__IO uint8_t *)(&buffer[2])+1); temp[11] = *((__IO uint8_t *)(&buffer[2])+0);

						writePxITMutex(port, (char *)&temp[0], 12*sizeof(uint8_t), 10);
						//memset(temp,0,12*sizeof(uint8_t));
				}
			else{
						messageParams[0]=port;
					  messageParams[1] = *((__IO uint8_t *)(&buffer[0])+3);  messageParams[2] = *((__IO uint8_t *)(&buffer[0])+2);
						messageParams[3] = *((__IO uint8_t *)(&buffer[0])+1);  messageParams[4] = *((__IO uint8_t *)(&buffer[0])+0);
				
					  messageParams[5] = *((__IO uint8_t *)(&buffer[1])+3);  messageParams[6] = *((__IO uint8_t *)(&buffer[1])+2);
						messageParams[7] = *((__IO uint8_t *)(&buffer[1])+1);  messageParams[8] = *((__IO uint8_t *)(&buffer[1])+0);
				
					  messageParams[9] = *((__IO uint8_t *)(&buffer[2])+3);  messageParams[10] = *((__IO uint8_t *)(&buffer[2])+2);
						messageParams[11] = *((__IO uint8_t *)(&buffer[2])+1); messageParams[12] = *((__IO uint8_t *)(&buffer[2])+0);
						SendMessageToModule(module, CODE_PORT_FORWARD, (sizeof(float)*3)+1);
					}
	return status;
}

Module_Status SampleMagMGaussToString(char *cstring, size_t maxLen)
{
	Module_Status status = H0BR4_OK;
	int x = 0, y = 0, z = 0;
	
	if ((status = LSM303SampleMagMGauss(&x, &y, &z)) != H0BR4_OK)
		return status;
	
	snprintf(cstring, maxLen, "Mag(mGauss) | X: %d, Y: %d, Z: %d\r\n", x, y, z);
	return status;
}

Module_Status SampleMagMGaussToBuf(float *buffer)
{
	int iMagMGauss[3];
	Module_Status status = LSM303SampleMagMGauss(iMagMGauss, iMagMGauss + 1, iMagMGauss + 2);
	
	buffer[0] = iMagMGauss[0];
	buffer[1] = iMagMGauss[1];
	buffer[2] = iMagMGauss[2];
	
	return status;
}

Module_Status SampleTempCelsius(float *temp)
{
	return LSM6DS3SampleTempCelsius(temp);
}

Module_Status SampleTempFahrenheit(float *temp)
{
	return LSM6DS3SampleTempFahrenheit(temp);
}

Module_Status SampleTempCToPort(uint8_t port, uint8_t module)
{
	float temp;
	static uint8_t tempD[4];
	Module_Status status = H0BR4_OK;
	
	if ((status = LSM6DS3SampleTempCelsius(&temp)) != H0BR4_OK)
		return status;
	
	/*memcpy(messageParams, &temp, sizeof(temp));
	if (SendMessageFromPort(port, myID, module, CODE_H0BR4_RESULT_TEMP, sizeof(temp)) != BOS_OK)
		status = H0BR4_ERR_TERMINATED;*/
	
if (module==myID){
						tempD[0] = *((__IO uint8_t *)(&temp)+3);
						tempD[1] = *((__IO uint8_t *)(&temp)+2);
						tempD[2] = *((__IO uint8_t *)(&temp)+1);
						tempD[3] = *((__IO uint8_t *)(&temp)+0);
						writePxMutex(port, (char *)&tempD[0], 4*sizeof(uint8_t), 10, 10);
						//writePxITMutex(port, (char *)&tempD[0], 4*sizeof(uint8_t), 10);
						//memset(tempD,0,4*sizeof(uint8_t));
				}
			else{
						messageParams[0]=port;
					  messageParams[1] = *((__IO uint8_t *)(&temp)+3);
						messageParams[2] = *((__IO uint8_t *)(&temp)+2);
						messageParams[3] = *((__IO uint8_t *)(&temp)+1);
						messageParams[4] = *((__IO uint8_t *)(&temp)+0);
						SendMessageToModule(module, CODE_PORT_FORWARD, sizeof(float)+1);
					}
	return status;
}

Module_Status SampleTempCToString(char *cstring, size_t maxLen)
{
	Module_Status status = H0BR4_OK;
	float temp;
	
	if ((status = LSM6DS3SampleTempCelsius(&temp)) != H0BR4_OK)
		return status;
	
	snprintf(cstring, maxLen, "Temp(Celsius) | %0.2f\r\n", temp);
	return status;
}

Module_Status StreamGyroDPSToPort(uint8_t port, uint8_t module, uint32_t period, uint32_t timeout)
{
	return StreamMemsToPort(port, module, period, timeout, SampleGyroDPSToPort);
}

Module_Status StreamGyroDPSToCLI(uint32_t period, uint32_t timeout)
{
	return StreamMemsToCLI(period, timeout, SampleGyroDPSToString);
}

Module_Status StreamGyroDPSToBuffer(float *buffer, uint32_t period, uint32_t timeout)
{
	return StreamMemsToBuf(buffer, sizeof(*buffer) * 3, period, timeout, SampleGyroDPSToBuf);
}

Module_Status StreamAccGToPort(uint8_t port, uint8_t module, uint32_t period, uint32_t timeout)
{
	return StreamMemsToPort(port, module, period, timeout, SampleAccGToPort);
}

Module_Status StreamAccGToCLI(uint32_t period, uint32_t timeout)
{
	return StreamMemsToCLI(period, timeout, SampleAccGToString);
}

Module_Status StreamAccGToBuffer(float *buffer, uint32_t period, uint32_t timeout)
{
	return StreamMemsToBuf(buffer, sizeof(*buffer) * 3, period, timeout, SampleAccGToBuf);
}

Module_Status StreamMagMGaussToPort(uint8_t port, uint8_t module, uint32_t period, uint32_t timeout)
{
	return StreamMemsToPort(port, module, period, timeout, SampleMagMGaussToPort);
}

Module_Status StreamMagMGaussToCLI(uint32_t period, uint32_t timeout)
{
	return StreamMemsToCLI(period, timeout, SampleMagMGaussToString);
}

Module_Status StreamMagMGaussToBuffer(float *buffer, uint32_t period, uint32_t timeout)
{
	return StreamMemsToBuf(buffer, sizeof(*buffer) * 3, period, timeout, SampleMagMGaussToBuf);
}

Module_Status StreamTempCToPort(uint8_t port, uint8_t module, uint32_t period, uint32_t timeout)
{
	return StreamMemsToPort(port, module, period, timeout, SampleTempCToPort);
}

Module_Status StreamTempCToCLI(uint32_t period, uint32_t timeout)
{
	return StreamMemsToCLI(period, timeout, SampleTempCToString);
}

Module_Status StreamTempCToBuffer(float *buffer, uint32_t period, uint32_t timeout)
{
	return StreamMemsToBuf(buffer, sizeof(*buffer), period, timeout, SampleTempCelsius);
}

void stopStreamMems(void)
{
	stopStream = true;
}

/* -----------------------------------------------------------------------
	|															Commands																 	|
   ----------------------------------------------------------------------- 
*/

static portBASE_TYPE SampleSensorCommand(int8_t *pcWriteBuffer, size_t xWriteBufferLen, const int8_t *pcCommandString)
{
	const char *const gyroCmdName = "gyro";
	const char *const accCmdName = "acc";
	const char *const magCmdName = "mag";
	const char *const tempCmdName = "temp";
	
	const char *pSensName = NULL;
	portBASE_TYPE sensNameLen = 0;
	
	// Make sure we return something
	*pcWriteBuffer = '\0';
	
	pSensName = (const char *)FreeRTOS_CLIGetParameter(pcCommandString, 1, &sensNameLen);
	
	if (pSensName == NULL) {
		snprintf((char *)pcWriteBuffer, xWriteBufferLen, "Invalid Arguments\r\n");
		return pdFALSE;
	}
	
	do {
		if (!strncmp(pSensName, gyroCmdName, strlen(gyroCmdName))) {
			if (SampleGyroDPSToString((char *)pcWriteBuffer, xWriteBufferLen) != H0BR4_OK)
				break;
			
		} else if (!strncmp(pSensName, accCmdName, strlen(accCmdName))) {
			if (SampleAccGToString((char *)pcWriteBuffer, xWriteBufferLen) != H0BR4_OK)
				break;
		
		} else if (!strncmp(pSensName, magCmdName, strlen(magCmdName))) {
			if (SampleMagMGaussToString((char *)pcWriteBuffer, xWriteBufferLen) != H0BR4_OK)
				break;
			
		} else if (!strncmp(pSensName, tempCmdName, strlen(tempCmdName))) {
			if (SampleTempCToString((char *)pcWriteBuffer, xWriteBufferLen) != H0BR4_OK)
				break;
			
		} else {
			snprintf((char *)pcWriteBuffer, xWriteBufferLen, "Invalid Arguments\r\n");
		}
	
		return pdFALSE;
	} while (0);
	
	snprintf((char *)pcWriteBuffer, xWriteBufferLen, "Error reading Sensor\r\n");
	return pdFALSE;
}

// Port Mode => false and CLI Mode => true
static bool StreamCommandParser(const int8_t *pcCommandString, const char **ppSensName, portBASE_TYPE *pSensNameLen, 
														bool *pPortOrCLI, uint32_t *pPeriod, uint32_t *pTimeout, uint8_t *pPort, uint8_t *pModule)
{
	const char *pPeriodMSStr = NULL;
	const char *pTimeoutMSStr = NULL;
	
	portBASE_TYPE periodStrLen = 0;
	portBASE_TYPE timeoutStrLen = 0;
	
	const char *pPortStr = NULL;
	const char *pModStr = NULL;
	
	portBASE_TYPE portStrLen = 0;
	portBASE_TYPE modStrLen = 0;
	
	*ppSensName = (const char *)FreeRTOS_CLIGetParameter(pcCommandString, 1, pSensNameLen);
	pPeriodMSStr = (const char *)FreeRTOS_CLIGetParameter(pcCommandString, 2, &periodStrLen);
	pTimeoutMSStr = (const char *)FreeRTOS_CLIGetParameter(pcCommandString, 3, &timeoutStrLen);
	
	// At least 3 Parameters are required!
	if ((*ppSensName == NULL) || (pPeriodMSStr == NULL) || (pTimeoutMSStr == NULL))
		return false;
	
	// TODO: Check if Period and Timeout are integers or not!
	*pPeriod = atoi(pPeriodMSStr);
	*pTimeout = atoi(pTimeoutMSStr);
	*pPortOrCLI = true;
	
	pPortStr = (const char *)FreeRTOS_CLIGetParameter(pcCommandString, 4, &portStrLen);
	pModStr = (const char *)FreeRTOS_CLIGetParameter(pcCommandString, 5, &modStrLen);
	
	if ((pModStr == NULL) && (pPortStr == NULL))
		return true;
	if ((pModStr == NULL) || (pPortStr == NULL))	// If user has provided 4 Arguments.
		return false;
	
	*pPort = atoi(pPortStr);
	*pModule = atoi(pModStr);
	*pPortOrCLI = false;
	
	return true;
}

static portBASE_TYPE StreamSensorCommand(int8_t *pcWriteBuffer, size_t xWriteBufferLen, const int8_t *pcCommandString)
{
	const char *const gyroCmdName = "gyro";
	const char *const accCmdName = "acc";
	const char *const magCmdName = "mag";
	const char *const tempCmdName = "temp";
	
	uint32_t period = 0;
	uint32_t timeout = 0;
	uint8_t port = 0;
	uint8_t module = 0;
	
	bool portOrCLI = true; // Port Mode => false and CLI Mode => true
	
	const char *pSensName = NULL;
	portBASE_TYPE sensNameLen = 0;
	
	// Make sure we return something
	*pcWriteBuffer = '\0';
	
	if (!StreamCommandParser(pcCommandString, &pSensName, &sensNameLen, &portOrCLI, &period, &timeout, &port, &module)) {
		snprintf((char *)pcWriteBuffer, xWriteBufferLen, "Invalid Arguments\r\n");
		return pdFALSE;
	}
	
	do {
		if (!strncmp(pSensName, gyroCmdName, strlen(gyroCmdName))) {
			if (portOrCLI) {
				if (StreamGyroDPSToCLI(period, timeout) != H0BR4_OK)
					break;
			} else {
				if (StreamGyroDPSToPort(port, module, period, timeout) != H0BR4_OK)
					break;
			}
			
		} else if (!strncmp(pSensName, accCmdName, strlen(accCmdName))) {
			if (portOrCLI) {
				if (StreamAccGToCLI(period, timeout) != H0BR4_OK)
					break;
			} else {
				if (StreamAccGToPort(port, module, period, timeout) != H0BR4_OK)
					break;
			}
			
		} else if (!strncmp(pSensName, magCmdName, strlen(magCmdName))) {
			if (portOrCLI) {
				if (StreamMagMGaussToCLI(period, timeout) != H0BR4_OK)
					break;
			} else {
				if (StreamMagMGaussToPort(port, module, period, timeout) != H0BR4_OK)
					break;
			}
			
		} else if (!strncmp(pSensName, tempCmdName, strlen(tempCmdName))) {
			if (portOrCLI) {
				if (StreamTempCToCLI(period, timeout) != H0BR4_OK)
					break;
			} else {
				if (StreamTempCToPort(port, module, period, timeout) != H0BR4_OK)
					break;
			}
			
		} else {
			snprintf((char *)pcWriteBuffer, xWriteBufferLen, "Invalid Arguments\r\n");
		}
	
		snprintf((char *)pcWriteBuffer, xWriteBufferLen, "\r\n");
		return pdFALSE;
	} while (0);
	
	snprintf((char *)pcWriteBuffer, xWriteBufferLen, "Error reading Sensor\r\n");
	return pdFALSE;
}

static portBASE_TYPE StopStreamCommand(int8_t *pcWriteBuffer, size_t xWriteBufferLen, const int8_t *pcCommandString)
{
	// Make sure we return something
	pcWriteBuffer[0] = '\0';
	snprintf((char *)pcWriteBuffer, xWriteBufferLen, "Stopping Streaming MEMS...\r\n");
	
	stopStreamMems();
	return pdFALSE;
}

/************************ (C) COPYRIGHT HEXABITZ *****END OF FILE****/
