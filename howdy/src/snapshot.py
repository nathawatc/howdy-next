# Create and save snapshots of auth attempts

# Import modules
import os
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

    # Add the Howdy logo if there's space to do so
    if len(frames) > 1:
        logo = cv2.imread(paths_factory.logo_path())
        if logo is not None:
            logo_y = frame_height + 20
            logo_x = max(0, frame_width * len(frames) - 210)
            logo_h, logo_w = logo.shape[:2]
            if logo_x + logo_w <= snap.shape[1] and logo_y + logo_h <= snap.shape[0]:
                snap[logo_y : logo_y + logo_h, logo_x : logo_x + logo_w] = logo

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
