#include "media_selection.h"

#include <algorithm>
#include <string_view>
#include <strings.h>

namespace media_selection {
namespace {
std::string_view BaseName(std::string_view path) {
    return path.substr(path.rfind('/') + 1);
}

std::string_view Parent(std::string_view path) {
    const auto slash = path.rfind('/');
    return slash == path.npos ? std::string_view() : path.substr(0, slash);
}

bool IsMp3(std::string_view name) {
    return name.size() >= 4 && !strncasecmp(name.data() + name.size() - 4, ".mp3", 4);
}

std::string Trim(std::string value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == value.npos) return {};
    return value.substr(start, value.find_last_not_of(" \t\r\n") - start + 1);
}

bool HasKeyword(std::string_view path, std::string_view keyword) {
    if (music_name_match(path.data(), path.size(), keyword.data(), keyword.size())) return true;
    for (std::string_view suffix : {"的版本", "版本", "版", "那个", "那個", "那首", "那本", "里的", "裡的"}) {
        if (keyword.size() > suffix.size() && keyword.ends_with(suffix)) {
            return music_name_match(path.data(), path.size(), keyword.data(), keyword.size() - suffix.size()) != 0;
        }
    }
    return false;
}

std::string ShortLabel(std::string_view label, bool keep_end = false) {
    constexpr size_t max_length = 18;
    size_t count = 0, prefix_end = 0;
    auto starts_character = [&](size_t i) { return (static_cast<unsigned char>(label[i]) & 0xc0) != 0x80; };
    for (size_t i = 0; i < label.size(); ++i) {
        if (starts_character(i) && ++count == max_length) prefix_end = i;
    }
    if (count <= max_length) return std::string(label);
    if (!keep_end) return std::string(label.substr(0, prefix_end)) + "…";
    size_t suffix_start = label.size();
    for (size_t remaining = max_length - 1; remaining != 0;) {
        if (starts_character(--suffix_start)) --remaining;
    }
    return "…" + std::string(label.substr(suffix_start));
}
} // namespace

Matches Find(music_index_t& index, uint32_t first, uint32_t end, bool story, const std::string& title,
             const std::vector<std::string>& keywords, const std::function<bool()>& keep_searching) {
    const bool full_path = title.find('/') != title.npos;
    const bool file_title = IsMp3(title);
    uint32_t track;
    if (full_path && file_title && keywords.empty() &&
        music_index_find(&index, title.c_str(), &track) && track >= first && track < end) {
        return {MUSIC_TITLE_FOUND, {{title, false}}};
    }
    if (full_path && (!story || file_title)) return {MUSIC_TITLE_MISSING, {}};
    if (full_path) {
        uint32_t book_first, book_end;
        if (!music_index_directory(&index, title.c_str(), &book_first, &book_end)) return {MUSIC_TITLE_INDEX_ERROR, {}};
        first = std::max(first, book_first);
        end = std::min(end, book_end);
        if (first >= end) return {MUSIC_TITLE_MISSING, {}};
    }

    std::vector<Candidate> matches;
    int best_rank = 0;
    auto add = [&](std::string_view path, bool book, int rank) {
        if (!rank || rank < best_rank) return;
        if (rank > best_rank) {
            matches.clear();
            best_rank = rank;
        }
        if (matches.size() > visible_count ||
            !std::all_of(keywords.begin(), keywords.end(), [&](const auto& key) { return HasKeyword(path, key); }) ||
            std::any_of(matches.begin(), matches.end(), [&](const auto& item) { return item.book == book && item.path == path; })) return;
        matches.push_back({std::string(path), book});
    };
    auto visit = [&](const char* value) {
        std::string_view path(value);
        const auto book = Parent(path);
        if (full_path) {
            if (book == title) add(book, true, 2);
            return matches.empty();
        }
        if (story && !file_title && !book.empty()) {
            const auto name = BaseName(book);
            add(book, true, music_name_match(name.data(), name.size(), title.c_str(), title.size()));
        }
        const auto name = BaseName(path);
        if (IsMp3(name)) {
            const size_t length = title.size() - (file_title ? 4 : 0);
            add(path, false, music_name_match(name.data(), name.size() - 4, title.c_str(), length));
        }
        return best_rank != 2 || matches.size() <= visible_count;
    };
    struct Scan {
        decltype(visit)& visit_track;
        const std::function<bool()>& keep_searching;
    } scan{visit, keep_searching};
    const auto status = music_index_scan(&index, first, end, [](uint32_t, const char* path, void* context) {
        return static_cast<Scan*>(context)->visit_track(path);
    }, [](void* context) { return static_cast<Scan*>(context)->keep_searching(); }, &scan);
    if (status != MUSIC_TITLE_FOUND) return {status, {}};
    if (matches.empty()) return {MUSIC_TITLE_MISSING, {}};
    return {matches.size() == 1 ? MUSIC_TITLE_FOUND : MUSIC_TITLE_AMBIGUOUS, std::move(matches)};
}

