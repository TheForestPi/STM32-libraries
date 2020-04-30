/* Copyright (C) 2020 Thomas Jespersen, TKJ Electronics. All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the MIT License
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the MIT License for further details.
 *
 * Contact information
 * ------------------------------------------
 * Thomas Jespersen, TKJ Electronics
 * Web      :  http://www.tkjelectronics.dk
 * e-mail   :  thomasj@tkjelectronics.dk
 * ------------------------------------------
 */
 
#include "SyncedPWMADC.h"
#include "stm32g4xx_hal.h"
#include "Debug.h"
#include <string.h> // for memset
#include <math.h> // for roundf

// Necessary to export for compiler to generate code to be called by interrupt vector
extern "C" __EXPORT void TIM1_UP_TIM16_IRQHandler(void);
extern "C" __EXPORT void TIM1_CC_IRQHandler(void);
 
void SyncedPWMADC::ConfigureDigitalPins()
{
	GPIO_InitTypeDef GPIO_InitStruct = {0};

	/* Peripheral clock enable */
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /** GPIO Configuration
    PA8     ------> TIM1_CH1		[CH1-Positive]
    PC13    ------> TIM1_CH1N		[CH1-Negative]
    PA9     ------> TIM1_CH2		[CH2-Positive]
	PA12    ------> TIM1_CH2N		[CH2-Negative]
    PA10    ------> TIM1_CH3		[CH3-Positive]
    PB15    ------> TIM1_CH3N		[CH3-Negative]
    PB5     ------> OUTPUT_PP		[CH3-Negative]
    */
    GPIO_InitStruct.Pin = GPIO_PIN_13;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    GPIO_InitStruct.Alternate = GPIO_AF4_TIM1;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_15;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    GPIO_InitStruct.Alternate = GPIO_AF4_TIM1;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_12;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    GPIO_InitStruct.Alternate = GPIO_AF6_TIM1;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    // GPIO_BEMF - Enables Back-EMF sense voltage divider when set low (GPIO_PIN_RESET)
    GPIO_InitStruct.Pin = GPIO_PIN_5;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
    HAL_GPIO_WritePin(GPIOB, GPIO_InitStruct.Pin, GPIO_PIN_RESET);
}

void SyncedPWMADC::DeInitDigitalPins()
{
	HAL_GPIO_DeInit(GPIOC, GPIO_PIN_13);
	HAL_GPIO_DeInit(GPIOB, GPIO_PIN_15);
	HAL_GPIO_DeInit(GPIOA, GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_12);
}

