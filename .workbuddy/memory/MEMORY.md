# 项目长期记忆

> 详细规格一律查文档，不在记忆里复述。本文件只存**决策、坑、易误判点**。
> 索引：`README.md` → `docs/项目文档.md`（v1.4 权威规格）/ `docs/protocol.md`（10 帧逐位表）/ `docs/技术方案与实施.md`（路线·OTA·安全·工具链·22 用例）/ `docs/求职项目陈述.md`（简历·25 面试题）。

## 定位

CAN 总线异构多控制器系统：ESP32-S3 核心机 + 2×STM32F103C8T6 节点，**求职作品集**用途，目标岗位嵌入式软件 / 车载·工业 CAN / IoT。

**实况**：已有协议库 + DBC + PC 单测（8/8 绿）+ 节点一固件**已接入真实 MCU 工程并可交叉编译出 ELF**。**仍无**：Bootloader/OTA、节点二/ESP32 固件实现、硬件台架、实测数据、原理图。所有指标证据等级仅"分析级"。

## ⭐ 仓库里有两套并存的"节点工程"，务必分清（2026-10-04 查明）

- **`CAN_Node1/`、`CAN_Node2/`**：用户原有的 **STM32CubeMX 生成工程**（CMake + Ninja + arm-none-eabi 工具链，`CMakePresets.json` 里 preset 名 `Debug`/`Release`）。最初是**空骨架**：`.ioc` 只配了 PD0/PD1 晶振 + PA13/PA14 SWD，`main.c` 主循环为空，**没有任何外设**。
- **`CAN_ESP32/hello_world/`**：ESP-IDF 官方 hello_world 模板，**未动**（不是本项目代码）。
- **`SimpleGUI-Stable/`**：与 CAN 项目**无关**的第三方 GUI 库（另有自己的 .workbuddy 记忆）。**不要动它**。
- **`firmware/`**：我们手写的跨平台层（协议库 + 节点业务/调度/BSP），**是唯一"零 HAL 可 PC 单测"的那一层**，现已被 CAN_Node1 的 CMake 引用并编入固件。
- ⚠️ **两套的关系 = 分层，不是重复**：CubeMX 工程负责"凑齐 HAL 驱动 + 配 72MHz 时钟 + 提供 `main()`"，`firmware/` 负责"协议、业务、调度、外设行为"。**引脚与位定时的唯一权威仍是我们自己的 BSP 与 `docs/引脚分配.md`，不是 .ioc**（.ioc 被 GUI 改一次就漂移）。

## CAN_Node1 接入固件的实操要点（勿重犯）

- **工程原本没有我们要用的 HAL 驱动源码**：CubeMX 只把"已配外设"的驱动拷进 `Drivers/`，缺 CAN/ADC/TIM/IWDG/UART。已从 `C:\Users\Administrator\STM32Cube\Repository\STM32Cube_FW_F1_V1.8.7` 补拷 `.c`/`.h`，并在 `Core/Inc/stm32f1xx_hal_conf.h` **末尾**（而非 CubeMX 会改写的那段）启用 `HAL_{ADC,CAN,IWDG,TIM,UART}_MODULE_ENABLED`。**每个模块都要连 `_ex` 变体一起拷**：`ADC` 需 `hal_adc_ex`（校准在里面），`TIM` 的 `hal_tim.h` 会 `#include "stm32f1xx_hal_tim_ex.h"`，漏了直接编译失败。
- **F1 的 HAL API 与 F4 不同，别凭印象写**：F1 **没有** `HAL_ADC_Enable/Disable`（校准由 `HAL_ADCEx_Calibration_Start()` 内部完成 Enable），也**没有** `HAL_TIM_SetCompare`（要用宏 `__HAL_TIM_SET_COMPARE`）。这类错误只表现为 warning + 链接失败。
- **两个"静默失效"陷阱（都会编译通过、烧进去却不工作）**：
  1. `SysTick_Handler` **不能**在 BSP 里定义（会与 CubeMX 的 `stm32f1xx_it.c` 重复；链接顺序不好时 `HAL_IncTick` 不再被调用，`HAL_Delay/HAL_GetTick` 全部失效）。正确做法：在 it.c 的 `USER CODE BEGIN SysTick_IRQn 1` 里调 `node1_bsp_tick_ms()`。
  2. **`USB_LP_CAN1_RX0_IRQHandler` 必须手工在 it.c 里实现**（启动文件里是 weak 空实现、链接不报错），里面调 `HAL_CAN_IRQHandler(&hcan1)`，再由 `HAL_CAN_RxFifo0MsgPendingCallback()` 转 `node1_can_rx_isr_handler()`。三步缺一 → 一帧都收不到。`hcan1` 因此**不能是 static**。
