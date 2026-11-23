#pragma once
#include <esp_http_server.h>
#include <string>
#include <vector>

struct SetupPeer { std::string id, name; };
struct SetupPrompt { std::string id, name, code; };
std::vector<SetupPeer> SetupPeers();
bool RemoveSetupPeer(const std::string& id);
SetupPrompt PendingSetupPrompt();
void AnswerSetupPrompt(const std::string& id, bool accept);
bool FirstManagementRequestAllowed(httpd_req_t* req);
bool SetupRequestAllowed(httpd_req_t* req);
esp_err_t RequestSetupAccess(httpd_req_t* req);
esp_err_t StartLanSetupServer(httpd_handle_t* server, std::string& fingerprint, const std::string& host);
