#!/usr/bin/env python3
"""Generate can/can_distributed.dbc from the protocol's single source of truth.

The frame table (id / name / direction / DLC / cycle) is parsed out of
``firmware/common/protocol/include/proto_id.h``; the per-byte signal layout
lives in :data:`SIGNALS` below and mirrors ``docs/protocol.md`` section 5.

Usage:
    python tools/gen_dbc.py --check     # validate committed DBC (CI / ctest)
    python tools/gen_dbc.py --write     # regenerate the DBC after a table change
    python tools/gen_dbc.py --stdout    # print without touching files

``--check`` also parses the DBC with cantools when available, so a malformed
DBC fails the build instead of silently shipping.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(
    REPO_ROOT, "firmware", "common", "protocol", "include", "proto_id.h"
)
DBC = os.path.join(REPO_ROOT, "can", "can_distributed.dbc")

BAUDRATE = 500_000
PROTOCOL_VERSION = 1

# --------------------------------------------------------------------------- #
# 信号定义：{消息名: [(信号名, 起始位, 位宽, 有无符号, 类型, 单位, 注释), ...]}
# 起始位 = 小端 Intel 布局下的最低有效位所在位号（bit 0 = 数据场 byte0 的 bit0）
# --------------------------------------------------------------------------- #
UNSIGNED, SIGNED, BITFIELD = "u", "s", "b"

SIGNALS: dict[str, list[tuple]] = {
    "TELEMETRY_NODE1": [
        ("Seq", 0, 8, UNSIGNED, "", "telemetry sequence; gap = dropped frame"),
        ("Valid", 8, 8, BITFIELD, "",
         "bit0=temperature valid, bit1=DO valid, bit2=current valid (needs hardware)"),
        ("Temperature", 16, 16, SIGNED, "degC",
         "signed; factor 0.1 -> raw 285 means 28.5 degC", 0.1),
        ("DoState", 32, 8, UNSIGNED, "", "thermistor digital output"),
        ("MotorState", 40, 8, UNSIGNED, "", "0=stop 1=forward 2=reverse 3=brake"),
        ("MotorDutyPct", 48, 8, UNSIGNED, "%", "current PWM duty 0..100"),
        ("Status", 56, 8, BITFIELD, "",
         "bit0=stall (needs current sampling), bit1=over-temp, bit2=sensor fault"),
    ],
    "TELEMETRY_NODE2": [
        ("Seq", 0, 8, UNSIGNED, "", "telemetry sequence"),
        ("Valid", 8, 8, BITFIELD, "",
         "bit0=light valid, bit1=IR DO valid, bit2=IR AO valid"),
        ("Light", 16, 16, UNSIGNED, "lux", "illuminance in lux (BH1750 recommended)"),
        ("IrDo", 32, 8, UNSIGNED, "", "IR digital output"),
        ("ServoAngle", 40, 8, UNSIGNED, "deg", "0..180, out of range is rejected"),
        ("IrAoPct", 48, 8, UNSIGNED, "%", "IR analog normalised 0..100"),
        ("Status", 56, 8, BITFIELD, "", "reserved / undefined until hardware is fixed"),
    ],
    "NODE1_EVENT": [
        ("EventCode", 0, 8, UNSIGNED, "", "see VAL_ table"),
        ("Level", 8, 8, UNSIGNED, "", "0=info 1=warning 2=critical"),
        ("Value", 16, 16, SIGNED, "",
         "trigger value; unit depends on EventCode (0.1 degC / lux / mA)"),
        ("Channel", 32, 8, UNSIGNED, "", "related channel; 0 = not applicable"),
        ("UptimeS", 40, 16, UNSIGNED, "s",
         "node uptime low 16 bits, for event ordering (not wall-clock)"),
        ("RepeatCount", 56, 8, UNSIGNED, "",
         "repeats since last report; saturates at 255 (storm suppression)"),
    ],
    "NODE2_EVENT": None,  # same layout as NODE1_EVENT
    "CMD_BROADCAST": [
        ("Seq", 0, 8, UNSIGNED, "",
         "command sequence; per-channel counter (broadcast vs unicast)"),
        ("Cmd", 8, 8, UNSIGNED, "", "see VAL_ table"),
        ("Device", 16, 8, UNSIGNED, "", "0=motor 1=servo 2=buzzer 3=system"),
        ("Channel", 24, 8, UNSIGNED, "", "channel / sub-index, meaning by Device"),
        ("Param", 32, 16, SIGNED, "",
         "motor/buzzer 0..1000, servo 0..180; out of range is rejected"),
        ("Options", 48, 8, BITFIELD, "", "reserved, send 0"),
    ],
    "CMD_NODE1": None,
    "CMD_NODE2": None,
    "ACK_NODE1": [
        ("EchoSeq", 0, 8, UNSIGNED, "", "echoes the command Seq"),
        ("Result", 8, 8, UNSIGNED, "", "0=success, otherwise protocol error code"),
        ("State", 16, 8, UNSIGNED, "", "device state after execution"),
    ],
    "ACK_NODE2": None,
    "MASTER_HEARTBEAT": [
        ("HeartbeatCount", 0, 8, UNSIGNED, "", "wraps; frozen value means master stuck"),
        ("SystemMode", 8, 8, BITFIELD, "", "reserved, send 0"),
        ("OnlineBitmap", 16, 16, UNSIGNED, "",
         "bit N set = node N online (bit1 = node1)"),
        ("FwVersionUniform", 32, 8, UNSIGNED, "", "0 = all nodes consistent"),
        ("ProtocolVersion", 40, 8, UNSIGNED, "", "protocol version, separate from fw"),
    ],
}

# 同布局消息的别名：省掉重复书写，同时保证两者永不偏离
LAYOUT_ALIAS = {
    "NODE2_EVENT": "NODE1_EVENT",
    "CMD_NODE1": "CMD_BROADCAST",
    "CMD_NODE2": "CMD_BROADCAST",
    "ACK_NODE2": "ACK_NODE1",
}

# --------------------------------------------------------------------------- #
# 枚举值表（与 proto_codec.h 的枚举一致）
# --------------------------------------------------------------------------- #
VALUE_TABLES: dict[str, dict[int, str]] = {
    "Cmd": {
        0x01: "SET", 0x02: "STOP", 0x03: "RESET", 0x04: "QUERY",
    },
    "Device": {
        0: "MOTOR", 1: "SERVO", 2: "BUZZER", 3: "SYSTEM",
    },
    "EventCode": {
        0x01: "OVER_TEMP", 0x02: "TEMP_LIMIT", 0x03: "IR_TRIGGER",
        0x04: "LIGHT_LIMIT", 0x05: "STALL_NEEDS_CURRENT_HW",
        0x06: "SENSOR_FAULT",
    },
    "Level": {
        0: "INFO", 1: "WARNING", 2: "CRITICAL",
    },
    "MotorState": {
        0: "STOP", 1: "FORWARD", 2: "REVERSE", 3: "BRAKE",
    },
    "Result": {
        0: "OK", 1: "ERR_LEN", 2: "ERR_ID", 3: "ERR_NODE", 4: "ERR_RANGE",
        5: "ERR_VALUE", 6: "ERR_STATE", 7: "ERR_BUSY", 8: "ERR_CRC",
        9: "ERR_UNSUPPORTED",
    },
}

BITFIELDS = {
    # 只给"单一位有意义"的位域建 VAL_ 表：位域是位掩码，逐位枚举会误导解析器
    # 与读者（例如 Status 的 bit0/bit1 是并发标志，不是互斥取值）。
    "Valid": [("temperature", 0), ("do_state", 1), ("current", 2)],
}

DIRECTION_MAP = {
    "NODE_TO_MASTER": ("NODE", "MASTER"),
    "MASTER_TO_ALL": ("MASTER", "ALL_NODES"),
    "MASTER_TO_NODE": ("MASTER", "NODE"),
}

FRAME_ROW = re.compile(
    r"\{\s*(0x[0-9A-Fa-f]+)u?\s*,\s*\"([A-Za-z0-9_]+)\"\s*,\s*"
    r"PROTO_DIR_([A-Z_]+)\s*,\s*PROTO_DLC\s*,\s*(\d+)u?\s*,",
    re.MULTILINE,
)

HEADER_LINE = (
    "// GENERATED by tools/gen_dbc.py from "
    "firmware/common/protocol/include/proto_id.h -- DO NOT EDIT BY HAND."
)


def parse_frames() -> list[dict]:
    """Parse the frame table out of proto_id.h (the single source of truth)."""
    with open(HEADER, "r", encoding="utf-8") as fp:
        text = fp.read()
    rows = FRAME_ROW.findall(text)
    if not rows:
        raise SystemExit(
            f"gen_dbc: no frames parsed from {HEADER}; "
            "the PROTO_FRAME_TABLE row format changed"
        )
    frames = []
    for raw_id, name, direction, cycle in rows:
        if direction not in DIRECTION_MAP:
            raise SystemExit(f"gen_dbc: unknown direction PROTO_DIR_{direction}")
        tx, rx = DIRECTION_MAP[direction]
        frames.append(
            {
                "id": int(raw_id, 16),
                "name": name,
                "tx": tx,
                "rx": rx,
                "cycle": int(cycle),
            }
        )
    ids = [f["id"] for f in frames]
    if len(set(ids)) != len(ids):
        raise SystemExit("gen_dbc: duplicate frame IDs in the table")
    if any(i > 0x7FF for i in ids):
        raise SystemExit("gen_dbc: frame ID exceeds 11 bits")
    return frames


def signals_for(name: str) -> list[tuple]:
    if name in SIGNALS and SIGNALS[name]:
        return SIGNALS[name]
    alias = LAYOUT_ALIAS.get(name)
    if alias is not None:
        return SIGNALS[alias]
    raise SystemExit(f"gen_dbc: no signal layout defined for {name}")


def signal_line(name: str, start: int, width: int, kind: str, unit: str,
                comment: str, factor: float = 1) -> str:
    unsigned = 0 if kind == SIGNED else 1
    if width > 1:
        if kind == SIGNED:
            min_val = float(-(1 << (width - 1)))
            max_val = float((1 << (width - 1)) - 1)
        else:
            min_val, max_val = 0.0, float((1 << width) - 1)
    else:
        min_val, max_val = 0.0, 1.0
    return (
        f' SG_ {name} : {start}|{width}@1{"+" if unsigned else "-"} '
        f"({factor:g},{0}) [{min_val:g}|{max_val:g}] \"{unit}\" "
    )


def build_dbc(frames: list[dict]) -> str:
    out: list[str] = []
    out.append(HEADER_LINE)
    out.append("")
    out.append("VERSION \"can_distributed v1.0\"")
    out.append("")
    out.append("NS_ :")
    out.append("")
    out.append("BS_:")
    out.append("")
    out.append("BU_: MASTER NODE ALL_NODES")
    out.append("")
    out.append(
        f"BA_DEF_ BU_ \"GenMsgCycleTime\" INT 0 65535;"
    )
    out.append("")

    val_lines: list[str] = []
    for frame in frames:
        signals = signals_for(frame["name"])
        # 接收方：从节点上报 → MASTER；主站广播 → ALL_NODES；主站单播 → 目标节点
        if frame["tx"] == "NODE":
            rx = "MASTER"
        else:
            rx = frame["rx"]
        out.append(
            f'BO_ {frame["id"]} {frame["name"]}: 8 {frame["tx"]}'
        )
        for sig in signals:
            name, start, width, kind, unit, comment = sig[:6]
            factor = sig[6] if len(sig) > 6 else 1
            line = signal_line(name, start, width, kind, unit, comment, factor) + rx
            out.append(line + f'  // {comment}')
            table = VALUE_TABLES.get(name)
            if table is not None:
                entries = " ".join(f'{k} "{v}"' for k, v in sorted(table.items()))
                val_lines.append(f"VAL_ {frame['id']} {name} {entries} ;")
            if name in BITFIELDS and kind == BITFIELD:
                entries = " ".join(f'{bit} "{label}"' for label, bit in BITFIELDS[name])
                val_lines.append(f"VAL_ {frame['id']} {name} {entries} ;")
        out.append("")
        if frame["cycle"]:
            out.append(
                f'BA_ "GenMsgCycleTime" BO_ {frame["id"]} {frame["cycle"]};'
            )
            out.append("")

    out.append("")
    out.extend(val_lines)
    out.append("")
    out.append(
        'BA_DEF_ "BusType" STRING ;'
    )
    out.append('BA_ "BusType" "CAN";')
    out.append(
        'BA_DEF_ "ProtocolVersion" INT 0 255;'
    )
    out.append(f'BA_ "ProtocolVersion" {PROTOCOL_VERSION};')
    out.append(
        'BA_DEF_DEF_ "GenMsgCycleTime" 0;'
    )
    out.append('BA_DEF_DEF_ "BusType" "CAN";')
    out.append(f'BA_DEF_DEF_ "ProtocolVersion" {PROTOCOL_VERSION};')
    out.append("")
    return "\n".join(out)


def validate_with_cantools(text: str, path: str) -> list[str]:
    """Parse the generated DBC with cantools; return a list of problems."""
    try:
        import cantools  # type: ignore
    except ImportError:
        return ["cantools not installed - skipped decoder validation"]

    problems: list[str] = []
    tmp = path + ".gen.tmp.dbc"
    try:
        with open(tmp, "w", encoding="utf-8") as fp:
            fp.write(text)
        db = cantools.database.load_file(tmp)
        names = sorted(m.name for m in db.messages)
        if len(db.messages) != 10:
            problems.append(f"cantools parsed {len(db.messages)} messages, want 10")
        if "TELEMETRY_NODE1" not in names:
            problems.append("TELEMETRY_NODE1 missing after parse")
        # spot check: signed temperature round-trip.
        # Temperature has factor 0.1, so encode a *scaled* value; the raw
        # integer must land little-endian at bytes 2..3.
        msg = db.get_message_by_name("TELEMETRY_NODE1")
        payload = msg.encode(
            {"Seq": 7, "Valid": 3, "Temperature": 28.5, "DoState": 0,
             "MotorState": 1, "MotorDutyPct": 60, "Status": 0},
            scaling=True, padding=True,
        )
        if payload[:2] != bytes([0x07, 0x03]):
            problems.append(f"TELEMETRY_NODE1 prefix wrong: {payload.hex()}")
        if payload[2] != 0x1D or payload[3] != 0x01:
            problems.append(f"Temperature is not little-endian int16: {payload.hex()}")
        raw = msg.decode(payload, scaling=False)
        if raw.get("Temperature") != 285:
            problems.append(f"raw temperature {raw.get('Temperature')!r} != 285")
        scaled = msg.decode(payload, scaling=True)
        if abs(scaled.get("Temperature", 0) - 28.5) > 1e-6:
            problems.append("scaled temperature did not round-trip to 28.5")
    except Exception as exc:  # noqa: BLE001 - report any parser failure as a problem
        problems.append(f"cantools failed: {exc}")
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="fail if the committed DBC differs from the generated one")
    ap.add_argument("--write", action="store_true", help="regenerate the DBC file")
    ap.add_argument("--stdout", action="store_true", help="print the DBC")
    args = ap.parse_args()

    frames = parse_frames()
    text = build_dbc(frames)

    if args.stdout:
        sys.stdout.write(text)
        return 0

    if args.write:
        os.makedirs(os.path.dirname(DBC), exist_ok=True)
        with open(DBC, "w", encoding="utf-8", newline="\n") as fp:
            fp.write(text)
        print(f"wrote {os.path.relpath(DBC, REPO_ROOT)} ({len(text)} bytes, "
              f"{len(frames)} frames)")
        problems = [p for p in validate_with_cantools(text, DBC) if "skipped" not in p]
        for p in problems:
            print(f"  cantools: {p}")
        return 1 if problems else 0

    if args.check:
        if not os.path.exists(DBC):
            print(f"check failed: {DBC} does not exist")
            return 1
        with open(DBC, "r", encoding="utf-8") as fp:
            committed = fp.read()
        ok = True
        if committed.replace("\r\n", "\n") != text:
            print("check failed: can_distributed.dbc is out of sync with "
                  "proto_id.h -- run: python tools/gen_dbc.py --write")
            ok = False
        problems = [p for p in validate_with_cantools(text, DBC)
                    if "skipped" not in p]
        for p in problems:
            print(f"check failed: cantools: {p}")
            ok = False
        if ok:
            print(f"ok: DBC in sync with proto_id.h ({len(frames)} frames)")
        return 0 if ok else 1

    ap.print_help()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
