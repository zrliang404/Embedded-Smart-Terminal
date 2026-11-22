#ifndef __SPI_H
#define __SPI_H
#include "sys.h"
 				  	    													  
void SPI1_Init(void);			 //初始化SPI口
void SPI1_SetSpeed(u8 SpeedSet); //设置SPI速度   
u8 SPI1_ReadWriteByte(u8 TxData);   //SPI总线读写一个字节
void SPI1_PinsToSPI(void);      /* PA5/PA6/PA7 切回 SPI1 复用（访问 Flash 前调用）*/
		 
#endif

