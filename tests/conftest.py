import configparser
import pytest
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "howdy", "src"))

@pytest.fixture
def mock_config():
    config = configparser.ConfigParser()
    config.read_dict({
        "video": {
            "device_path": "/dev/video0",
            "recording_plugin": "opencv",
            "width": 640,
            "height": 480,
        },
        "core": {
            "use_cnn": False,
        },
    })
    return config