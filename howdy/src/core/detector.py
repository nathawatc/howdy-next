# Shared OpenCV DNN face detection and encoding utilities

from __future__ import annotations

import configparser
import os
import sys
from dataclasses import dataclass

import cv2
import numpy as np
import paths_factory
from i18n import _

BACKEND_NAME = "opencv_dnn_sface"
YUNET_MODEL = "face_detection_yunet_2023mar_int8bq.onnx"
SFACE_MODEL = "face_recognition_sface_2021dec_int8bq.onnx"
YUNET_URL = (
    "https://huggingface.co/opencv/face_detection_yunet/resolve/main/" + YUNET_MODEL
)
SFACE_URL = (
    "https://huggingface.co/opencv/face_recognition_sface/resolve/main/" + SFACE_MODEL
)


@dataclass(frozen=True)
class FaceMatch:
    """Best match metadata for one detected face."""

    index: int
    score: float
    accepted: bool


def _face_detector_create(
    model_path: str,
    input_size: tuple[int, int],
    score_threshold: float,
    nms_threshold: float,
    top_k: int,
):
    try:
        if hasattr(cv2, "FaceDetectorYN") and hasattr(cv2.FaceDetectorYN, "create"):
            return cv2.FaceDetectorYN.create(
                model_path,
                "",
                input_size,
                score_threshold,
                nms_threshold,
                top_k,
            )
        if hasattr(cv2, "FaceDetectorYN_create"):
            return cv2.FaceDetectorYN_create(
                model_path,
                "",
                input_size,
                score_threshold,
                nms_threshold,
                top_k,
            )
    except cv2.error as err:
        _print_model_parse_error(model_path, err)
        sys.exit(1)
    print(_("OpenCV was built without FaceDetectorYN support"))
    sys.exit(1)


def _print_model_parse_error(model_path: str, err: cv2.error) -> None:
    print(_("OpenCV could not parse face model:") + " " + model_path)
    print(_("OpenCV version:") + " " + cv2.__version__)
    if "int8bq" in os.path.basename(model_path).lower():
        print(
            _(
                "INT8BQ OpenCV Zoo models need newer OpenCV DNN support; OpenCV 4.x may fail here."
            )
        )
        print(_("Use OpenCV 5.x-pre/newer, or configure FP32 models instead."))
    print(str(err))


def _face_recognizer_create(model_path: str):
    try:
        if hasattr(cv2, "FaceRecognizerSF") and hasattr(cv2.FaceRecognizerSF, "create"):
            return cv2.FaceRecognizerSF.create(model_path, "")
        if hasattr(cv2, "FaceRecognizerSF_create"):
            return cv2.FaceRecognizerSF_create(model_path, "")
    except cv2.error as err:
        _print_model_parse_error(model_path, err)
        sys.exit(1)
    print(_("OpenCV was built without FaceRecognizerSF support"))
    sys.exit(1)


def _read_config(config: configparser.ConfigParser | None) -> configparser.ConfigParser:
    if config is not None:
        return config
    loaded_config = configparser.ConfigParser()
    loaded_config.read(paths_factory.config_file_path())
    return loaded_config


def _model_path(
    config: configparser.ConfigParser,
    option: str,
    fallback: str,
) -> str:
    value = config.get("face", option, fallback=fallback).strip()
    if value in ("", "default", "none"):
        return fallback
    return value


def bad_model_download(path: str) -> bool:
    """Return True if path looks like an LFS pointer or HTML error page."""
    if not os.path.isfile(path):
        return True
    with open(path, "rb") as model_file:
        header = model_file.read(256)
    return header.startswith(
        b"version https://git-lfs.github.com/spec/v1"
    ) or header.lstrip().startswith(b"<")


def check_data_files(config: configparser.ConfigParser | None = None) -> bool:
    """Check if required OpenCV ONNX model files exist."""
    config = _read_config(config)
    yunet_model = _model_path(config, "yunet_model", paths_factory.yunet_model_path())
    sface_model = _model_path(config, "sface_model", paths_factory.sface_model_path())
    missing = [path for path in (yunet_model, sface_model) if not os.path.isfile(path)]
    invalid = [
        path
        for path in (yunet_model, sface_model)
        if os.path.isfile(path) and bad_model_download(path)
    ]
    if not missing and not invalid:
        return True

    if missing:
        print(_("OpenCV face model files are missing:"))
        for path in missing:
            print("\t" + path)
    if invalid:
        print(_("OpenCV face model files are invalid downloads:"))
        for path in invalid:
            print("\t" + path)
    print(_("Download them from:"))
    print("\t" + YUNET_URL)
    print("\t" + SFACE_URL)
    print(_("Place them in:") + " " + paths_factory.models_dir_path())
    return False


