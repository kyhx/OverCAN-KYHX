# CAN 分布式控制节点系统

> 基于 **CAN 总线**的异构多控制器嵌入式系统：ESP32-S3 作为核心机负责决策与人机交互，两片 STM32F103 作为分布式节点负责执行与感知，构成"感知—决策—执行"闭环，并支持 **CAN 固件升级（IAP）与网关式 FOTA**。

**技术栈**：ESP32-S3（ESP-IDF / FreeRTOS）· STM32F103（HAL / FreeRTOS）· CAN 2.0B · I2S 音频 · SPI Flash 日志与固件仓库
**目标方向**：嵌入式软件 · 车载 / 工业 CAN 通信 · IoT 终端

---

## 系统架构

```
                    ┌──────────────────────────────┐
                    │  核心机（总机）ESP32-S3       │
                    │  决策 / 交互 / 记录 / 语音    │
                    │  内置 TWAI 控制器             │
                    └──────────────┬───────────────┘
                                   │
                        CAN 总线 500 kbps
                     CANH / CANL 差分 ×3
                  两端各 120Ω（建议分裂终端）
                                   │
              ┌────────────────────┴────────────────────┐
              │                                         │
   ┌──────────▼──────────┐                  ┌───────────▼─────────┐
   │  节点一 STM32F103    │                  │  节点二 STM32F103    │
   │  内置 bxCAN          │                  │  内置 bxCAN          │
   │  温度 / 电机 / 蜂鸣   │                  │  光照 / 舵机 / 红外   │
   └─────────────────────┘                  └─────────────────────┘
```

| 板卡 | 核心器件 | 职责 |
| --- | --- | --- |
| 核心机 | ESP32-S3-WROOM-1-N16R8 | 决策调度、OLED/编码器/按键交互、W25Q64 日志、I2S 语音链路、MPU6050 姿态 |
| 节点一 | STM32F103C8T6 | 温度采集、TB6612 电机驱动、蜂鸣报警 |
| 节点二 | STM32F103C8T6 | 光照采集、SG90 舵机、红外检测 |

**通信协议**：11 位标准帧，ID = `类别(3) | 节点号(4) | 帧类型(4)`，按紧急度排序仲裁优先级（事件 > 命令 > 遥测 > 心跳/ACK）。详见 [项目文档.md](docs/项目文档.md) §5。

---

## ⚠️ 当前状态

> **项目处于"规格冻结 + 协议层代码起步"阶段：协议规范、DBC 与 PC 单元测试已完成并通过；固件主体、硬件台架与实测数据尚未开始。**
> 文档中所有量化指标的证据等级仍为"**分析**"，即仅经推算，未经代码或实测验证。

| 项 | 状态 |
| --- | --- |
| 设计规格（硬件 / 引脚 / 协议 / 指标） | ✅ 已完成 |
| 技术方案与实施路线 | ✅ 已完成 |
| 协议规范独立成文 + DBC | ✅ 已完成（[protocol.md](docs/protocol.md) · [can_distributed.dbc](can/can_distributed.dbc)） |
| 协议库（纯 C / 零 HAL）+ PC 单元测试 | ✅ 已完成（`ctest`） |
| **节点一业务逻辑（命令 / 遥测 / 事件 / 安全态）** | ✅ 已完成，**8/8 测试通过**（23 + 9 个用例） |
| 节点一 BSP（bxCAN / ADC / TIM1 / GPIO / IWDG） | 🟡 代码完成、**交叉编译通过**，**未上板验证** |
| 节点二固件 | ❌ 未开始 |
| Bootloader / OTA 实现 | ❌ 未开始 |
| 硬件台架与实测 | ❌ 未开始 |

> ⚠️ **节点一无实测数据**：温标系数（`node1_config.h`）为占位值，必须实测标定；
> PWM 频率、CAN 位定时、掉线安全态均**仅经交叉编译与逻辑单测验证，未经示波器/分析仪确认**。

---

## 文档索引

