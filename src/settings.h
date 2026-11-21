#pragma once

#include <windows.h>
#include <string>
#include <vector>

bool ReadMultiSetting(HKEY root, const wchar_t* path, const wchar_t* name, std::vector<std::wstring>& values);
bool WriteMultiSetting(HKEY root, const wchar_t* path, const wchar_t* name, const std::vector<std::wstring>& values);

// Imports missing values from a read-only legacy hive until a complete pass succeeds.
bool ImportLegacySettings(HKEY root, const wchar_t* legacy, const wchar_t* destination, bool panel);

// The removal journal lets startup finish an interrupted removal before registering devices.
bool RecoverDeviceRemoval(const wchar_t* path, std::wstring& pending);
bool ForgetDeviceProfile(const wchar_t* path, const std::wstring& key, const std::wstring& preference,
                         std::wstring& pending);
