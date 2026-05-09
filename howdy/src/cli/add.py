# Save the face of the user in encoded form

from __future__ import annotations

# Import required modules
import builtins
import configparser
import json
import os
import sys
import tempfile
import time

import cv2
import numpy as np
import paths_factory
from core.detector import BACKEND_NAME, FaceModel, clahe_enabled, create_clahe
from i18n import _
from recorders.video_capture import VideoCapture

config = configparser.ConfigParser()
config.read(paths_factory.config_file_path())

face_model = FaceModel(config)

howdy_args = getattr(builtins, "howdy_args")
user = str(getattr(builtins, "howdy_user"))
# The permanent file to store the encoded model in
enc_file = paths_factory.user_model_path(user)
# Known encodings
encodings = []

# Make the ./models folder if it doesn't already exist
if not os.path.exists(paths_factory.user_models_dir_path()):
    print(_("No face model folder found, creating one"))
    os.makedirs(paths_factory.user_models_dir_path())

# To try read a premade encodings file if it exists
try:
    with open(enc_file) as f:
        encodings = json.load(f)
except FileNotFoundError:
    encodings = []

# Previous backend descriptors are incompatible with SFace descriptors.
if any(model.get("backend") != BACKEND_NAME for model in encodings):
    print(_("Existing face models use an incompatible backend."))
    print(_("Please run `howdy clear` and enroll again with `howdy add`."))
    sys.exit(1)

# Print a warning if too many encodings are being added
if len(encodings) > 3:
    print(
        _(
            "NOTICE: Each additional model slows down the face recognition engine slightly"
        )
    )
    print(_("Press Ctrl+C to cancel\n"))

# Make clear what we are doing if not human
if not howdy_args.plain:
    print(_("Adding face model for the user ") + user)

# Set the default label
label = "Initial model"

# some id's can be skipped, but the last id is always the maximum
next_id = encodings[-1]["id"] + 1 if encodings else 0

# Get the label from the cli arguments if provided
if howdy_args.arguments:
    label = howdy_args.arguments[0]

# Or set the default label
else:
    label = _("Model #") + str(next_id)

# Keep de default name if we can't ask questions
if howdy_args.y:
    print(_('Using default label "%s" because of -y flag') % (label,))
else:
    # Ask the user for a custom label
    label_in = input(_("Enter a label for this new model [{}]: ").format(label))

    # Set the custom label (if any) and limit it to 24 characters
    if label_in != "":
        label = label_in[:24].replace("\n", "").replace("\r", "")

# Remove illegal characters
if "," in label:
    print(_('NOTICE: Removing illegal character "," from model name'))
    label = label.replace(",", "")

# Prepare the metadata for insertion
insert_model = {
    "time": int(time.time()),
    "label": label,
    "id": next_id,
    "backend": BACKEND_NAME,
    "metric": face_model.metric,
    "model": "face_recognition_sface_2021dec_int8bq.onnx",
    "data": [],
}

# Set up video_capture
video_capture = VideoCapture(config)

print(_("\nPlease look straight into the camera"))

# Give the user time to read
time.sleep(2)

# Count the number of read frames
frames = 0
# Count the number of illuminated read frames
valid_frames = 0
# Count the number of illuminated frames that
# were rejected for being too dark
dark_tries = 0
# Track the running darkness total
dark_running_total = 0
face_locations: list[np.ndarray] = []
frame = None

dark_threshold = config.getfloat("video", "dark_threshold", fallback=60)
clahe = create_clahe(config)

# Loop through frames till we hit a timeout or find a face
while frames < 60 and not face_locations:
    frames += 1
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

    # Calculate frame darkness
    darkness = hist[0] / hist_total * 100

    # If the image is fully black due to a bad camera read,
    # skip to the next frame
    if (hist_total == 0) or (darkness == 100):
        continue

    # Include this frame in calculating our average session brightness
    dark_running_total += darkness
    valid_frames += 1

    # If the image exceeds darkness threshold due to subject distance,
    # skip to the next frame
    if darkness > dark_threshold:
        dark_tries += 1
        continue

    # YuNet expects 3-channel input. IR grayscale is broadcast to BGR.
    frame = face_model.prepare_frame(gsframe)
    face_locations = face_model.detect(frame)

    # If we've found at least one, we can continue
    if face_locations:
        break

video_capture.release()

# If we've found no faces, try to determine why
if not face_locations:
    if valid_frames == 0:
        print(_("Camera saw only black frames - is IR emitter working?"))
    elif valid_frames == dark_tries:
        print(_("All frames were too dark, please check dark_threshold in config"))
        print(
            _("Average darkness: {avg}, Threshold: {threshold}").format(
                avg=str(dark_running_total / valid_frames),
                threshold=str(dark_threshold),
            )
        )
    else:
        print(_("No face detected, aborting"))
    sys.exit(1)

# If more than 1 faces are detected we can't know which one belongs to the user
elif len(face_locations) > 1:
    print(_("Multiple faces detected, aborting"))
    sys.exit(1)

if frame is None:
    print(_("No valid frame captured, aborting"))
    sys.exit(1)

face_location = face_locations[0]
face_encoding = face_model.encode(frame, face_location)

insert_model["data"].append(face_encoding.tolist())

# Insert full object into the list
encodings.append(insert_model)

# Save the new encodings to disk
fd, tmp_path = tempfile.mkstemp(dir=os.path.dirname(enc_file), suffix=".tmp")
try:
    with os.fdopen(fd, "w") as datafile:
        json.dump(encodings, datafile)
    os.replace(tmp_path, enc_file)
except Exception:
    if os.path.exists(tmp_path):
        os.unlink(tmp_path)
    raise

# Give let the user know how it went
print(
    _("""\nScan complete
Added a new model to """)
    + user
)