std::string ChoiceKeyword(std::string choice) {
    choice = Trim(std::move(choice));
    for (std::string_view suffix : {"。", "！", "？", ".", "!", "?", "」", "”", "》"}) {
        if (choice.ends_with(suffix)) choice.resize(choice.size() - suffix.size());
    }
    for (std::string_view prefix : {"请", "請", "我想听", "我想聽", "我要听", "我要聽", "我选", "我選", "播放", "想听", "想聽",
                                    "我要", "选", "選", "听", "聽", "播", "「", "“", "《"}) {
        if (choice.size() > prefix.size() && choice.starts_with(prefix)) choice.erase(0, prefix.size());
    }
    return Trim(std::move(choice));
}

size_t ChoiceNumber(const std::string& choice) {
    std::string_view number(choice);
    if (number.starts_with("第")) number.remove_prefix(std::string_view("第").size());
    for (std::string_view suffix : {"个", "個", "项", "項", "首", "本", "号", "號"}) {
        if (number.ends_with(suffix)) {
            number.remove_suffix(suffix.size());
            break;
        }
    }
    constexpr std::string_view digits[] = {"一", "二", "三", "四", "五", "六", "七", "八", "九"};
    constexpr std::string_view full_width[] = {"１", "２", "３", "４", "５", "６", "７", "８", "９"};
    for (size_t i = 0; i < std::size(digits); ++i) {
        if (number == digits[i] || number == full_width[i] ||
            (number.size() == 1 && number[0] == static_cast<char>('1' + i))) return i + 1;
    }
    if (number == "两" || number == "兩") return 2;
    return 0;
}

std::string Prompt(const std::vector<Candidate>& candidates, bool story, bool retry) {
    const bool more = candidates.size() > visible_count;
    std::string prompt = retry ? "这个选项没有匹配，当前候选是：" :
        more ? "找到超过三项，先列出前三项：" :
        "找到" + std::to_string(candidates.size()) + (story ? "个故事候选：" : "首歌曲：");
    constexpr std::string_view numbers[] = {"第一项", "第二项", "第三项"};
    for (size_t i = 0; i < std::min(visible_count, candidates.size()); ++i) {
        const auto& candidate = candidates[i];
        auto name = BaseName(candidate.path);
        if (!candidate.book && IsMp3(name)) name.remove_suffix(4);
        auto parent = Parent(candidate.path);
        auto folder = BaseName(parent);
        for (size_t j = 0; j < std::min(visible_count, candidates.size()); ++j) {
            const auto& other = candidates[j];
            if (j != i && candidate.book == other.book && BaseName(other.path) == BaseName(candidate.path) &&
                BaseName(Parent(other.path)) == folder) folder = parent;
        }
        prompt += "\n";
        prompt += numbers[i];
        prompt += "，";
        if (story) prompt += candidate.book ? "书名 " : "章节 ";
        prompt += ShortLabel(name);
        if (!folder.empty()) {
            prompt += "，在 ";
            prompt += ShortLabel(folder, true);
        }
        prompt += "。";
    }
    prompt += more ? "\n可以说编号，或再说一个关键词。" : "\n你想听哪一项？";
    return prompt;
}
} // namespace media_selection
