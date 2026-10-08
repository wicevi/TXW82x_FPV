#ifndef __N32L40X_IT_H__
#define __N32L40X_IT_H__

#include "n32l40x.h"

void NMI_Handler(void);
void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);
void SVC_Handler(void);
void DebugMon_Handler(void);
void PendSV_Handler(void);
void SysTick_Handler(void);

#endif /* __N32L40X_IT_H__ */
