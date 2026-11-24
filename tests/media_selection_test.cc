#include "media_selection.h"

#include <cassert>

int main(int argc, char** argv) {
    assert(argc == 2);
    music_index_t index{};
    assert(music_index_open(&index, argv[1]));
    auto find = [&](bool story, const std::string& title, const std::vector<std::string>& words = {}) {
        uint32_t first, end;
        assert(music_index_directory(&index, story ? "故事" : "音乐", &first, &end));
        return media_selection::Find(index, first, end, story, title, words, [] { return true; });
    };
    auto songs = find(false, "晴天");
    assert(songs.status == MUSIC_TITLE_AMBIGUOUS && songs.candidates.size() == 2);
    auto prompt = media_selection::Prompt(songs.candidates, false, false);
    assert(prompt.find("第一项") != prompt.npos && prompt.find("钢琴曲") != prompt.npos);
    auto piano = find(false, "晴天", {"钢琴版"});
    assert(piano.status == MUSIC_TITLE_FOUND && piano.candidates.front().path == "音乐/钢琴曲/晴天.mp3");
    assert(find(false, "晴天", {"儿歌"}).status == MUSIC_TITLE_MISSING);
    assert(find(false, "童谣").candidates.size() > media_selection::visible_count);
    auto live = find(false, "童谣", {"现场"});
    assert(live.status == MUSIC_TITLE_FOUND && live.candidates.front().path == "音乐/现场/童谣.mp3");
    auto root = media_selection::Find(index, 0, index.count, false, "晨光.mp3", {}, [] { return true; });
    assert(root.status == MUSIC_TITLE_AMBIGUOUS && root.candidates.size() == 2);

    auto books = find(true, "格林童话");
    assert(books.status == MUSIC_TITLE_AMBIGUOUS && books.candidates.size() == 2);
    assert(books.candidates[0].book && books.candidates[1].book);
    auto book = find(true, "故事/少儿/格林童话");
    assert(book.status == MUSIC_TITLE_FOUND && book.candidates.front().book);
    auto chapters = find(true, "第一章");
    assert(chapters.status == MUSIC_TITLE_AMBIGUOUS && chapters.candidates.size() == 2);
    auto chapter = find(true, "第一章", {"少儿版"});
    assert(chapter.status == MUSIC_TITLE_FOUND && !chapter.candidates.front().book);
    assert(chapter.candidates.front().path == "故事/少儿/格林童话/第一章.mp3");
    assert(find(false, "第一章").status == MUSIC_TITLE_MISSING);

    assert(media_selection::ChoiceNumber(media_selection::ChoiceKeyword("请播放第二首。")) == 2);
    assert(media_selection::ChoiceNumber(media_selection::ChoiceKeyword("第３项")) == 3);
    assert(media_selection::ChoiceNumber("第二章") == 0);
    music_index_close(&index);
}
