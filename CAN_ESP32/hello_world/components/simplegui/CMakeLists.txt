# SimpleGUI 显示组件（移植到 ESP32-S3 / ESP-IDF v6.x）
#
# 目录职责
#   GUI/    SimpleGUI 核心库（上游原样，16 个 .c）—— 不要改，便于日后比对上游
#   HMI/    交互引擎（上游原样：页面栈 + 事件分发）
#   port/   平台移植层（**唯一与 MCU 相关的地方**）
#             src/screen.c   显存 + 脏区刷新（平台无关，直接复用 STM32 版）
#             src/ssd1306.c  SSD1306 驱动（ESP32 / ESP-IDF I2C 版）
#             include/RTC.h  引擎需要的占位 RTC
#   demo/   演示界面与中文字库（后续可替换为核心机的真实界面）
#
# ⚠️ 必须显式列出 SRCS
#   `idf_component_register` **不会递归**收集子目录里的 .c，它只收组件根目录下
#   的源文件。不写 SRCS 时，本组件会被正常"注册"（出现在 Components 列表、
#   也生成 build/esp-idf/simplegui/ 目录），但**一个目标文件都不会编**，
#   直到链接期才报一片 `undefined reference to SGUI_* / SCREEN_* / SSD1306_*`。
#   2026-10-04 踩到：现象具有很强的迷惑性 —— 看日志像"组件没被识别"，
#   实际是"识别了但没源文件"。
#   这里刻意逐个列出（而不是 GLOB），既保证确定性，也让依赖一目了然。
#
# ⚠️ 编译开关不放这里！SGUI_USE_HARDWARE_PLATFORM / SGUI_GET_POINT_FUNC_EN
#    定义在**顶层 CMakeLists.txt**。原因：ESP-IDF v6.1 里组件 CMakeLists 解析
#    阶段的 ${COMPONENT_LIB} 仍是 INTERFACE 库，无论 CMake 原生
#    target_compile_definitions（带 PRIVATE/PUBLIC/INTERFACE）还是 IDF 的
#    component_compile_definitions() 包装函数，都会报
#    "may only set INTERFACE properties on INTERFACE targets"。
#    项目级 add_compile_definitions() 对所有组件生效，且能保证
#    "库 / 移植层 / main"看到的宏完全一致 —— 这点很关键：
#    SGUI_GET_POINT_FUNC_EN 会改变 SGUI_SCR_DEV 的成员布局。


idf_component_register(
    SRCS
        # --- SimpleGUI 核心库（上游原样，16 个）---
        "GUI/src/SGUI_Basic.c"
        "GUI/src/SGUI_Common.c"
        "GUI/src/SGUI_Curve.c"
        "GUI/src/SGUI_FontResource.c"
        "GUI/src/SGUI_IconResource.c"
        "GUI/src/SGUI_Interface.c"
        "GUI/src/SGUI_ItemsBase.c"
        "GUI/src/SGUI_List.c"
        "GUI/src/SGUI_Menu.c"
        "GUI/src/SGUI_Notice.c"
        "GUI/src/SGUI_PolarCoordinates.c"
        "GUI/src/SGUI_ProcessBar.c"
        "GUI/src/SGUI_RealtimeGraph.c"
        "GUI/src/SGUI_ScrollBar.c"
        "GUI/src/SGUI_Text.c"
        "GUI/src/SGUI_VariableBox.c"
        # --- HMI 交互引擎（上游原样）---
        "HMI/src/HMI_Engine.c"
        # --- 平台移植层（唯一与 MCU 相关）---
        "port/src/screen.c"
        "port/src/ssd1306.c"
        "port/src/mpu6050.c"
        "port/src/input.c"
        "port/src/can_twai.c"
        # --- 演示界面 + 中文字库 ---
        "demo/src/sgui_demo.c"
        "demo/src/sgui_font_gb2312.c"
        # --- 共用协议库（零 HAL，与节点一、PC 单测共用同一份源码）---
        # ⚠️ 这里用相对路径引用工程外的 firmware/ 目录，而不是复制一份。
        #    理由：协议是**两端唯一必须逐字节一致**的东西，复制两份就等于
        #    埋下"改了这边忘了那边"的隐患。同一份源码是"单源真相"的落地方式。
        #    路径层级：components/simplegui/ -> hello_world/ -> CAN_ESP32/ -> 仓库根
        "${COMPONENT_DIR}/../../../../firmware/common/protocol/src/proto_id.c"
        "${COMPONENT_DIR}/../../../../firmware/common/protocol/src/proto_codec.c"
        "${COMPONENT_DIR}/../../../../firmware/common/protocol/src/proto_seq.c"
        "${COMPONENT_DIR}/../../../../firmware/common/protocol/src/proto_crc.c"
        "${COMPONENT_DIR}/../../../../firmware/common/protocol/src/proto_node.c"
    INCLUDE_DIRS
        "port/include"
        "GUI/inc"
        "HMI/inc"
        "demo/include"
        # 协议库头文件（与上面的 .c 同一份源码）
        "${COMPONENT_DIR}/../../../../firmware/common/protocol/include"
    REQUIRES
        driver
        esp_driver_i2c
        esp_driver_gpio
        # 编码器用 PCNT 硬件计数（port/src/input.c include driver/pulse_cnt.h）。
        # 不列这一项时编译会明确提示 "provided by esp_driver_pcnt component(s)"。
        esp_driver_pcnt
        # CAN：用 ESP32-S3 内置 TWAI 控制器（port/src/can_twai.c include driver/twai.h）
        esp_driver_twai
        freertos
)
