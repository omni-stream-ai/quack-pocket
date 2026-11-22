#include "setup_access.h"
#include "setup_ble.h"
#include <algorithm>
#include <array>
#include <mutex>
#include <cstring>
#include "esp_timer.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "lwip/sockets.h"

namespace {
std::mutex mutex;
SetupPrompt pending;
int64_t expires;
constexpr unsigned limit = 12;
std::string Slot(unsigned index) { return "p" + std::to_string(index); }
std::string Read(nvs_handle_t db, unsigned slot) {
    char data[128]{};
    size_t size = sizeof(data);
    return nvs_get_str(db, Slot(slot).c_str(), data, &size) == ESP_OK ? data : "";
}
void Expire() {
    if (esp_timer_get_time() >= expires) pending = {};
}
std::string Identity(httpd_req_t* req) {
    char token[33];
    if (httpd_req_get_hdr_value_str(req,"X-Pocket-Key",token,sizeof(token)) != ESP_OK ||
        strlen(token) != 32 || strspn(token,"0123456789abcdef") != 32) return {};
    unsigned char digest[32];
    if (mbedtls_sha256(reinterpret_cast<unsigned char*>(token),32,digest,0)) return {};
    char hex[65];
    for (unsigned i=0;i<32;++i) snprintf(hex+2*i,3,"%02x",digest[i]);
    return hex;
}
esp_err_t Reply(httpd_req_t* req,const char* status,const char* message) {
    httpd_resp_set_status(req,status);
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    httpd_resp_set_type(req,"text/plain; charset=utf-8");
    return httpd_resp_sendstr(req,message);
}
bool Known(const std::string& id) {
    auto peers = SetupPeers();
    return std::any_of(peers.begin(),peers.end(),[&](const auto& p){return p.id == id;});
}
}

std::vector<SetupPeer> SetupPeers() {
    std::lock_guard<std::mutex> guard(mutex);
    std::vector<SetupPeer> peers;
    nvs_handle_t db;
    if (nvs_open_from_partition("omni_ui","browsers",NVS_READONLY,&db) != ESP_OK) return peers;
    for (unsigned i=0;i<limit;++i) {
        auto entry=Read(db,i);
        if (entry.size()>64) peers.push_back({entry.substr(0,64),entry.substr(64)});
    }
    nvs_close(db);
    return peers;
}
bool RemoveSetupPeer(const std::string& id) {
    std::lock_guard<std::mutex> guard(mutex);
    nvs_handle_t db;
    if (nvs_open_from_partition("omni_ui","browsers",NVS_READWRITE,&db) != ESP_OK) return false;
    bool removed=false;
    for (unsigned i=0;i<limit;++i) {
        if (Read(db,i).substr(0,64)==id) {
            removed=nvs_erase_key(db,Slot(i).c_str())==ESP_OK && nvs_commit(db)==ESP_OK;
            break;
        }
    }
    nvs_close(db);
    return removed;
}
SetupPrompt PendingSetupPrompt() {
    std::lock_guard<std::mutex> guard(mutex);
    Expire();
    return pending;
}
void AnswerSetupPrompt(const std::string& ticket,bool accept) {
    std::lock_guard<std::mutex> guard(mutex);
    Expire();
    if (ticket.empty() || ticket != pending.id + pending.code) return;
    nvs_handle_t db;
    if (accept && nvs_open_from_partition("omni_ui","browsers",NVS_READWRITE,&db)==ESP_OK) {
        for(unsigned i=0;i<limit;++i) {
            if (!Read(db,i).empty()) continue;
            auto entry=pending.id+pending.name;
            if (nvs_set_str(db,Slot(i).c_str(),entry.c_str())==ESP_OK) nvs_commit(db);
            break;
        }
        nvs_close(db);
    }
    pending={};
}
bool SetupRequestAllowed(httpd_req_t* req) {
    // The paired BLE bridge forwards over loopback on port 8871 (or the
    // setup AP listener on port 80). External clients still need a browser key.
    char secret[65];
    if (httpd_req_get_hdr_value_str(req,"X-Pocket-BLE",secret,sizeof(secret))==ESP_OK &&
        IsSetupBluetoothSecret(secret)) {
        sockaddr_storage peer{};
        socklen_t n=sizeof(peer);
        if (!getpeername(httpd_req_to_sockfd(req),reinterpret_cast<sockaddr*>(&peer),&n)) {
            if (peer.ss_family==AF_INET && reinterpret_cast<sockaddr_in*>(&peer)->sin_addr.s_addr==htonl(INADDR_LOOPBACK)) return true;
            const uint8_t mapped[16]={0,0,0,0,0,0,0,0,0,0,255,255,127,0,0,1};
            if (peer.ss_family==AF_INET6 && !memcmp(&reinterpret_cast<sockaddr_in6*>(&peer)->sin6_addr,mapped,16)) return true;
        }
    }
    sockaddr_storage local{};
    socklen_t size=sizeof(local);
    if (!getsockname(httpd_req_to_sockfd(req),reinterpret_cast<sockaddr*>(&local),&size)) {
        if (local.ss_family==AF_INET) {
            auto* addr=reinterpret_cast<sockaddr_in*>(&local);
            if(ntohs(addr->sin_port)==80 && addr->sin_addr.s_addr==inet_addr("192.168.4.1"))return true;
        } else if (local.ss_family==AF_INET6) {
            auto* addr=reinterpret_cast<sockaddr_in6*>(&local);
            const uint8_t mapped[16]={0,0,0,0,0,0,0,0,0,0,255,255,192,168,4,1};
            if(ntohs(addr->sin6_port)==80 && !memcmp(&addr->sin6_addr,mapped,16))return true;
        }
    }
    auto id=Identity(req);
    if (!id.empty() && Known(id)) return true;
    Reply(req,"403 Forbidden","请先在设备上授权此浏览器");
    return false;
}
esp_err_t RequestSetupAccess(httpd_req_t* req) {
    auto id=Identity(req);
    if(id.empty() || req->content_len>48) return Reply(req,"400 Bad Request","请求格式有误");
    std::string name(req->content_len,'\0');
    size_t pos=0;
    while(pos<name.size()){
        int n=httpd_req_recv(req,name.data()+pos,name.size()-pos);
        if(n<=0)return ESP_FAIL;
        pos+=n;
    }
    if(std::any_of(name.begin(),name.end(),[](unsigned char c){return c<32 || c==127;}))
        return Reply(req,"400 Bad Request","请使用单行设备名称");
    auto peers=SetupPeers();
    for(auto& peer:peers) if(peer.id==id) return Reply(req,"200 OK","已授权");
    std::lock_guard<std::mutex> guard(mutex);
    Expire();
    if(pending.id==id)return Reply(req,"202 Accepted",pending.code.c_str());
    if(name.empty())return Reply(req,"403 Forbidden","需要授权或请求已结束");
    if(!pending.id.empty())return Reply(req,"409 Conflict","设备正在处理其他请求，请稍后重试");
    if(peers.size()>=limit)return Reply(req,"409 Conflict","请先在设备上删除旧授权");
    char code[7];snprintf(code,sizeof(code),"%06u",unsigned(esp_random()%1000000));
    pending={id,name,code};expires=esp_timer_get_time()+90000000;
    return Reply(req,"202 Accepted",pending.code.c_str());
}
