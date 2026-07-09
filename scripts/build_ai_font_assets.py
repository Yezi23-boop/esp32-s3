#!/usr/bin/env python3
"""为 hand-written AI 页面生成与 xiaozhi-esp32 兼容的 assets.bin。"""

from __future__ import annotations

import argparse
import os
import struct
from pathlib import Path


NAME_LENGTH = 32
MAGIC_PREFIX = b"\x5A\x5A"


def compute_checksum(data: bytes) -> int:
    return sum(data) & 0xFFFF


def iter_asset_files(assets_dir: Path) -> list[Path]:
    files = [path for path in assets_dir.iterdir() if path.is_file()]
    files.sort(key=lambda path: path.name)
    return files


def build_assets_image(assets_dir: Path, output_file: Path) -> None:
    if not assets_dir.is_dir():
        raise FileNotFoundError(f"assets directory not found: {assets_dir}")

    merged_data = bytearray()
    file_info_list: list[tuple[str, int, int, int, int]] = []

    for file_path in iter_asset_files(assets_dir):
        file_name = file_path.name
        file_size = file_path.stat().st_size
        offset = len(merged_data)
        width = 0
        height = 0
        file_info_list.append((file_name, offset, file_size, width, height))

        merged_data.extend(MAGIC_PREFIX)
        merged_data.extend(file_path.read_bytes())

    mmap_table = bytearray()
    for file_name, offset, file_size, width, height in file_info_list:
        encoded_name = file_name.encode("utf-8")
        if len(encoded_name) > NAME_LENGTH:
            raise ValueError(f"asset name too long for mmap table: {file_name}")
        fixed_name = encoded_name.ljust(NAME_LENGTH, b"\0")
        mmap_table.extend(fixed_name)
        mmap_table.extend(struct.pack("<I", file_size))
        mmap_table.extend(struct.pack("<I", offset))
        mmap_table.extend(struct.pack("<H", width))
        mmap_table.extend(struct.pack("<H", height))

    combined_data = mmap_table + merged_data
    header = bytearray()
    header.extend(struct.pack("<I", len(file_info_list)))
    header.extend(struct.pack("<I", compute_checksum(combined_data)))
    header.extend(struct.pack("<I", len(combined_data)))

    output_file.parent.mkdir(parents=True, exist_ok=True)
    output_file.write_bytes(header + combined_data)


def main() -> None:
    parser = argparse.ArgumentParser(description="打包 AI 字体 assets 分区镜像")
    parser.add_argument("--assets-dir", required=True, help="资源目录")
    parser.add_argument("--output", required=True, help="输出 bin 路径")
    parser.add_argument("--max-size", help="最大镜像大小，支持十进制或 0x 十六进制")
    args = parser.parse_args()

    output_file = Path(args.output)
    build_assets_image(Path(args.assets_dir), output_file)

    if args.max_size:
        max_size = int(args.max_size, 0)
        image_size = output_file.stat().st_size
        if image_size > max_size:
            raise ValueError(
                f"assets image too large: {image_size} bytes > partition size {max_size} bytes"
            )


if __name__ == "__main__":
    main()
