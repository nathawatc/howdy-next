# AGENTS.md - Tests/

## OVERVIEW

pytest test infrastructure for howdy-next, with fixtures for ConfigParser mocking and path injection
from project root.

## WHERE TO LOOK

| Item          | Location                              | Notes                                |
| ------------- | ------------------------------------- | ------------------------------------ |
| conftest.py   | tests/conftest.py                     | mock_config fixture for ConfigParser |
| ffmpeg tests  | tests/recorders/test_ffmpeg_reader.py | unittest.TestCase with mock patches  |
| test fixtures | tests/conftest.py                     | pytest fixtures                      |

## CONVENTIONS

- Import path from project root: `sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "howdy", "src", "lib"))`
- Use `unittest.TestCase` for recorder tests
- Mock modules via `sys.modules["module_name"] = mock_object`
- ffmpeg_reader uses `@patch` decorators for `Popen` and `ffmpeg.probe`
- Dimensions from ffmpeg probe are `int` (cast with `int()` if needed)

## ANTI-PATTERNS

- Do NOT use `fileinput.input()` in tests (race condition pattern)
- Do NOT import modules before setting up `sys.modules` mock
- Do NOT hardcode `/dev/video0` in tests without making it configurable via fixture
