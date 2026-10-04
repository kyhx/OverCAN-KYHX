cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME Node1)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
# ---------------------------------------------------------------------------
# HAL 驱动补丁：为什么这些不写在 cmake/stm32cubemx/CMakeLists.txt 里
#   那个文件由 STM32CubeMX 每次重新生成时**整份重写**，只包含 `.ioc` 里已配
#   外设的驱动。该项目的外设初始化写在端口层（node1_bsp.c）而非 MX_xxx_Init()，
#   所以 `.ioc` 之外的驱动会随一次"重新生成"被删掉。
#   2026-10-04 已实际踩到两次：
#     ① 第一次：can/tim/tim_ex/iwdg 四个驱动一起被删 → 链接期满屏 undefined reference；
#     ② 第二次：CubeMX 把 CAN 勾进了 .ioc（它现在会生成 hal_can.c），
#        而本文件仍无条件补一份 → **同一文件被编译两次** →
#        "multiple definition of HAL_CAN_GetError"。
#
# 因此这里**不能**写死"哪几个要补"，而是**逐个判断 CubeMX 是否已经提供了**：
#   已提供 → 不补（避免重复定义）
#   未提供 → 补上（避免缺失）
# 这样无论 .ioc 将来怎么变，两种情况都不会出错。
#
# 驱动源码来自官方包 STM32Cube_FW_F1_V1.8.7（本项目在
# C:/Users/Administrator/STM32Cube/Repository/ 下已有）。
# 启用项见本文件下方的 target_compile_definitions —— **定义在编译命令行上**，
# 与文件归属无关，CubeMX 重新生成也不会丢。
# ---------------------------------------------------------------------------

# 我们**需要用、但不一定由 CubeMX 生成**的驱动。
# 判据是"node1_bsp.c 里实际调用了它们的 HAL 函数"：
#   CAN  → HAL_CAN_Init / HAL_CAN_ConfigFilter ...
#   TIM  → HAL_TIM_PWM_Init / HAL_TIM_PWM_ConfigChannel ...
#   TIMEX→ HAL_TIMEx_PWMN_Start（PB15 互补输出的启动函数，在 tim_ex.c 里）
#   IWDG → HAL_IWDG_Init / HAL_IWDG_Refresh
set(NODE1_WANTED_HAL_DRIVERS
    stm32f1xx_hal_can.c
    stm32f1xx_hal_tim.c
    stm32f1xx_hal_tim_ex.c
    stm32f1xx_hal_iwdg.c
)

# 判断"CubeMX 是否已经提供了某个驱动"。
#
# ⚠️ 不要试图从 CMake 目标模型里取（`get_target_property(... SOURCES)` /
#   `get_property(TARGET ... PROPERTY SOURCES)` 都取不到——`stm32cubemx` 是由
#   CubeMX 生成的 INTERFACE 风格目标，源文件挂在它内部的 `STM32_Drivers` 上，
#   两者在配置期的可见性都不稳定。2026-10-04 在这上面连试两种写法均为空列表）。
#
# 改为**直接读 CubeMX 生成的那份 CMakeLists 文件**：它是"谁被编译"的**唯一事实来源**，
# 且与 CMake 版本无关、不依赖目标模型。
set(NODE1_CUBEMX_CMAKE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/stm32cubemx/CMakeLists.txt")
set(_cubemx_text "")
if(EXISTS "${NODE1_CUBEMX_CMAKE}")
    file(READ "${NODE1_CUBEMX_CMAKE}" _cubemx_text)
else()
    message(STATUS "提示：未找到 ${NODE1_CUBEMX_CMAKE}，HAL 驱动将全部由本文件补入")
endif()

set(NODE1_EXTRA_HAL_DRIVERS)
foreach(_drv IN LISTS NODE1_WANTED_HAL_DRIVERS)
    set(_path "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32F1xx_HAL_Driver/Src/${_drv}")

    # 文件不存在 → 必须报错（CubeMX 会删掉 .ioc 之外的驱动，需从官方包补拷）
    if(NOT EXISTS "${_path}")
        message(FATAL_ERROR
            "缺少 HAL 驱动源码：${_path}\n"
            "CubeMX 重新生成会删掉 .ioc 之外的外设驱动，请从官方包补拷：\n"
            "  C:/Users/Administrator/STM32Cube/Repository/STM32Cube_FW_F1_V1.8.7/"
            "Drivers/STM32F1xx_HAL_Driver/{Src,Inc}")
    endif()

    get_filename_component(_path_abs "${_path}" REALPATH)

    # CubeMX 生成的文件里提到了这个名字 → 它已经在编译 → 不要重复加入
    string(FIND "${_cubemx_text}" "${_drv}" _found_at)
    if(NOT _found_at EQUAL -1)
        message(STATUS "HAL 驱动 ${_drv}：CubeMX 已提供，跳过补丁")
    else()
        list(APPEND NODE1_EXTRA_HAL_DRIVERS "${_path_abs}")
        message(STATUS "HAL 驱动 ${_drv}：CubeMX 未提供，由本文件补入")
    endif()
endforeach()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # 跨平台协议库（零 HAL 依赖，与 PC 单测、ESP32 侧共用同一份源码）
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/common/protocol/src/proto_id.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/common/protocol/src/proto_codec.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/common/protocol/src/proto_seq.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/common/protocol/src/proto_crc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/common/protocol/src/proto_node.c
    # 节点一：业务层（零 HAL，可 PC 单测）+ 调度层（与 RTOS 无关）
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/node1/src/node1_app.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/node1/src/node1_sched.c
    # 节点一：STM32 端口层（唯一接触 HAL/寄存器的地方）+ 固件入口
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/node1/port/stm32f103/node1_bsp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/node1/port/stm32f103/node1_main.c
    # 上面那段 HAL 驱动补丁
    ${NODE1_EXTRA_HAL_DRIVERS}
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/common/protocol/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/node1/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/node1/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/node1/port/stm32f103
)

