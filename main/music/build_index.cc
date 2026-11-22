#include "build_index.h"
#include "music_index.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <string_view>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#ifdef ESP_PLATFORM
#include "ff.h"
#endif

namespace {
using File = std::unique_ptr<FILE, int(*)(FILE*)>;
File Open(const std::string& name, const char* mode) {
    return File(fopen(name.c_str(), mode), fclose);
}

bool PutNumber(FILE* output, uint32_t value) {
    unsigned char bytes[4] = {static_cast<unsigned char>(value), static_cast<unsigned char>(value >> 8),
        static_cast<unsigned char>(value >> 16), static_cast<unsigned char>(value >> 24)};
    return fwrite(bytes, 1, 4, output) == 4;
}

bool GetNumber(FILE* input, uint32_t& value) {
    unsigned char bytes[4];
    if (fread(bytes, 1, 4, input) != 4) return false;
    value = uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
    return true;
}

bool WriteName(FILE* out, const std::string& name) {
    return PutNumber(out, name.size()) && fwrite(name.data(), 1, name.size(), out) == name.size();
}

bool ReadName(FILE* in, std::string& name) {
    uint32_t size;
    if (!GetNumber(in, size) || size >= MUSIC_PATH_CAPACITY) return false;
    name.resize(size);
    return fread(name.data(), 1, size, in) == size;
}

// A run header holds its record count and its encoded byte length.
bool FlushBatch(FILE* output, std::vector<std::string>& batch) {
    uint32_t bytes = 0;
    std::sort(batch.begin(), batch.end());
    for (const auto& path : batch) bytes += 4 + path.size();
    if (!PutNumber(output, batch.size()) || !PutNumber(output, bytes)) return false;
    for (const auto& path : batch) if (!WriteName(output, path)) return false;
    batch.clear();
    return true;
}

bool Collect(const std::string& root, const std::string& work, uint32_t& runs,
             const std::function<bool()>& keep_scanning, int fat_drive) {
    auto directories = Open(work + "/dirs.tmp", "w+b");
    auto out = Open(work + "/a.tmp", "wb");
    if (!directories || !out || !WriteName(directories.get(), "")) return false;
    long next = 0;
    long tail = ftell(directories.get());
    std::vector<std::string> batch;
    constexpr size_t batch_size = 32;
    batch.reserve(batch_size);
    size_t entries = 0;
    while (next < tail) {
        std::string folder;
        if (fseek(directories.get(), next, SEEK_SET) || !ReadName(directories.get(), folder)) return false;
        next = ftell(directories.get());
        if (next < 0 || fseek(directories.get(), tail, SEEK_SET)) return false;
        auto collect_entry = [&](std::string_view name, bool is_dir, bool regular) {
            if (++entries % 64 == 0 && !keep_scanning()) return false;
            if (name == "." || name == ".." || (folder.empty() && name == ".omni-music")) return true;
            std::string path = folder.empty() ? std::string(name) : folder + "/" + std::string(name);
            if (path.size() >= MUSIC_PATH_CAPACITY) return false;
            if (is_dir) return WriteName(directories.get(), path);
            if (regular && name.size() >= 4 && !strcasecmp(name.data() + name.size() - 4, ".mp3")) {
                batch.emplace_back(std::move(path));
                if (batch.size() == batch_size) {
                    if (!FlushBatch(out.get(), batch)) return false;
                    ++runs;
                }
            }
            return true;
        };
#ifdef ESP_PLATFORM
        // FILINFO includes size in the directory read. A stat per MP3 would
        // retraverse the FAT directory for every file on large cards.
        FF_DIR directory;
        if (f_opendir(&directory, (std::to_string(fat_drive) + ":/" + folder).c_str()) != FR_OK) return false;
        FILINFO info;
        bool ok = true;
        while (ok) {
            ok = f_readdir(&directory, &info) == FR_OK;
            if (!ok || !info.fname[0]) break;
            ok = collect_entry(info.fname, info.fattrib & AM_DIR, info.fsize >= 10);
        }
        if (f_closedir(&directory) != FR_OK || !ok) return false;
#else
        (void)fat_drive;
        auto directory = root + "/" + folder;
        std::unique_ptr<DIR, int(*)(DIR*)> dir(opendir(directory.c_str()), closedir);
        if (!dir) return false;
        for (;;) {
            errno = 0;
            auto* entry = readdir(dir.get());
            if (!entry) {
                if (errno) return false;
                break;
            }
            struct stat info;
            if (lstat((directory + "/" + entry->d_name).c_str(), &info)) return false;
            if (!collect_entry(entry->d_name, S_ISDIR(info.st_mode),
                               S_ISREG(info.st_mode) && info.st_size >= 10)) return false;
        }
#endif
        tail = ftell(directories.get());
        if (tail < 0) return false;
    }
    if (!batch.empty() || runs == 0) {
        if (!FlushBatch(out.get(), batch)) return false;
        ++runs;
    }
    return fclose(out.release()) == 0;
}

struct Run {
    FILE* file;
    uint32_t count = 0;
    uint32_t bytes = 0;
    std::string head;
    bool Header() { return GetNumber(file, count) && GetNumber(file, bytes); }
    bool Advance() {
        if (!count) { head.clear(); return true; }
        --count;
        return ReadName(file, head) && !head.empty();
    }
};

bool Merge(const std::string& input, const std::string& output, uint32_t runs,
           const std::function<bool()>& keep_scanning) {
    auto a = Open(input, "rb"), b = Open(input, "rb"), out = Open(output, "wb");
    if (!a || !b || !out) return false;
    long offset = 0;
    for (uint32_t i = 0; i < runs; i += 2) {
        Run left{a.get(), 0, 0, {}}, right{b.get(), 0, 0, {}};
        if (fseek(a.get(), offset, SEEK_SET) || !left.Header()) return false;
        uint64_t end = uint64_t(offset) + 8 + left.bytes;
        if (end > INT32_MAX) return false;
        if (i + 1 < runs) {
            if (fseek(b.get(), end, SEEK_SET) || !right.Header()) return false;
            end += 8ULL + right.bytes;
        }
        if (end > INT32_MAX) return false;
        offset = end;
        if (!PutNumber(out.get(), left.count + right.count) || !PutNumber(out.get(), left.bytes + right.bytes) ||
            !left.Advance() || !right.Advance()) return false;
        size_t copied = 0;
        while (!left.head.empty() || !right.head.empty()) {
            auto& source = right.head.empty() || (!left.head.empty() && left.head < right.head) ? left : right;
            if (!WriteName(out.get(), source.head) || !source.Advance()) return false;
            if (++copied % 64 == 0 && !keep_scanning()) return false;
        }
        if (!keep_scanning()) return false;
    }
    return fclose(out.release()) == 0;
}

bool WriteIndex(const std::string& input, const std::string& stage, const std::function<bool()>& keep_scanning) {
    auto sorted = Open(input, "rb"), out = Open(stage, "wb");
    uint32_t count, bytes;
    if (!sorted || !out || !GetNumber(sorted.get(), count) || !GetNumber(sorted.get(), bytes)) return false;
    if (12ULL + bytes > INT32_MAX || 4ULL * count > bytes) return false;
    uint32_t offset = 12 + 4 * count;
    if (fwrite("OMI1", 1, 4, out.get()) != 4 || !PutNumber(out.get(), count) || !PutNumber(out.get(), offset)) return false;
    std::string path;
    for (unsigned pass = 0; pass < 2; ++pass) {
        if (fseek(sorted.get(), 8, SEEK_SET)) return false;
        for (uint32_t i = 0; i < count; ++i) {
            if (!ReadName(sorted.get(), path)) return false;
            if (pass == 0) {
                offset += path.size();
                if (!PutNumber(out.get(), offset)) return false;
            } else if (fwrite(path.data(), 1, path.size(), out.get()) != path.size()) return false;
            if (i % 64 == 0 && !keep_scanning()) return false;
        }
    }
    return fflush(out.get()) == 0 && fsync(fileno(out.get())) == 0 && fclose(out.release()) == 0;
}
}

