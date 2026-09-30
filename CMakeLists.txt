cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2 CACHE STRING "Board type")
include(pico_sdk_import.cmake)

project(picolog C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

pico_sdk_init()

set(PICOLOG_USB_VID 0x1209 CACHE STRING "USB vendor ID (default: pid.codes)")
set(PICOLOG_USB_PID 0x0001 CACHE STRING "USB product ID (default: pid.codes test PID)")
set(PICOLOG_UART_BAUD 115200 CACHE STRING "Baud rate of the captured UART")
set(PICOLOG_UART_RX_PIN 1 CACHE STRING "GPIO used as UART0 RX (GP1 = pin 2)")
set(PICOLOG_UART_DATA_BITS 8 CACHE STRING "UART data bits (5-8)")
set(PICOLOG_UART_STOP_BITS 1 CACHE STRING "UART stop bits (1 or 2)")
set(PICOLOG_UART_PARITY NONE CACHE STRING "UART parity: NONE, EVEN or ODD")
set_property(CACHE PICOLOG_UART_PARITY PROPERTY STRINGS NONE EVEN ODD)

string(TOUPPER "${PICOLOG_UART_PARITY}" PICOLOG_UART_PARITY_UC)
if (NOT PICOLOG_UART_PARITY_UC MATCHES "^(NONE|EVEN|ODD)$")
    message(FATAL_ERROR "PICOLOG_UART_PARITY must be NONE, EVEN or ODD")
endif ()

set(PICOLOG_SOURCES
    src/main.c
    src/capture.c
    src/history.c
    src/usb_ports.c
    src/usb_descriptors.c
)

add_executable(picolog ${PICOLOG_SOURCES})

target_include_directories(picolog PRIVATE src)

target_compile_definitions(picolog PRIVATE
    PICOLOG_USB_VID=${PICOLOG_USB_VID}
    PICOLOG_USB_PID=${PICOLOG_USB_PID}
    PICOLOG_UART_BAUD=${PICOLOG_UART_BAUD}
    PICOLOG_UART_RX_PIN=${PICOLOG_UART_RX_PIN}
    PICOLOG_UART_DATA_BITS=${PICOLOG_UART_DATA_BITS}
    PICOLOG_UART_STOP_BITS=${PICOLOG_UART_STOP_BITS}
    PICOLOG_UART_PARITY=UART_PARITY_${PICOLOG_UART_PARITY_UC}
)

# -Werror only on our own sources: SDK sources are compiled into this target too.
set_source_files_properties(${PICOLOG_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Werror")

target_link_libraries(picolog PRIVATE
    pico_stdlib
    hardware_dma
    hardware_uart
    hardware_irq
    pico_unique_id
    tinyusb_device
)

# UART0 is the capture input and USB CDC belongs to us: no stdio at all.
pico_enable_stdio_usb(picolog 0)
pico_enable_stdio_uart(picolog 0)

pico_add_extra_outputs(picolog)
