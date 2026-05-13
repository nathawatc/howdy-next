# Top level class for a video capture providing simplified API's for common
# functions

from __future__ import annotations

import configparser
import os
import sys
from typing import Optional, Tuple, Union

import cv2
import numpy
from i18n import _

# The internal recorder can be accessed with 'video_capture.internal'


class VideoCapture:
    config: configparser.ConfigParser
    internal: cv2.VideoCapture
    fw: Optional[int]
    fh: Optional[int]
    fps: Optional[int]

    def __init__(self, config: Union[str, configparser.ConfigParser]) -> None:
        """
        Creates a new VideoCapture instance depending on the settings in the
        provided config file.

        Config can either be a string to the path, or a pre-setup configparser.
        """

        if isinstance(config, str):
            self.config = configparser.ConfigParser()
            self.config.read(config)
        else:
            self.config = config

        device_path = self.config.get("video", "device_path")
        if device_path != "none" and not os.path.exists(device_path):
            if self.config.getboolean("video", "warn_no_device", fallback=True):
                print(
                    _(
                        "Howdy could not find a camera device at the path specified in the config file."
                    )
                )
                print(
                    _(
                        "It is very likely that the path is not configured correctly, please edit the 'device_path' config value by running:"
                    )
                )
                print("\n\tsudo howdy config\n")
            sys.exit(14)

        self.internal = None
        self.fw = None
        self.fh = None
        self._create_reader()

        # Request a frame to wake the camera up
        self.internal.grab()

    def __del__(self) -> None:
        try:
            self.internal.release()
        except AttributeError:
            pass  # Internal was never initialized, nothing to release

    def release(self) -> None:
        self.internal.release()

    def read_frame(self) -> Tuple[numpy.ndarray, numpy.ndarray]:
        """
        Reads a frame, returns the frame and an attempted grayscale conversion of
        the frame in a tuple:

        (frame, grayscale_frame)

        If the grayscale conversion fails, both items in the tuple are identical.
        """

        ret, frame = self.internal.read()
        if not ret:
            print(
                _(
                    "Failed to read camera specified in the 'device_path' config option, aborting"
                )
            )
            sys.exit(14)

        try:
            gsframe = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        except RuntimeError:
            gsframe = frame
        except cv2.error:
            print("\nAn error occurred in OpenCV\n")
            raise
        return frame, gsframe

    def _create_reader(self) -> None:
        """
        Sets up the video reader instance
        """
        recording_plugin = self.config.get(
            "video", "recording_plugin", fallback="opencv"
        )
        if recording_plugin != "opencv":
            print(
                _(
                    "Only the OpenCV recorder is supported in this version, forcing recording_plugin=opencv"
                )
            )

        self.internal = cv2.VideoCapture(
            self.config.get("video", "device_path"), cv2.CAP_V4L
        )
        # Without this OpenCV uses the first detected (possibly lower) frame rate.
        # Use 0 as fallback to avoid breaking existing setups.
        self.fps = self.config.getint("video", "device_fps", fallback=0)
        if self.fps != 0:
            self.internal.set(cv2.CAP_PROP_FPS, self.fps)

        if self.config.getboolean("video", "force_mjpeg", fallback=False):
            # Magic number enables MJPEG; badly documented in OpenCV.
            self.internal.set(cv2.CAP_PROP_FOURCC, 1196444237)

        # Set the frame width and height if requested
        self.fw = self.config.getint("video", "frame_width", fallback=-1)
        self.fh = self.config.getint("video", "frame_height", fallback=-1)
        if self.fw != -1:
            self.internal.set(cv2.CAP_PROP_FRAME_WIDTH, self.fw)
        if self.fh != -1:
            self.internal.set(cv2.CAP_PROP_FRAME_HEIGHT, self.fh)
