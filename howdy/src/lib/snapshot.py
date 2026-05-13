# Create and save snapshots of auth attempts

import os
from i18n import _
from datetime import datetime, timezone

import cv2
import numpy as np
import paths_factory


def generate(frames, text_lines):
    """Generate a snapshot from given frames"""

    if len(frames) == 0:
        return

    # Get frame dimensions
    frame_height, frame_width, cc = frames[0].shape
    snap = np.concatenate(frames, axis=1)

    pad_color = [44, 44, 44]
    text_color = [255, 255, 255]

    snap = cv2.copyMakeBorder(
        snap, 0, len(text_lines) * 20 + 40, 0, 0, cv2.BORDER_CONSTANT, value=pad_color
    )

    line_number = 0
    for line in text_lines:
        padding_top = frame_height + 30 + (line_number * 20)
        cv2.putText(
            snap,
            line,
            (30, padding_top),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.4,
            text_color,
            0,
            cv2.LINE_AA,
        )

        line_number += 1

    os.makedirs(paths_factory.snapshots_dir_path(), exist_ok=True)

    filename = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.jpg")
    filepath = paths_factory.snapshot_path(filename)
    result = cv2.imwrite(filepath, snap)
    if not result:
        print(_("Warning: Failed to write snapshot to {}").format(filepath))

    return filepath
