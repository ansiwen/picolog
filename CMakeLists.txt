cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2 CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(picolog C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

set(PICOLOG_USB_VID "0x1209" CACHE STRING "USB vendor ID (default: pid.codes test VID)")
set(PICOLOG_USB_PID "0x0001" CACHE STRING "USB product ID (default: pid.codes test PID)")
set(PICOLOG_UART_BAUD 115200 CACHE STRING "Target UART baud rate")
set(PICOLOG_UART_RX_PIN 1 CACHE STRING "GPIO for UART0 RX (must be a UART0 RX-capable pin)")
set(PICOLOG_UART_DATA_BITS 8 CACHE STRING "UART data bits (5-8)")
set(PICOLOG_UART_STOP_BITS 1 CACHE STRING "UART stop bits (1-2)")
set(PICOLOG_UART_PARITY UART_PARITY_NONE CACHE STRING "UART_PARITY_NONE, UART_PARITY_EVEN or UART_PARITY_ODD")
set(PICOLOG_WATCHDOG_MS 2000 CACHE STRING "Watchdog timeout in ms")

set(PICOLOG_SOURCES
    src/main.c
    src/capture.c
    src/history.c
    src/usb_ports.c
    src/usb_descriptors.c
)

add_executable(picolog ${PICOLOG_SOURCES})

# Strict warnings for our own sources only; SDK sources are compiled into the
# same target and are not held to -Werror.
set_source_files_properties(${PICOLOG_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")

target_include_directories(picolog PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)

target_compile_definitions(picolog PRIVATE
    PICOLOG_USB_VID=${PICOLOG_USB_VID}
    PICOLOG_USB_PID=${PICOLOG_USB_PID}
    PICOLOG_UART_BAUD=${PICOLOG_UART_BAUD}
    PICOLOG_UART_RX_PIN=${PICOLOG_UART_RX_PIN}
    PICOLOG_UART_DATA_BITS=${PICOLOG_UART_DATA_BITS}
    PICOLOG_UART_STOP_BITS=${PICOLOG_UART_STOP_BITS}
    PICOLOG_UART_PARITY=${PICOLOG_UART_PARITY}
    PICOLOG_WATCHDOG_MS=${PICOLOG_WATCHDOG_MS}
)

target_link_libraries(picolog PRIVATE
    pico_stdlib
    hardware_dma
    hardware_uart
    hardware_irq
    pico_unique_id
    tinyusb_device
)

# No stdio at all: USB CDC is ours (pico_stdio_usb would claim it) and
# UART0 is the capture input.
pico_enable_stdio_usb(picolog 0)
pico_enable_stdio_uart(picolog 0)

pico_add_extra_outputs(picolog)
