# Stand-alone firmware for a *second* Pico that acts as a 3.3 V UART source
# for testing picolog. Build it separately from the main firmware:
#
#   cmake -S test/uart_source -B build-uart-source
#   cmake --build build-uart-source
cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2 CACHE STRING "Board type of the source Pico (pico, pico2, ...)")
include(${CMAKE_CURRENT_LIST_DIR}/../../pico_sdk_import.cmake)

project(uart_source C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

pico_sdk_init()

# UART0 TX pins the boards bring out. Others would build, but the pin would stay silent.
# RP2350: 2, 14 and 18 use the UART_AUX function (see UART_FUNCSEL_NUM).
if (PICO_PLATFORM MATCHES "^rp2350")
    set(SOURCE_UART_TX_PINS 0 2 12 14 16 18 28)
else ()
    set(SOURCE_UART_TX_PINS 0 12 16 28)
endif ()

set(SOURCE_UART_BAUD 115200 CACHE STRING "Baud rate (must match PICOLOG_UART_BAUD)")
set(SOURCE_UART_TX_PIN 0 CACHE STRING "GPIO used as UART0 TX (GP0 = pin 1)")
set(SOURCE_UART_DATA_BITS 8 CACHE STRING "UART data bits (5-8)")
set(SOURCE_UART_STOP_BITS 1 CACHE STRING "UART stop bits (1 or 2)")
set(SOURCE_UART_PARITY NONE CACHE STRING "UART parity: NONE, EVEN or ODD")
set_property(CACHE SOURCE_UART_PARITY PROPERTY STRINGS NONE EVEN ODD)
set(SOURCE_PERIOD_MS 100 CACHE STRING "Interval between timestamps in milliseconds")

if (NOT SOURCE_UART_TX_PIN IN_LIST SOURCE_UART_TX_PINS)
    string(REPLACE ";" ", " pins "${SOURCE_UART_TX_PINS}")
    message(FATAL_ERROR "SOURCE_UART_TX_PIN must be a UART0 TX pin on ${PICO_PLATFORM}: ${pins}")
endif ()

string(TOUPPER "${SOURCE_UART_PARITY}" SOURCE_UART_PARITY_UC)
if (NOT SOURCE_UART_PARITY_UC MATCHES "^(NONE|EVEN|ODD)$")
    message(FATAL_ERROR "SOURCE_UART_PARITY must be NONE, EVEN or ODD")
endif ()

add_executable(uart_source uart_source.c)

target_compile_definitions(uart_source PRIVATE
    SOURCE_UART_BAUD=${SOURCE_UART_BAUD}
    SOURCE_UART_TX_PIN=${SOURCE_UART_TX_PIN}
    SOURCE_UART_DATA_BITS=${SOURCE_UART_DATA_BITS}
    SOURCE_UART_STOP_BITS=${SOURCE_UART_STOP_BITS}
    SOURCE_UART_PARITY=UART_PARITY_${SOURCE_UART_PARITY_UC}
    SOURCE_PERIOD_MS=${SOURCE_PERIOD_MS}
)

set_source_files_properties(uart_source.c PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Werror")

target_link_libraries(uart_source PRIVATE pico_stdlib hardware_uart)

# UART0 TX is the output; no stdio.
pico_enable_stdio_usb(uart_source 0)
pico_enable_stdio_uart(uart_source 0)

pico_add_extra_outputs(uart_source)