void SyncedPWMADC::InitTimer(uint32_t frequency)
{
	/* Peripheral clock enable */
	__HAL_RCC_TIM1_CLK_ENABLE();

	Timer_Configure(frequency);

	TIM_ClockConfigTypeDef sClockSourceConfig = {0};
	sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
	if (HAL_TIM_ConfigClockSource(&hTimer, &sClockSourceConfig) != HAL_OK)
	{
		ERROR("Could not configure PWM timer clock source");
	}

	if (HAL_TIM_PWM_Init(&hTimer) != HAL_OK)
	{
		ERROR("Could not initialize PWM");
	}

	// Configure timer output triggers (used to trigger ADC sampling)
	TIM_MasterConfigTypeDef sMasterConfig = {0};
	sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;
	sMasterConfig.MasterOutputTrigger2 = TIM_TRGO2_OC5REF_RISING_OC6REF_RISING; //TIM_TRGO2_OC5REF_RISING_OC6REF_RISING;
	sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
	if (HAL_TIMEx_MasterConfigSynchronization(&hTimer, &sMasterConfig) != HAL_OK)
	{
		ERROR("Could not configure timer synchronization/trigger");
	}

	TIM_OC_InitTypeDef sConfigOC = {0};
	sConfigOC.OCMode = TIM_OCMODE_PWM1;
	sConfigOC.Pulse = 0;
	sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;

	// Define polarity when channel is enabled and disabled
	sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
	sConfigOC.OCIdleState = TIM_OCIDLESTATE_RESET;
	sConfigOC.OCNPolarity = TIM_OCPOLARITY_HIGH;
	sConfigOC.OCNIdleState = TIM_OCIDLESTATE_RESET;

	/* Configure main H-bridge PWM outputs */
	if (HAL_TIM_PWM_ConfigChannel(&hTimer, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
		ERROR("Could not configure PWM channel");
	__HAL_TIM_ENABLE_OCxPRELOAD(&hTimer, TIM_CHANNEL_1); // Enable preload to ensure that duty cycles will only be changed at the beginning of each PWM period
	if (HAL_TIM_PWM_ConfigChannel(&hTimer, &sConfigOC, TIM_CHANNEL_3) != HAL_OK)
		ERROR("Could not configure PWM channel");
	__HAL_TIM_ENABLE_OCxPRELOAD(&hTimer, TIM_CHANNEL_3); // Enable preload to ensure that duty cycles will only be changed at the beginning of each PWM period

	/* Configure debug channels whose outputs are not enabled */
	// These are only used to trigger Capture-Compare interrupt for debugging purposes
	if (DEBUG_MODE_ENABLED) {
		if (HAL_TIM_PWM_ConfigChannel(&hTimer, &sConfigOC, TIM_CHANNEL_2) != HAL_OK)
			ERROR("Could not configure PWM channel");
		__HAL_TIM_ENABLE_OCxPRELOAD(&hTimer, TIM_CHANNEL_2); // Enable preload to ensure that duty cycles will only be changed at the beginning of each PWM period
		if (HAL_TIM_PWM_ConfigChannel(&hTimer, &sConfigOC, TIM_CHANNEL_4) != HAL_OK)
			ERROR("Could not configure PWM channel");
		__HAL_TIM_ENABLE_OCxPRELOAD(&hTimer, TIM_CHANNEL_4); // Enable preload to ensure that duty cycles will only be changed at the beginning of each PWM period
	}

	/* Configure sample-trigger channels (internal channels) */
	sConfigOC.OCMode = TIM_OCMODE_PWM2;
	if (HAL_TIM_PWM_ConfigChannel(&hTimer, &sConfigOC, TIM_CHANNEL_5) != HAL_OK)
		ERROR("Could not configure PWM channel");
	__HAL_TIM_ENABLE_OCxPRELOAD(&hTimer, TIM_CHANNEL_5); // Enable preload to ensure that duty cycles will only be changed at the beginning of each PWM period
	if (HAL_TIM_PWM_ConfigChannel(&hTimer, &sConfigOC, TIM_CHANNEL_6) != HAL_OK)
		ERROR("Could not configure PWM channel");
	__HAL_TIM_ENABLE_OCxPRELOAD(&hTimer, TIM_CHANNEL_6); // Enable preload to ensure that duty cycles will only be changed at the beginning of each PWM period

	TIM_BreakDeadTimeConfigTypeDef sBreakDeadTimeConfig = {0};
	sBreakDeadTimeConfig.OffStateRunMode = TIM_OSSR_DISABLE;
	sBreakDeadTimeConfig.OffStateIDLEMode = TIM_OSSI_DISABLE;
	sBreakDeadTimeConfig.LockLevel = TIM_LOCKLEVEL_OFF;
	sBreakDeadTimeConfig.DeadTime = 0;
	sBreakDeadTimeConfig.BreakState = TIM_BREAK_DISABLE;
	sBreakDeadTimeConfig.BreakPolarity = TIM_BREAKPOLARITY_HIGH;
	sBreakDeadTimeConfig.BreakFilter = 0;
	sBreakDeadTimeConfig.BreakAFMode = TIM_BREAK_AFMODE_INPUT;
	sBreakDeadTimeConfig.Break2State = TIM_BREAK2_DISABLE;
	sBreakDeadTimeConfig.Break2Polarity = TIM_BREAK2POLARITY_HIGH;
	sBreakDeadTimeConfig.Break2Filter = 0;
	sBreakDeadTimeConfig.Break2AFMode = TIM_BREAK_AFMODE_INPUT;
	sBreakDeadTimeConfig.AutomaticOutput = TIM_AUTOMATICOUTPUT_DISABLE;
	if (HAL_TIMEx_ConfigBreakDeadTime(&hTimer, &sBreakDeadTimeConfig) != HAL_OK)
	{
		ERROR("Could not configure PWM break and dead time");
	}

	/* TIM1 interrupt Init */
	HAL_NVIC_SetPriority(TIM1_BRK_TIM15_IRQn, 7, 0);
	HAL_NVIC_EnableIRQ(TIM1_BRK_TIM15_IRQn);

	if (!SAMPLE_IN_EVERY_PWM_CYCLE) {
		HAL_NVIC_SetPriority(TIM1_UP_TIM16_IRQn, 5, 0);
		HAL_NVIC_EnableIRQ(TIM1_UP_TIM16_IRQn);
	}

	if (DEBUG_MODE_ENABLED) {
		HAL_NVIC_SetPriority(TIM1_CC_IRQn, 1, 0);
		HAL_NVIC_EnableIRQ(TIM1_CC_IRQn);
	}
}

// Timer_Configure can also be called after being configured to change the prescaler
// For frequency and duty cycle changes it is however recommended to use the according functions:
//  - SetPWMFrequency(uint32_t frequency)
//  - void SetDutyCycle(float duty)
void SyncedPWMADC::Timer_Configure(uint32_t frequency)
{
	timerSettingsNext.Frequency = frequency;

	bool ShouldRestart = false;
	if (_TimerEnabled) {
		ShouldRestart = true;
		StopPWM();
	}

	volatile uint32_t TimerClock = HAL_RCC_GetPCLK2Freq();

	/* Configure the timer frequency */
	hTimer.Instance = TIM1;
	hTimer.Init.CounterMode = TIM_COUNTERMODE_UP; //TIM_COUNTERMODE_CENTERALIGNED1;
	hTimer.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
	hTimer.Init.RepetitionCounter = _samplingInterval - 1;
	hTimer.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;

	// Find PWM prescaler to yield the largest possible ARR while still keeping it within 16-bit
	// Based on "A few useful formulas" from the STM32 Timer slides (PDF)
	// fPWM = fTIM / ((ARR+1)*(PSC+1))

	// Start with no prescaler: _PWM_prescaler = PSC+1 = 1
	uint32_t ARR = 0x10000;
	timerSettingsNext.Prescaler = 0;
	while (ARR > 0xFFFF) {
		timerSettingsNext.Prescaler++;
		// Compute ARR and check value
		ARR = TimerClock / (timerSettingsNext.Frequency * timerSettingsNext.Prescaler) - 1;
	}
	hTimer.Init.Period = ARR;
	hTimer.Init.Prescaler = timerSettingsNext.Prescaler - 1;

	// Configure timer prescaler based on desired frequency
	//   fCNT = (ARR+1) * fPERIOD
	//   PSC = (fTIM / fCNT) - 1
	// Added prescaler computation as float such that rounding can happen
	/*float prescaler = ((float)TimerClock / ((hTimer.Init.Period+1) * _PWM_frequency));
	_PWM_prescaler = (uint32_t)roundf(prescaler);
	hTimer.Init.Prescaler = _PWM_prescaler - 1;
	hTimer.Init.Period = _PWM_maxValue - 1; // this will allow duty cycles (CCR register) to go from 0 to maxValue, with maxValue giving 100% duty cycle (fully on)
	*/

	// Recompute actual frequency
	timerSettingsNext.Frequency = TimerClock / ((hTimer.Init.Period+1) * timerSettingsNext.Prescaler);

	if (HAL_TIM_Base_Init(&hTimer) != HAL_OK)
	{
		ERROR("Could not initialize PWM timer");
	}

	// Reset duty cycle values
	__HAL_TIM_SET_COMPARE(&hTimer, TIM_CHANNEL_1, 0);
	__HAL_TIM_SET_COMPARE(&hTimer, TIM_CHANNEL_2, 0);
	__HAL_TIM_SET_COMPARE(&hTimer, TIM_CHANNEL_3, 0);
	__HAL_TIM_SET_COMPARE(&hTimer, TIM_CHANNEL_4, 0);
	__HAL_TIM_SET_COMPARE(&hTimer, TIM_CHANNEL_5, 0);
	__HAL_TIM_SET_COMPARE(&hTimer, TIM_CHANNEL_6, 0);

	if (ShouldRestart) {
		StartPWM();
	}
}


void SyncedPWMADC::DeInitTimer()
{
	/* Peripheral clock disable */
    __HAL_RCC_TIM1_CLK_DISABLE();

    /* TIM1 interrupt DeInit */
    HAL_NVIC_DisableIRQ(TIM1_BRK_TIM15_IRQn);

    if (!SAMPLE_IN_EVERY_PWM_CYCLE) {
    	HAL_NVIC_DisableIRQ(TIM1_UP_TIM16_IRQn);
    }

    if (DEBUG_MODE_ENABLED) {
    	HAL_NVIC_DisableIRQ(TIM1_CC_IRQn);
    }
}

void SyncedPWMADC::Timer_ConfigureBrakeMode()
{
	// Enable all timer outputs to enable Brake mode
	TIM_CCxChannelCmd(hTimer.Instance, TIM_CHANNEL_1, TIM_CCx_ENABLE);
	TIM_CCxNChannelCmd(hTimer.Instance, TIM_CHANNEL_1, TIM_CCxN_ENABLE);
	TIM_CCxChannelCmd(hTimer.Instance, TIM_CHANNEL_3, TIM_CCx_ENABLE);
	TIM_CCxNChannelCmd(hTimer.Instance, TIM_CHANNEL_3, TIM_CCxN_ENABLE);
}

void SyncedPWMADC::Timer_ConfigureCoastMode(bool direction)
{
	if (direction) { // Forward
		// In forward direction it is CH3,CH3N that changes.
		// During COAST mode CH3N is forced to be LOW at all times, causing LOW PWM to deactivate the CH3,CH3N MOSFET
		// Back-EMF should thus be sampled from OUT3 = BEMF3
		TIM_CCxChannelCmd(hTimer.Instance, TIM_CHANNEL_1, TIM_CCx_ENABLE);
		TIM_CCxNChannelCmd(hTimer.Instance, TIM_CHANNEL_1, TIM_CCxN_ENABLE);
		TIM_CCxChannelCmd(hTimer.Instance, TIM_CHANNEL_3, TIM_CCx_ENABLE);
		TIM_CCxNChannelCmd(hTimer.Instance, TIM_CHANNEL_3, TIM_CCxN_DISABLE);
	} else { // Backward
		// In backward direction it is CH1,CH1N that changes.
		// During COAST mode CH1N is forced to be LOW at all times, causing LOW PWM to deactivate the CH1,CH1N MOSFET
		// Back-EMF should thus be sampled from OUT1 = BEMF1
		TIM_CCxChannelCmd(hTimer.Instance, TIM_CHANNEL_1, TIM_CCx_ENABLE);
		TIM_CCxNChannelCmd(hTimer.Instance, TIM_CHANNEL_1, TIM_CCxN_DISABLE);
		TIM_CCxChannelCmd(hTimer.Instance, TIM_CHANNEL_3, TIM_CCx_ENABLE);
		TIM_CCxNChannelCmd(hTimer.Instance, TIM_CHANNEL_3, TIM_CCxN_ENABLE);
	}
}

void SyncedPWMADC::SetConstantOutput(bool OnOff, bool direction)
{
	// OBS. There is no recovery from entering this mode without restarting MCU!
    /** GPIO Configuration
    PA8     ------> TIM1_CH1		[CH1-Positive]
    PC13    ------> TIM1_CH1N		[CH1-Negative]
    PA9     ------> TIM1_CH2		[CH2-Positive]
	PA12    ------> TIM1_CH2N		[CH2-Negative]
    PA10    ------> TIM1_CH3		[CH3-Positive]
    PB15    ------> TIM1_CH3N		[CH3-Negative]
    PB5     ------> OUTPUT_PP		[CH3-Negative]
    */

	GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;

    GPIO_InitStruct.Pin = GPIO_PIN_13;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_15;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_12;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    // CH2=LOW
	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_9, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_12, GPIO_PIN_SET);

    if (direction) {
    	// CH1=LOW
    	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);
    	HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);

    	if (OnOff) {
    	    // CH3=HIGH
    		HAL_GPIO_WritePin(GPIOA, GPIO_PIN_10, GPIO_PIN_SET);
    		HAL_GPIO_WritePin(GPIOB, GPIO_PIN_15, GPIO_PIN_RESET);
    	} else {
    	    // CH3=LOW
    		HAL_GPIO_WritePin(GPIOA, GPIO_PIN_10, GPIO_PIN_RESET);
    		HAL_GPIO_WritePin(GPIOB, GPIO_PIN_15, GPIO_PIN_SET);
    	}
    }
    else {
    	// CH3=LOW
		HAL_GPIO_WritePin(GPIOA, GPIO_PIN_10, GPIO_PIN_RESET);
		HAL_GPIO_WritePin(GPIOB, GPIO_PIN_15, GPIO_PIN_SET);

    	if (OnOff) {
    	    // CH1=HIGH
        	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_SET);
        	HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET);
    	} else {
    	    // CH1=LOW
        	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);
        	HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);
    	}
    }
}

