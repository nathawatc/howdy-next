# Show a window with the video stream and testing information

from __future__ import annotations

import builtins
import configparser
import json
import sys
import time

import cv2
import numpy as np
import paths_factory
from core.detector import BACKEND_NAME, FaceModel, clahe_enabled, create_clahe
from i18n import _
from recorders.video_capture import VideoCapture

# Read config from disk
config = configparser.ConfigParser()
config.read(paths_factory.config_file_path())

if config.get("video", "recording_plugin", fallback="opencv") != "opencv":
    print(
        _(
            "Howdy has been configured to use a recorder which doesn't support the test command yet, aborting"
        )
    )
    sys.exit(12)

video_capture = VideoCapture(config)

# Read config values to use in the main loop
exposure = config.getint("video", "exposure", fallback=-1)
dark_threshold = config.getfloat("video", "dark_threshold", fallback=60)

# Let the user know what's up
print(
    _("""
Opening a window with a test feed

Press ctrl+C in this terminal to quit
Click on the image to enable or disable slow mode
""")
)


def mouse(event: int, x: int, y: int, flags: int, param: int) -> None:
    """Handle mouse events"""
    global slow_mode

    # Toggle slowmode on click
    if event == cv2.EVENT_LBUTTONDOWN:
        slow_mode = not slow_mode


def print_text(line_number: int, text: str) -> None:
    """Print the status text by line number"""
    cv2.putText(
        overlay,
        text,
        (10, height - 10 - (10 * line_number)),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.3,
        (0, 255, 0),
        0,
        cv2.LINE_AA,
    )


face_model = FaceModel(config)

encodings = []
encoding_models = []
models = None

try:
    user = str(getattr(builtins, "howdy_user"))
    with open(paths_factory.user_model_path(user)) as f:
        models = json.load(f)
    if any(model.get("backend") != BACKEND_NAME for model in models):
        print(
            _(
                "Warning: Stored face models use an incompatible backend; matching disabled"
            )
        )
        models = None
    else:
        for model in models:
            for encoding in model["data"]:
                encodings.append(encoding)
                encoding_models.append(model)
except FileNotFoundError:
    print(
        _(
            "Warning: No face model found for this user, detection will run without matching"
        )
    )

known_encodings = np.asarray(encodings, dtype=np.float32) if encodings else None
clahe = create_clahe(config)

# Open the window and attach a a mouse listener
cv2.namedWindow("Howdy Test")
cv2.setMouseCallback("Howdy Test", mouse)

# Enable a delay in the loop
slow_mode = False
# Count all frames ever
total_frames = 0
# Count all frames per second
sec_frames = 0
# Last seconds FPS
fps = 0
# The current second we're counting
sec = int(time.time())
# recognition time
rec_tm = 0

