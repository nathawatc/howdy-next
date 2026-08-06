# Third-Party Notices

Howdy Next includes adapted documentation and downloads model files maintained
by third parties. These materials remain subject to their own licenses and are
not relicensed under the Howdy Next project license.

## Contributor Covenant

Material:

- `CODE_OF_CONDUCT.md`

Source:

- Contributor Covenant, version 2.0
- <https://www.contributor-covenant.org/version/2/0/code_of_conduct/>

License:

- Creative Commons Attribution 4.0 International (`CC-BY-4.0`)
- <https://creativecommons.org/licenses/by/4.0/>

The Howdy Next version was shortened and modified. Its attribution section
also credits Mozilla's code-of-conduct enforcement ladder.

## YuNet Face-Detection Model

Artifact:

- `face_detection_yunet_2026may.onnx`
- SHA-256: `ebafce4e3c118d6554634be5c27ab333b4c047a9a8c3faf1d7cf93101c22f0f0`

Source:

- OpenCV Zoo, revision `26cc381e4d2594bb9f47a26eb8fd96c94a13660d`
- <https://github.com/opencv/opencv_zoo/tree/26cc381e4d2594bb9f47a26eb8fd96c94a13660d/models/face_detection_yunet>

License:

- MIT
- Copyright (c) 2020 Shiqi Yu <shiqi.yu@gmail.com>
- Full text: `third_party/licenses/YUNET-MIT.txt` in source distributions;
  installed as `YUNET-MIT.txt` beside this notice.

Howdy Next does not bundle this model in the source tree. The
`howdy download-models` command downloads the pinned artifact.

## SFace Face-Recognition Model

Artifact:

- `face_recognition_sface_2021dec_int8.onnx`
- SHA-256: `2b0e941e6f16cc048c20aee0c8e31f569118f65d702914540f7bfdc14048d78a`

Source:

- OpenCV Zoo, revision `088c3571ec70df15100a5e4c26894d95951e92e9`
- <https://github.com/opencv/opencv_zoo/tree/088c3571ec70df15100a5e4c26894d95951e92e9/models/face_recognition_sface>

License:

- Apache License 2.0 (`Apache-2.0`)
- Full text: `third_party/licenses/SFACE-APACHE-2.0.txt` in source distributions;
  installed as `SFACE-APACHE-2.0.txt` beside this notice.

Howdy Next does not bundle this model in the source tree. The
`howdy download-models` command downloads the pinned artifact. The pinned
SFace directory does not contain an additional `NOTICE` file.