- **`main()` 归 CubeMX**：`firmware/.../node1_main.c` 不再定义 `main`，改为导出不返回的 `node1_firmware_run()`，由 `Core/Src/main.c` 的 `USER CODE BEGIN 2` 段调用。这样 CubeMX 重新生成也冲不掉我们的逻辑。
- **构建命令**（ninja 不在 PATH，需先加）：
  `$env:PATH = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;" + $env:PATH` 然后 `cmake --preset Debug; cmake --build --preset Debug`。
- **当前基线**：`CAN_Node1` **零告警链接成功**，RAM 2544 B / 20 KB（12.4%）、FLASH 26560 B / 64 KB（40.5%）；已核实 `node1_firmware_run`/`HAL_CAN_IRQHandler`/`HAL_CAN_RxFifo0MsgPendingCallback`/`USB_LP_CAN1_RX0_IRQHandler`/`proto_rx_seq_handle` 等关键符号确实在 ELF 里。
- **未做**：节点一**从未上板**（PWM/CAN/看门狗只有编译与逻辑单测）；`CAN_Node2` 仅补齐了 HAL 驱动与启用项，**尚无 BSP/业务代码**，因此还没接入固件。

**推送阻塞（2026-10-03 未解决，处置办法记此）**：`origin` = `https://github.com/kyhx/OverCAN-KYHX.git`（远程仍为空仓库），本地提交齐全（`ea35135` 基线、`e60b9db` 节点一骨架）。**`git push` 被网络层重置**：HTTPS 读操作（`ls-remote`）可用但 push 的 POST 被拦——换可达 IP（140.82.112/113/114.3、20.27.177.113，TCP 均通）、`http.curloptResolve` 钉 IP、`http.version HTTP/1.1`、查本地代理**全部无效**。**唯一可行 = SSH**：`ssh.github.com:443` 与 `github.com:22` 均可达，`ssh -T -p 443 git@ssh.github.com` 返回 `Permission denied (publickey)`（服务器正常，仅缺密钥；`~/.ssh` 不存在）。恢复办法：① 生成 ed25519 → 公钥加 GitHub → `git remote set-url origin git@ssh.github.com:kyhx/OverCAN-KYHX.git` + `git config core.sshCommand "ssh -p 443"` → push；② 挂代理后 `git config http.proxy <代理>`；③ 换网络直接 `git push -u origin main`。

## ⭐ CubeMX 重新生成会吃掉我们的外设（2026-10-04 实际踩到，务必牢记）

**现象**：用户在 CubeMX 里点"重新生成代码"后，`CAN_Node1` 构建崩掉，报满屏
`unknown type name 'CAN_HandleTypeDef' / 'TIM_HandleTypeDef' / 'IWDG_HandleTypeDef'`。

**根因链**（三处被 CubeMX 按 `.ioc` 重写，且它不认识我们的外设）：
1. `.ioc` 里没有 CAN1/TIM4/IWDG（我们的外设初始化写在端口层 `node1_bsp.c`，不走 `MX_xxx_Init()`，
   所以 CubeMX 侧永远看不到它们）→ 重新生成后这三项的配置被删，`PB6/PB7` 引脚也从引脚表消失。
2. `Core/Inc/stm32f1xx_hal_conf.h` 整份重写 → `HAL_CAN/TIM/IWDG_MODULE_ENABLED` 被重新注释掉。
3. `cmake/stm32cubemx/CMakeLists.txt` 整份重写 → CAN/TIM/IWDG 的驱动源文件从构建里消失；
   `Drivers/` 里对应的 `.c/.h` 也被 CubeMX 删除。

**结论：补丁绝不要放进这三个文件。** 正确的落脚点是 CubeMX 明确保证不改的地方：
- **`<项目>/CMakeLists.txt`（顶层，CubeMX 注明"只生成一次，用户可自由修改"）**
  → 在这里 ① 用 `target_sources` 补 HAL 驱动源文件；② 用
  `target_compile_definitions` 定义 `HAL_CAN/TIM/IWDG_MODULE_ENABLED`。
- **`Drivers/STM32F1xx_HAL_Driver/{Src,Inc}`** → 驱动源码仍需从官方包手工拷回
  （CubeMX 重新生成会删），所以顶层 CMakeLists 里加了 `foreach + EXISTS`，**缺文件直接 FATAL_ERROR**
  并打印补拷来源，避免退化成一堆 `undefined reference`。

