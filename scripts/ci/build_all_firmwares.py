#!/usr/bin/env python3
"""Сборка всех вариантов прошивок BMCU и формирование manifest.txt."""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import sys
import zlib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
OUT_DIR = REPO_ROOT / "build"
PIO_ENV = "fw"

MODE_DIRS = {
    0: "standard(A1)",
    1: "high_force_load(P1S)",
}

SOLO_RETRACT = "0.095f"
RETRACTS = [
    "0.10",
    "0.20", "0.25", "0.30", "0.35", "0.40",
    "0.45", "0.50", "0.55", "0.60", "0.65",
    "0.70", "0.75", "0.80", "0.85", "0.90",
]


def ensure_tool(name: str) -> None:
    """Проверить наличие внешнего инструмента в PATH."""
    if shutil.which(name) is None:
        raise RuntimeError(f"Не найден инструмент '{name}' в PATH")


def run_build(*, ams_num: int, retract_len: str, dm: int, rgb: int, p1s: int, out_path: Path) -> None:
    """Собрать один вариант прошивки и скопировать firmware.bin в целевую папку."""
    print(
        f"=== СБОРКА: P1S={p1s} DM={dm} RGB={rgb} AMS_NUM={ams_num} "
        f"RETRACT={retract_len} -> {out_path}"
    )

    env = os.environ.copy()
    env.update(
        {
            "BAMBU_BUS_AMS_NUM": str(ams_num),
            "AMS_RETRACT_LEN": retract_len,
            "BMCU_DM_TWO_MICROSWITCH": str(dm),
            "BMCU_ONLINE_LED_FILAMENT_RGB": str(rgb),
            "DBMCU_P1S": str(p1s),
            "BMCU_SOFT_LOAD": "0",
        }
    )

    subprocess.run(["pio", "run", "-e", PIO_ENV], check=True, cwd=REPO_ROOT, env=env)

    src = REPO_ROOT / ".pio" / "build" / PIO_ENV / "firmware.bin"
    if not src.exists():
        raise FileNotFoundError(f"Ожидаемый файл не найден: {src}")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, out_path)


def create_manifest(root: Path) -> None:
    """Сформировать manifest.txt с SHA256/CRC32/размером для каждого файла."""
    entries: list[tuple[str, str, str, int]] = []

    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue

        rel = path.relative_to(root).as_posix()
        size = 0
        crc = 0
        h = hashlib.sha256()

        with path.open("rb") as f:
            while True:
                chunk = f.read(1024 * 1024)
                if not chunk:
                    break
                size += len(chunk)
                crc = zlib.crc32(chunk, crc)
                h.update(chunk)

        entries.append((rel, h.hexdigest(), f"{crc & 0xFFFFFFFF:08X}", size))

    manifest = root / "manifest.txt"
    with manifest.open("w", encoding="utf-8") as out:
        out.write("# format: SHA256_HEX CRC32_HEX SIZE_BYTES REL_PATH\n")
        for rel, sha256_hex, crc32_hex, size in entries:
            out.write(f"{sha256_hex} {crc32_hex} {size} {rel}\n")


def main() -> int:
    ensure_tool("pio")

    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    for p1s in (0, 1):
        mode_base = OUT_DIR / MODE_DIRS[p1s]

        for dm in (1, 0):
            dm_dir = "AUTOLOAD" if dm == 1 else "NO_AUTOLOAD"

            for rgb in (1, 0):
                rgb_dir = "FILAMENT_RGB_ON" if rgb == 1 else "FILAMENT_RGB_OFF"
                base = mode_base / dm_dir / rgb_dir

                run_build(
                    ams_num=0,
                    retract_len=SOLO_RETRACT,
                    dm=dm,
                    rgb=rgb,
                    p1s=p1s,
                    out_path=base / "SOLO" / f"solo_{SOLO_RETRACT}.bin",
                )

                for idx, slot in enumerate(("A", "B", "C", "D")):
                    for r in RETRACTS:
                        run_build(
                            ams_num=idx,
                            retract_len=f"{r}f",
                            dm=dm,
                            rgb=rgb,
                            p1s=p1s,
                            out_path=base / f"AMS_{slot}" / f"ams_{slot.lower()}_{r}f.bin",
                        )

    create_manifest(OUT_DIR)
    print("\nГотово. Результаты в каталоге:", OUT_DIR)
    print("Manifest:", OUT_DIR / "manifest.txt")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:  # noqa: BLE001
        print(f"Ошибка сборки: {exc}", file=sys.stderr)
        raise
