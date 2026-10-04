# SimpleGUI 显示组件（本地组件，见 components/simplegui/）
#
# 为什么必须在 main 里显式 REQUIRES：
#   顶层 CMakeLists.txt 设了 `idf_build_set_property(MINIMAL_BUILD ON)`，
#   只编译 main 及其依赖闭包。不写在这里，simplegui 组件不会被编译，
#   直到链接时才报一堆未定义符号（SGUI_*、SCREEN_*、SSD1306_*）。
#
# 显示栈（自下而上）：
#   i2c_master（ESP-IDF 驱动）
#     -> ssd1306.c    SSD1306 控制器驱动（ESP32 移植版）
#       -> screen.c   显存 + 脏区刷新，实现 SGUI_SCR_DEV 回调
#         -> SimpleGUI GUI/HMI 库
#           -> demo 界面（后续替换为核心机状态页）
#
# 引脚：SDA = GPIO1，SCL = GPIO2（docs/引脚分配.md §2.1 核心机 I2C0）
#
# 注意：本文件是 CMake 脚本，注释只能用 #，不能写 /* */（会解析失败）。
#
# PRIV_REQUIRES 里每一项都对应一个实际 include 的组件头：
#   spi_flash  → esp_flash.h（打印 flash 容量）
#   esp_timer  → esp_timer.h（运行时长，仪表盘显示）
#   simplegui  → 本项目的显示组件
idf_component_register(SRCS "hello_world_main.c"
                       PRIV_REQUIRES spi_flash simplegui esp_timer
                       INCLUDE_DIRS "")