**⭐ 为什么启用项必须定义在命令行（这是最隐蔽的一环）**：HAL 驱动源码
（`stm32f1xx_hal_can.c` 等）**整个文件体**被 `#ifdef HAL_CAN_MODULE_ENABLED` 包着，
而这些 `.c` **不会**包含我们自己的头文件。若只把启用项写在某个头里，
这些 `.c` 会被编成**空目标文件**（里面只有调试信息、没有任何代码），
而链接命令里**明明有这些 obj** → 报 `undefined reference to HAL_CAN_Init` 时极难定位。
只有"定义在编译命令行上"才能同时覆盖 HAL 驱动源码与我们自己的源文件。

**Node2 的额外坑**：节点二的 `.ioc` 没启用 DMA，而 `stm32f1xx_hal_adc.h` / `hal_tim.h`
引用 `DMA_HandleTypeDef`（由 `hal_dma.h` 提供）→ 只开 TIM/ADC 会报
`unknown type name 'DMA_HandleTypeDef'`。节点一恰好启用了 DMA 所以没事。

**另一个自作自受的教训**：用 PowerShell `-replace` 改 `hal_conf.h` 时，
正则把**注释里**的 `#define HAL_RCC_MODULE_ENABLED` 也匹配了，把启用块插到了文件中间、
并吃掉了标准启用块（RCC/GPIO/CORTEX/PWR/FLASH/EXTI 全失效）。**修这种文件要用 edit 工具精确替换，
或干脆从可用副本复制**（本次最终用 Node1 的 `hal_conf.h` 覆盖修复了 Node2——两者 MCU/时钟相同）。

**当前状态**：`CAN_Node1` 零告警（RAM 2664 B / FLASH 27596 B，42.1% PDF）。
`CAN_Node2` 零告警（还是空骨架：RAM 1584 B / FLASH 3644 B，未接入固件）。
两者都在顶层 CMakeLists 里带上了上述补丁与文件存在性检查。



## 工程机制（勿破坏）

- **分层（节点固件已落地，节点二/ESP32 照抄）**：`node1_main.c → node1_sched.c`（RTOS 无关）`→ node1_app.c`（**零 HAL**）`→ proto_* → node1_hal_t` **vtable** → `port/stm32f103/node1_bsp.c` 或 PC mock。用**函数指针 vtable** 而非 extern 做依赖注入，mock 只需换指针即可断言硬件动作。调度逻辑独立成纯逻辑 → 时序可 PC 单测，FreeRTOS 化只换调用点。
- `node1_config.h` = 引脚与可调参数的**单一来源**，驱动代码不含魔数。温标系数为**占位值待实测标定**。
- **烧板前必跑 `bash tools/check_portable.sh`**：零 HAL 层在 arm-none-eabi-gcc(Cortex-M3) + PC 上 `-Wpedantic -Werror` 零告警。**PC 全绿 ≠ 能烧板**（`PROTO_STATIC_ASSERT` 单层宏拼接就曾在 PC 全绿、GCC+C99 才炸）。
- ⚠️ **本机环境坑**：`HTTP_PROXY` 与 `http_proxy`（及 HTTPS 大小写两对）并存会让 **MSBuild 崩溃**（MSB6001 "CL.exe 命令行开关无效"，实为 Hashtable 重复键）。编译前 `unset http_proxy https_proxy`。
- **单一定义源 = `proto_id.h` 的 `PROTO_FRAME_TABLE`**。DBC 由 `tools/gen_dbc.py` 生成，`test_dbc_drift.c` 每次构建比对，`validate_dbc.py` 用 cantools 与 C 库对拍。**改协议必须 `python tools/gen_dbc.py --write`，绝不可手改 DBC。**
- 帧表解析边界 = `PROTO_FRAME_TABLE_BEGIN/END` 标记；其附近注释**不得出现 `{ 0x…, "…" }` 形状的字面示例**（生成器与 C 解析器都会误当表项，已踩过）。
- `tests/harness/utest.h` 自注册（MSVC `.CRT$XCU` / GCC `constructor`）。新用例只写 `UTEST_CASE(名){}`，**不维护用例列表**（旧 `UTEST_SUITE_FUNCS` 已移除）。
- 三处"黄金报文"须一致：`test_codec.c` / `test_dbc_drift.c` / `validate_dbc.py`。
- **构建**：`cmake -S . -B build && cmake --build build --config Debug && ctest --test-dir build -C Debug`。MSVC **必须 `/utf-8`**（中文注释否则 C4819）。
- ⚠️ **增量编译不可靠**：改完头文件测试仍报旧错 → 删 `build/` 重配。2026-10-03 两次踩到（`test_dbc_drift` 假失败、`node1_app.c` 改动未生效）。**"改了没生效"先怀疑增量编译，再核对测试期望值本身是否写错**（该轮有 4 处是期望值写错而非代码错）。
- ⚠️ **写时序测试必须保证时间单向推进**（两次 `run_until(0→A)` + `run_until(0→B)` 会让 B 段跑两遍）；**越界帧不能靠编码器构造**（编码器先拒），须手搓字节。
- cantools：有 `VAL_` 表的信号 `decode()` 返枚举名字符串；factor≠1 的 float 信号在 `scaling=False` 下返原始整数；位域信号别建逐位 VAL_ 表。
- ⚠️ 判断"文件是否存在"要用 `Test-Path` / `ls -a`，**不能靠 glob 结果做否定结论**（glob 不返回隐藏目录，差点误判"无 git 仓库"）。

