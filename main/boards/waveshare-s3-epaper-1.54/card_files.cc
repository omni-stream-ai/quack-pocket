#include "card_files.h"
#include "music_sd.h"
#include "pocket_media.h"

#include <esp_random.h>
#include <ff.h>
#include <mbedtls/base64.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace pocket_library {
namespace {
constexpr size_t chunk_size = 8192;
std::mutex operations;
struct Mount {
    bool ok = music_sd_acquire() == ESP_OK;
    ~Mount() { if (ok) music_sd_release(); }
};
struct File {
    std::unique_ptr<FIL> handle;
    bool open = false;
    FRESULT Open(const std::string& path, BYTE flags) {
        handle.reset(new(std::nothrow) FIL{});
        if (!handle) return FR_NOT_ENOUGH_CORE;
        auto result = f_open(handle.get(),path.c_str(),flags);
        open = result == FR_OK;
        return result;
    }
    FRESULT Close() {
        if (!open) return FR_OK;
        open = false;
        return f_close(handle.get());
    }
    ~File() { Close(); }
};
Result Error(const char* text) { return {false,text}; }
Result Failure(FRESULT code) {
    switch (code) {
    case FR_NO_FILE: case FR_NO_PATH: return Error("文件或目录已不存在，请刷新");
    case FR_EXIST: return Error("目标名称已存在");
    case FR_LOCKED: return Error("文件正在使用，请停止播放或录音后重试");
    case FR_DENIED: return Error("操作失败，请检查目录是否为空、卡空间和写保护");
    default: return Error("读写 SD 卡失败，请检查卡后重试");
    }
}
bool UInt(const cJSON* data,const char* key,uint32_t& result) {
    auto* n = cJSON_GetObjectItemCaseSensitive(data,key);
    if (!cJSON_IsNumber(n) || n->valuedouble < 0 || n->valuedouble > UINT32_MAX ||
        std::floor(n->valuedouble) != n->valuedouble) return false;
    result = static_cast<uint32_t>(n->valuedouble);
    return true;
}
std::string Parent(const std::string& path) {
    auto pos = path.rfind('/');
    return pos == std::string::npos ? "" : path.substr(0,pos+1);
}
FRESULT DirectoryCluster(const std::string& path, uint32_t& cluster) {
    FF_DIR dir{};
    auto result = f_opendir(&dir,path.c_str());
    if (result != FR_OK) return result;
    cluster = dir.obj.sclust;
    return f_closedir(&dir);
}
struct Info { bool exists=false, directory=false; uint32_t size=0, cluster=0; std::string revision; };
FRESULT Inspect(const std::string& path, Info& info) {
    FILINFO stat{};
    auto result = f_stat(path.c_str(),&stat);
    if (result == FR_NO_FILE) return FR_OK;
    if (result != FR_OK) return result;
    info.exists = true;
    info.directory = stat.fattrib & AM_DIR;
    info.size = stat.fsize;
    if (info.directory) {
        result = DirectoryCluster(path,info.cluster);
        if (result != FR_OK) return result;
    } else {
        File file;
        result = file.Open(path,FA_READ);
        if (result != FR_OK) return result;
        info.cluster = file.handle->obj.sclust;
    }
    char stamp[48];
    snprintf(stamp,sizeof(stamp),"%08lx-%08lx-%04x-%04x",static_cast<unsigned long>(info.cluster),
             static_cast<unsigned long>(info.size),stat.fdate,stat.ftime);
    info.revision = stamp;
    return FR_OK;
}
Result Read(const std::string& path, const cJSON* data, const Info& info) {
    uint32_t offset;
    if (!info.exists || info.directory || !UInt(data,"offset",offset) || offset > info.size)
        return Error("无效的文件或读取位置");
    if (pocket_media::Text(data,"revision") != info.revision) return Error("文件已改变，请重新读取");
    File file;
    auto result = file.Open(path,FA_READ);
    if (result != FR_OK) return Failure(result);
    if (f_lseek(file.handle.get(),offset) != FR_OK) return Error("定位文件失败");
    std::vector<unsigned char> bytes(std::min<uint32_t>(chunk_size,info.size-offset));
    UINT count = 0;
    if (!bytes.empty() && (f_read(file.handle.get(),bytes.data(),bytes.size(),&count) != FR_OK || count != bytes.size()))
        return Error("读取文件失败");
    if (bytes.empty()) return {true,""};
    std::string encoded(4*((count+2)/3)+1,'\0');
    size_t length;
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(encoded.data()),encoded.size(),&length,
                              bytes.data(),count) != 0) return Error("编码失败");
    encoded.resize(length);
    return {true,std::move(encoded)};
}
Result WriteChunk(const std::string& path,const cJSON* data) {
    uint32_t offset;
    auto encoded = pocket_media::Text(data,"bytes");
    if (!UInt(data,"offset",offset) || encoded.empty() || encoded.size() > 4*((chunk_size+2)/3))
        return Error("上传数据块无效");
    std::vector<unsigned char> bytes(chunk_size);
    size_t length;
    if (mbedtls_base64_decode(bytes.data(),bytes.size(),&length,
        reinterpret_cast<const unsigned char*>(encoded.data()),encoded.size()) || !length || length > UINT32_MAX-offset)
        return Error("上传数据格式无效");
    File file;
    auto result = file.Open(path,FA_READ|FA_WRITE);
    if (result != FR_OK) return Failure(result);
    auto size = f_size(file.handle.get());
    if (offset > size || f_lseek(file.handle.get(),offset) != FR_OK) return Error("上传位置不一致");
    UINT count=0;
    if (size > offset) {
        // A lost response can cause a retry of an already committed chunk.
        std::vector<unsigned char> previous(length);
        if (size-offset != length || f_read(file.handle.get(),previous.data(),length,&count) != FR_OK ||
            count != length || memcmp(previous.data(),bytes.data(),length)) return Error("上传内容已改变");
    } else if (f_write(file.handle.get(),bytes.data(),length,&count) != FR_OK || count != length) {
        return Error("写入失败，请检查卡空间");
    }
    if (file.Close() != FR_OK) return Error("保存上传数据失败");
    return {true,"已写入"};
}
}

