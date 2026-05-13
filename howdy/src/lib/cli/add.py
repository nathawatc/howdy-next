from __future__ import annotations

import builtins
import json
import os
import sys
import tempfile
import time

import cv2
import numpy as np
import paths_factory
from config_utils import load_config
from core.detector import BACKEND_NAME, FaceModel, clahe_enabled, create_clahe
from i18n import _
from recorders.video_capture import VideoCapture

config = load_config()

face_model = FaceModel(config)

howdy_args = getattr(builtins, "howdy_args")
user = str(getattr(builtins, "howdy_user"))
enc_file = paths_factory.user_model_path(user)
encodings = []

if not os.path.exists(paths_factory.user_models_dir_path()):
    print(_("No face model folder found, creating one"))
    os.makedirs(paths_factory.user_models_dir_path())

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

if len(encodings) > 3:
    print(
        _(
            "NOTICE: Each additional model slows down the face recognition engine slightly"
        )
    )
    print(_("Press Ctrl+C to cancel\n"))

if not howdy_args.plain:
    print(_("Adding face model for the user ") + user)

label = "Initial model"

next_id = encodings[-1]["id"] + 1 if encodings else 0

if howdy_args.arguments:
    label = howdy_args.arguments[0]
else:
    label = _("Model #") + str(next_id)

if howdy_args.y:
    print(_('Using default label "%s" because of -y flag') % (label,))
else:
    label_in = input(_("Enter a label for this new model [{}]: ").format(label))

    if label_in != "":
        label = label_in[:24].replace("\n", "").replace("\r", "")

# Remove illegal characters
if "," in label:
    print(_('NOTICE: Removing illegal character "," from model name'))
    label = label.replace(",", "")

insert_model = {
    "time": int(time.time()),
    "label": label,
    "id": next_id,
    "backend": BACKEND_NAME,
    "metric": face_model.metric,
    "model": "face_recognition_sface_2021dec_int8bq.onnx",
    "data": [],
}

video_capture = VideoCapture(config)

print(_("\nPlease look straight into the camera"))

time.sleep(2)

frames = 0
valid_frames = 0
dark_tries = 0
dark_running_total = 0
face_locations: list[np.ndarray] = []
frame = None

dark_threshold = config.getfloat("video", "dark_threshold", fallback=60)
clahe = create_clahe(config)

while frames < 60 and not face_locations:
    frames += 1
    frame, gsframe = video_capture.read_frame()
    if gsframe.ndim != 2:
        gsframe = cv2.cvtColor(gsframe, cv2.COLOR_BGR2GRAY)
    if clahe_enabled(config):
        gsframe = clahe.apply(gsframe)

    hist = cv2.calcHist([gsframe], [0], None, [8], [0, 256])
    hist_total = np.sum(hist)

    darkness = hist[0] / hist_total * 100
    if (hist_total == 0) or (darkness == 100):
        continue

    dark_running_total += darkness
    valid_frames += 1

    if darkness > dark_threshold:
        dark_tries += 1
        continue

    # YuNet expects 3-channel input. IR grayscale is broadcast to BGR.
    frame = face_model.prepare_frame(gsframe)
    face_locations = face_model.detect(frame)

    if face_locations:
        break

video_capture.release()

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

elif len(face_locations) > 1:
    print(_("Multiple faces detected, aborting"))
    sys.exit(1)

if frame is None:
    print(_("No valid frame captured, aborting"))
    sys.exit(1)

face_location = face_locations[0]
face_encoding = face_model.encode(frame, face_location)

insert_model["data"].append(face_encoding.tolist())

encodings.append(insert_model)

fd, tmp_path = tempfile.mkstemp(dir=os.path.dirname(enc_file), suffix=".tmp")
try:
    with os.fdopen(fd, "w") as datafile:
        json.dump(encodings, datafile)
    os.replace(tmp_path, enc_file)
except Exception:
    if os.path.exists(tmp_path):
        os.unlink(tmp_path)
    raise

print(
    _("""\nScan complete
Added a new model to """)
    + user
)