## 引脚分配（2026-10-03 定稿并已核对官方文档）

- **落地手册 = `docs/引脚分配.md`**（三块板全量分配 + 未分配引脚清单 + 风险表 R-1~R-11 + 上电前核对清单 + §9 已核实事实含引用）。配置头：`firmware/node1/include/node1_config.h`、`firmware/node2/include/node2_config.h`。**引脚变更必须三处同步（本手册 / 项目文档 §4 / 配置头）**。
- ⭐ **节点一电机 PWM 已从 TIM1 的 PA8/PA9 改为 TIM4 的 PB6/PB7**（`node1_config.h` + `node1_bsp.c` 已改）。理由：① PA9 兼作 USART1_TX，占用后节点一失去唯一调试串口；② TIM1 是高级定时器，**不调 `HAL_TIM_CtrlPWMOutputs` 就没有波形**。改后 **PA9/PA10 作 USART1 调试口**（115200），且 BSP 里已加真实 `uart_log_init()`（NODE1_UART_LOG_ENABLE=1）。**切勿改回 TIM1**，除非同时补上 MOE 那一行。
- **节点二调试从设计上就是 SEGGER RTT**：PA2/PA3 被红外 ADC/DO 占用，**腾不出任何 UART**（与节点一不同，节点一能靠挪 PWM 腾出）。舵机在 TIM3_CH1/PA6。
- ⭐ **已核实的硬事实（勿凭印象推翻，依据见 `docs/引脚分配.md` §9）**：
  - ESP32-S3 的 **strapping 引脚只有 GPIO0 / GPIO3 / GPIO45 / GPIO46**；**GPIO8、GPIO9 不是**（早期误判过，易与 ESP32-C3 混淆）。GPIO46 兼启动模式与 ROM 日志、GPIO45 定 VDD_SPI 电压、GPIO3 仅在烧 `STRAP_JTAG_SEL` eFuse 后才作 JTAG 源选择。
  - **GPIO47/48 只有型号带 "V"（N16R16V）才是 1.8V 域**；本项目 **N16R8 是 3.3V 域**。GPIO47 非 strapping、无上电毛刺，但**复位后是输入使能无上下拉（悬空）→ 按键必须使能内部上拉**。
  - **STM32F103C8T6 的 CAN1 在 LQFP48 上只有两组**：PA11/PA12（默认）或 **PB8/PB9**（`CAN_REMAP=10`）；**PD0/PD1 在 48 脚封装不存在**，RM0008 明文禁止 36/48/64 脚封装做 Port D 的 CAN 重映射。
  - **PA15/PB3/PB4 释放为 GPIO**：开 AFIO 时钟 → `AFIO_MAPR` 的 `SWJ_CFG=010`（关 JTAG-DP 留 SW-DP）→ 再配 GPIO，**SWD 仍可用**；`SWJ_CFG` 是**只写位，禁止读-改-写**。
  - **PA0~PA7/PB0/PB1 这 10 个 ADC 脚不是 5V 容忍**（DS5319 中未标 FT）= Standard I/O，上限 VDD+0.3V/绝对最大 4.0V，**模拟与数字模式同属引脚级限制**。LQFP48 恰好只有 ADC1_IN0~IN9。
  - LQFP48 GPIO 总数 = PA0~15 + PB0~15 + PC13~15 = **35**（PC13/14/15 灌电流很弱）。
- **`tools/check_portable.sh` 已扩展**：新增"板级配置头自洽性"检查（用一个临时 TU 同时 include node1/node2 配置头，强制触发里面的 `#error`）——因为**没人包含的头文件里的 `#error` 等于没写**。已验证该检查能真的抓到越界（把舵机角度改成 200 会 FAIL）。跑法：`& 'F:\Program Files\Git\bin\bash.exe' tools/check_portable.sh`（bash 不在 PATH）。
- **厂商 PDF 不入库**：`docs/*.pdf`、`docs/*.txt`（rm0008 12.5MB、ds5319 1.9MB）已加进 `.gitignore`。

