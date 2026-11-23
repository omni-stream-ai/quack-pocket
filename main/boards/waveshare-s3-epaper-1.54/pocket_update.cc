#include "pocket_update.h"
#include "settings.h"

#include <algorithm>
#include <mutex>
#include <charconv>
#include <cstring>
#include <memory>
#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_ota_ops.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

namespace pocket_update {
namespace {
constexpr char board[] = "waveshare-s3-epaper-1.54-v2";
constexpr char default_source[] = "https://gaga.omnistreamai.com/ota/manifest.json";
extern const unsigned char public_key[] asm("_binary_ota_public_pem_start");

bool Version(const std::string& text, std::array<unsigned, 3>& parts) {
    const char* p = text.data();
    const char* end = p + text.size();
    for (unsigned i = 0; i < parts.size(); ++i) {
        auto result = std::from_chars(p, end, parts[i]);
        if (result.ec != std::errc{} || (result.ptr - p > 1 && *p == '0')) return false;
        p = result.ptr;
        if (i != 2 && (p == end || *p++ != '.')) return false;
    }
    return p == end;
}

bool Unhex(const std::string& text, uint8_t* out, size_t bytes) {
    if (text.size() != bytes * 2) return false;
    for (size_t i = 0; i < bytes; ++i) {
        auto p = text.data() + 2*i;
        auto result = std::from_chars(p, p+2, out[i], 16);
        if (result.ec != std::errc{} || result.ptr != p+2) return false;
    }
    return true;
}

bool Verify(const std::string& message, const std::string& signature) {
    uint8_t hash[32], der[80];
    size_t length = signature.size()/2;
    if (length == 0 || length > sizeof(der) || !Unhex(signature, der, length) ||
        mbedtls_sha256(reinterpret_cast<const uint8_t*>(message.data()), message.size(), hash, 0)) return false;
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    bool valid = !mbedtls_pk_parse_public_key(&key, public_key, strlen(reinterpret_cast<const char*>(public_key))+1) &&
        !mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, hash, sizeof(hash), der, length);
    mbedtls_pk_free(&key);
    return valid;
}

esp_http_client_config_t HttpConfig(const std::string& url) {
    esp_http_client_config_t cfg{};
    cfg.url = url.c_str();
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 15000;
    cfg.disable_auto_redirect = true;
    return cfg;
}
}

namespace {
std::mutex preference_lock;
bool& AutoPreference() {
    static bool enabled = Settings("omni_ui").GetInt("auto_update",0) != 0;
    return enabled;
}
}
bool AutomaticChecks() {
    std::lock_guard<std::mutex> guard(preference_lock);
    return AutoPreference();
}
void SetAutomaticChecks(bool enabled) {
    std::lock_guard<std::mutex> guard(preference_lock);
    Settings("omni_ui",true).SetInt("auto_update",enabled);
    AutoPreference() = enabled;
}

std::string Source() {
    auto url = Settings("omni_ui").GetString("update_url");
    return url.empty() ? default_source : url;
}

bool ValidSource(const std::string& url) {
    auto slash = url.find('/', 8);
    return url.size() <= 512 && url.compare(0, 8, "https://") == 0 &&
        slash != std::string::npos && slash > 8 && slash + 1 < url.size() &&
        url.find_first_of("?#@\\") == std::string::npos &&
        std::all_of(url.begin(), url.end(), [](unsigned char c) { return c > 32 && c < 127; });
}

