# Create and save snapshots of auth attempts

# Import modules
import os
from i18n import _
from datetime import datetime, timezone

import cv2
import numpy as np
import paths_factory


def generate(frames, text_lines):
    """Generate a snapshot from given frames"""

    # Don't execute if no frames were given
    if len(frames) == 0:
        return

    # Get frame dimensions
    frame_height, frame_width, cc = frames[0].shape
    # Spread the given frames out horizontally
    snap = np.concatenate(frames, axis=1)

    # Create colors
    pad_color = [44, 44, 44]
    text_color = [255, 255, 255]

    # Add a gray square at the bottom of the image
    snap = cv2.copyMakeBorder(
        snap, 0, len(text_lines) * 20 + 40, 0, 0, cv2.BORDER_CONSTANT, value=pad_color
    )

    # Go through each line
    line_number = 0
    for line in text_lines:
        # Calculate how far the line should be from the top
        padding_top = frame_height + 30 + (line_number * 20)
        # Print the line onto the image
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

    # Made sure a snapshot folder exist
    os.makedirs(paths_factory.snapshots_dir_path(), exist_ok=True)

    # Generate a filename based on the current time
    filename = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.jpg")
    filepath = paths_factory.snapshot_path(filename)
    # Write the image to that file
    result = cv2.imwrite(filepath, snap)
    if not result:
        print(_("Warning: Failed to write snapshot to {}").format(filepath))

    # Return the saved file location
    return filepath
