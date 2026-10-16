# ESP32-S3-RS485-CAN project stub
# Board: Waveshare ESP32-S3-RS485-CAN (ESP32-S3R8, 16MB flash, 8MB PSRAM)
#
# Pin mapping (confirmed hardware defaults):
#   IO17 -> RS485 TX (UART TXD)
#   IO18 -> RS485 RX (UART RXD)
#   IO21 -> RS485 EN (DE/RE direction control)

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ESP32_growatt_vEM)