#include "pocket_memo.h"
#include "pocket_media.h"
#include "music_sd.h"
#include "application.h"
#include "config.h"
#include <esp_random.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
constexpr unsigned rate = AUDIO_OUTPUT_SAMPLE_RATE;
constexpr unsigned frame = rate / 50;
struct WaveHeader {
    char riff[4] = {'R','I','F','F'};
    uint32_t size;
    char format[8] = {'W','A','V','E','f','m','t',' '};
    uint32_t fmt_size = 16;
    uint16_t pcm = 1, channels = 1;
    uint32_t sample_rate = rate, byte_rate = rate * 2;
    uint16_t align = 2, bits = 16;
    char data[4] = {'d','a','t','a'};
    uint32_t length;
    explicit WaveHeader(uint32_t bytes) : size(36 + bytes), length(bytes) {}
};
static_assert(sizeof(WaveHeader) == 44);
bool SaveLatest(const std::string& path, bool unread) {
    pocket_media::Json state(cJSON_CreateObject(),cJSON_Delete);
    cJSON_AddStringToObject(state.get(),"path",path.c_str());
    cJSON_AddBoolToObject(state.get(),"unread",unread);
    return pocket_media::Save("latest_memo",state.get());
}
struct Mount {
    bool ready = music_sd_acquire() == ESP_OK;
    ~Mount() { if (ready) music_sd_release(); }
};
bool EnsureFolders(const std::string& path) {
    std::string parent = MUSIC_SD_ROOT;
    for (size_t start = 0; start < path.size();) {
        auto end = path.find('/',start);
        parent += "/" + path.substr(start,end-start);
        if (mkdir(parent.c_str(),0775) != 0 && errno != EEXIST) return false;
        if (end == std::string::npos) break;
        start = end+1;
    }
    return true;
}
}

bool PocketMemoUnread() {
    auto data = pocket_media::Load("latest_memo");
    return cJSON_IsTrue(cJSON_GetObjectItem(data.get(),"unread"));
}

std::string RecordPocketMemo(const std::function<bool()>& stop) {
    Mount mount;
    if (!mount.ready) return "请插入可写入的 SD 卡";
    pocket_media::Config config;
    if (!ValidPocketPath(config.messages) || !EnsureFolders(config.messages)) return "无法创建留言目录";
    auto& audio = Application::GetInstance().GetAudioService();
    auto microphone = audio.AcquireCommandMicrophone();
    if (audio.IsAudioProcessorRunning()) return "请先结束语音通话";
    char name[32];
    snprintf(name,sizeof(name),"memo-%08lx.wav",static_cast<unsigned long>(esp_random()));
    auto relative = config.messages.empty() ? std::string(name) : config.messages + "/" + name;
    auto path = std::string(MUSIC_SD_ROOT "/") + relative;
    auto partial = path + ".part";
    int fd = open(partial.c_str(),O_CREAT | O_EXCL | O_WRONLY,0664);
    if (fd < 0) return "无法写入新留言，请检查卡空间";
    FILE* file = fdopen(fd,"wb");
    if (!file) { close(fd); unlink(partial.c_str()); return "打开录音文件失败"; }
    WaveHeader header(0);
    bool ok = fwrite(&header, sizeof(header),1,file) == 1;
    uint32_t samples = 0;
    std::vector<int16_t> pcm;
    while (ok && samples < config.record_seconds * rate && !stop() && !audio.IsAudioProcessorRunning()) {
        ok = audio.ReadAudioData(pcm,rate,frame) && pcm.size() == frame &&
             fwrite(pcm.data(),sizeof(int16_t),frame,file) == frame;
        if (ok) samples += frame;
    }
    header = WaveHeader(samples * 2);
    ok = ok && samples && fseek(file,0,SEEK_SET) == 0 && fwrite(&header,sizeof(header),1,file) == 1;
    if (fclose(file)) ok = false;
    if (!ok) { unlink(partial.c_str()); return "未保存留言，请检查录音与卡空间"; }
    if (rename(partial.c_str(),path.c_str()) != 0) { unlink(partial.c_str()); return "完成录音文件失败"; }
    if (!SaveLatest(relative,true)) return "录音已存入卡中，登记留言失败";
    return "留言已保存";
}

std::string PlayPocketMemo(const std::function<bool()>& stop) {
    auto state = pocket_media::Load("latest_memo");
    auto path = pocket_media::Text(state.get(),"path");
    if (path.empty() || !ValidPocketPath(path)) return "还没有留言";
    Mount mount;
    if (!mount.ready) return "请插入留言所在的卡";
    std::unique_ptr<FILE,decltype(&fclose)> file(fopen((std::string(MUSIC_SD_ROOT "/")+path).c_str(),"rb"),fclose);
    WaveHeader header(0);
    if (!file || fread(&header,sizeof(header),1,file.get()) != 1) return "无法读取留言";
    WaveHeader expected(header.length);
    if (memcmp(&header,&expected,sizeof(header)) || !header.length || header.length % 2 ||
        header.length > rate*2*300) return "录音文件损坏";
    auto& audio = Application::GetInstance().GetAudioService();
    std::vector<int16_t> pcm;
    for (uint32_t left = header.length / 2; left;) {
        if (stop() || audio.IsAudioProcessorRunning()) return "已退出留言播放";
        if (pcm.empty()) {
            pcm.resize(std::min<unsigned>(frame,left));
            if (fread(pcm.data(),sizeof(int16_t),pcm.size(),file.get()) != pcm.size()) return "录音读取失败";
        }
        size_t count = pcm.size();
        if (audio.PushMusicPcm(pcm)) left -= count;
        else vTaskDelay(1);
    }
    if (!SaveLatest(path,false)) return "播放完毕，保存已听状态失败";
    return "留言播放完毕";
}
