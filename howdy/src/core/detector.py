# Shared face detection and encoding utilities
# All face detector/encoder initialization lives here to avoid duplication

from __future__ import annotations

import os
import sys

import dlib
import paths_factory
from i18n import _


def check_data_files() -> bool:
    """Check if required dlib data files exist. Print error and return False if missing."""
    if not os.path.isfile(paths_factory.shape_predictor_5_face_landmarks_path()):
        print(_("Data files have not been downloaded, please run the following commands:"))
        print("\n\tcd " + paths_factory.dlib_data_dir_path())
        print("\tsudo ./install.sh\n")
        return False
    return True


def init_face_detector(use_cnn: bool = False):
    """
    Initialize and return face detector.

    Args:
        use_cnn: If True, use CNN detector. Otherwise use HOG-based detector.

    Returns:
        Face detector instance (dlib.pipeline or dlib.fhog_object_detector)
    """
    if not check_data_files():
        sys.exit(1)

    if use_cnn:
        return dlib.cnn_face_detection_model_v1(
            paths_factory.mmod_human_face_detector_path()
        )
    else:
        return dlib.get_frontal_face_detector()


def init_pose_predictor():
    """
    Initialize and return pose predictor.

    Returns:
        Shape predictor for facial landmarks
    """
    if not check_data_files():
        sys.exit(1)

    return dlib.shape_predictor(
        paths_factory.shape_predictor_5_face_landmarks_path()
    )


def init_face_encoder():
    """
    Initialize and return face encoder.

    Returns:
        Face recognition model for encoding faces
    """
    if not check_data_files():
        sys.exit(1)

    return dlib.face_recognition_model_v1(
        paths_factory.dlib_face_recognition_resnet_model_v1_path()
    )


class FaceModel:
    """Convenience class that holds all three face processing components."""

    def __init__(self, use_cnn: bool = False) -> None:
        """
        Initialize all face processing models.

        Args:
            use_cnn: If True, use CNN detector. Otherwise use HOG-based detector.
        """
        self.detector = init_face_detector(use_cnn)
        self.predictor = init_pose_predictor()
        self.encoder = init_face_encoder()

    def __repr__(self) -> str:
        return f"FaceModel(detector={type(self.detector).__name__})"
