#pragma once

#define SMOD_ADMIN_START 1
#define SMOD_ADMIN_END 16
#define SMOD_LOGGED_OUT -1
#define SMOD_TEAM_FORCEPOWER_DELIMITER '-'

#include "server/server.h"

namespace SMOD {
	typedef struct smodcmd_s {
		const char* name;
		const int lvl;
		void(*func)(client_t*);
	} smodcmd_t;

	bool IsEnabled();

	int GetMaxWarnLevel();

	void AuthenticateClient(client_t *cl, const int& id, const char* password);

	void LogoutClient(client_t* cl);

	bool IsLoggedIn(const client_t* cl);

	const smodcmd_t* GetCommandFromString(const char* cmd);

	bool IsAuthorized(const client_t* cl, const smodcmd_t* cmd);

	void Print(client_t* cl, const char* msg);

	bool CommandCheck(client_t* src, const smodcmd_t* cmd);

	bool Execute(client_t* src, const char* cmdStr);

	client_t* GetClient(client_t* src, const char* handle);

	client_t* GetClientByID(const int& id);

	client_t* GetClientByHandle(client_t* src, const char* handle);

	void Freeze(client_t* src);

	void Warn(client_t* src);

	void WarnLevel(client_t* src);

	void JAguid(client_t* src);

	void Tell(client_t* src);

	void Slay(client_t* src);

	void Cheats(client_t* src);

}