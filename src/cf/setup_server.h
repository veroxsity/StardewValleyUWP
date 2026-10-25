#pragma once

#include <string>

// a one page lan server so the key can be pasted from a phone, only up while the setup screen is open
namespace stardew_uwp
{
// port 0 picks whatever is free
bool SetupServerStart(const std::wstring &localState, unsigned short port);
void SetupServerStop();

// empty until the server is up
std::string SetupServerUrl();
std::string SetupServerError();

bool SetupServerSaved();
}