# Wrap everything in an keyboard interrupt handler
try:
    while True:
        frame_tm = time.time()

        # Increment the frames
        total_frames += 1
        sec_frames += 1

        # Id we've entered a new second
        if sec != int(frame_tm):
            # Set the last seconds FPS
            fps = sec_frames

            # Set the new second and reset the counter
            sec = int(frame_tm)
            sec_frames = 0

        # Grab a single frame of video
        _orig_frame, frame = video_capture.read_frame()
        if frame.ndim != 2:
            frame = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)

        if clahe_enabled(config):
            frame = clahe.apply(frame)

        # Make a frame to put overlays in
        overlay = cv2.cvtColor(frame.copy(), cv2.COLOR_GRAY2BGR)

        # Fetch the frame height and width
        height, width = frame.shape[:2]

        # Create a histogram of the image with 8 values
        hist = cv2.calcHist([frame], [0], None, [8], [0, 256])
        # All values combined for percentage calculation
        hist_total = int(hist.sum())
        # Fill with the overall containing percentage
        hist_perc = []

        # Loop though all values to calculate a percentage and add it to the overlay
        for index, value in enumerate(hist):
            value_perc = float(value[0]) / max(hist_total, 1) * 100
            hist_perc.append(value_perc)

            # Top left point, 10px margins
            p1 = (20 + (10 * index), 10)
            # Bottom right point makes the bar 10px thick, with an height of half the percentage
            p2 = (10 + (10 * index), int(value_perc / 2 + 10))
            # Draw the bar in green
            cv2.rectangle(overlay, p1, p2, (0, 200, 0), thickness=cv2.FILLED)

        # Print the statis in the bottom left
        print_text(0, _("RESOLUTION: %dx%d") % (height, width))
        print_text(1, _("FPS: %d") % (fps,))
        print_text(2, _("FRAMES: %d") % (total_frames,))
        print_text(3, _("RECOGNITION: %dms") % (round(rec_tm * 1000),))
        print_text(4, _("BACKEND: OpenCV YuNet/SFace"))
        print_text(5, _("CLAHE: %s") % (_("on") if clahe_enabled(config) else _("off")))

        # Show that slow mode is on, if it's on
        if slow_mode:
            cv2.putText(
                overlay,
                _("SLOW MODE"),
                (width - 66, height - 10),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.3,
                (0, 0, 255),
                0,
                cv2.LINE_AA,
            )

        # Ignore dark frames
        if hist_perc[0] > dark_threshold:
            # Show that this is an ignored frame in the top right
            cv2.putText(
                overlay,
                _("DARK FRAME"),
                (width - 68, 16),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.3,
                (0, 0, 255),
                0,
                cv2.LINE_AA,
            )
        else:
            # Show that this is an active frame
            cv2.putText(
                overlay,
                _("SCAN FRAME"),
                (width - 68, 16),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.3,
                (0, 255, 0),
                0,
                cv2.LINE_AA,
            )

            rec_tm = time.time()
            face_frame = face_model.prepare_frame(frame)
            face_locations = face_model.detect(face_frame)
            rec_tm = time.time() - rec_tm

            for face in face_locations:
                color = (0, 0, 230)
                x, y, w, h = face_model.detection_box(face)
                confidence = face_model.detection_confidence(face)

                if known_encodings is not None:
                    face_encoding = face_model.encode(face_frame, face)
                    match = face_model.best_match(known_encodings, face_encoding)

                    if match.accepted:
                        color = (0, 230, 0)
                        model = encoding_models[match.index]
                        face_text = "{} (score: {:.3f})".format(
                            model["label"], match.score
                        )
                    else:
                        face_text = "no match ({:.3f})".format(match.score)

                    cv2.putText(
                        overlay,
                        face_text,
                        (x, max(0, y - 8)),
                        cv2.FONT_HERSHEY_SIMPLEX,
                        0.3,
                        color,
                        0,
                        cv2.LINE_AA,
                    )

                cv2.rectangle(overlay, (x, y), (x + w, y + h), color, 2)
                cv2.putText(
                    overlay,
                    "{:.2f}".format(confidence),
                    (x, min(height - 4, y + h + 12)),
                    cv2.FONT_HERSHEY_SIMPLEX,
                    0.3,
                    color,
                    0,
                    cv2.LINE_AA,
                )
                for point in face_model.detection_landmarks(face):
                    cv2.circle(overlay, point, 2, (0, 255, 255), -1)

        # Add the overlay to the frame with some transparency
        alpha = 0.65
        frame = cv2.cvtColor(frame, cv2.COLOR_GRAY2BGR)
        cv2.addWeighted(overlay, alpha, frame, 1 - alpha, 0, frame)

        # Show the image in a window
        cv2.imshow("Howdy Test", frame)

        # Quit on any keypress
        if cv2.waitKey(1) != -1:
            raise KeyboardInterrupt()

        frame_time = time.time() - frame_tm

        # Delay the frame if slowmode is on
        if slow_mode:
            time.sleep(max([0.5 - frame_time, 0.0]))

        if exposure != -1:
            # For a strange reason on some cameras (e.g. Lenoxo X1E)
            # setting manual exposure works only after a couple frames
            # are captured and even after a delay it does not
            # always work. Setting exposure at every frame is
            # reliable though.
            video_capture.internal.set(cv2.CAP_PROP_AUTO_EXPOSURE, 1.0)  # 1 = Manual
            video_capture.internal.set(cv2.CAP_PROP_EXPOSURE, float(exposure))

except KeyboardInterrupt:
    cv2.destroyAllWindows()
    video_capture.release()
