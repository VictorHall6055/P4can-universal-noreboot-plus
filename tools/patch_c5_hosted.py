#!/usr/bin/env python3
"""Patch ESP-Hosted transport_drv.c.obj from esp32p4-libs 3.3.12 to accept ESP32-C5.

Core 3.3.12 prebuilt libespressif__esp_hosted.a hardcodes
CONFIG_ESP_HOSTED_CP_TARGET_ESP32C6 (chip_type == 13) in
process_priv_communication(). When paired with an ESP32-C5 co-processor
(chip_type == 23), it logs:
    E ...: Identified slave [esp32c5] != Expected [esp32c6]
and aborts via __assert_func(), leaving Wi-Fi dead.

This script extracts transport_drv.c.obj into a build-local object file and
swaps the expected chip_type check (13 <-> 23) and expected string
("esp32c6" <-> "esp32c5") without modifying any global Arduino core files.
Passing the patched object via compiler.c.elf.extra_flags overrides the archive
member at link time.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import struct
import subprocess
import sys

ELF32_HDR_FMT = "<16sHHIIIIIHHHHHH"
ELF32_SHDR_FMT = "<IIIIIIIIII"
CHIP_ESP32C6 = 13
CHIP_ESP32C5 = 23


def parse_elf32_sections(blob: bytes) -> dict[str, tuple[int, int, int]]:
    if len(blob) < 52 or blob[:4] != b"\x7fELF" or blob[4] != 1 or blob[5] != 1:
        raise ValueError("Not a little-endian 32-bit ELF object")
    hdr = struct.unpack_from(ELF32_HDR_FMT, blob, 0)
    e_shoff = hdr[6]
    e_shentsize = hdr[11]
    e_shnum = hdr[12]
    e_shstrndx = hdr[13]
    shdrs = [
        struct.unpack_from(ELF32_SHDR_FMT, blob, e_shoff + i * e_shentsize)
        for i in range(e_shnum)
    ]
    str_sh = shdrs[e_shstrndx]
    shstrtab = blob[str_sh[4] : str_sh[4] + str_sh[5]]

    sections: dict[str, tuple[int, int, int]] = {}
    for idx, sh in enumerate(shdrs):
        sh_name, _sh_type, _flags, _addr, sh_offset, sh_size, *_ = sh
        end = shstrtab.find(b"\x00", sh_name)
        name = shstrtab[sh_name:end].decode("ascii", errors="replace") if end >= 0 else ""
        sections[name] = (idx, sh_offset, sh_size)
    return sections


def patch_object(obj_bytes: bytes) -> bytearray:
    data = bytearray(obj_bytes)
    sections = parse_elf32_sections(obj_bytes)

    text_name = ".text.process_priv_communication"
    rodata_name = ".rodata.process_priv_communication.str1.4"
    if text_name not in sections or rodata_name not in sections:
        raise RuntimeError(f"Expected sections not found in object: {text_name} / {rodata_name}")

    _, text_off, text_sz = sections[text_name]
    _, ro_off, ro_sz = sections[rodata_name]

    # Verify exact RISC-V compressed instructions before patching:
    # 0x11a: 47b5  c.li a5, 13 (ESP_CHIP_ID_ESP32C6) -> beq s0, a5, .L138
    # 0x15a: 47dd  c.li a5, 23 (ESP_CHIP_ID_ESP32C5) -> beq s0, a5, .L147
    off_c6 = 0x11A
    off_c5 = 0x15A
    if off_c5 + 6 > text_sz:
        raise RuntimeError(f"Unexpected section size {text_sz:#x} for {text_name}")

    ins_c6 = data[text_off + off_c6 : text_off + off_c6 + 2]
    ins_c5 = data[text_off + off_c5 : text_off + off_c5 + 2]
    # c.li a5, imm5 (bits [11:7]=15, funct3=2): imm=13 -> 0x47b5 (le: b5 47), imm=23 -> 0x47dd (le: dd 47)
    if ins_c6 != b"\xb5\x47" or ins_c5 != b"\xdd\x47":
        raise RuntimeError(
            f"Signature mismatch in process_priv_communication: "
            f"@0x11a={ins_c6.hex()} @0x15a={ins_c5.hex()}"
        )
    # Also confirm both are followed cleanly by beq s0, a5 (<...>863 / <...>163)
    if data[text_off + 0x122 : text_off + 0x124] != b"\x40\x0ef"[1:]:  # lower byte check below
        pass
    beq_c6 = struct.unpack_from("<I", data, text_off + 0x120)[0]
    beq_c5 = struct.unpack_from("<I", data, text_off + 0x15C)[0]
    if (beq_c6 & 0x01FFF07F) != 0x00F40063 or (beq_c5 & 0x01FFF07F) != 0x00F40063:
        raise RuntimeError(
            f"Unexpected branch opcodes after c.li: {beq_c6:#010x}, {beq_c5:#010x}"
        )

    # Swap 13 <-> 23
    data[text_off + off_c6 : text_off + off_c6 + 2] = b"\xdd\x47"  # c.li a5, 23 (C5 OK)
    data[text_off + off_c5 : text_off + off_c5 + 2] = b"\xb5\x47"  # c.li a5, 13 (C6 mismatch)

    # Swap exact 8-byte null-terminated target strings in .rodata so Expected == "esp32c5"
    ro_slice = bytes(data[ro_off : ro_off + ro_sz])
    pos_c6 = ro_slice.find(b"esp32c6\x00")
    pos_c5 = ro_slice.find(b"esp32c5\x00")
    if pos_c6 < 0 or pos_c5 < 0:
        raise RuntimeError(f"Expected rodata strings missing (c6={pos_c6}, c5={pos_c5})")
    data[ro_off + pos_c6 : ro_off + pos_c6 + 8] = b"esp32c5\x00"
    data[ro_off + pos_c5 : ro_off + pos_c5 + 8] = b"esp32c6\x00"
    return data


def find_esp32p4_tools() -> tuple[Path, Path, Path]:
    local_app = Path.home() / "AppData" / "Local" / "Arduino15" / "packages" / "esp32" / "tools"
    libs = sorted((local_app / "esp32p4-libs").glob("*/lib/libespressif__esp_hosted.a"))
    ars = sorted((local_app / "esp-rv32").glob("*/bin/riscv32-esp-elf-ar*"))
    objdumps = sorted((local_app / "esp-rv32").glob("*/bin/riscv32-esp-elf-objdump*"))
    if not libs or not ars or not objdumps:
        raise FileNotFoundError(f"Could not locate esp32p4-libs / esp-rv32 tools under {local_app}")
    # Prefer exact 3.3.12 if multiple versions exist
    lib_312 = [p for p in libs if "3.3.12" in p.parts]
    lib = lib_312[0] if lib_312 else libs[-1]
    return lib, ars[-1], objdumps[-1]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="Output path for transport_drv_c5.o")
    ap.add_argument("--archive", help="Path to libespressif__esp_hosted.a")
    ap.add_argument("--ar", help="Path to riscv32-esp-elf-ar")
    ap.add_argument("--objdump", help="Path to riscv32-esp-elf-objdump")
    args = ap.parse_args()

    default_lib, default_ar, default_objdump = find_esp32p4_tools()
    lib = Path(args.archive) if args.archive else default_lib
    ar = Path(args.ar) if args.ar else default_ar
    objdump = Path(args.objdump) if args.objdump else default_objdump
    out_path = Path(args.out).resolve()
    out_dir = out_path.parent
    out_dir.mkdir(parents=True, exist_ok=True)

    extracted = out_dir / "transport_drv.c.obj"
    if extracted.exists():
        extracted.unlink()
    subprocess.run([str(ar), "x", str(lib), "transport_drv.c.obj"], cwd=str(out_dir), check=True)
    if not extracted.is_file():
        raise FileNotFoundError(f"ar failed to extract {extracted}")

    patched = patch_object(extracted.read_bytes())
    out_path.write_bytes(bytes(patched))
    extracted.unlink()

    dis = subprocess.run(
        [str(objdump), "-d", "--section=.text.process_priv_communication", str(out_path)],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    ok_line = [ln.strip() for ln in dis.splitlines() if "11a:" in ln or "15a:" in ln]
    if not any("li\ta5,23" in ln or "li a5,23" in ln or "11a:\t47dd" in ln for ln in ok_line):
        raise RuntimeError(f"Disassembly check failed after patching: {ok_line}")
    print(f"Patched C5 Hosted object written: {out_path} ({len(patched)} bytes) -> {' | '.join(ok_line)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
