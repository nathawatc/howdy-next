from __future__ import annotations

import configparser
import fcntl
import os
import tempfile
from typing import Iterable

import paths_factory


def load_config() -> configparser.ConfigParser:
    config = configparser.ConfigParser()
    config.read(paths_factory.config_file_path())
    return config


def read_config_lines(config_path: str, *, lock: bool = False) -> list[str]:
    with open(config_path, "r") as f:
        if lock:
            fcntl.flock(f.fileno(), fcntl.LOCK_EX)
        return f.readlines()


def atomic_write_lines(config_path: str, lines: Iterable[str]) -> None:
    fd, tmp_path = tempfile.mkstemp(dir=os.path.dirname(config_path), suffix=".tmp")
    try:
        with os.fdopen(fd, "w") as tmp:
            tmp.writelines(lines)
        os.replace(tmp_path, config_path)
    except Exception:
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)
        raise


def update_config_value(
    config_path: str, key: str, value: str, *, lock: bool = False
) -> None:
    lines = read_config_lines(config_path, lock=lock)
    for index, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith(key + " =") or stripped.startswith(key + " "):
            lines[index] = key + " = " + value + "\n"
            atomic_write_lines(config_path, lines)
            return
    raise KeyError(key)
