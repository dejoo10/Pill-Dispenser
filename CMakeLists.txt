cmake_minimum_required(VERSION 3.13)

# Include Pico SDK
include(pico_sdk_import.cmake OPTIONAL)

project(pill_dispenser C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize Raspberry Pi Pico SDK
pico_sdk_init()

# Define project executable
add_executable(pill_dispenser
    main.c
)

# Link essential hardware libraries
target_link_libraries(pill_dispenser
    pico_stdlib
    hardware_gpio
    hardware_irq
)

# Enable USB serial output and disable UART serial by default
pico_enable_stdio_usb(pill_dispenser 1)
pico_enable_stdio_uart(pill_dispenser 0)

# Generate UF2, hex, and bin files for flashing
pico_add_extra_outputs(pill_dispenser)