bool BuildMusicIndex(const std::string& root, const std::function<bool()>& keep_scanning, int fat_drive) {
    const std::string work = root + "/.omni-music";
    if (mkdir(work.c_str(), 0700) && errno != EEXIST) return false;
    const auto catalog = work + "/catalog-v2", backup = work + "/previous-v2", stage = work + "/new.tmp";
    if (access(catalog.c_str(), F_OK) && !access(backup.c_str(), F_OK) && rename(backup.c_str(), catalog.c_str())) return false;
    std::string a = work + "/a.tmp", b = work + "/b.tmp";
    uint32_t runs = 0;
    bool ok = Collect(root, work, runs, keep_scanning, fat_drive);
    while (ok && runs > 1) {
        ok = Merge(a, b, runs, keep_scanning);
        std::swap(a, b);
        runs = (runs + 1) / 2;
    }
    if (ok) ok = WriteIndex(a, stage, keep_scanning);
    if (ok) {
        bool old = !access(catalog.c_str(), F_OK);
        if (old) {
            unlink(backup.c_str());
            ok = rename(catalog.c_str(), backup.c_str()) == 0;
        }
        if (ok) {
            ok = rename(stage.c_str(), catalog.c_str()) == 0;
            if (!ok && old) rename(backup.c_str(), catalog.c_str());
            if (ok) unlink(backup.c_str());
        }
    }
    for (auto suffix : {"/a.tmp", "/b.tmp", "/dirs.tmp", "/new.tmp"}) unlink((work + suffix).c_str());
    return ok;
}
