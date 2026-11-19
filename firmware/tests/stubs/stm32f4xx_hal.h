/* Host-side stand-in for the STM32 HAL - just enough for firmware/tests. */
#ifndef STUB_STM32F4XX_HAL_H
#define STUB_STM32F4XX_HAL_H

#include <stdint.h>

typedef enum { HAL_OK = 0, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET } GPIO_PinState;

typedef struct { uint32_t ODR; } GPIO_TypeDef;
typedef struct { uint32_t CR1; } SPI_TypeDef;
typedef struct { uint32_t SR; } USART_TypeDef;
typedef struct { SPI_TypeDef *Instance; } SPI_HandleTypeDef;
typedef struct { USART_TypeDef *Instance; } UART_HandleTypeDef;

extern GPIO_TypeDef stub_gpioa, stub_gpiob, stub_gpioc;
#define GPIOA (&stub_gpioa)
#define GPIOB (&stub_gpiob)
#define GPIOC (&stub_gpioc)
#define GPIO_PIN_10 ((uint16_t)0x0400)
#define GPIO_PIN_13 ((uint16_t)0x2000)
#define GPIO_PIN_14 ((uint16_t)0x4000)

#define HAL_MAX_DELAY 0xFFFFFFFFU
#define SPI_CR1_BR_Msk 0x38U
#define SPI_BAUDRATEPRESCALER_2 0x00U
#define SPI_BAUDRATEPRESCALER_256 0x38U

#define UART_FLAG_ORE 0x08U
#define __HAL_UART_GET_FLAG(h, flag) ((((h)->Instance->SR) & (flag)) == (flag))
#define __HAL_UART_CLEAR_OREFLAG(h) ((h)->Instance->SR &= ~UART_FLAG_ORE)

uint32_t HAL_GetTick(void);
void HAL_Delay(uint32_t ms);
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state);
HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *h, uint8_t *data, uint16_t size, uint32_t timeout);
HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *h, uint8_t *data, uint16_t size, uint32_t timeout);
HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *h, uint8_t *tx, uint8_t *rx, uint16_t size,
                                          uint32_t timeout);
HAL_StatusTypeDef HAL_UART_Receive(UART_HandleTypeDef *h, uint8_t *data, uint16_t size, uint32_t timeout);

/* Test hooks (hal_stub.c) */
void stub_reset(void);
void stub_advance_ms(uint32_t ms);
void stub_uart_push(const char *text);
void stub_uart_set_overrun(void);
int stub_uart_pending(void);

#endif
