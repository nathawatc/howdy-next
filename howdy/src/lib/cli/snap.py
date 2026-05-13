# Create a snapshot

from __future__ import annotations

# Import required modules
from datetime import datetime, timezone

import snapshot
from config_utils import load_config
from i18n import _
from recorders.video_capture import VideoCapture

# Read the config
config = load_config()

# Start video capture
video_capture = VideoCapture(config)

dark_threshold = config.getfloat("video", "dark_threshold", fallback=60)

# Collection of recorded frames
frames = []

while True:
    # Grab a single frame of video
    frame, gsframe = video_capture.read_frame()

    # Add the frame to the list
    frames.append(frame)

    # Stop the loop if we have 4 frames
    if len(frames) >= 4:
        break

# Generate a snapshot image from the frames
file = snapshot.generate(
    frames,
    [
        _("GENERATED SNAPSHOT"),
        _("Date: ") + datetime.now(timezone.utc).strftime("%Y/%m/%d %H:%M:%S UTC"),
        _("Dark threshold config: ")
        + str(config.getfloat("video", "dark_threshold", fallback=60.0)),
        _("SFace threshold config: ")
        + str(config.getfloat("face", "sface_threshold", fallback=0.363)),
    ],
)

# Show the file location in console
print(_("Generated snapshot saved as"))
print(file)
