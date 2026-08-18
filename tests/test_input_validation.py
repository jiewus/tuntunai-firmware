import os
import subprocess
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parent.parent


class InputValidationTest(unittest.TestCase):
    def test_native_validation_cases(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "input_validation_test"
            compiler = os.environ.get("CXX", "c++")
            subprocess.run(
                [
                    compiler,
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(PROJECT_ROOT / "main"),
                    str(PROJECT_ROOT / "main/system/input_validation.cc"),
                    str(PROJECT_ROOT / "tests/input_validation_test.cc"),
                    "-o",
                    str(executable),
                ],
                check=True,
            )
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
