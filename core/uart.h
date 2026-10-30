#ifndef UART_H
#define UART_H
#include <stdint.h>
typedef struct UART UART;
UART *uart_new(int irq, void *pic, void (*set_irq)(void *pic, int irq, int level), int mouse);
uint8_t uart_read(UART *u, int reg);
void uart_write(UART *u, int reg, uint8_t val);
void uart_step(UART *u);
void uart_mouse_event(UART *u, int dx, int dy, int buttons);
#endif
