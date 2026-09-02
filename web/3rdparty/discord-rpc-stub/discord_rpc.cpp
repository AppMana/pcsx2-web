// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "discord_rpc.h"

extern "C" {

void Discord_Initialize(const char* applicationId, DiscordEventHandlers* handlers, int autoRegister, const char* optionalSteamId)
{
}

void Discord_Shutdown(void)
{
}

void Discord_RunCallbacks(void)
{
}

void Discord_UpdatePresence(const DiscordRichPresence* presence)
{
}

void Discord_ClearPresence(void)
{
}

void Discord_Respond(const char* userid, int reply)
{
}

void Discord_UpdateHandlers(DiscordEventHandlers* handlers)
{
}
}
