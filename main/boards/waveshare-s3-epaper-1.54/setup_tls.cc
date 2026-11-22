#include "setup_access.h"
#include <cstring>
#include <memory>
#include "esp_https_server.h"
#include "esp_random.h"
#include "mbedtls/ecp.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"

namespace {
struct Identity {
    unsigned char cert[704], key[160];
    uint16_t cert_len, key_len;
};
int Random(void*,unsigned char* output,size_t size) {
    esp_fill_random(output,size);
    return 0;
}
bool Generate(Identity& id, const std::string& host) {
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_pk_init(&key); mbedtls_x509write_crt_init(&crt);
    unsigned char serial[16]; esp_fill_random(serial,sizeof(serial)); serial[0]=1;
    bool ok=mbedtls_pk_setup(&key,mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))==0 &&
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,mbedtls_pk_ec(key),Random,nullptr)==0;
    if(ok){
        mbedtls_x509write_crt_set_version(&crt,MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&crt,MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&crt,&key);
        mbedtls_x509write_crt_set_issuer_key(&crt,&key);
        mbedtls_x509_san_list san{};
        san.node.type = MBEDTLS_X509_SAN_DNS_NAME;
        san.node.san.unstructured_name.p = reinterpret_cast<unsigned char*>(const_cast<char*>(host.data()));
        san.node.san.unstructured_name.len = host.size();
        ok=!mbedtls_x509write_crt_set_subject_alternative_name(&crt,&san) &&
           !mbedtls_x509write_crt_set_serial_raw(&crt,serial,sizeof(serial)) &&
           !mbedtls_x509write_crt_set_issuer_name(&crt,"CN=Quack Pocket") &&
           !mbedtls_x509write_crt_set_subject_name(&crt,"CN=Quack Pocket") &&
           !mbedtls_x509write_crt_set_validity(&crt,"20250101000000","20491231235959");
    }
    if(ok){
        int c=mbedtls_x509write_crt_der(&crt,id.cert,sizeof(id.cert),Random,nullptr);
        int k=mbedtls_pk_write_key_der(&key,id.key,sizeof(id.key));
        ok=c>0 && k>0;
        if(ok){
            id.cert_len=c;id.key_len=k;
            memmove(id.cert,id.cert+sizeof(id.cert)-c,c);
            memmove(id.key,id.key+sizeof(id.key)-k,k);
        }
    }
    mbedtls_x509write_crt_free(&crt);mbedtls_pk_free(&key);
    return ok;
}
}
esp_err_t StartLanSetupServer(httpd_handle_t* server,std::string& fingerprint, const std::string& host){
    static Identity id{};
    nvs_handle_t db;
    auto err=nvs_open_from_partition("omni_ui","setup_tls",NVS_READWRITE,&db);
    if(err!=ESP_OK)return err;
    size_t size=sizeof(id);
    err=nvs_get_blob(db,"identity",&id,&size);
    if(err==ESP_ERR_NVS_NOT_FOUND && Generate(id,host)){
        size=sizeof(id);
        err=nvs_set_blob(db,"identity",&id,sizeof(id));
        if(err==ESP_OK)err=nvs_commit(db);
    }
    nvs_close(db);
    if(err!=ESP_OK || size!=sizeof(id) || !id.cert_len || id.cert_len>sizeof(id.cert) ||
       !id.key_len || id.key_len>sizeof(id.key))return ESP_FAIL;
    unsigned char digest[32];
    if(mbedtls_sha256(id.cert,id.cert_len,digest,0))return ESP_FAIL;
    char prefix[25];
    for(unsigned i=0;i<12;++i)snprintf(prefix+2*i,3,"%02X",digest[i]);
    fingerprint=prefix;
    httpd_ssl_config_t conf=HTTPD_SSL_CONFIG_DEFAULT();
    conf.httpd.max_uri_handlers=12;conf.httpd.max_open_sockets=3;
    conf.httpd.lru_purge_enable=true;
    conf.servercert=id.cert;conf.servercert_len=id.cert_len;
    conf.prvtkey_pem=id.key;conf.prvtkey_len=id.key_len;
    return httpd_ssl_start(server,&conf);
}
