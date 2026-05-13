import os
import sys
import unittest
from unittest.mock import MagicMock, patch

sys.path.insert(
    0, os.path.join(os.path.dirname(__file__), "..", "..", "howdy", "src", "lib")
)

mock_ffmpeg = MagicMock()
sys.modules["ffmpeg"] = mock_ffmpeg

from cv2 import CAP_PROP_FRAME_HEIGHT, CAP_PROP_FRAME_WIDTH  # noqa: E402
from recorders.ffmpeg_reader import ffmpeg_reader  # type: ignore[import]


class TestFfmpegReader(unittest.TestCase):
    def setUp(self):
        self.device_path = "/dev/video0"
        self.device_format = "v4l2"

    @patch("recorders.ffmpeg_reader.Popen")
    @patch("recorders.ffmpeg_reader.ffmpeg.probe")
    def test_probe_with_int_from_ffmpeg_probe(self, mock_ffmpeg_probe, mock_popen):
        mock_process = MagicMock()
        mock_process.poll.return_value = 1
        mock_process.communicate.return_value = (b"", b"")
        mock_popen.return_value = mock_process

        mock_probe_result = {"streams": [{"height": 480, "width": 640}]}
        mock_ffmpeg_probe.return_value = mock_probe_result

        reader = ffmpeg_reader(self.device_path, self.device_format)
        reader.probe()

        self.assertEqual(reader.height, 480)
        self.assertEqual(reader.width, 640)

    @patch("recorders.ffmpeg_reader.Popen")
    @patch("recorders.ffmpeg_reader.ffmpeg.probe")
    def test_probe_with_regex_parsing(self, mock_ffmpeg_probe, mock_popen):
        mock_process = MagicMock()
        mock_process.poll.return_value = 1
        mock_process.communicate.return_value = (b"", b"  Format: YUV420P 640x480")
        mock_popen.return_value = mock_process

        mock_ffmpeg_probe.return_value = {"streams": []}
        mock_ffmpeg_probe.side_effect = Exception("Should not be called")

        reader = ffmpeg_reader(self.device_path, self.device_format)
        reader.probe()

        self.assertEqual(reader.height, 640)
        self.assertEqual(reader.width, 480)

    @patch("recorders.ffmpeg_reader.Popen")
    @patch("recorders.ffmpeg_reader.ffmpeg.probe")
    def test_probe_with_zero_dimensions(self, mock_ffmpeg_probe, mock_popen):
        mock_process = MagicMock()
        mock_process.poll.return_value = 1
        mock_process.communicate.return_value = (b"", b"")
        mock_popen.return_value = mock_process

        mock_probe_result = {"streams": [{"height": 0, "width": 0}]}
        mock_ffmpeg_probe.return_value = mock_probe_result

        reader = ffmpeg_reader(self.device_path, self.device_format)
        reader.probe()

        self.assertEqual(reader.height, 0)
        self.assertEqual(reader.width, 0)

    @patch("recorders.ffmpeg_reader.Popen")
    @patch("recorders.ffmpeg_reader.ffmpeg.probe")
    def test_probe_does_not_override_existing_dimensions(
        self, mock_ffmpeg_probe, mock_popen
    ):
        mock_process = MagicMock()
        mock_process.poll.return_value = 1
        mock_process.communicate.return_value = (b"", b"")
        mock_popen.return_value = mock_process

        mock_probe_result = {"streams": [{"height": 720, "width": 1280}]}
        mock_ffmpeg_probe.return_value = mock_probe_result

        reader = ffmpeg_reader(self.device_path, self.device_format)
        reader.set(CAP_PROP_FRAME_HEIGHT, 480)
        reader.set(CAP_PROP_FRAME_WIDTH, 640)
        reader.probe()

        self.assertEqual(reader.height, 480)
        self.assertEqual(reader.width, 640)


if __name__ == "__main__":
    unittest.main()
