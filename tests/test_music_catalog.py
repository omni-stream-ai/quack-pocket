import random
import subprocess
import tempfile
import unittest
from pathlib import Path

from scripts.index_music import write_index


class MusicCatalog(unittest.TestCase):
    def test_voice_media_candidates(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            obj = folder / "reader.o"
            reader = folder / "select"
            board = "main/boards/waveshare-s3-epaper-1.54"
            subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-c",
                            "main/music/music_index.c", "-o", obj], check=True)
            subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", "main", "-I", board,
                            "tests/media_selection_test.cc", f"{board}/media_selection.cc", obj, "-o", reader], check=True)
            card = folder / "card"
            for path in ("音乐/周杰伦/晴天.mp3", "音乐/钢琴曲/晴天.mp3", "音乐/儿歌/晴天娃娃.mp3",
                         "音乐/A/童谣.mp3", "音乐/B/童谣.mp3", "音乐/C/童谣.mp3", "音乐/现场/童谣.mp3",
                         "晨光.mp3", "音乐/现场/晨光.mp3",
                         "故事/原版/格林童话/第一章.mp3", "故事/少儿/格林童话/第一章.mp3",
                         "故事/少儿/格林童话/第二章.mp3", "故事/少儿/格林童话/附录/朗读.mp3",
                         "故事/少儿/格林童话/第三章.mp3"):
                target = card / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(bytes(10))
            write_index(card)
            subprocess.run([reader, card / "omni.idx"], check=True)

    def test_sorted_paths_and_interrupted_rescan(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            card = folder / "card"
            card.mkdir()
            reader = folder / "check"
            obj = folder / "reader.o"
            subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-c",
                            "main/music/music_index.c", "-o", obj], check=True)
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", "main/music",
                            "tests/catalog_check.cc", "main/music/build_index.cc", obj, "-o", reader], check=True)
            catalog = card / ".omni-music" / "catalog-v2"
            (card / "empty.mp3").touch()
            for count in (0, 1, 32, 33, 65, 2000):
                order = list(range(count))
                random.Random(7).shuffle(order)
                for number in order:
                    path = card / f"音乐{number % 23}" / f"专辑{number % 7}" / f"{number}.MP3"
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(bytes(10))
                subprocess.run([reader, card, str(count), "-1"], check=True)
                self.assertEqual(write_index(card), count)
                self.assertEqual(catalog.read_bytes(), (card / "omni.idx").read_bytes())
            original = catalog.read_bytes()
            (card / "new.mp3").write_bytes(bytes(10))
            interrupted = subprocess.run([reader, card, "2001", "2"])
            self.assertEqual(interrupted.returncode, 2)
            self.assertEqual(catalog.read_bytes(), original)
            # A power cut can leave the old catalog under its recovery name.
            catalog.rename(catalog.with_name("previous-v2"))
            subprocess.run([reader, card, "2001", "-1"], check=True)
            self.assertFalse(catalog.with_name("previous-v2").exists())
            (card / "1.MP3").write_bytes(bytes(10))
            subprocess.run([reader, card, "2002", "-1"], check=True)
            for count, title in enumerate(("晴天娃娃", "放晴天际", "晴天"), start=2003):
                (card / (title + ".mp3")).write_bytes(bytes(10))
                subprocess.run([reader, card, str(count), "-1"], check=True)