// Set the interval of PWM periods to sample all the ADC channels
void SyncedPWMADC::SetSamplingInterval(uint16_t samplingInterval)
{
	if (SAMPLE_IN_EVERY_PWM_CYCLE) return; // sampling interval is not supported when sampling in every PWM cycle

	// In SAMPLE_IN_EVERY_PWM_CYCLE mode sample interval should at least be 2 PWM periods
	if (samplingInterval >= 2) {
		_samplingInterval = samplingInterval;
		hTimer.Instance->RCR = _samplingInterval - 1;
	}
}

void SyncedPWMADC::TIM_CCxNChannelCmd(TIM_TypeDef *TIMx, uint32_t Channel, uint32_t ChannelNState)
{
	// Copied from stm32fg4xx_hal_tim_ex.c
	uint32_t tmp;

	tmp = TIM_CCER_CC1NE << (Channel & 0x1FU); /* 0x1FU = 31 bits max shift */

	/* Reset the CCxNE Bit */
	TIMx->CCER &=  ~tmp;

	/* Set or reset the CCxNE Bit */
	TIMx->CCER |= (uint32_t)(ChannelNState << (Channel & 0x1FU)); /* 0x1FU = 31 bits max shift */
}

void SyncedPWMADC::StartPWM()
{
	_TimerEnabled = true;
	timerSettingsCurrent = timerSettingsNext;

	// Start PWM interface
    HAL_TIM_PWM_Start(&hTimer, TIM_CHANNEL_1);
    HAL_TIMEx_PWMN_Start(&hTimer, TIM_CHANNEL_1);
    HAL_TIM_PWM_Start(&hTimer, TIM_CHANNEL_3);
    HAL_TIMEx_PWMN_Start(&hTimer, TIM_CHANNEL_3);
    HAL_TIM_PWM_Start(&hTimer, TIM_CHANNEL_5);
    HAL_TIM_PWM_Start(&hTimer, TIM_CHANNEL_6);

    // Enable update interrupt if we need to trigger DMA at n-th period
    if (!SAMPLE_IN_EVERY_PWM_CYCLE) {
    	__HAL_TIM_ENABLE_IT(&hTimer, TIM_IT_UPDATE);
    }

    // Enable debug channels (only used internally)
    if (DEBUG_MODE_ENABLED) {
    	HAL_TIM_PWM_Start_IT(&hTimer, TIM_CHANNEL_2); // __HAL_TIM_ENABLE_IT(&htim1, TIM_IT_CC2);
    	HAL_TIM_PWM_Start_IT(&hTimer, TIM_CHANNEL_4); // __HAL_TIM_ENABLE_IT(&htim1, TIM_IT_CC4);
    }
}

