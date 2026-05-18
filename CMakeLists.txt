cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(Bluetooth_keyboard)

zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR})

target_sources(app PRIVATE
    src/ble_hid_module.c
    src/display_assets.c
    src/display_module.c
    src/key_matrix.c
    src/keymap.c
    src/main.c
    src/mode_event.c
    src/mode_switch.c
    src/power_trace.c
    src/power_module.c
    src/rgb_led.c
    src/rotary.c
    src/transport_manager.c
    src/usb_hid_module.c
    src/usb_host_proto.c
)
