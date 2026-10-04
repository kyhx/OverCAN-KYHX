# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# ---------------------------------------------------------------------------
# SimpleGUI 的编译开关（项目级，对所有组件生效）
#
# 为什么不写在 components/simplegui/CMakeLists.txt 里：
#   ESP-IDF v6.1 中组件 CMakeLists 解析阶段的 ${COMPONENT_LIB} 是 INTERFACE 库，
#   对它调用 target_compile_definitions（无论 PRIVATE/PUBLIC/INTERFACE）或 IDF 的
#   component_compile_definitions() 包装函数，都会报：
#       target_compile_definitions may only set INTERFACE properties
#       on INTERFACE targets
#   放在项目级则对所有组件生效，同时**保证各编译单元看到的宏一致** ——
#   这对 SGUI_GET_POINT_FUNC_EN 尤其重要：它会改变 SGUI_SCR_DEV 的成员布局，
#   若有编译单元没带上它，结构体布局不一致会静默读写错字段。
#
# SGUI_USE_HARDWARE_PLATFORM
#   由 GUI/inc/SGUI_Config.h 判断"编硬件还是编模拟器"。**不定义**它，
#   库会走 _SIMPLE_GUI_IN_VIRTUAL_SDK_ 分支去 #include <windows.h>，
#   在 ESP32 上必然编译失败。上游的设计就是"不改源码，只在工程里定义本宏"。
#
# SGUI_GET_POINT_FUNC_EN
#   解锁"读像素"能力（SGUI_SCR_DEV 多出 fnGetPixel，SGUI_Basic_GetPoint 等
#   函数参与编译）。开启后必须给 fnGetPixel 赋非空实现，否则库会静默返回背景色；
#   本项目的 port/src/screen.c 已实现 SCREEN_GetPixel，装配处也已挂上。
#
# 注意：**不要**定义 _SIMPLE_GUI_ENCODE_TEXT_（会引 iconv → 链接失败）、
#   _SIMPLE_GUI_ENABLE_DYNAMIC_MEMORY_、USE_CHS_TEXT。
#   中文直接以 GB2312 转义写进源码，运行时不需要转码库。
# ---------------------------------------------------------------------------
add_compile_definitions(SGUI_USE_HARDWARE_PLATFORM)
add_compile_definitions(SGUI_GET_POINT_FUNC_EN)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(hello_world)
