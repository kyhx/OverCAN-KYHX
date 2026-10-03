# can_distributed 协议库 —— PC 侧构建与单元测试
#
# 构建的是**与 MCU 侧完全相同的源码**：协议库零 HAL 依赖，所以能在 PC 上编译、
# 在 PC 上跑测试。两块异构 MCU（ESP32-S3 / STM32F103）共用这一份实现。
#
#   cmake -S . -B build
#   cmake --build build --config Debug
#   ctest --test-dir build -C Debug --output-on-failure
#
# 或直接： cmake --build build --target check
#
cmake_minimum_required(VERSION 3.16)
project(can_distributed_protocol C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

enable_testing()

find_package(Python3 COMPONENTS Interpreter)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(PROTO_DIR "${REPO_ROOT}/firmware/common/protocol")
set(NODE1_DIR "${REPO_ROOT}/firmware/node1")

# ---------------------------------------------------------------------------
# 被测试对象：协议静态库
# ---------------------------------------------------------------------------
add_library(proto STATIC
    "${PROTO_DIR}/src/proto_id.c"
    "${PROTO_DIR}/src/proto_codec.c"
    "${PROTO_DIR}/src/proto_seq.c"
    "${PROTO_DIR}/src/proto_crc.c"
    "${PROTO_DIR}/src/proto_node.c"
)
target_include_directories(proto PUBLIC "${PROTO_DIR}/include")
# DBC 漂移测试需要知道交付物与定义源的路径
target_compile_definitions(proto PUBLIC
    PROTO_DBC_PATH="${REPO_ROOT}/can/can_distributed.dbc"
    PROTO_ID_HEADER_PATH="${PROTO_DIR}/include/proto_id.h"
)

if(MSVC)
    # /utf-8：源码含中文注释（UTF-8 无 BOM），否则 MSVC 在 936 代码页下报 C4819 并可能截断
    target_compile_options(proto PRIVATE /W4 /utf-8)
    target_compile_definitions(proto PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(proto PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# 测试可执行文件（每个 .c 自带 main，零外部测试框架依赖）
# ---------------------------------------------------------------------------
function(add_proto_test name)
    add_executable(${name} "tests/${name}.c")
    target_link_libraries(${name} PRIVATE proto)
    target_include_directories(${name} PRIVATE "${REPO_ROOT}/tests/harness")
    if(MSVC)
        target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${name} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_proto_test(test_codec)
add_proto_test(test_seq)
add_proto_test(test_crc)
add_proto_test(test_dbc_drift)

# ---------------------------------------------------------------------------
# 节点一业务逻辑（node1_app）—— 同样零 HAL 依赖，故可注入 mock HAL 在 PC 上测
#
# 这一层是"协议库之上、寄存器之下"的决策逻辑：命令执行 / 遥测组装 /
# 事件判定 / 掉线安全态。它能测，就能证明**没有硬件台架时逻辑也是对的**。
# ---------------------------------------------------------------------------
add_library(node1_app STATIC
    "${NODE1_DIR}/src/node1_app.c"
    "${NODE1_DIR}/src/node1_sched.c"
)
target_include_directories(node1_app PUBLIC
    "${NODE1_DIR}/include"
    "${NODE1_DIR}/src"     # node1_sched.h 与实现同目录，放在这里便于单测引用
    "${PROTO_DIR}/include"
)
# 温标换算用 logf/expf，MSVC 下 logf 在 ucrt、Linux 下在 libm
if(MSVC)
    target_compile_definitions(node1_app PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(node1_app PRIVATE /W4 /utf-8)
else()
    target_compile_options(node1_app PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(node1_app PUBLIC m)
endif()

function(add_node1_test name)
    add_executable(${name} "tests/${name}.c")
    target_link_libraries(${name} PRIVATE node1_app proto)
    target_include_directories(${name} PRIVATE "${REPO_ROOT}/tests/harness")
    if(MSVC)
        target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${name} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_node1_test(test_node_app)
add_node1_test(test_node_sched)

# ---------------------------------------------------------------------------
# DBC 与代码的漂移检测 + cantools 解码级校验
# ---------------------------------------------------------------------------
if(Python3_Interpreter_FOUND)
    add_test(NAME dbc_generator_in_sync
             COMMAND "${Python3_EXECUTABLE}" "${REPO_ROOT}/tools/gen_dbc.py" --check)
    add_test(NAME dbc_decoder_matches_codec
             COMMAND "${Python3_EXECUTABLE}" "${REPO_ROOT}/tools/validate_dbc.py")
else()
    message(WARNING "Python3 not found: DBC consistency tests will be skipped")
endif()

add_custom_target(check
    COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure -C $<CONFIG>
    DEPENDS test_codec test_seq test_crc test_dbc_drift test_node_app test_node_sched
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    USES_TERMINAL
)
