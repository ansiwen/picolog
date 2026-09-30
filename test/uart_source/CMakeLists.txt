cmake_minimum_required(VERSION 3.13)

# Standalone project: build it on its own, it is not part of the picolog build.
# The default matches picolog; use -DPICO_BOARD=pico for an RP2040 Pico.
set(PICO_BOARD pico2 CACHE STRING "Board type")
include(pico_sdk_import.cmake)

project(uart_source C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

pico_sdk_init()

set(UART_SOURCE_BAUD 115200 CACHE STRING "Baud rate")
set(UART_SOURCE_TX_PIN 0 CACHE STRING "GPIO used as UART0 TX: 0, 12 or 16 (GP0 = pin 1)")
set(UART_SOURCE_PERIOD_MS 100 CACHE STRING "Interval between lines in ms")
set_property(CACHE UART_SOURCE_TX_PIN PROPERTY STRINGS 0 12 16)

if (NOT UART_SOURCE_TX_PIN MATCHES "^(0|12|16)$")
    message(FATAL_ERROR "UART_SOURCE_TX_PIN must be a UART0 TX pin: 0, 12 or 16")
endif ()

add_executable(uart_source uart_source.c)

target_compile_definitions(uart_source PRIVATE
    UART_SOURCE_BAUD=${UART_SOURCE_BAUD}
    UART_SOURCE_TX_PIN=${UART_SOURCE_TX_PIN}
    UART_SOURCE_PERIOD_MS=${UART_SOURCE_PERIOD_MS}
)

set_source_files_properties(uart_source.c PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Werror")

target_link_libraries(uart_source PRIVATE pico_stdlib hardware_uart)

# UART0 is the output: no stdio.
pico_enable_stdio_usb(uart_source 0)
pico_enable_stdio_uart(uart_source 0)

pico_add_extra_outputs(uart_source)