# Add project symbols (macros)
# ---------------------------------------------------------------------------
# ⚠️ HAL 模块启用项必须**定义在编译命令行上**，不能只写在头文件里。
#
# 踩坑记录（2026-10-04）：把启用项放进 Core/Inc/node1_hal_conf_extra.h 是不够的。
#   HAL 驱动源码（如 stm32f1xx_hal_can.c）的整个文件体被
#   `#ifdef HAL_CAN_MODULE_ENABLED` 包着，而它们**先**包含 stm32f1xx_hal_conf.h、
#   并不包含我们的守卫头。于是这些 .c 编出来是**空的目标文件**（只有调试信息），
#   链接期报一堆 "undefined reference to HAL_CAN_Init / HAL_TIM_PWM_Init"。
#   由于目录里有 obj 文件、链接命令里也有它，这种失败极具迷惑性。
#
# 为什么不用改 stm32f1xx_hal_conf.h：那个文件整份由 CubeMX 生成，每次重新生成
# 都会按 .ioc 重写。定义在命令行上则与文件归属无关，CubeMX 重新生成也不会丢。
#
# ⚠️ 但**不能无条件定义**：CubeMX 一旦把某个外设勾进 .ioc，它就会在 hal_conf.h 里
#   `#define HAL_XXX_MODULE_ENABLED`，此时命令行再定义一次 → 
#   `warning: "HAL_XXX_MODULE_ENABLED" redefined`（2026-10-04 实际踩到：
#   CAN 被勾进 .ioc 后出现 7 个该告警）。
#   所以这里与上面的驱动补丁同样的思路：**先读 hal_conf.h，只补它没启用的那些**。
# ---------------------------------------------------------------------------
set(_hal_conf "${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc/stm32f1xx_hal_conf.h")
file(READ "${_hal_conf}" _hal_conf_text)

# 逐个判断"hal_conf.h 里是否已经**启用**（未注释）了该模块"
function(_node1_need_module_enable MODULE OUT_VAR)
    # 匹配行首（允许空白）的 #define HAL_X_MODULE_ENABLED
    # 被 /*#define ... */ 注释掉的行不会被匹配到
    string(REGEX MATCH "(^|\n)[ \t]*#define[ \t]+${MODULE}[ \t]*(\n|$)"
           _hit "${_hal_conf_text}")
    if(_hit)
        set(${OUT_VAR} FALSE PARENT_SCOPE)  # 已启用 → 不需要命令行补
    else()
        set(${OUT_VAR} TRUE PARENT_SCOPE)   # 未启用 → 必须命令行补
    endif()
endfunction()

set(NODE1_HAL_ENABLE_DEFS)
foreach(_m IN ITEMS HAL_CAN_MODULE_ENABLED HAL_TIM_MODULE_ENABLED HAL_IWDG_MODULE_ENABLED)
    _node1_need_module_enable("${_m}" _need)
    if(_need)
        list(APPEND NODE1_HAL_ENABLE_DEFS "${_m}")
        message(STATUS "HAL 启用项 ${_m}：hal_conf.h 未启用，由命令行补入")
    else()
        message(STATUS "HAL 启用项 ${_m}：hal_conf.h 已启用，命令行不再重复定义")
    endif()
endforeach()

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE ${NODE1_HAL_ENABLE_DEFS})

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