Result CardFileAction(const std::string& action,const cJSON* data) {
    auto path = pocket_media::Text(data,"path");
    if (path.empty() || !ValidPocketPath(path)) return Error("卡内路径无效");
    std::lock_guard<std::mutex> guard(operations);
    Mount mount;
    if (!mount.ok) return Error("请插入 SD 卡");
    const std::string root = std::to_string(music_sd_drive())+":/";
    auto full = root+path;
    auto parent = root+Parent(path);
    auto name = path.substr(Parent(path).size());
    auto token = pocket_media::Text(data,"token");
    bool valid_token = token.size()==16 && token.find_first_not_of("0123456789abcdef")==std::string::npos;
    auto stage = parent+".qp-"+token;
    if (action=="sd_chunk" || action=="sd_cancel") {
        if (!valid_token) return Error("上传标识无效");
        if (action=="sd_chunk") return WriteChunk(stage+"/data.part",data);
        f_unlink((stage+"/data.part").c_str());
        auto result=f_unlink(stage.c_str());
        return result==FR_OK || result==FR_NO_FILE ? Result{true,"已取消"} : Failure(result);
    }
    if (action=="sd_mkdir") {
        auto result=f_mkdir(full.c_str());
        return result==FR_OK ? Result{true,"目录已创建"} : Failure(result);
    }
    Info info;
    auto result=Inspect(full,info);
    if (result!=FR_OK) return Failure(result);
    if (action=="sd_info") {
        return {true,"{\"exists\":"+std::string(info.exists?"true":"false")+
            ",\"directory\":"+(info.directory?"true":"false")+",\"size\":"+std::to_string(info.size)+
            ",\"revision\":\""+info.revision+"\"}",true};
    }
    if (action=="sd_read") return Read(full,data,info);
    auto* revision=cJSON_GetObjectItemCaseSensitive(data,"revision");
    if (!cJSON_IsString(revision) || info.revision!=revision->valuestring) return Error("目标已改变，请刷新后重试");
    if (action=="sd_remove") {
        if (!info.exists) return Error("目标不存在");
        result=f_unlink(full.c_str());
        return result==FR_OK ? Result{true,"已删除"} : Failure(result);
    }
    if (action=="sd_move") {
        auto dest=pocket_media::Text(data,"destination");
        if (!info.exists || dest.empty() || !ValidPocketPath(dest)) return Error("目标路径无效");
        if (info.directory) {
            // FAT resolves short names and case variants to the same directory.
            for (auto slash=dest.find('/'); slash!=std::string::npos; slash=dest.find('/',slash+1)) {
                uint32_t ancestor;
                result=DirectoryCluster(root+dest.substr(0,slash),ancestor);
                if (result!=FR_OK) return Failure(result);
                if (ancestor==info.cluster) return Error("无法移动到自身子目录");
            }
        }
        result=f_rename(full.c_str(),(root+dest).c_str());
        return result==FR_OK ? Result{true,"已移动"} : Failure(result);
    }
    if (info.directory) return Error("目标是目录");
    if (action=="sd_begin") {
        char id[17];
        snprintf(id,sizeof(id),"%08lx%08lx",static_cast<unsigned long>(esp_random()),static_cast<unsigned long>(esp_random()));
        stage=parent+".qp-"+id;
        result=f_mkdir(stage.c_str());
        if (result!=FR_OK) return Failure(result);
        File file;
        result=file.Open(stage+"/data.part",FA_WRITE|FA_CREATE_NEW);
        if (result!=FR_OK || file.Close()!=FR_OK) { f_unlink(stage.c_str()); return Error("无法创建上传文件"); }
        return {true,id};
    }
    if (action!="sd_finish" || !valid_token) return Error("未知文件操作");
    uint32_t size;
    FILINFO uploaded{};
    if (!UInt(data,"size",size) || f_stat((stage+"/data.part").c_str(),&uploaded)!=FR_OK ||
        uploaded.fsize!=size || (uploaded.fattrib & AM_DIR)) return Error("上传不完整");
    auto old=stage+"/original";
    auto backup=old+"/"+name;
    if (info.exists) {
        result=f_mkdir(old.c_str());
        if (result!=FR_OK) return Failure(result);
        result=f_rename(full.c_str(),backup.c_str());
        if (result!=FR_OK) { f_unlink(old.c_str()); return Failure(result); }
    }
    result=f_rename((stage+"/data.part").c_str(),full.c_str());
    if (result!=FR_OK) {
        if (info.exists && f_rename(backup.c_str(),full.c_str())!=FR_OK)
            return Error("替换中断，原件保存在上传临时目录的 original 中");
        if (info.exists) f_unlink(old.c_str());
        return Failure(result);
    }
    if (info.exists) { f_unlink(backup.c_str()); f_unlink(old.c_str()); }
    f_unlink(stage.c_str());
    return {true,"文件已保存"};
}
}