| 文档 | 内容 | 读者 |
| --- | --- | --- |
| [项目文档.md](docs/项目文档.md) | **唯一权威设计规格**：硬件清单、通信架构、引脚分配、CAN 协议（10 个帧全部定义）、物理层与位定时、RTOS 任务划分、量化指标、工程约束、可行性风险、路线图 | 所有 |
| [protocol.md](docs/protocol.md) | **应用层协议规范 v1.0**：3+4+4 帧 ID 规划、10 帧逐位字段表、幂等状态机、重传/风暴抑制/安全态、错误码、兼容性策略。机器可读定义源为 [proto_id.h](firmware/common/protocol/include/proto_id.h) | 实现者 / 对接方 |
| [技术方案与实施.md](docs/技术方案与实施.md) | 技术提升路线（P0/P1/P2）、代码骨架与架构约定、**OTA 四条升级链路**、CAN 安全威胁建模、工具链与采购防坑、22 条验收测试用例、参考资料 | 实施者 |
| [求职项目陈述.md](docs/求职项目陈述.md) | 岗位匹配、简历 bullet、主方向叙事、**25 道定制面试题（含答案骨架）**、材料清单、叙事风险 | 求职用 |

**代码与工程产物**：

| 路径 | 内容 |
| --- | --- |
| [can/can_distributed.dbc](can/can_distributed.dbc) | CAN 数据库（10 消息 / 55 信号，cantools 可解析），**由帧表自动生成**，与代码一致性由测试守护 |
| [firmware/common/protocol/](firmware/common/protocol) | 协议库：纯 C99、**零 HAL 依赖**、无 malloc、无 C 位域——ESP32 与 STM32 共用同一份实现 |
| [firmware/node1/](firmware/node1) | 节点一固件：`include/`（配置 + HAL vtable + 业务接口）· `src/`（业务逻辑 + 调度，**零 HAL**）· `port/stm32f103/`（唯一接触寄存器的地方） |
| [tests/](tests) | 6 个 PC 单元测试套件（协议编解码 / 幂等 / CRC / DBC 漂移 / **节点业务** / **调度时序**），零外部框架依赖 |
| [tools/gen_dbc.py](tools/gen_dbc.py) · [tools/validate_dbc.py](tools/validate_dbc.py) | DBC 生成器与 cantools 解码级校验器 |
| [tools/check_portable.sh](tools/check_portable.sh) | 可移植性检查：零 HAL 层在 Cortex-M3 与 PC 上 `-Wpedantic -Werror` 零告警 |
| [CMakeLists.txt](CMakeLists.txt) | 构建入口：`cmake -S . -B build && cmake --build build --target check` |

### 节点一的分层：为什么业务逻辑能测

```
node1_main.c ──► node1_sched.c ──► node1_app.c ──► proto_*（协议库）
   port层          纯逻辑(RTOS无关)   纯逻辑(零HAL)      纯 C99
                                      │
                                      ▼
                              node1_hal_t vtable
                                      │
                    ┌─────────────────┴─────────────────┐
              node1_bsp.c (STM32)                   mock (PC 单测)
```

**协议库之上再包一层"零 HAL"是本阶段的关键决策**：它让"没有硬件台架时逻辑是否正确"
成为可回答的问题。23 个业务用例 + 9 个调度用例覆盖幂等、越界拒收、掉线安全态、
温标换算、时基回绕等路径 —— 全部在 PC 上跑，无需一块板子。

---

## 实施路线概览

| 阶段 | 周期 | 交付物 | 验收标准 |
| --- | --- | --- | --- |
| 1. 单节点打通 | 1 周 | 节点一固件 | ADC 误差 < 1 ℃；PWM 示波器可测 |
| 2. 点对点 CAN | 1 周 | 两板互通 + 位定时 87.5% + 擦 flash 丢帧先行实验 | 连续 30 min 无错误帧；丢帧率入库 |
| 3. 协议层 | 2~3 周 | 协议库 + DBC + PC 单元测试 | 10/10 帧有定义；CI 绿 |
| 4. 主站接入 | 1~2 周 | 心跳、掉线检测、ACK+重传+幂等 | 1 h 浸泡 0 误报 |
| 5. 供电与执行机构 | 1 周 | 分轨供电 + 电机/舵机 | 堵转时 MCU 不复位 |
| 6. 闭环控制 | 2 周 | 编码器 + 速度环 PID | 阶跃响应指标入库 |
| 7. 固件升级 | 2~3 周 | CAN Bootloader + OTA | 掉电 10 次不变砖 |
| 8. 健壮性与实测 | 1~2 周 | 故障注入 + 指标回填 | 全部指标替换为实测值 |

