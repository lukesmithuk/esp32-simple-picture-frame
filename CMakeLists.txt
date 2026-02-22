cmake_minimum_required(VERSION 3.16)

# Project-local components (pmic, epd, rtc, …)
list(APPEND EXTRA_COMPONENT_DIRS components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-picture-frame)
