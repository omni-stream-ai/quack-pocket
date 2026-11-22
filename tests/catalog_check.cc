#include "build_index.h"
#include "music_index.h"

#include <cassert>
#include <cstdlib>
#include <string>

// Host harness for the same index builder and reader used by the device task.
int main(int argc, char** argv) {
    assert(argc == 4);
    const std::string root = argv[1];
    const size_t expected_count = std::stoul(argv[2]);
    int budget = std::stoi(argv[3]);
    bool built = BuildMusicIndex(root, [&budget]() {
        if (budget == 0) return false;
        if (budget > 0) --budget;
        return true;
    });
    if (!built) return 2;
    music_index_t index{};
    assert(music_index_open(&index, (root + "/.omni-music/catalog-v2").c_str()));
    assert(index.count == expected_count);
    char path[MUSIC_PATH_CAPACITY];
    std::string previous;
    for (uint32_t number = 0; number < index.count; ++number) {
        assert(music_index_get(&index, number, path));
        assert(previous < path);
        previous = path;
        uint32_t found;
        assert(music_index_find(&index, path, &found) && found == number);
    }
    uint32_t first, end;
    assert(music_index_directory(&index, "", &first, &end) && first == 0 && end == index.count);
    for (auto dir : {"音乐0", "音乐1", "音乐10", "音乐0/专辑0", "空目录"}) {
        assert(music_index_directory(&index, dir, &first, &end));
        for (uint32_t i = 0; i < index.count; ++i) {
            assert(music_index_get(&index, i, path));
            bool inside = std::string(path).rfind(std::string(dir) + "/",0) == 0;
            assert(inside == (first <= i && i < end));
        }
    }
    uint32_t found = UINT32_MAX;
    assert(music_index_find_title(&index, "missing", &found, nullptr, nullptr) == MUSIC_TITLE_MISSING);
    if (expected_count) {
        uint32_t expected;
        assert(music_index_find(&index, "音乐0/专辑0/0.MP3", &expected));
        assert(music_index_find_title(&index, "0", &found, nullptr, nullptr) == MUSIC_TITLE_FOUND && found == expected);
        assert(music_index_find_title(&index, "0.mp3", &found, nullptr, nullptr) == MUSIC_TITLE_FOUND);
        assert(music_index_find_title(&index, "0", &found, [](void*) { return false; }, nullptr) == MUSIC_TITLE_CANCELLED);
    }
    if (expected_count >= 2002) {
        assert(music_index_directory(&index,"音乐1",&first,&end));
        assert(music_index_find_title_in(&index,"1",&found,first,end,nullptr,nullptr) == MUSIC_TITLE_FOUND);
    }
    if (expected_count >= 2002)
        assert(music_index_find_title(&index, "1", &found, nullptr, nullptr) == MUSIC_TITLE_AMBIGUOUS);
    if (expected_count >= 2003) {
        auto result = music_index_find_title(&index, "晴天", &found, nullptr, nullptr);
        if (expected_count == 2004) {
            assert(result == MUSIC_TITLE_AMBIGUOUS);
        } else {
            assert(result == MUSIC_TITLE_FOUND && music_index_get(&index, found, path));
            assert(std::string(path) == (expected_count == 2003 ? "晴天娃娃.mp3" : "晴天.mp3"));
        }
    }
    assert(!music_index_get(&index, index.count, path));
    music_index_close(&index);
}
