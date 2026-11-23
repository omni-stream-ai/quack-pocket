#pragma once
#include <string>
class CustomLcdDisplay;
void RunPhoneSetup(CustomLcdDisplay* display, bool lan = false);
bool PhoneSetupActive();
bool ShowPhoneSetupIfActive(bool approve = true);
int RequestPocketReset(const std::string& confirmation, std::string& result);
int SavePairedVoiceControls(const std::string& body, std::string& result);
int ReadPairedBluetoothProfile(std::string& body);
int BluetoothSetupPort(CustomLcdDisplay* display);
void InitializeBluetoothPrompts(CustomLcdDisplay* display);
void ShowBluetoothPairing(bool visible);
void EnableOfflineSetupScan();
void InitializeBackgroundManagement();
void FinishFirstManagement();
void StartBackgroundManagement(CustomLcdDisplay* display);
bool BackgroundManagementEnabled();
bool ApplyBackgroundManagement(CustomLcdDisplay* display, bool enabled);
bool ManagementInUse();

bool TogglePocketVoiceService();
void SetPocketWakeEnabled(bool enabled);
