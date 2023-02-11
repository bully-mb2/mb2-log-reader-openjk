#include "smod.h"
#include "sv_gameapi.h"
#include <algorithm>
/*
==================
Util
==================
*/
static SMOD::smodcmd_t smodcmds[] = {
	{"freeze", 0x40000, SMOD::Freeze},
	{"warn", 0x80000, SMOD::Warn},
	{"warnlvl", 0x100000, SMOD::WarnLevel},
	{"bring", 0x1000000, SMOD::Bring},
	{"tp", 0x1000000, SMOD::Teleport},
	{"cheats", 0x2000000, SMOD::Cheats},
	{"jaguid", 0x4000000, SMOD::JAguid},
	{"tell", 0x8000000, SMOD::Tell},
	{"slay", 0x10000000, SMOD::Slay},

	{NULL, NULL, NULL}
};

bool SMOD::IsEnabled() {
	return Cvar_Get("g_smodconfig_1", "", CVAR_ARCHIVE)->integer != 0;
}

int SMOD::GetMaxWarnLevel() {
	return Cvar_Get("g_maxWarnLevel", "3", CVAR_ARCHIVE)->integer;
}

void SMOD::AuthenticateClient(client_t* cl, const int& id, const char* password) {
	if (cl->state != CS_ACTIVE || !SMOD::IsEnabled()) {
		return;
	}

	if (id < SMOD_ADMIN_START || id > SMOD_ADMIN_END) {
		return;
	}

	char config[64];
	sprintf(config, "g_smodAdminPassword_%d", id);
	char* adminPass = Cvar_Get(config, "", CVAR_ARCHIVE)->string;
	if (!strcmp("", adminPass)) {
		return;
	}

	if (!strcmp(password, adminPass)) {
		char smodString[64];
		sprintf(smodString, "g_smodconfig_%d", id);
		cl->smodID = id;
		cl->smod = Cvar_Get(smodString, "", CVAR_ARCHIVE)->integer;
	}
}

void SMOD::LogoutClient(client_t* cl) {
	cl->smodID = SMOD_LOGGED_OUT;
	cl->smod = SMOD_LOGGED_OUT;
	cl->isFrozen = qfalse;
	cl->warnLevel = 0;
}

bool SMOD::IsLoggedIn(const client_t* cl) {
	return (cl->smod > SMOD_LOGGED_OUT) && (cl->smodID > SMOD_LOGGED_OUT);
}

const SMOD::smodcmd_t* SMOD::GetCommandFromString(const char* cmd) {
	for (SMOD::smodcmd_t* c = smodcmds; c->name; c++) {
		if (!strcmp(cmd, c->name)) {
			return c;
		}
	}

	return NULL;
}

bool SMOD::IsAuthorized(const client_t* cl, const SMOD::smodcmd_t* cmd) {
	if (cmd == NULL)
		return false;

	if ((cl->smod & cmd->lvl) == cmd->lvl)
		return true;

	return false;
}

void SMOD::Print(client_t* cl, const char* msg) {
	SV_SendServerCommand(cl, "print \"%s%s\n\"\n", S_COLOR_YELLOW, msg);
}

bool SMOD::CommandCheck(client_t* src, const SMOD::smodcmd_t* cmd) {
	if (src->state != CS_ACTIVE || !SMOD::IsEnabled()) {
		return false; //Fallback to native SMOD
	}

	if (cmd == NULL) {
		return false; //Fallback to native SMOD
	}

	if (!SMOD::IsLoggedIn(src)) {
		SMOD::Print(src, "You have to be logged in in order to use this command.");
		return false;
	}

	if (!SMOD::IsAuthorized(src, cmd)) {
		SMOD::Print(src, "This command is not enabled for your SMOD admin account.");
		return false;
	}
	
	return true;
}

bool SMOD::Execute(client_t* src, const char* cmdStr) {
	const SMOD::smodcmd_t* cmd = SMOD::GetCommandFromString(cmdStr);

	if (!SMOD::CommandCheck(src, cmd)) {
		return false;
	}

	cmd->func(src);
	return true;
}

client_t* SMOD::GetClient(client_t* src, const char* handle) {
	if (!strcmp(handle, "")) {
		return NULL;
	}

	if (isdigit(handle[0])) {
		return SMOD::GetClientByID(atoi(handle));
	}

	return SMOD::GetClientByHandle(src, handle);
}

client_t* SMOD::GetClientByID(const int& id) {
	if (id >= 0 && id < sv_maxclients->integer) {
		client_t* cl = &svs.clients[id];
		if (cl->state == CS_ACTIVE) {
			return cl;
		}
	}

	return NULL;
}

