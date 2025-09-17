#pragma once

// ===== Board and modem wiring (from Arduino reference) =====
// LilyGO TTGO T-A7670 (ESP32 + SIMCom A7670)

// UART configuration for the GSM modem
#define PPP_UART_NUM              UART_NUM_1
#define PPP_UART_BAUDRATE         115200

// UART pins (ESP32 side)
#define PPP_UART_TX_GPIO          (GPIO_NUM_26)
#define PPP_UART_RX_GPIO          (GPIO_NUM_27)

// Flow control unused
#define PPP_UART_RTS_GPIO         (-1)
#define PPP_UART_CTS_GPIO         (-1)

// Modem control pins
#define BOARD_POWERON_GPIO        (GPIO_NUM_12)
#define BOARD_PWRKEY_GPIO         (GPIO_NUM_4)
#define MODEM_DTR_GPIO            (GPIO_NUM_25)
#define PPP_MODEM_RST_GPIO        (GPIO_NUM_5)

// Reset line is active HIGH per Arduino sequence provided
#define MODEM_RESET_LEVEL         1   // HIGH

// APN and SIM PIN
#define PPP_APN                   "internet.itelcel.com"
#define PPP_SIM_PIN               ""
#define PPP_USER                  "webgprs"
#define PPP_PASS                  "webgprs2002"

// For SIM7600/A76xx, use CGDATA to enter PPP data mode
#define PPP_DIAL  "AT+CGDATA=\"PPP\",1"

// Timeouts (ms)
#define PPP_AT_TIMEOUT_MS         5000
#define PPP_CONNECT_TIMEOUT_MS    60000
#define PPP_MODEM_BOOT_MS         3000

