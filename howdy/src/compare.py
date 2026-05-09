# Compare incoming video with known faces
# Running in a local python instance to get around PATH issues

# Import time so we can start timing asap
import time

# Start timing
timings = {"st": time.time()}

# Import required modules
import configparser
import json
import os
import sys
import threading
from datetime import datetime, timezone
from typing import cast

import cv2
import numpy as np
import paths_factory
import snapshot
from core.detector import BACKEND_NAME, FaceModel, clahe_enabled, create_clahe
from i18n import _
from recorders.video_capture import VideoCapture


def exit(code=None):
    if code is not None:
        sys.exit(code)


def init_detector(lock):
    global face_model
    face_model = FaceModel(config)
    timings["ll"] = time.time() - timings["ll"]
    lock.release()


def make_snapshot(
    type,
    snapframes_list: list,
    frames_count: int,
    best_score_val: float,
    frame_time: float,
):
    """Generate snapshot after detection"""
    snapshot.generate(
        snapframes_list,
        [
            type + _(" LOGIN"),
            _("Date: ") + datetime.now(timezone.utc).strftime("%Y/%m/%d %H:%M:%S UTC"),
            _("Scan time: ") + str(round(frame_time, 2)) + "s",
            _("Frames: ")
            + str(frames_count)
            + " ("
            + str(round(frames_count / frame_time, 2))
            + "FPS)",
            _("Hostname: ") + os.uname().nodename,
            _("Best match score: ") + str(round(best_score_val, 3)),
        ],
    )


def update_best_score(current: float | None, score: float, metric: str) -> float:
    if current is None:
        return score
    if metric == "cosine":
        return max(current, score)
    return min(current, score)


# Make sure we were given an username to test against
if len(sys.argv) < 2:
    exit(12)

# The username of the user being authenticated
user = sys.argv[1]
# The model file contents
models = []
# Encoded face models
encodings = []
# Model metadata per encoding
encoding_models = []
# Amount of ignored 100% black frames
black_tries = 0
# Amount of ignored dark frames
dark_tries = 0
# Total amount of frames captured
frames = 0
# Captured frames for snapshot capture
snapframes = []
# Tracks the best score in the loop
best_score = None
face_model: FaceModel | None = None

# Try to load the face model from the models folder
try:
    with open(paths_factory.user_model_path(user)) as f:
        models = json.load(f)
    for model in models:
        if model.get("backend") != BACKEND_NAME:
            print(
                _("Stored face models use an incompatible backend; re-enroll required")
            )
            exit(10)
        for encoding in model["data"]:
            encodings.append(encoding)
            encoding_models.append(model)
except FileNotFoundError:
    exit(10)

# Check if the file contains a model
if len(models) < 1 or len(encodings) < 1:
    exit(10)

# Read config from disk
config = configparser.ConfigParser()
config.read(paths_factory.config_file_path())

# Get all config values needed
timeout = config.getint("video", "timeout", fallback=4)
dark_threshold = config.getfloat("video", "dark_threshold", fallback=50.0)
end_report = config.getboolean("debug", "end_report", fallback=False)
save_failed = config.getboolean("snapshots", "save_failed", fallback=False)
save_successful = config.getboolean("snapshots", "save_successful", fallback=False)
rotate = config.getint("video", "rotate", fallback=0)
timings["in"] = time.time() - timings["st"]

# Import face recognition, takes some time
timings["ll"] = time.time()

# Start threading and wait for init to finish
lock = threading.Lock()
lock.acquire()
threading.Thread(target=init_detector, args=(lock,), daemon=True).start()

# Start video capture on the IR camera
timings["ic"] = time.time()

video_capture = VideoCapture(config)

# Read exposure from config to use in the main loop
exposure = config.getint("video", "exposure", fallback=-1)

# Note the time it took to open the camera
timings["ic"] = time.time() - timings["ic"]

# wait for thread to finish
lock.acquire()
lock.release()
del lock

if face_model is None:
    print(_("Face model not initialized"))
    exit(1)
assert face_model is not None

active_face_model = cast(FaceModel, face_model)
known_encodings = np.asarray(encodings, dtype=np.float32)

# Fetch the max frame height
max_height = config.getfloat("video", "max_height", fallback=320.0)

# Get the height of the image (which would be the width if screen is portrait oriented)
height = video_capture.internal.get(cv2.CAP_PROP_FRAME_HEIGHT) or 1
if rotate == 2:
    height = video_capture.internal.get(cv2.CAP_PROP_FRAME_WIDTH) or 1
# Calculate the amount the image has to shrink
scaling_factor = max_height / max(height, 1)

# Initiate histogram equalization
clahe = create_clahe(config)

# Start the read loop
valid_frames = 0
timings["fr"] = time.time()
dark_running_total = 0