client_t* SMOD::GetClientByHandle(client_t* src, const char* handle) {
	int i;
	char cleanName[64];
	client_t *cl, *tar = NULL;
	std::string handleStr = handle;
	std::string name;
	std::vector<client_t*> found;
	std::transform(handleStr.begin(), handleStr.end(), handleStr.begin(), ::tolower);
	for (i = 0,  cl = svs.clients; i < sv_maxclients->integer; i++, cl++) {
		if (cl->state != CS_ACTIVE) {
			continue;
		}

		Q_strncpyz(cleanName, cl->name, sizeof(cleanName));
		Q_StripColor(cleanName);
		name = cleanName;
		std::transform(name.begin(), name.end(), name.begin(), ::tolower);
		if (name.find(handleStr) != std::string::npos) {
			if (!tar) {
				tar = cl;
			}

			found.emplace_back(cl);
		}
	}

	if (found.size() > 1) {
		std::string response = "Multiple candidates found:\n";
		for (client_t* f : found) {
			char line[128];
			sprintf(line, "%s[%d] %s\n", S_COLOR_YELLOW, (int) (f - svs.clients), f->name);
			response += line;
		}
		SMOD::Print(src, response.c_str());
		return NULL;
	}

	return tar;
}

/*
==================
Custom commands
==================
*/
void SMOD::Freeze(client_t* src) {
	char* target = Cmd_Argv(2);
	char* reason = Cmd_ArgsFrom(3);
	if (!strcmp(target, "")) {
		SMOD::Print(src, "Usage: smod tell <clientid or name> <optional: reason>");
		return;
	}

	client_t* tar = SMOD::GetClient(src, target);
	if (!tar) {
		SMOD::Print(src, "Couldn't find target with given parameter");
		return;
	}

	if (tar->isFrozen) {
		tar->isFrozen = qfalse;
		SV_SendServerCommand(NULL, "chat \"%s%s %swas %sunfrozen %sby Admin %s#%d\n\"\n", S_COLOR_WHITE, tar->name, S_COLOR_WHITE, S_COLOR_RED, S_COLOR_WHITE, S_COLOR_YELLOW, src->smodID);
	} else {
		tar->isFrozen = qtrue;
		if (strcmp(reason, "")) {
			SV_SendServerCommand(NULL, "chat \"%s%s %swas %sfrozen %sby Admin %s#%d%s for %s%s\n\"\n", S_COLOR_WHITE, tar->name, S_COLOR_WHITE, S_COLOR_RED, S_COLOR_WHITE, S_COLOR_YELLOW, src->smodID, S_COLOR_WHITE, S_COLOR_RED, reason);
		} else {
			SV_SendServerCommand(NULL, "chat \"%s%s %swas %sfrozen %sby Admin %s#%d\n\"\n", S_COLOR_WHITE, tar->name, S_COLOR_WHITE, S_COLOR_RED, S_COLOR_WHITE, S_COLOR_YELLOW, src->smodID);
		}
	}
}

void SMOD::Warn(client_t* src) {
	char* target = Cmd_Argv(2);
	char* level = Cmd_Argv(3);
	if (!strcmp(target, "")) {
		SMOD::Print(src, "Usage: smod warn <clientid or name> <optional: level>");
		return;
	}

	client_t* tar = SMOD::GetClient(src, target);
	if (!tar) {
		SMOD::Print(src, "Couldn't find target with given parameter");
		return;
	}

	int setLevel = 0;
	Com_DPrintf("^2@Bully 1: level %s = %d", level, setLevel);
	if (strcmp(level, "")) {
		Com_DPrintf("^2@Bully 2: level %s = %d", level, setLevel);
		if (isdigit(level[0])) {
			Com_DPrintf("^2@Bully 3: level %s = %d", level, setLevel);
			setLevel = atoi(level);
		}
	}

	if (setLevel > 0) {
		tar->warnLevel = setLevel;
	} else {
		tar->warnLevel = tar->warnLevel + 1;
	}

	SV_SendServerCommand(src, "print \"%sWarning %s %s%d/%d\n\"\n", S_COLOR_YELLOW, tar->name, S_COLOR_YELLOW, tar->warnLevel, SMOD::GetMaxWarnLevel());
	SV_SendServerCommand(NULL, "chat \"%s%s %swarning %s%d/%d %sby Admin %s#%d\n\"\n", S_COLOR_WHITE, tar->name, S_COLOR_RED, S_COLOR_WHITE, tar->warnLevel, SMOD::GetMaxWarnLevel(), S_COLOR_WHITE, S_COLOR_YELLOW, src->smodID);
}

void SMOD::WarnLevel(client_t* src) {
	char* target = Cmd_Argv(2);
	if (!strcmp(target, "")) {
		SMOD::Print(src, "Usage: smod warnlvl <clientid or name>");
		return;
	}

	client_t* tar = SMOD::GetClient(src, target);
	if (!tar) {
		SMOD::Print(src, "Couldn't find target with given parameter");
		return;
	}

	SV_SendServerCommand(src, "print \"%s%s's warn level is %d/%d\n\"\n", tar->name, S_COLOR_YELLOW, tar->warnLevel, SMOD::GetMaxWarnLevel());
}

void SMOD::JAguid(client_t* src) {
	char* target = Cmd_Argv(2);
	if (!strcmp(target, "")) {
		SMOD::Print(src, "Usage: smod jaguid <clientid or name>");
		return;
	}

	client_t* tar = SMOD::GetClient(src, target);
	if (!tar) {
		SMOD::Print(src, "Couldn't find target with given parameter");
		return;
	}

	SV_SendServerCommand(src, "print \"%s%s's JA GUID is %s\n\"\n", tar->name, S_COLOR_YELLOW, Info_ValueForKey(tar->userinfo, "ja_guid"));
}

