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
set(PICOLOG_UART_RX_PIN 1 CACHE STRING "GPIO used as UART0 RX: 1, 3, 13, 15, 17 or 19 (GP1 = pin 2)")
set_property(CACHE PICOLOG_UART_RX_PIN PROPERTY STRINGS 1 3 13 15 17 19)
set(PICOLOG_UART_DATA_BITS 8 CACHE STRING "UART data bits (5-8)")
set(PICOLOG_UART_STOP_BITS 1 CACHE STRING "UART stop bits (1 or 2)")
set(PICOLOG_UART_PARITY NONE CACHE STRING "UART parity: NONE, EVEN or ODD")
set_property(CACHE PICOLOG_UART_PARITY PROPERTY STRINGS NONE EVEN ODD)
option(PICOLOG_SELFTEST_TX "Core 1 sends timestamps on UART1 TX for a loopback test (off: no code at all)" OFF)
set(PICOLOG_SELFTEST_TX_PIN 4 CACHE STRING "GPIO used as UART1 TX by the self-test: 4, 6, 8, 10, 20, 22, 24 or 26 (GP4 = pin 6)")
set(PICOLOG_SELFTEST_PERIOD_MS 100 CACHE STRING "Interval between self-test lines in ms")

string(TOUPPER "${PICOLOG_UART_PARITY}" PICOLOG_UART_PARITY_UC)
if (NOT PICOLOG_UART_PARITY_UC MATCHES "^(NONE|EVEN|ODD)$")
    message(FATAL_ERROR "PICOLOG_UART_PARITY must be NONE, EVEN or ODD")
endif ()

# The capture code is hard-wired to UART0. These are the UART0 RX pins that the
# Pico 2 brings out (3, 15 and 19 use the RP2350's UART_AUX function; GPIO29 is
# UART0 RX-capable too but not on the header).
if (NOT PICOLOG_UART_RX_PIN MATCHES "^(1|3|13|15|17|19)$")
    message(FATAL_ERROR "PICOLOG_UART_RX_PIN must be a UART0 RX pin: 1, 3, 13, 15, 17 or 19")
endif ()

set(PICOLOG_SOURCES
    src/main.c
    src/capture.c
    src/history.c
    src/usb_ports.c
    src/usb_descriptors.c
)

# The self-test is compiled in only on request, so the default build is exactly
# the firmware without it: no extra source, no definitions, no extra libraries.
if (PICOLOG_SELFTEST_TX)
    if (NOT PICOLOG_SELFTEST_TX_PIN MATCHES "^(4|6|8|10|20|22|24|26)$")
        message(FATAL_ERROR "PICOLOG_SELFTEST_TX_PIN must be a UART1 TX pin: 4, 6, 8, 10, 20, 22, 24 or 26")
    endif ()
    if (NOT PICOLOG_SELFTEST_PERIOD_MS MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "PICOLOG_SELFTEST_PERIOD_MS must be a positive integer")
    endif ()
    list(APPEND PICOLOG_SOURCES src/selftest_tx.c src/selftest_line.c)
endif ()

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

if (PICOLOG_SELFTEST_TX)
    target_compile_definitions(picolog PRIVATE
        PICOLOG_SELFTEST_TX
        PICOLOG_SELFTEST_TX_PIN=${PICOLOG_SELFTEST_TX_PIN}
        PICOLOG_SELFTEST_PERIOD_MS=${PICOLOG_SELFTEST_PERIOD_MS}
    )
    target_link_libraries(picolog PRIVATE pico_multicore)
endif ()

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
