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
set(CMAKE_PROJECT_NAME Node2)

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
# HAL 驱动补丁（同 CAN_Node1，理由一致）
#   CubeMX 只把 `.ioc` 里已配外设的驱动拷进 Drivers/，并在每次"重新生成"时
#   重写 cmake/stm32cubemx/CMakeLists.txt。本项目的外设初始化将写在端口层
#   （node2_bsp.c）而非 MX_xxx_Init()，因此 CAN/TIM 的驱动必须在这里补上。
#   驱动源码来自官方包 STM32Cube_FW_F1_V1.8.7，需手工拷入
#   Drivers/STM32F1xx_HAL_Driver/{Src,Inc}（含 _ex 变体）。
#
# 节点二当前还未接入固件（无 BSP / 业务层），这里先把 HAL 侧准备好。
# ---------------------------------------------------------------------------
set(NODE2_EXTRA_HAL_DRIVERS
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_can.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_tim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_tim_ex.c
)

foreach(_src IN LISTS NODE2_EXTRA_HAL_DRIVERS)
    if(NOT EXISTS "${_src}")
        message(FATAL_ERROR
            "缺少 HAL 驱动源码：${_src}\n"
            "CubeMX 重新生成会删掉 .ioc 之外的外设驱动，请从官方包补拷：\n"
            "  C:/Users/Administrator/STM32Cube/Repository/STM32Cube_FW_F1_V1.8.7/"
            "Drivers/STM32F1xx_HAL_Driver/{Src,Inc}")
    endif()
endforeach()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${NODE2_EXTRA_HAL_DRIVERS}
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
)

# Add project symbols (macros)
#
# ⚠️ HAL 模块启用项定义在**编译命令行**上，不写进 Core/Inc/stm32f1xx_hal_conf.h。
#   两个原因：
#   1) hal_conf.h 整份由 CubeMX 生成，每次重新生成都会按 .ioc 重写，把 .ioc 之外
#      的外设启用项重新注释掉（节点一 2026-10-04 已因此断过构建）；
#   2) HAL 驱动源码（stm32f1xx_hal_can.c 等）整个文件体被 #ifdef HAL_xxx_MODULE_ENABLED
#      包着，而它们不包含我们的头文件——只写在头里会让这些 .c 编成**空目标文件**，
#      链接期报一堆 undefined reference。
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # 只定义 hal_conf.h 里没有的（CubeMX 按 .ioc 生成的启用项不含 CAN/TIM）。
    # ADC / UART / DMA 已由 .ioc 配置并在 hal_conf.h 中启用，这里再定义会产生
    # "HAL_xxx_MODULE_ENABLED redefined" 告警。
    HAL_CAN_MODULE_ENABLED
    HAL_TIM_MODULE_ENABLED
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
