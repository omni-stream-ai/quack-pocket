#pragma once
class CustomLcdDisplay;
void InitializeSetupBluetooth(CustomLcdDisplay* display);
bool SetSetupBluetoothEnabled(bool enabled);
bool SetupBluetoothEnabled();
bool SetupBluetoothConnected();
unsigned SetupBluetoothPasskey();
bool IsSetupBluetoothSecret(const char* value);

#include <string>
int DispatchManagement(unsigned route,const std::string& body,std::string& result);
