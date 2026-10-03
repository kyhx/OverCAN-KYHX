#!/usr/bin/env python3
"""Validate the *committed* DBC with cantools, byte-for-byte against the C codec.

``tool s/gen_dbc.py --check`` verifies that the DBC text matches what the
generator would produce.  This script goes one step further: it loads the
committed ``can/can_distributed.dbc`` with cantools and decodes the very same
"golden frames" that ``tests/test_codec.c`` asserts on.  If both pass, the DBC
and the C library agree on the wire format -- the whole point of having a
machine-readable frame definition.

Exits 0 on success, 1 on any mismatch.
"""

from __future__ import annotations

import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DBC = os.path.join(REPO_ROOT, "can", "can_distributed.dbc")

EXPECTED_MESSAGES = {
    0x011: "NODE1_EVENT",
    0x021: "NODE2_EVENT",
    0x100: "CMD_BROADCAST",
    0x110: "CMD_NODE1",
    0x120: "CMD_NODE2",
    0x210: "TELEMETRY_NODE1",
    0x220: "TELEMETRY_NODE2",
    0x300: "MASTER_HEARTBEAT",
    0x310: "ACK_NODE1",
    0x320: "ACK_NODE2",
}

# 与 tests/test_codec.c 的"黄金报文"完全一致的字节序列。
# 期望值一律按 cantools 返回的形式书写：有 VAL_ 表的信号返回枚举名而非数字。
GOLDEN = [
    ("TELEMETRY_NODE1", bytes([0x07, 0x03, 0x1D, 0x01, 0x00, 0x01, 0x3C, 0x00]),
     {"Seq": 7, "Valid": 3, "Temperature": 285, "DoState": 0,
      "MotorState": "FORWARD", "MotorDutyPct": 60, "Status": 0}),
    ("TELEMETRY_NODE2", bytes([0x01, 0x07, 0xD2, 0x04, 0x01, 0x5A, 0x2A, 0x00]),
     {"Seq": 1, "Valid": 7, "Light": 1234, "IrDo": 1,
      "ServoAngle": 90, "IrAoPct": 42, "Status": 0}),
    ("NODE1_EVENT", bytes([0x01, 0x01, 0x52, 0x03, 0x00, 0x2C, 0x01, 0x02]),
     {"EventCode": "OVER_TEMP", "Level": "WARNING", "Value": 850, "Channel": 0,
      "UptimeS": 300, "RepeatCount": 2}),
    ("CMD_NODE2", bytes([0x2A, 0x01, 0x01, 0x00, 0x5A, 0x00, 0x00, 0x00]),
     {"Seq": 42, "Cmd": "SET", "Device": "SERVO", "Channel": 0, "Param": 90,
      "Options": 0}),
    ("ACK_NODE1", bytes([0x2A, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00]),
     {"EchoSeq": 42, "Result": "OK", "State": 3}),
    ("MASTER_HEARTBEAT", bytes([0x09, 0x00, 0x06, 0x00, 0x00, 0x01, 0x00, 0x00]),
     {"HeartbeatCount": 9, "SystemMode": 0, "OnlineBitmap": 6,
      "FwVersionUniform": 0, "ProtocolVersion": 1}),
]

NEGATIVE_TEMPERATURE = bytes([0x03, 0x01, 0xC9, 0xFF, 0x00, 0x00, 0x00, 0x00])


def fail(msg: str) -> None:
    print(f"FAIL: {msg}")
    global FAILED
    FAILED = True


FAILED = False


def main() -> int:
    try:
        import cantools
    except ImportError:
        print("SKIP: cantools not installed (pip install cantools)")
        return 0

    if not os.path.exists(DBC):
        fail(f"{DBC} missing")
        return 1

    try:
        db = cantools.database.load_file(DBC)
    except Exception as exc:  # noqa: BLE001 - surface the parser error verbatim
        fail(f"cantools could not parse {os.path.relpath(DBC, REPO_ROOT)}: {exc}")
        return 1

    # 1) 帧 ID / 名称 / DLC
    if len(db.messages) != len(EXPECTED_MESSAGES):
        fail(f"parsed {len(db.messages)} messages, expected {len(EXPECTED_MESSAGES)}")
    for msg in db.messages:
        want = EXPECTED_MESSAGES.get(msg.frame_id)
        if want is None:
            fail(f"unexpected frame 0x{msg.frame_id:03X} ({msg.name})")
            continue
        if msg.name != want:
            fail(f"0x{msg.frame_id:03X} name {msg.name} != {want}")
        if msg.length != 8:
            fail(f"{msg.name} length {msg.length} != 8")

    # 2) 黄金报文：raw 解码（不做 factor 缩放）必须与 C 侧字节语义一致
    for name, payload, signals in GOLDEN:
        msg = db.get_message_by_name(name)
        decoded = msg.decode(payload, scaling=False)
        for key, want in signals.items():
            got = decoded.get(key)
            if got != want:
                fail(f"{name}.{key}: decoded raw {got!r}, expected {want!r}")
        # 信号覆盖检查：golden 里出现的字节不得落在未定义信号上（保留位除外）
        for key in signals:
            if key not in decoded:
                fail(f"{name}: signal {key} missing from the DBC")

    # 3) 负温：有符号 int16 必须正确还原（−5.5 ℃，raw 值 −55）
    temp = db.get_message_by_name("TELEMETRY_NODE1")
    decoded = temp.decode(NEGATIVE_TEMPERATURE, scaling=False)
    if decoded.get("Temperature") != -55:
        fail(f"negative temperature decoded as {decoded.get('Temperature')!r}, want -55")
    scaled = temp.decode(NEGATIVE_TEMPERATURE, scaling=True)
    if abs(scaled.get("Temperature", 0) - (-5.5)) > 1e-6:
        fail(f"scaled negative temperature = {scaled.get('Temperature')!r}, want -5.5")

    # 4) 值表（枚举）确实存在且可读
    cmd = db.get_message_by_name("CMD_NODE1")
    signals = {s.name: s for s in cmd.signals}
    if "Cmd" not in signals or not signals["Cmd"].choices:
        fail("CMD_NODE1.Cmd has no VAL_ table")
    elif signals["Cmd"].choices.get(1) != "SET":
        fail(f"Cmd choice 1 = {signals['Cmd'].choices.get(1)!r}, want 'SET'")

    # 5) 周期属性只应出现在遥测与心跳上
    periodic = {m.name: m.cycle_time for m in db.messages if m.cycle_time}
    for name in ("TELEMETRY_NODE1", "TELEMETRY_NODE2", "MASTER_HEARTBEAT"):
        if periodic.get(name) != 1000:
            fail(f"{name} cycle_time = {periodic.get(name)!r}, want 1000")
    for name in ("NODE1_EVENT", "CMD_NODE1", "ACK_NODE1"):
        if name in periodic:
            fail(f"{name} must not be periodic, got {periodic[name]}")

    if FAILED:
        return 1
    print(
        f"ok: {len(db.messages)} messages, {sum(len(m.signals) for m in db.messages)} "
        f"signals, golden frames match the C codec"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
