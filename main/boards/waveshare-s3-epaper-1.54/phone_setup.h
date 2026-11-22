#pragma once
class CustomLcdDisplay;
void RunPhoneSetup(CustomLcdDisplay* display, bool lan = false);
bool PhoneSetupActive();
bool ShowPhoneSetupIfActive(bool approve = true);
