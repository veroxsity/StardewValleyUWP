#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct SteamInstalledRecord;

enum class SteamState {
	SignedOut,
	Starting,
	Linking,
	Linked,
	Syncing,
	RelinkRequired,
	Error,
};

struct SteamStatus {
	SteamState state = SteamState::SignedOut;
	std::string message;
	std::string challengeUrl;
	std::string steamId;
	std::string accountName;
};

void steamInit(const wchar_t* localState, void (*log)(const char*), void (*onChanged)(void));
SteamStatus steamStatus();
bool steamBeginLink();
void steamCancelLink();
void steamSignOut();
