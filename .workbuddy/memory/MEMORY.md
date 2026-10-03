# 项目长期记忆

> 详细规格一律查文档，不在记忆里复述。本文件只存**决策、坑、易误判点**。
> 索引：`README.md` → `docs/项目文档.md`（v1.4 权威规格）/ `docs/protocol.md`（10 帧逐位表）/ `docs/技术方案与实施.md`（路线·OTA·安全·工具链·22 用例）/ `docs/求职项目陈述.md`（简历·25 面试题）。

## 定位

CAN 总线异构多控制器系统：ESP32-S3 核心机 + 2×STM32F103C8T6 节点，**求职作品集**用途，目标岗位嵌入式软件 / 车载·工业 CAN / IoT。

**实况**：已有协议库 + DBC + PC 单测（6/6 绿，已提交）。**仍无**：固件主体、Bootloader/OTA、硬件台架、实测数据、原理图。所有指标证据等级仅"分析级"。

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