std::string Check(Release& release) {
    release = {};
    auto source = Source();
    if (!ValidSource(source)) return "请在手机设置中检查更新地址";
    auto cfg = HttpConfig(source);
    auto client = esp_http_client_init(&cfg);
    if (!client) return "内存不足，请稍后重试";
    std::unique_ptr<esp_http_client, decltype(&esp_http_client_cleanup)> close(client, esp_http_client_cleanup);
    if (esp_http_client_open(client, 0) != ESP_OK || esp_http_client_fetch_headers(client) < 0 ||
        esp_http_client_get_status_code(client) != 200) return "未取得发布清单，请检查网络和更新地址";
    std::string body;
    char chunk[512];
    int n;
    while ((n = esp_http_client_read(client, chunk, sizeof(chunk))) > 0) {
        if (body.size() + n > 2048) return "发布清单过大";
        body.append(chunk, n);
    }
    if (n < 0 || !esp_http_client_is_complete_data_received(client)) return "清单下载中断，请重试";
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(cJSON_ParseWithOpts(body.c_str(), nullptr, true), cJSON_Delete);
    auto field = [&](const char* name) -> std::string {
        const auto* v = cJSON_GetObjectItemCaseSensitive(json.get(), name);
        return cJSON_IsString(v) ? v->valuestring : "";
    };
    auto version = field("version"), sha = field("sha256");
    const auto* size = cJSON_GetObjectItemCaseSensitive(json.get(), "size");
    auto part = esp_ota_get_next_update_partition(nullptr);
    std::array<unsigned,3> current{}, next{};
    Release candidate;
    if (field("board") != board || !Version(version, next) || !Version(esp_app_get_description()->version, current) ||
        !part || !cJSON_IsNumber(size) || size->valueint <= 0 || size->valuedouble != size->valueint ||
        size->valuedouble > part->size || !Unhex(sha, candidate.digest.data(), candidate.digest.size()))
        return "清单与本机不匹配";
    auto message = std::string(board) + "\n" + version + "\n" + std::to_string(size->valueint) + "\n" + sha + "\n";
    if (!Verify(message, field("signature"))) return "发布签名无效";
    // Early releases inherited the upstream numbering. A cached legacy feed
    // must not roll a 0.x device back to one of those builds.
    if (current[0] == 0 && (version == "2.0.5" || version == "2.0.6")) return {};
    if (next > current) {
        candidate.version = version;
        candidate.bytes = size->valueint;
        candidate.image_url = source.substr(0, source.rfind('/')+1) + sha + ".bin";
        release = std::move(candidate);
    }
    return {};
}

std::string Install(const Release& release, const std::function<void(int)>& progress) {
    auto part = esp_ota_get_next_update_partition(nullptr);
    if (!part || !release.bytes || release.bytes > part->size) return "应用分区不兼容";
    auto http = HttpConfig(release.image_url);
    esp_https_ota_config_t cfg{};
    cfg.http_config = &http;
    esp_https_ota_handle_t handle = nullptr;
    if (esp_https_ota_begin(&cfg, &handle) != ESP_OK) return "无法下载固件，请检查网络";
    auto fail = [&](const char* message) { esp_https_ota_abort(handle); return std::string(message); };
    esp_app_desc_t desc{};
    if (esp_https_ota_get_image_size(handle) != release.bytes || esp_https_ota_get_img_desc(handle, &desc) != ESP_OK ||
        strnlen(desc.version, sizeof(desc.version)) == sizeof(desc.version) || release.version != desc.version ||
        strncmp(desc.project_name, esp_app_get_description()->project_name, sizeof(desc.project_name)) != 0)
        return fail("固件信息与清单不符");
    esp_err_t status;
    int shown = -1;
    while ((status = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int percent = 100 * esp_https_ota_get_image_len_read(handle) / release.bytes;
        if (percent / 10 != shown) { shown = percent / 10; progress(shown * 10); }
    }
    if (status != ESP_OK || !esp_https_ota_is_complete_data_received(handle) ||
        esp_https_ota_get_image_len_read(handle) != release.bytes) return fail("下载中断或镜像损坏");
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    int err = mbedtls_sha256_starts(&ctx, 0);
    uint8_t block[1024];
    for (int pos = 0; !err && pos < release.bytes; pos += sizeof(block)) {
        size_t bytes = std::min<int>(sizeof(block), release.bytes-pos);
        err = esp_partition_read(part, pos, block, bytes);
        if (!err) err = mbedtls_sha256_update(&ctx, block, bytes);
    }
    std::array<uint8_t,32> digest{};
    if (!err) err = mbedtls_sha256_finish(&ctx, digest.data());
    mbedtls_sha256_free(&ctx);
    if (err || digest != release.digest) return fail("固件校验失败");
    if (esp_https_ota_finish(handle) != ESP_OK) return "固件验证失败，保留当前版本";
    return {};
}
}
