# Core utilities module
# Shared functionality for face detection and comparison

from __future__ import annotations

from core.detector import BACKEND_NAME, FaceMatch, FaceModel, check_data_files

__all__ = [
    "BACKEND_NAME",
    "FaceMatch",
    "FaceModel",
    "check_data_files",
]
