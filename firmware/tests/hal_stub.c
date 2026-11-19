/* Fake HAL, FatFs and OLED back-ends for the host-side firmware tests. */
#include <string.h>

#include "fatfs.h"
#include "sd_card_sim.h"
#include "ssd1306.h"
#include "ssd1306_fonts.h"
#include "stm32f4xx_hal.h"

GPIO_TypeDef stub_gpioa, stub_gpiob, stub_gpioc;
static SPI_TypeDef spi1_regs;
static USART_TypeDef usart1_regs;
SPI_HandleTypeDef hspi1 = {&spi1_regs};
UART_HandleTypeDef huart1 = {&usart1_regs};

/* ---- time: 1 tick = 1 ms; every SPI byte costs 1/8 ms (62.5 kHz clock) ---- */
static uint32_t tick_ms;
static uint32_t spi_bytes;

uint32_t HAL_GetTick(void) { return tick_ms; }
void HAL_Delay(uint32_t ms) { tick_ms += ms; }
void stub_advance_ms(uint32_t ms) { tick_ms += ms; }

/* ---- GPIO / SPI -> simulated SD card ---- */
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{
    if (port == GPIOB && pin == GPIO_PIN_10) {
        sd_sim_set_cs(state == GPIO_PIN_RESET);
    }
}

static uint8_t spi_byte(uint8_t mosi)
{
    if ((++spi_bytes & 7U) == 0U) {
        tick_ms++;
    }
    return sd_sim_xfer(mosi);
}

HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *h, uint8_t *data, uint16_t size, uint32_t timeout)
{
    (void)h;
    (void)timeout;
    for (uint16_t i = 0; i < size; i++) {
        spi_byte(data[i]);
    }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *h, uint8_t *tx, uint8_t *rx, uint16_t size,
                                          uint32_t timeout)
{
    (void)h;
    (void)timeout;
    for (uint16_t i = 0; i < size; i++) {
        rx[i] = spi_byte(tx[i]);
    }
    return HAL_OK;
}

/* Like the real F4 HAL in 2-line master mode: the RX buffer is also the TX data. */
HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *h, uint8_t *data, uint16_t size, uint32_t timeout)
{
    return HAL_SPI_TransmitReceive(h, data, data, size, timeout);
}

/* ---- UART: byte queue ---- */
static uint8_t uart_fifo[4096];
static int uart_head, uart_len;

void stub_uart_push(const char *text)
{
    while (*text && uart_len < (int)sizeof(uart_fifo)) {
        uart_fifo[(uart_head + uart_len++) % (int)sizeof(uart_fifo)] = (uint8_t)*text++;
    }
}

void stub_uart_set_overrun(void) { usart1_regs.SR |= UART_FLAG_ORE; }
int stub_uart_pending(void) { return uart_len; }

HAL_StatusTypeDef HAL_UART_Receive(UART_HandleTypeDef *h, uint8_t *data, uint16_t size, uint32_t timeout)
{
    (void)size;
    if (uart_len == 0) {
        tick_ms += timeout;
        return HAL_TIMEOUT;
    }
    *data = uart_fifo[uart_head];
    uart_head = (uart_head + 1) % (int)sizeof(uart_fifo);
    uart_len--;
    h->Instance->SR &= ~UART_FLAG_ORE; /* SR read + DR read clears ORE */
    return HAL_OK;
}

/* ---- FatFs: one in-memory file ---- */
StubDisk stub_disk;

FRESULT f_mount(FATFS *fs, const char *path, BYTE opt)
{
    (void)path;
    (void)opt;
    stub_disk.mount_calls++;
    fs->mounted = (stub_disk.mount_result == FR_OK);
    return stub_disk.mount_result;
}

FRESULT f_open(FIL *fp, const char *path, BYTE mode)
{
    (void)path;
    (void)mode;
    if (stub_disk.open_result != FR_OK) {
        return stub_disk.open_result;
    }
    fp->open = 1;
    stub_disk.open_files++;
    return FR_OK;
}

FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw)
{
    (void)fp;
    *bw = 0;
    if (stub_disk.write_result != FR_OK) {
        return stub_disk.write_result;
    }
    if (stub_disk.short_write) {
        btw /= 2;
    }
    if (stub_disk.size + btw > sizeof(stub_disk.data) - 1) {
        btw = (UINT)(sizeof(stub_disk.data) - 1 - stub_disk.size);
    }
    memcpy(stub_disk.data + stub_disk.size, buff, btw);
    stub_disk.size += btw;
    stub_disk.data[stub_disk.size] = '\0';
    *bw = btw;
    return FR_OK;
}

int f_puts(const char *str, FIL *fp)
{
    UINT bw;
    UINT len = (UINT)strlen(str);
    return (f_write(fp, str, len, &bw) == FR_OK && bw == len) ? (int)len : -1;
}

FRESULT f_sync(FIL *fp)
{
    (void)fp;
    return stub_disk.write_result;
}

FRESULT f_close(FIL *fp)
{
    fp->open = 0;
    stub_disk.open_files--;
    return FR_OK;
}

size_t stub_f_size(void) { return stub_disk.size; }

/* ---- OLED: remember the text rows ---- */
const SSD1306_Font_t Font_6x8 = {6, 8};
const SSD1306_Font_t Font_7x10 = {7, 10};
char stub_oled[STUB_OLED_ROWS][64];
int stub_oled_updates;
static int oled_row;

void ssd1306_Init(void) {}
void ssd1306_Fill(SSD1306_COLOR color)
{
    (void)color;
    memset(stub_oled, 0, sizeof(stub_oled));
}
void ssd1306_SetCursor(uint8_t x, uint8_t y)
{
    (void)x;
    oled_row = y / 12;
}
char ssd1306_WriteString(char *str, SSD1306_Font_t font, SSD1306_COLOR color)
{
    (void)font;
    (void)color;
    if (oled_row < STUB_OLED_ROWS) {
        strncpy(stub_oled[oled_row], str, sizeof(stub_oled[0]) - 1);
    }
    return 0;
}
void ssd1306_UpdateScreen(void) { stub_oled_updates++; }

void stub_reset(void)
{
    tick_ms = 0;
    spi_bytes = 0;
    uart_head = uart_len = 0;
    usart1_regs.SR = 0;
    spi1_regs.CR1 = 0;
    memset(&stub_disk, 0, sizeof(stub_disk));
    memset(stub_oled, 0, sizeof(stub_oled));
    stub_oled_updates = 0;
    sd_sim_reset();
}
