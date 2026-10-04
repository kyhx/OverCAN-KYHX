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
#   外设的驱动。本项目的外设初始化写在端口层（node1_bsp.c）而非 MX_xxx_Init()，
#   所以 CAN/TIM/IWDG 的驱动会随一次"重新生成"被删掉，表现为满屏
#   "undefined reference to HAL_CAN_Init / HAL_TIM_PWM_Init / HAL_IWDG_Init"。
#   2026-10-04 已实际踩到。
#
#   驱动源码本身来自官方包 STM32Cube_FW_F1_V1.8.7，需手工拷入
#   Drivers/STM32F1xx_HAL_Driver/{Src,Inc}（含 _ex 变体：adc_ex 装校准、
#   tim_ex 被 hal_tim.h 直接 include）。启用项由 Core/Inc/node1_hal_conf_extra.h
#   提供（同样不会被 CubeMX 覆盖）。
# ---------------------------------------------------------------------------
set(NODE1_EXTRA_HAL_DRIVERS
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_can.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_tim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_tim_ex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_iwdg.c
)

# 文件缺失时给出清晰报错，而不是让链接器报一堆 undefined reference
foreach(_src IN LISTS NODE1_EXTRA_HAL_DRIVERS)
    if(NOT EXISTS "${_src}")
        message(FATAL_ERROR
            "缺少 HAL 驱动源码：${_src}\n"
            "CubeMX 重新生成会删掉 .ioc 之外的外设驱动，请从官方包补拷：\n"
            "  C:/Users/Administrator/STM32Cube/Repository/STM32Cube_FW_F1_V1.8.7/"
            "Drivers/STM32F1xx_HAL_Driver/{Src,Inc}")
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
# 都会按 .ioc 重写，把 .ioc 之外的外设启用项重新注释掉。
# 定义在命令行上则与文件归属无关，CubeMX 重新生成也不会丢。
# ---------------------------------------------------------------------------
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # 只定义 CubeMX 不认识的那三个（我们的外设初始化不走 MX_xxx_Init，所以它们
    # 永远不会出现在 .ioc 里）。ADC / UART 已在 .ioc 中配置、由 hal_conf.h 启用，
    # 这里若再定义会产生 "HAL_xxx_MODULE_ENABLED redefined" 告警。
    HAL_CAN_MODULE_ENABLED
    HAL_TIM_MODULE_ENABLED
    HAL_IWDG_MODULE_ENABLED
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