void SyncedPWMADC::StopPWM()
{
	_TimerEnabled = false;

	HAL_TIM_PWM_Stop_IT(&hTimer, TIM_CHANNEL_1);
	HAL_TIM_PWM_Stop_IT(&hTimer, TIM_CHANNEL_3);
	HAL_TIM_PWM_Stop_IT(&hTimer, TIM_CHANNEL_5);
	HAL_TIM_PWM_Stop_IT(&hTimer, TIM_CHANNEL_6);

	if (!SAMPLE_IN_EVERY_PWM_CYCLE) {
		__HAL_TIM_DISABLE_IT(&hTimer, TIM_IT_UPDATE);
	}

	if (DEBUG_MODE_ENABLED) {
		HAL_TIM_PWM_Stop_IT(&hTimer, TIM_CHANNEL_2);
		HAL_TIM_PWM_Stop_IT(&hTimer, TIM_CHANNEL_4);
	}
}

void TIM1_UP_TIM16_IRQHandler(void)
{
	// Update interrupt - used to trigger DMA if SAMPLING_INTERVAL > 1
	if (!SyncedPWMADC::globalObject) return;

	/* TIM Update event */
	if (__HAL_TIM_GET_FLAG(&SyncedPWMADC::globalObject->hTimer, TIM_FLAG_UPDATE) != RESET &&
		__HAL_TIM_GET_IT_SOURCE(&SyncedPWMADC::globalObject->hTimer, TIM_IT_UPDATE) != RESET)
	{
		__HAL_TIM_CLEAR_IT(&SyncedPWMADC::globalObject->hTimer, TIM_IT_UPDATE);
		SyncedPWMADC::TriggerSample(SyncedPWMADC::globalObject);
	}
	else {
		// It must have been TIM16 who called this. Clear all interrupts to avoid deadlocks.
		TIM16->SR = ~(uint32_t)(TIM_IT_UPDATE | TIM_IT_CC1 | TIM_IT_CC2 | TIM_IT_CC3 | TIM_IT_CC4 | TIM_IT_COM | TIM_IT_TRIGGER | TIM_IT_BREAK); // clear all interrupts
	}
}

void TIM1_CC_IRQHandler(void)
{
	// For debugging purposes
	if (!SyncedPWMADC::globalObject) return;

	// Is only used for capture compare
	__HAL_TIM_CLEAR_IT(&SyncedPWMADC::globalObject->hTimer, TIM_IT_CC1 | TIM_IT_CC2 | TIM_IT_CC3 | TIM_IT_CC4);
	SyncedPWMADC::TimerCaptureCallback(SyncedPWMADC::globalObject);
}