## 已定决策（勿凭记忆推翻）

- **帧 ID = `类别(3)|节点号(4)|帧类型(4)`**，升序 = 紧急度降序。10 帧：事件 0x011/0x021、命令 0x100(广播)/0x110/0x120、遥测 0x210/0x220、心跳 0x300、ACK 0x310/0x320；扩展节点三占 0x031/0x130/0x230/0x330。
- **幂等两条铁律**：① **seq 按通道分离**（广播与单播各一个 last_seq；共用会因 8 位回绕撞号 → 节点只回 ACK 不执行）；② **`seq_valid` 标志**（上电/复位/OTA 后置 false，首命令无条件执行后才启用重复判定）。
- **TJA1050 ×3 保留**（5V 器件配 3.3V MCU）：RX 串 1kΩ 限流靠 ESD 钳位，不达标再补 2kΩ 对地分压；TX 直连 3.3V 属超规格，**逐片实测**（典型阈值 2.0~2.5V 通常可用）。不稳单路换 3.3V 收发器。
- **⭐ N16R8 的 GPIO33~37 被 Octal PSRAM 占用；N8R2 是 Quad → 该组引脚空闲**。本项目无大词汇量语音模型，2MB PSRAM 够用 —— 选 N8R2 引脚规划宽松很多。**切勿买 WROOM-2（N32R16V 类）**：1.8V 域，IO47/48 只输出 1.8V。
- **供电必须分轨 5V/5A**（动力峰值 3.4A，2A 不够）+ 肖特基隔离 + 共地。
- **W25Q64 = 节点固件仓库**；内部 littlefs(~7.5MB) = 仅日志；ESP32 自 OTA 直接写 ota_1。
- 节点 Flash：**推荐 STM32F103CBT6（128KB）**方案 B —— C8T6 的 64KB 做 Boot+A/B+标志会把 App 压到 24KB 太紧。
- OTA 耗时**由 flash 特性决定，不由总线带宽**（64KB：CAN 传输 ~1.05s vs flash 擦写 ~3s；提到 1Mbps 只省 ~0.5s）。预期实测 4~8s。
- **bxCAN 14 组硬件滤波器** = 安全建模"ID 白名单"的零成本硬件落地（节点只收 0x100/0x110(或0x120)/0x300）。Boot 与 App 滤波配置不同。
- **调试用 SEGGER RTT**（走 SWD，OpenOCD 支持，不需 J-Link）——解决节点一无 USART1、节点二 PA2/PA3 被占。
- 勿从零造：CANopen 用 **CANopenNode**、差分 OTA 用 **esp_delta_ota**、PC 侧 can-utils/SavvyCAN/cantools/python-can。

## 已被测试抓出的 4 个真实缺陷（面试可讲"被测试抓出来"）

1. **CRC-32 用了非反射多项式** `0x04C11DB7` 右移 → 非标准值；须用反射形式 **`0xEDB88320`**。由 `"123456789"` 标准值 0xCBF43926 抓出。
2. **`node < PROTO_NODE_MIN` 整型提升**：`uint8_t` 与枚举常量比较提升为 int，`node=0` 被误判越界 → 主站帧判非法。须显式 `(unsigned)` 比较。
3. **心跳与 ACK 共用 `HEARTBEAT_ACK` 类别**：整类套用"必须来自从节点"会把**主站自己的心跳 0x300 判非法**。须按帧语义分别判断。
4. **事件风暴抑制跨窗口累加**：窗口到期放行时未清零 `repeat_count`，"距上次上报的重复次数"语义不成立。

## 求职叙事红线

- 主方向：**分布式 CAN 控制节点**；小车/家居仅作"可拓展形态"佐证。
- **不得宣称**：离线完整语音识别（硬件只能录音播放）、电机 PID（无编码器）、堵转电流检测（无采样器件）。
- 所有 `[ ]` 占位符必须换成实测值才可投递（清单见 `docs/求职项目陈述.md`）。
- **最佳故事 = T-18「擦 flash 期间不丢帧」**：现象→分析→验证→对策→实测链条完整，是"跑通过"与"想清楚过"的分界线，面试官几乎必追问。

## 下一步（规划已饱和，转产出）

① 节点一固件骨架（阶段 1）② 分区表 CSV + Bootloader 链接脚本 ③ ADR 文档（0001 为何选 CANopen / 0002 为何 OTA 需双区 / 0003 为何不用 C 位域）④ 9 张专业图表 ⑤ GitHub 仓库骨架。