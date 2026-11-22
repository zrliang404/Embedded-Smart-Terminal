#ifndef __W25QXX_H
#define __W25QXX_H			    
#include "sys.h"  
//////////////////////////////////////////////////////////////////////////////////	 


extern u16 W25QXX_TYPE;					//定义W25QXX芯片型号		   

#define	W25QXX_CS 		PBout(0)  		//W25QXX的片选信号

////////////////////////////////////////////////////////////////////////////////// 
//指令表
#define W25X_WriteEnable		0x06 
#define W25X_WriteDisable		0x04 
#define W25X_ReadStatusReg		0x05 
#define W25X_WriteStatusReg		0x01 
#define W25X_ReadData			0x03 
#define W25X_FastReadData		0x0B 
#define W25X_FastReadDual		0x3B 
#define W25X_PageProgram		0x02 
#define W25X_BlockErase			0xD8 
#define W25X_SectorErase		0x20 
#define W25X_ChipErase			0xC7 
#define W25X_PowerDown			0xB9 
#define W25X_ReleasePowerDown	0xAB 
#define W25X_DeviceID			0xAB 
#define W25X_ManufactDeviceID	0x90 
#define W25X_JedecDeviceID		0x9F 

void W25QXX_Init(void);
u16  W25QXX_ReadID(void);  	    		//读取FLASH ID
u8	 W25QXX_ReadSR(void);        		//读取状态寄存器 
void W25QXX_Write_SR(u8 sr);  			//写状态寄存器
void W25QXX_Write_Enable(void);  		//写使能 
void W25QXX_Write_Disable(void);		//写保护
void W25QXX_Write_NoCheck(u8* pBuffer,u32 WriteAddr,u16 NumByteToWrite);
void W25QXX_Read(u8* pBuffer,u32 ReadAddr,u16 NumByteToRead);   //读取flash
void W25QXX_Write(u8* pBuffer,u32 WriteAddr,u16 NumByteToWrite);//写入flash
void W25QXX_Erase_Chip(void);    	  	//整片擦除
void W25QXX_Erase_Sector(u32 Dst_Addr);
void W25QXX_Erase_Block(u32 BlockAddr);
void W25QXX_ReadJEDEC(u8 *out);	//扇区擦除
void W25QXX_Wait_Busy(void);           	//等待空闲
void W25QXX_PowerDown(void);        	//进入掉电模式
void W25QXX_WAKEUP(void);				//唤醒
/* ---- W25Q16 最后一个块保护（block31: 0x1F0000 ~ 0x1FFFFF，64KB）---- */
#define W25Q_LAST_BLOCK_START    0x001F0000u
#define W25Q_LAST_BLOCK_END      0x00200000u

void W25QXX_Calib_Unlock(void);   /* 临时解除最后一块保护（仅校准参数使用）*/
void W25QXX_Calib_Lock(void);     /* 恢复保护 */

#endif
















