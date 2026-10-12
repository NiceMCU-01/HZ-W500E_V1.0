#ifndef BOARD_PINS_H
#define BOARD_PINS_H

/*
 * 引脚定义来自项目根目录 脚为定义.txt
 *
 *   控制芯片 : ESP32-C3FH4
 *   网口芯片 : W5500
 *
 *   W5500_CS   = 9
 *   W5500_RST  = 6
 *   W5500_SCK  = 7
 *   W5500_MISO = 3
 *   W5500_MOSI = 10
 *   W5500_INT  = 8
 */
#define PIN_W5500_CS    9
#define PIN_W5500_RST   6
#define PIN_W5500_SCK   7
#define PIN_W5500_MISO  3
#define PIN_W5500_MOSI  10
#define PIN_W5500_INT   8

/* SPI 速率(MHz) 与 W5500 PHY 地址 */
#define PIN_W5500_SPI_MHZ     20
#define PIN_W5500_PHY_ADDR    1

/* AT 指令走 UART0（CH340 接到 C3 的 UART0 默认脚） */
#define AT_UART_NUM     0
#define AT_UART_BAUD    115200

#endif /* BOARD_PINS_H */