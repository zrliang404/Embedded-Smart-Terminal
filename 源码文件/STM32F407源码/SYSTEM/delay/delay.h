#ifndef __DELAY_H
#define __DELAY_H 			   
#include <sys.h>	  


void Delay_Init(uint8_t SYSCLK);
void delay_us(uint32_t nus);
void delay_ms(uint32_t nms);


#endif





























