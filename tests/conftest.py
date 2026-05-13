import configparser
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "howdy", "src", "lib"))


@pytest.fixture
def mock_config():
    config = configparser.ConfigParser()
    config.read_dict(
        {
            "video": {
                "device_path": "/dev/video0",
                "width": 640,
                "height": 480,
            },
            "face": {
                "yunet_model": "default",
                "sface_model": "default",
                "sface_metric": "cosine",
                "sface_threshold": "0.363",
            },
        }
    )
    return config
