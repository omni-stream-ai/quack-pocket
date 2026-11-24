from pathlib import Path
import subprocess
import tempfile
import unittest


class ReminderTime(unittest.TestCase):
    def test_local_schedule_and_repeat(self):
        with tempfile.TemporaryDirectory() as folder:
            binary = Path(folder) / "reminder_time"
            subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            "-I", "main/boards/waveshare-s3-epaper-1.54",
                            "tests/reminder_time_test.cc", "-o", binary], check=True)
            subprocess.run([binary], check=True)
