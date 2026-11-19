/* Host-side stand-in for the SSD1306 driver: records what would be drawn. */
#ifndef STUB_SSD1306_H
#define STUB_SSD1306_H

#include <stdint.h>

typedef enum { Black = 0x00, White = 0x01 } SSD1306_COLOR;
typedef struct { uint8_t width, height; } SSD1306_Font_t;

void ssd1306_Init(void);
void ssd1306_Fill(SSD1306_COLOR color);
void ssd1306_SetCursor(uint8_t x, uint8_t y);
char ssd1306_WriteString(char *str, SSD1306_Font_t font, SSD1306_COLOR color);
void ssd1306_UpdateScreen(void);

/* Test hooks (hal_stub.c): text of the five display rows of the last frame. */
#define STUB_OLED_ROWS 5
extern char stub_oled[STUB_OLED_ROWS][64];
extern int stub_oled_updates;

#endif