while True:
    # Increment the frame count every loop
    frames += 1

    # Stop if we've exceeded the time limit
    if time.time() - timings["fr"] > timeout:
        # Create a timeout snapshot if enabled
        if save_failed:
            make_snapshot(
                _("FAILED"),
                snapframes,
                frames,
                best_score or 0.0,
                time.time() - timings["fr"],
            )

        if dark_tries > 0 and valid_frames == 0:
            print(_("All frames were too dark, please check dark_threshold in config"))
            print(
                _("Average darkness: {avg}, Threshold: {threshold}").format(
                    avg=str(dark_running_total / max(1, valid_frames)),
                    threshold=str(dark_threshold),
                )
            )
            exit(13)
        else:
            exit(11)

    # Grab a single frame of video
    frame, gsframe = video_capture.read_frame()
    if gsframe.ndim != 2:
        gsframe = cv2.cvtColor(gsframe, cv2.COLOR_BGR2GRAY)
    if clahe_enabled(config):
        gsframe = clahe.apply(gsframe)

    # Create a histogram of the image with 8 values
    hist = cv2.calcHist([gsframe], [0], None, [8], [0, 256])
    # All values combined for percentage calculation
    hist_total = np.sum(hist)

    # If the image is fully black due to a bad camera read,
    # skip to the next frame
    if hist_total == 0:
        black_tries += 1
        continue

    # Calculate frame darkness
    darkness = hist[0] / hist_total * 100

    if darkness == 100:
        black_tries += 1
        continue

    dark_running_total += darkness
    valid_frames += 1

    # If the image exceeds darkness threshold due to subject distance,
    # skip to the next frame
    if darkness > dark_threshold:
        dark_tries += 1
        continue

    # If the height is too high
    if scaling_factor != 1:
        # Apply that factor to the frame
        gsframe = cv2.resize(
            gsframe,
            None,
            fx=scaling_factor,
            fy=scaling_factor,
            interpolation=cv2.INTER_AREA,
        )

    # If camera is configured to rotate = 1, check portrait in addition to landscape
    if rotate == 1:
        if frames % 3 == 1:
            gsframe = cv2.rotate(gsframe, cv2.ROTATE_90_COUNTERCLOCKWISE)
        if frames % 3 == 2:
            gsframe = cv2.rotate(gsframe, cv2.ROTATE_90_CLOCKWISE)

    # If camera is configured to rotate = 2, check portrait orientation
    elif rotate == 2:
        if frames % 2 == 0:
            gsframe = cv2.rotate(gsframe, cv2.ROTATE_90_COUNTERCLOCKWISE)
        else:
            gsframe = cv2.rotate(gsframe, cv2.ROTATE_90_CLOCKWISE)

    frame = active_face_model.prepare_frame(gsframe)

    # If snapshots have been turned on
    if save_failed or save_successful:
        # Start capturing frames for the snapshot
        if len(snapframes) < 3:
            snapframes.append(frame)

    # Get all faces from that frame as encodings
    face_locations = active_face_model.detect(frame)
    for face in face_locations:
        face_encoding = active_face_model.encode(frame, face)
        match = active_face_model.best_match(known_encodings, face_encoding)
        best_score = update_best_score(
            best_score, match.score, active_face_model.metric
        )

        # Check if a match that's confident enough
        if match.accepted:
            timings["tt"] = time.time() - timings["st"]
            timings["fl"] = time.time() - timings["fr"]

            # If set to true in the config, print debug text
            if end_report:

                def print_timing(label, k):
                    """Helper function to print a timing from the list"""
                    print("  %s: %dms" % (label, round(timings[k] * 1000)))

                # Print a nice timing report
                print(_("Time spent"))
                print_timing(_("Starting up"), "in")
                print(
                    _("  Open cam + load libs: %dms")
                    % (
                        round(
                            max(timings["ll"], timings["ic"]) * 1000,
                        )
                    )
                )
                print_timing(_("  Opening the camera"), "ic")
                print_timing(_("  Importing recognition libs"), "ll")
                print_timing(_("Searching for known face"), "fl")
                print_timing(_("Total time"), "tt")

                print(_("\nResolution"))
                width = video_capture.fw or 1
                print(_("  Native: %dx%d") % (height, width))
                # Save the new size for diagnostics
                scale_height, scale_width = frame.shape[:2]
                print(_("  Used: %dx%d") % (scale_height, scale_width))

                # Show the total number of frames and calculate the FPS by dividing it by the total scan time
                print(
                    _("\nFrames searched: %d (%.2f fps)")
                    % (frames, frames / timings["fl"])
                )
                print(_("Black frames ignored: %d ") % (black_tries,))
                print(_("Dark frames ignored: %d ") % (dark_tries,))
                print(_("Winning score: %.3f") % (match.score,))

                winning_model = encoding_models[match.index]
                print(
                    _('Winning model: %d ("%s")')
                    % (winning_model["id"], winning_model["label"])
                )

            # Make snapshot if enabled
            if save_successful:
                make_snapshot(
                    _("SUCCESSFUL"),
                    snapframes,
                    frames,
                    best_score or 0.0,
                    time.time() - timings["fr"],
                )

            # End peacefully
            exit(0)

    if exposure != -1:
        # For a strange reason on some cameras (e.g. Lenoxo X1E) setting manual exposure works only after a couple frames
        # are captured and even after a delay it does not always work. Setting exposure at every frame is reliable though.
        video_capture.internal.set(cv2.CAP_PROP_AUTO_EXPOSURE, 1.0)  # 1 = Manual
        video_capture.internal.set(cv2.CAP_PROP_EXPOSURE, float(exposure))
