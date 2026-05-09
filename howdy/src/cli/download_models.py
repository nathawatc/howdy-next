# Download OpenCV YuNet and SFace ONNX models

from __future__ import annotations

import os
import tempfile
import urllib.request

import paths_factory
from core.detector import (
    SFACE_MODEL,
    SFACE_URL,
    YUNET_MODEL,
    YUNET_URL,
    bad_model_download,
)
from i18n import _

MODELS = [
    (
        YUNET_MODEL,
        YUNET_URL,
        paths_factory.yunet_model_path(),
    ),
    (
        SFACE_MODEL,
        SFACE_URL,
        paths_factory.sface_model_path(),
    ),
]

os.makedirs(paths_factory.models_dir_path(), exist_ok=True)

for name, url, destination in MODELS:
    if os.path.exists(destination) and not bad_model_download(destination):
        print(_("Model already exists: ") + destination)
        continue
    if os.path.exists(destination):
        print(_("Replacing invalid model download: ") + destination)

    print(_("Downloading {name}").format(name=name))
    fd, temp_path = tempfile.mkstemp(
        dir=paths_factory.models_dir_path(),
        prefix=name + ".",
        suffix=".tmp",
    )
    os.close(fd)
    try:
        urllib.request.urlretrieve(url, temp_path)
        if bad_model_download(temp_path):
            raise RuntimeError(_("Downloaded file is not an ONNX model: ") + url)
        os.replace(temp_path, destination)
    except Exception:
        if os.path.exists(temp_path):
            os.unlink(temp_path)
        raise

print(_("OpenCV face models ready in: ") + paths_factory.models_dir_path())