void SMOD::Tell(client_t* src) {
	char* target = Cmd_Argv(2);
	char* message = Cmd_ArgsFrom(3);
	if (!strcmp(target, "") || !strcmp(message, "")) {
		SMOD::Print(src, "Usage: smod tell <clientid or name> <message>");
		return;
	}

	client_t* tar = SMOD::GetClient(src, target);
	if (!tar) {
		SMOD::Print(src, "Couldn't find target with given parameter");
		return;
	}

	if (tar == src) {
		SMOD::Print(src, "Attempting to smod tell self, stopping");
		return;
	}

	SV_SendServerCommand(src, "chat \"%s[Admin %s#%d%s->%s%s]%s%s\n\"\n", S_COLOR_WHITE, S_COLOR_YELLOW, src->smodID, S_COLOR_WHITE, tar->name, S_COLOR_WHITE, S_COLOR_MAGENTA, message);
	SV_SendServerCommand(tar, "chat \"%s[Admin %s#%d%s->%s%s]%s%s\n\"\n", S_COLOR_WHITE, S_COLOR_YELLOW, src->smodID, S_COLOR_WHITE, tar->name, S_COLOR_WHITE, S_COLOR_MAGENTA, message);
}

void SMOD::Slay(client_t* src) {
	char* target = Cmd_Argv(2);
	if (!strcmp(target, "")) {
		SMOD::Print(src, "Usage: smod slay <clientid or name>");
		return;
	}

	client_t* tar = SMOD::GetClient(src, target);
	if (!tar) {
		SMOD::Print(src, "Couldn't slay target with given parameter");
		return;
	}

	SV_SendServerCommand(src, "print \"%sSlaying %s\n\"\n", S_COLOR_YELLOW, tar->name);
	SV_SendServerCommand(NULL, "chat \"%s%s %swas %sslain %sby Admin %s#%d\n\"\n", S_COLOR_WHITE, tar->name, S_COLOR_WHITE, S_COLOR_RED, S_COLOR_WHITE, S_COLOR_YELLOW, src->smodID);
	tar->gentity->playerState->fallingToDeath = 1;
}

void SMOD::Cheats(client_t* src) {
	char* enabled = Cmd_Argv(2);
	if (!strcmp(enabled, "") || (strcmp(enabled, "1") && strcmp(enabled, "0"))) {
		SMOD::Print(src, "Usage: smod cheats <0: disabled or 1: enabled>");
		return;
	}

	Cvar_Set("g_cheats", enabled);
	Cvar_Set("sv_cheats", enabled);

	SV_SendServerCommand(src, "print \"%sSet sv_cheats = %s\n\"\n", S_COLOR_YELLOW, enabled);
}

void SMOD::Bring(client_t* src) {
	char* target = Cmd_Argv(2);
	if (!strcmp(target, "")) {
		SMOD::Print(src, "Usage: smod bring <clientid or name>");
		return;
	}

	client_t* tar = SMOD::GetClient(src, target);
	if (!tar) {
		SMOD::Print(src, "Couldn't bring target with given parameter");
		return;
	}

	SMOD::ExecuteTeleport(src, tar, src);
}

void SMOD::Teleport(client_t* src) {
	char* from = Cmd_Argv(2);
	char* to = Cmd_Argv(3);
	if (!strcmp(from, "")) {
		SMOD::Print(src, "Usage: smod tp <clientid or name> <optional: target clientid or name>");
		return;
	}

	client_t* fromClient = SMOD::GetClient(src, from);
	if (!fromClient) {
		SMOD::Print(src, "Couldn't tp to target with given parameter");
		return;
	}

	client_t* toClient = NULL;
	if (strcmp(to, "")) {
		toClient = SMOD::GetClient(src, to);
		if (!toClient) {
			SMOD::Print(src, "Couldn't tp to target1 to target2 with given parameter");
			return;
		}
	}

	if (toClient == NULL) {
		toClient = fromClient;
		fromClient = src;
	}

	SMOD::ExecuteTeleport(src, fromClient, toClient);
}

void SMOD::ExecuteTeleport(client_t* src, client_t* fromClient, client_t* toClient) {
	if (fromClient == toClient) {
		SMOD::Print(src, "Can't tp targets because they are the same person!");
		return;
	}

	SV_SendServerCommand(src, "print \"%sTeleporting %s%s to %s\n\"\n", S_COLOR_YELLOW, fromClient->name, S_COLOR_YELLOW, toClient->name);
	SV_SendServerCommand(NULL, "chat \"%s%s %swas %steleported %sto %s %sby Admin %s#%d\n\"\n", S_COLOR_WHITE, fromClient->name, S_COLOR_WHITE, S_COLOR_RED, S_COLOR_WHITE, toClient->name, S_COLOR_WHITE, S_COLOR_YELLOW, src->smodID);
	VectorCopy(toClient->gentity->playerState->origin, fromClient->gentity->playerState->origin);
}