class FaceModel:
    """OpenCV YuNet detector and SFace encoder."""

    backend = BACKEND_NAME

    def __init__(
        self,
        legacy_arg: bool | configparser.ConfigParser = False,
        config: configparser.ConfigParser | None = None,
    ) -> None:
        # Keep first positional arg compatible with older FaceModel(config) call sites.
        if isinstance(legacy_arg, configparser.ConfigParser):
            config = legacy_arg
        self.config = _read_config(config)

        if not check_data_files(self.config):
            sys.exit(1)

        self.yunet_model = _model_path(
            self.config, "yunet_model", paths_factory.yunet_model_path()
        )
        self.sface_model = _model_path(
            self.config, "sface_model", paths_factory.sface_model_path()
        )
        self.score_threshold = self.config.getfloat(
            "face", "yunet_score_threshold", fallback=0.9
        )
        self.nms_threshold = self.config.getfloat(
            "face", "yunet_nms_threshold", fallback=0.3
        )
        self.top_k = self.config.getint("face", "yunet_top_k", fallback=5000)
        self.metric = self.config.get("face", "sface_metric", fallback="cosine").lower()
        self.threshold = self.config.getfloat(
            "face",
            "sface_threshold",
            fallback=0.363 if self.metric == "cosine" else 1.128,
        )
        self._input_size = (320, 320)
        self.detector = _face_detector_create(
            self.yunet_model,
            self._input_size,
            self.score_threshold,
            self.nms_threshold,
            self.top_k,
        )
        self.recognizer = _face_recognizer_create(self.sface_model)

    def __repr__(self) -> str:
        return "FaceModel(backend=opencv_dnn_sface)"

    def prepare_frame(self, frame: np.ndarray) -> np.ndarray:
        """Return 3-channel BGR frame suitable for YuNet and SFace."""
        if frame.ndim == 2:
            return cv2.cvtColor(frame, cv2.COLOR_GRAY2BGR)
        if frame.ndim == 3 and frame.shape[2] == 1:
            return cv2.cvtColor(frame, cv2.COLOR_GRAY2BGR)
        return frame

    def set_input_size_from_frame(self, frame: np.ndarray) -> None:
        height, width = frame.shape[:2]
        size = (int(width), int(height))
        if size != self._input_size:
            self.detector.setInputSize(size)
            self._input_size = size

    def detect(self, frame: np.ndarray) -> list[np.ndarray]:
        """Detect faces and return YuNet face rows."""
        frame = self.prepare_frame(frame)
        self.set_input_size_from_frame(frame)
        _, faces = self.detector.detect(frame)
        if faces is None:
            return []
        return [face.astype(np.float32) for face in faces]

    def encode(self, frame: np.ndarray, face: np.ndarray) -> np.ndarray:
        """Align face with YuNet landmarks and return SFace feature vector."""
        frame = self.prepare_frame(frame)
        aligned = self.recognizer.alignCrop(frame, face)
        feature = self.recognizer.feature(aligned)
        return np.asarray(feature, dtype=np.float32).reshape(-1)

    def detection_box(self, face: np.ndarray) -> tuple[int, int, int, int]:
        x, y, width, height = face[:4]
        return int(x), int(y), int(width), int(height)

    def detection_landmarks(self, face: np.ndarray) -> list[tuple[int, int]]:
        points = face[4:14].reshape(5, 2)
        return [(int(x), int(y)) for x, y in points]

    def detection_confidence(self, face: np.ndarray) -> float:
        return float(face[14]) if len(face) > 14 else 0.0

    def match_scores(self, known: np.ndarray, probe: np.ndarray) -> np.ndarray:
        known = np.asarray(known, dtype=np.float32)
        probe = np.asarray(probe, dtype=np.float32).reshape(-1)
        if known.ndim == 1:
            known = known.reshape(1, -1)

        if self.metric == "cosine":
            known_norm = np.linalg.norm(known, axis=1)
            probe_norm = np.linalg.norm(probe)
            denom = np.maximum(known_norm * probe_norm, 1e-12)
            return np.dot(known, probe) / denom
        return np.linalg.norm(known - probe, axis=1)

    def best_match(self, known: np.ndarray, probe: np.ndarray) -> FaceMatch:
        scores = self.match_scores(known, probe)
        if self.metric == "cosine":
            index = int(np.argmax(scores))
            score = float(scores[index])
            accepted = score >= self.threshold
        else:
            index = int(np.argmin(scores))
            score = float(scores[index])
            accepted = 0 < score <= self.threshold
        return FaceMatch(index=index, score=score, accepted=accepted)


def create_clahe(config: configparser.ConfigParser):
    clip_limit = config.getfloat("video", "clahe_clip_limit", fallback=2.0)
    tile_size = config.getint("video", "clahe_tile_grid_size", fallback=8)
    return cv2.createCLAHE(clipLimit=clip_limit, tileGridSize=(tile_size, tile_size))


def clahe_enabled(config: configparser.ConfigParser) -> bool:
    return config.getboolean("video", "clahe_enabled", fallback=True)