**合计约 12~16 周**（业余时间）。

---

## 快速开始（协议库 + 测试）

协议库不依赖任何 HAL，因此可以在 PC 上直接编译并跑测试：

```bash
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure    # 或：cmake --build build --target check
```

8 个测试目标：`test_codec`（黄金报文字节 / 负值 / 越界拒收）、`test_seq`（幂等与重传，钉死两个历史缺陷）、
`test_crc`（标准校验值 + 流式等价）、`test_dbc_drift`（DBC 与代码漂移）、
`test_node_app`（节点一业务：命令执行 / 幂等 / 遥测 / 事件 / 掉线安全态）、
`test_node_sched`（调度时序：周期计数 / 时基回绕 / 不补跑）、
`dbc_generator_in_sync`、`dbc_decoder_matches_codec`（cantools 解码对拍）。

**烧板前请再跑一次可移植性检查**：

```bash
bash tools/check_portable.sh     # 零 HAL 层在 Cortex-M3 + PC 上 -Werror 零告警
```

这一步不是形式主义：协议库的 `PROTO_STATIC_ASSERT` 宏就曾在 PC（MSVC/C11）下全绿，
却因单层宏拼接在 GCC + C99 + `-Wpedantic` 下报 *redefinition of typedef* ——
**换编译器才炸**的典型。详见 `tools/check_portable.sh` 头部说明。

**改动协议的正确姿势**：只改 [proto_id.h](firmware/common/protocol/include/proto_id.h) 的帧表与 `proto_codec.h` 的字段，
然后 `python tools/gen_dbc.py --write` 重新生成 DBC——**不要手改 DBC**，否则漂移测试会失败。

---

## 开始之前必读

**五个 P0 阻塞项**（详见 [项目文档.md](docs/项目文档.md) §11.2）：

1. CAN 收发器电平 —— **在用 TJA1050 ×3（5V 器件）**，已确定保留并做电平处理：RX 先串 1kΩ 限流（不达标再补 2kΩ 分压）+ TX 逐片实测（详见 §9.1）；后续可换 SN65HVD230 / MCP2562
2. N16R8 的 GPIO33~37 被 Octal PSRAM 占用（**若改用 N8R2 等 Quad PSRAM 型号则可释放**）
3. 舵机/电机堵转合计 ≥3.1 A，必须分轨独立供电（**动力电源建议 5V/5A，2A 不够**）
4. STM32 的 ADC 引脚不支持 5 V —— AO 在 5V 供电下**必须分压**
5. F103 无双 bank、仅 20 KB SRAM —— 固件升级须用 A/B 双区 + 流式写入

**采购前请先读** [技术方案与实施.md](docs/技术方案与实施.md) §6.3 采购防坑清单（PSRAM 模式、WROOM-2 电压域、SH1106、MPU6050 翻新、CAN 接线等）。

---

## 边界声明（明确不做什么）

- ❌ **不做离线完整语音识别** —— 现有硬件只能录音与播放；如需"听懂"须额外跑唤醒词/命令词模型
- ❌ **不做 CAN-FD** —— ESP32-S3 的 TWAI 仅支持 Classic CAN；如需 FD 须外挂控制器
- ❌ **不做功能安全认证 / EMC 认证** —— 本项目为台架原型，非车规或工业认证产品
- ❌ **不做 SLAM / 视觉导航** —— 与设计主线无关

---

## 硬件状态

硬件为现成模块搭建（面包板 / 洞洞板），无自研 PCB。硬件缺口清单（电流采样、编码器、RTC、USB-CAN 分析仪、终端电阻、SWD 排针等）见 [项目文档.md](docs/项目文档.md) §11.3。

**全系统成本估算**：约 ¥310~560（不含 PCB 打样与仪器），仪器预算约 ¥250~450。
