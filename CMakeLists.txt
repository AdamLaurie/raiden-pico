cmake_minimum_required(VERSION 3.13)

# Set board type before including SDK
set(PICO_BOARD pico2 CACHE STRING "Board type")

# Pull in SDK (must be before project)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(raiden_pico C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the SDK
pico_sdk_init()

# Main executable
add_executable(raiden_pico
    src/main.c
    src/uart_cli.c
    src/command_parser.c
    src/platform.c
    src/glitch.c
    src/chipshot_uart.c
    src/target_uart.c
)

# Include directories
target_include_directories(raiden_pico PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
)

# Generate PIO headers
pico_generate_pio_header(raiden_pico ${CMAKE_CURRENT_LIST_DIR}/src/glitch.pio)
pico_generate_pio_header(raiden_pico ${CMAKE_CURRENT_LIST_DIR}/src/platform.pio)
# uart.pio not used - target UART uses hardware UART (uart1), not PIO
# pico_generate_pio_header(raiden_pico ${CMAKE_CURRENT_LIST_DIR}/src/uart.pio)

# Link libraries
target_link_libraries(raiden_pico
    pico_stdlib
    hardware_pio
    hardware_uart
    hardware_pwm
    hardware_gpio
    hardware_timer
)

# Enable USB-only serial output (no UART stdio)
pico_enable_stdio_usb(raiden_pico 1)
pico_enable_stdio_uart(raiden_pico 0)

# Create map/bin/hex/uf2 files
pico_add_extra_outputs(raiden_pico)
