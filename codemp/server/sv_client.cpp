/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
Copyright (C) 2005 - 2015, ioquake3 contributors
Copyright (C) 2013 - 2015, OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

// sv_client.c -- server code for dealing with clients

#include "server.h"
#include "qcommon/stringed_ingame.h"

#ifdef USE_INTERNAL_ZLIB
#include "zlib/zlib.h"
#else
#include <zlib.h>
#endif

#include "server/sv_gameapi.h"

static void SV_CloseDownload( client_t *cl );

/*
=================
SV_GetChallenge

A "getchallenge" OOB command has been received
Returns a challenge number that can be used
in a subsequent connectResponse command.
We do this to prevent denial of service attacks that
flood the server with invalid connection IPs.  With a
challenge, they must give a valid IP address.

If we are authorizing, a challenge request will cause a packet
to be sent to the authorize server.

When an authorizeip is returned, a challenge response will be
sent to that ip.

ioquake3/openjk: we added a possibility for clients to add a challenge
to their packets, to make it more difficult for malicious servers
to hi-jack client connections.
=================
*/
void SV_GetChallenge( netadr_t from ) {
	int		challenge;
	int		clientChallenge;

	// ignore if we are in single player
	/*
	if ( Cvar_VariableValue( "g_gametype" ) == GT_SINGLE_PLAYER || Cvar_VariableValue("ui_singlePlayerActive")) {
		return;
	}
	*/
	if (Cvar_VariableValue("ui_singlePlayerActive"))
	{
		return;
	}

	// Create a unique challenge for this client without storing state on the server
	challenge = SV_CreateChallenge(from);

	// Grab the client's challenge to echo back (if given)
	clientChallenge = atoi(Cmd_Argv(1));

	NET_OutOfBandPrint( NS_SERVER, from, "challengeResponse %i %i", challenge, clientChallenge );
}

/*
==================
SV_IsBanned

Check whether a certain address is banned
==================
*/

static qboolean SV_IsBanned( netadr_t *from, qboolean isexception )
{
	int index;
	serverBan_t *curban;

	if ( !serverBansCount ) {
		return qfalse;
	}

	if ( !isexception )
	{
		// If this is a query for a ban, first check whether the client is excepted
		if ( SV_IsBanned( from, qtrue ) )
			return qfalse;
	}

	for ( index = 0; index < serverBansCount; index++ )
	{
		curban = &serverBans[index];

		if ( curban->isexception == isexception )
		{
			if ( NET_CompareBaseAdrMask( curban->ip, *from, curban->subnet ) )
				return qtrue;
		}
	}

	return qfalse;
}

/*
==================
SV_DirectConnect

A "connect" OOB command has been received
==================
*/
void SV_DirectConnect( netadr_t from ) {
	char		userinfo[MAX_INFO_STRING];
	int			i;
	client_t	*cl, *newcl;
	client_t	temp;
	sharedEntity_t *ent;
	int			clientNum;
	int			version;
	int			qport;
	int			challenge;
	char		*password;
	int			startIndex;
	char		*denied;
	int			count;
	char		*ip;

	Com_DPrintf ("SVC_DirectConnect ()\n");

	// Check whether this client is banned.
	if ( SV_IsBanned( &from, qfalse ) )
	{
		NET_OutOfBandPrint( NS_SERVER, from, "print\nYou are banned from this server.\n" );
		Com_DPrintf( "    rejected connect from %s (banned)\n", NET_AdrToString(from) );
		return;
	}

	Q_strncpyz( userinfo, Cmd_Argv(1), sizeof(userinfo) );

	version = atoi( Info_ValueForKey( userinfo, "protocol" ) );
	if ( version != PROTOCOL_VERSION ) {
		NET_OutOfBandPrint( NS_SERVER, from, "print\nServer uses protocol version %i (yours is %i).\n", PROTOCOL_VERSION, version );
		Com_DPrintf ("    rejected connect from version %i\n", version);
		return;
	}

	challenge = atoi( Info_ValueForKey( userinfo, "challenge" ) );
	qport = atoi( Info_ValueForKey( userinfo, "qport" ) );

	// quick reject
	for (i=0,cl=svs.clients ; i < sv_maxclients->integer ; i++,cl++) {

/* This was preventing sv_reconnectlimit from working.  It seems like commenting this
   out has solved the problem.  HOwever, if there is a future problem then it could
   be this.

		if ( cl->state == CS_FREE ) {
			continue;
		}
*/

		if ( NET_CompareBaseAdr( from, cl->netchan.remoteAddress )
			&& ( cl->netchan.qport == qport
			|| from.port == cl->netchan.remoteAddress.port ) ) {
			if (( svs.time - cl->lastConnectTime)
				< (sv_reconnectlimit->integer * 1000)) {
				NET_OutOfBandPrint( NS_SERVER, from, "print\nReconnect rejected : too soon\n" );
				Com_DPrintf ("%s:reconnect rejected : too soon\n", NET_AdrToString (from));
				return;
			}
			break;
		}
	}

	// don't let "ip" overflow userinfo string
	if ( NET_IsLocalAddress (from) )
		ip = "localhost";
	else
		ip = (char *)NET_AdrToString( from );
	if( ( strlen( ip ) + strlen( userinfo ) + 4 ) >= MAX_INFO_STRING ) {
		NET_OutOfBandPrint( NS_SERVER, from,
			"print\nUserinfo string length exceeded.  "
			"Try removing setu cvars from your config.\n" );
		return;
	}
	Info_SetValueForKey( userinfo, "ip", ip );

	// see if the challenge is valid (localhost clients don't need to challenge)
	if (!NET_IsLocalAddress(from))
	{
		// Verify the received challenge against the expected challenge
		if (!SV_VerifyChallenge(challenge, from))
		{
			NET_OutOfBandPrint( NS_SERVER, from, "print\nIncorrect challenge for your address.\n" );
			return;
		}
	}

	newcl = &temp;
	Com_Memset (newcl, 0, sizeof(client_t));

	// if there is already a slot for this ip, reuse it
	for (i=0,cl=svs.clients ; i < sv_maxclients->integer ; i++,cl++) {
		if ( cl->state == CS_FREE ) {
			continue;
		}
		if ( NET_CompareBaseAdr( from, cl->netchan.remoteAddress )
			&& ( cl->netchan.qport == qport
			|| from.port == cl->netchan.remoteAddress.port ) ) {
			Com_Printf ("%s:reconnect\n", NET_AdrToString (from));
			newcl = cl;
			// VVFIXME - both SOF2 and Wolf remove this call, claiming it blows away the user's info
			// disconnect the client from the game first so any flags the
			// player might have are dropped
			GVM_ClientDisconnect( newcl - svs.clients );
			//
			goto gotnewcl;
		}
	}

	// find a client slot
	// if "sv_privateClients" is set > 0, then that number
	// of client slots will be reserved for connections that
	// have "password" set to the value of "sv_privatePassword"
	// Info requests will report the maxclients as if the private
	// slots didn't exist, to prevent people from trying to connect
	// to a full server.
	// This is to allow us to reserve a couple slots here on our
	// servers so we can play without having to kick people.

	// check for privateClient password
	password = Info_ValueForKey( userinfo, "password" );
	if ( !strcmp( password, sv_privatePassword->string ) ) {
		startIndex = 0;
	} else {
		// skip past the reserved slots
		startIndex = sv_privateClients->integer;
	}

	newcl = NULL;
	for ( i = startIndex; i < sv_maxclients->integer ; i++ ) {
		cl = &svs.clients[i];
		if (cl->state == CS_FREE) {
			newcl = cl;
			break;
		}
	}

	if ( !newcl ) {
		if ( NET_IsLocalAddress( from ) ) {
			count = 0;
			for ( i = startIndex; i < sv_maxclients->integer ; i++ ) {
				cl = &svs.clients[i];
				if (cl->netchan.remoteAddress.type == NA_BOT) {
					count++;
				}
			}
			// if they're all bots
			if (count >= sv_maxclients->integer - startIndex) {
				SV_DropClient(&svs.clients[sv_maxclients->integer - 1], "only bots on server");
				newcl = &svs.clients[sv_maxclients->integer - 1];
			}
			else {
				Com_Error( ERR_FATAL, "server is full on local connect\n" );
				return;
			}
		}
		else {
			const char *SV_GetStringEdString(char *refSection, char *refName);
			NET_OutOfBandPrint( NS_SERVER, from, va("print\n%s\n", SV_GetStringEdString("MP_SVGAME","SERVER_IS_FULL")));
			Com_DPrintf ("Rejected a connection.\n");
			return;
		}
	}

	// we got a newcl, so reset the reliableSequence and reliableAcknowledge
	cl->reliableAcknowledge = 0;
	cl->reliableSequence = 0;

gotnewcl:

	// build a new connection
	// accept the new client
	// this is the only place a client_t is ever initialized
	*newcl = temp;
	clientNum = newcl - svs.clients;
	ent = SV_GentityNum( clientNum );
	newcl->gentity = ent;

	// save the challenge
	newcl->challenge = challenge;

	// save the address
	Netchan_Setup (NS_SERVER, &newcl->netchan , from, qport);

	// save the userinfo
	Q_strncpyz( newcl->userinfo, userinfo, sizeof(newcl->userinfo) );

	// get the game a chance to reject this connection or modify the userinfo
	#ifdef DEDICATED
    if (svs.servermod == SVMOD_JAPLUS && Cvar_VariableIntegerValue("g_teamAutoJoin")
        && !Cvar_VariableIntegerValue("g_gametype") && Cvar_VariableIntegerValue("jp_teamLock") & (1<<2)) //i guess?
    {
        char *team = Info_ValueForKey(userinfo, "team");
        if (VALIDSTRING(team) && team[0] == 's')
            Info_SetValueForKey(userinfo, "team", "f");
    }
    #endif
	
	denied = GVM_ClientConnect( clientNum, qtrue, qfalse ); // firstTime = qtrue
	if ( denied ) {
		NET_OutOfBandPrint( NS_SERVER, from, "print\n%s\n", denied );
		Com_DPrintf ("Game rejected a connection: %s.\n", denied);
		return;
	}

	if (svs.hibernation.enabled) {
		svs.hibernation.enabled = qfalse;
		Com_Printf("Server restored from hibernation\n");
	}

	SV_UserinfoChanged( newcl );

	// send the connect packet to the client
	NET_OutOfBandPrint( NS_SERVER, from, "connectResponse" );

	Com_DPrintf( "Going from CS_FREE to CS_CONNECTED for %s\n", newcl->name );

	newcl->state = CS_CONNECTED;
	newcl->nextSnapshotTime = svs.time;
	newcl->lastPacketTime = svs.time;
	newcl->lastConnectTime = svs.time;

	// when we receive the first packet from the client, we will
	// notice that it is from a different serverid and that the
	// gamestate message was not just sent, forcing a retransmit
	newcl->gamestateMessageNum = -1;

	newcl->lastUserInfoChange = 0; //reset the delay
	newcl->lastUserInfoCount = 0; //reset the count

#ifdef DEDICATED
	newcl->chatLogPolicySentTime = 0;
	newcl->chatLogPolicySent = qfalse;
#endif

	// if this was the first client on the server, or the last client
	// the server can hold, send a heartbeat to the master.
	count = 0;
	for (i=0,cl=svs.clients ; i < sv_maxclients->integer ; i++,cl++) {
		if ( svs.clients[i].state >= CS_CONNECTED ) {
			count++;
		}
	}
	if ( count == 1 || count == sv_maxclients->integer ) {
		SV_Heartbeat_f();
	}
}


/*
=====================
SV_DropClient

Called when the player is totally leaving the server, either willingly
or unwillingly.  This is NOT called if the entire server is quiting
or crashing -- SV_FinalMessage() will handle that
=====================
*/
void SV_DropClient( client_t *drop, const char *reason ) {
	int		i;
	const bool isBot = drop->netchan.remoteAddress.type == NA_BOT;

	if ( drop->state == CS_ZOMBIE ) {
		return;		// already dropped
	}

	// Kill any download
	SV_CloseDownload( drop );

#ifdef DEDICATED
	drop->chatLogPolicySentTime = 0;
	drop->chatLogPolicySent = qfalse;
#endif

	// tell everyone why they got dropped
	SV_SendServerCommand( NULL, "print \"%s" S_COLOR_WHITE " %s\n\"", drop->name, reason );

	// call the prog function for removing a client
	// this will remove the body, among other things
	GVM_ClientDisconnect( drop - svs.clients );

	// add the disconnect command
	SV_SendServerCommand( drop, "disconnect \"%s\"", reason );

	if ( isBot ) {
		SV_BotFreeClient( drop - svs.clients );
	}

	// nuke user info
	SV_SetUserinfo( drop - svs.clients, "" );

	if ( isBot ) {
		// bots shouldn't go zombie, as there's no real net connection.
		drop->state = CS_FREE;
	} else {
		Com_DPrintf( "Going to CS_ZOMBIE for %s\n", drop->name );
		drop->state = CS_ZOMBIE;		// become free in a few seconds
	}

	if ( drop->demo.demorecording ) {
		SV_StopRecordDemo( drop );
	}

	// if this was the last client on the server, send a heartbeat
	// to the master so it is known the server is empty
	// send a heartbeat now so the master will get up to date info
	// if there is already a slot for this ip, reuse it

	qboolean humans = qfalse;
	for (i = 0; i < sv_maxclients->integer; i++) {
		if (svs.clients[i].state >= CS_CONNECTED && svs.clients[i].netchan.remoteAddress.type != NA_BOT) {
			humans = qtrue;
			break;
		}
	}

	for (i=0 ; i < sv_maxclients->integer ; i++ ) {
		if ( svs.clients[i].state >= CS_CONNECTED ) {
			break;
		}
	}
	if ( i == sv_maxclients->integer ) {
		SV_Heartbeat_f();
	}

	if (!humans) {
		svs.hibernation.lastTimeDisconnected = Sys_Milliseconds();
	}

}

void SV_CreateClientGameStateMessage( client_t *client, msg_t *msg ) {
	int			start;
	entityState_t	*base, nullstate;

	// NOTE, MRE: all server->client messages now acknowledge
	// let the client know which reliable clientCommands we have received
	MSG_WriteLong( msg, client->lastClientCommand );

	// send any server commands waiting to be sent first.
	// we have to do this cause we send the client->reliableSequence
	// with a gamestate and it sets the clc.serverCommandSequence at
	// the client side
	SV_UpdateServerCommandsToClient( client, msg );

	// send the gamestate
	MSG_WriteByte( msg, svc_gamestate );
	MSG_WriteLong( msg, client->reliableSequence );

	// write the configstrings
	for ( start = 0 ; start < MAX_CONFIGSTRINGS ; start++ ) {
		if (sv.configstrings[start][0]) {
			MSG_WriteByte( msg, svc_configstring );
			MSG_WriteShort( msg, start );
			MSG_WriteBigString( msg, sv.configstrings[start] );
		}
	}

	// write the baselines
	Com_Memset( &nullstate, 0, sizeof( nullstate ) );
	for ( start = 0 ; start < MAX_GENTITIES; start++ ) {
		base = &sv.svEntities[start].baseline;
		if ( !base->number ) {
			continue;
		}
		MSG_WriteByte( msg, svc_baseline );
		MSG_WriteDeltaEntity( msg, &nullstate, base, qtrue );
	}

	MSG_WriteByte( msg, svc_EOF );

	MSG_WriteLong( msg, client - svs.clients);

	// write the checksum feed
	MSG_WriteLong( msg, sv.checksumFeed);

	// For old RMG system.
	MSG_WriteShort ( msg, 0 );
}

/*
================
SV_SendClientGameState

Sends the first message from the server to a connected client.
This will be sent on the initial connection and upon each new map load.

It will be resent if the client acknowledges a later message but has
the wrong gamestate.
================
*/
void SV_SendClientGameState( client_t *client ) {
	msg_t		msg;
	byte		msgBuffer[MAX_MSGLEN];

	MSG_Init( &msg, msgBuffer, sizeof( msgBuffer ) );

	// MW - my attempt to fix illegible server message errors caused by
	// packet fragmentation of initial snapshot.
	while(client->state&&client->netchan.unsentFragments)
	{
		// send additional message fragments if the last message
		// was too large to send at once

		Com_Printf ("[ISM]SV_SendClientGameState() [2] for %s, writing out old fragments\n", client->name);
		SV_Netchan_TransmitNextFragment(&client->netchan);
	}

	Com_DPrintf ("SV_SendClientGameState() for %s\n", client->name);
	Com_DPrintf( "Going from CS_CONNECTED to CS_PRIMED for %s\n", client->name );
	if ( client->state == CS_CONNECTED )
		client->state = CS_PRIMED;
	client->pureAuthentic = 0;
	client->gotCP = qfalse;

	// when we receive the first packet from the client, we will
	// notice that it is from a different serverid and that the
	// gamestate message was not just sent, forcing a retransmit
	client->gamestateMessageNum = client->netchan.outgoingSequence;

	SV_CreateClientGameStateMessage( client, &msg );

	// deliver this to the client
	SV_SendMessageToClient( &msg, client );
}

#ifdef DEDICATED
void SV_SendClientChatLogPolicy( client_t *client )
{
	if (!client)
		return;
	if (!com_logChat || com_logChat->integer >= 2)
		return;

	if (!svs.gameLoggingEnabled && (!com_logfile || !com_logfile->integer))
		return;

	if (svs.servermod == SVMOD_UNKNOWN || (svs.servermod == SVMOD_MBII && client->chatLogPolicySent))
		return;

	if (svs.time - client->chatLogPolicySentTime <= 5000) //don't send more than once every 5 seconds
		return;

	//SV_SendServerCommand(client, "print \"%sThis server logs %s chat messages\n\"", S_COLOR_CYAN, com_logChat->integer == 1 ? "all public and team" : "no");
	if (com_logChat->integer == 1) {
		SV_SendServerCommand(client, "print \"%scom_logChat is 1: This server does not log private messages (/tell) to protect player privacy\n\"", S_COLOR_CYAN);
	}
	else {
		SV_SendServerCommand(client, "print \"%scom_logChat is 0: This server has a no-logging policy to protect player privacy\n\"", S_COLOR_CYAN);
	}
	client->chatLogPolicySentTime = svs.time;
	client->chatLogPolicySent = qtrue;
}
#endif

void SV_SendClientMapChange( client_t *client )
{
	msg_t		msg;
	byte		msgBuffer[MAX_MSGLEN];

	MSG_Init( &msg, msgBuffer, sizeof( msgBuffer ) );

	// NOTE, MRE: all server->client messages now acknowledge
	// let the client know which reliable clientCommands we have received
	MSG_WriteLong( &msg, client->lastClientCommand );

	// send any server commands waiting to be sent first.
	// we have to do this cause we send the client->reliableSequence
	// with a gamestate and it sets the clc.serverCommandSequence at
	// the client side
	SV_UpdateServerCommandsToClient( client, &msg );

	// send the gamestate
	MSG_WriteByte( &msg, svc_mapchange );

	// deliver this to the client
	SV_SendMessageToClient( &msg, client );
}

/*
==================
SV_ClientEnterWorld
==================
*/
void SV_ClientEnterWorld( client_t *client, usercmd_t *cmd ) {
	int		clientNum;
	sharedEntity_t *ent;

	Com_DPrintf( "Going from CS_PRIMED to CS_ACTIVE for %s\n", client->name );
	client->state = CS_ACTIVE;

	if (sv_autoWhitelist->integer) {
		SVC_WhitelistAdr( client->netchan.remoteAddress );
	}

	// resend all configstrings using the cs commands since these are
	// no longer sent when the client is CS_PRIMED
	SV_UpdateConfigstrings( client );

	// set up the entity for the client
	clientNum = client - svs.clients;
	ent = SV_GentityNum( clientNum );
	ent->s.number = clientNum;
	client->gentity = ent;

	client->lastUserInfoChange = 0; //reset the delay
	client->lastUserInfoCount = 0; //reset the count

	client->deltaMessage = -1;
	client->nextSnapshotTime = svs.time;	// generate a snapshot immediately

	if(cmd)
		memcpy(&client->lastUsercmd, cmd, sizeof(client->lastUsercmd));
	else
		memset(&client->lastUsercmd, '\0', sizeof(client->lastUsercmd));

	// call the game begin function
	GVM_ClientBegin( client - svs.clients );

	SV_BeginAutoRecordDemos();
}

/*
============================================================

CLIENT COMMAND EXECUTION

============================================================
*/

/*
==================
SV_CloseDownload

clear/free any download vars
==================
*/
static void SV_CloseDownload( client_t *cl ) {
	int i;

	// EOF
	if (cl->download) {
		FS_FCloseFile( cl->download );
	}
	cl->download = 0;
	*cl->downloadName = 0;

	// Free the temporary buffer space
	for (i = 0; i < MAX_DOWNLOAD_WINDOW; i++) {
		if (cl->downloadBlocks[i]) {
			Z_Free( cl->downloadBlocks[i] );
			cl->downloadBlocks[i] = NULL;
		}
	}

}

/*
==================
SV_StopDownload_f

Abort a download if in progress
==================
*/
static void SV_StopDownload_f( client_t *cl ) {
	if ( cl->state == CS_ACTIVE )
		return;

	if (*cl->downloadName)
		Com_DPrintf( "clientDownload: %d : file \"%s\" aborted\n", cl - svs.clients, cl->downloadName );

	SV_CloseDownload( cl );
}

/*
==================
SV_DoneDownload_f

Downloads are finished
==================
*/
static void SV_DoneDownload_f( client_t *cl ) {
	if ( cl->state == CS_ACTIVE )
		return;

	Com_DPrintf( "clientDownload: %s Done\n", cl->name);
	// resend the game state to update any clients that entered during the download
	SV_SendClientGameState(cl);
}

/*
==================
SV_NextDownload_f

The argument will be the last acknowledged block from the client, it should be
the same as cl->downloadClientBlock
==================
*/
static void SV_NextDownload_f( client_t *cl )
{
	int block = atoi( Cmd_Argv(1) );

	if ( cl->state == CS_ACTIVE )
		return;

	if (block == cl->downloadClientBlock) {
		Com_DPrintf( "clientDownload: %d : client acknowledge of block %d\n", cl - svs.clients, block );

		// Find out if we are done.  A zero-length block indicates EOF
		if (cl->downloadBlockSize[cl->downloadClientBlock % MAX_DOWNLOAD_WINDOW] == 0) {
			Com_Printf( "clientDownload: %d : file \"%s\" completed\n", cl - svs.clients, cl->downloadName );
			SV_CloseDownload( cl );
			return;
		}

		cl->downloadSendTime = svs.time;
		cl->downloadClientBlock++;
		return;
	}
	// We aren't getting an acknowledge for the correct block, drop the client
	// FIXME: this is bad... the client will never parse the disconnect message
	//			because the cgame isn't loaded yet
	SV_DropClient( cl, "broken download" );
}

/*
==================
SV_BeginDownload_f
==================
*/
static void SV_BeginDownload_f( client_t *cl ) {
	if ( cl->state == CS_ACTIVE )
		return;

	// Kill any existing download
	SV_CloseDownload( cl );

	// cl->downloadName is non-zero now, SV_WriteDownloadToClient will see this and open
	// the file itself
	Q_strncpyz( cl->downloadName, Cmd_Argv(1), sizeof(cl->downloadName) );
}

/*
==================
SV_WriteDownloadToClient

Check to see if the client wants a file, open it if needed and start pumping the client
Fill up msg with data
==================
*/
void SV_WriteDownloadToClient(client_t *cl, msg_t *msg)
{
	int curindex;
	int rate;
	int blockspersnap;
	int unreferenced = 1;
	char errorMessage[1024];
	char pakbuf[MAX_QPATH], *pakptr;
	int numRefPaks;

	if (!*cl->downloadName)
		return;	// Nothing being downloaded

	if(!cl->download)
	{
		qboolean idPack = qfalse;
		qboolean missionPack = qfalse;

 		// Chop off filename extension.
		Com_sprintf(pakbuf, sizeof(pakbuf), "%s", cl->downloadName);
		pakptr = strrchr(pakbuf, '.');

		if(pakptr)
		{
			*pakptr = '\0';

			// Check for pk3 filename extension
			if(!Q_stricmp(pakptr + 1, "pk3"))
			{
				const char *referencedPaks = FS_ReferencedPakNames();

				// Check whether the file appears in the list of referenced
				// paks to prevent downloading of arbitrary files.
				Cmd_TokenizeStringIgnoreQuotes(referencedPaks);
				numRefPaks = Cmd_Argc();

				for(curindex = 0; curindex < numRefPaks; curindex++)
				{
					if(!FS_FilenameCompare(Cmd_Argv(curindex), pakbuf))
					{
						unreferenced = 0;

						// now that we know the file is referenced,
						// check whether it's legal to download it.
						missionPack = FS_idPak(pakbuf, "missionpack");
						idPack = missionPack;
						idPack = (qboolean)(idPack || FS_idPak(pakbuf, BASEGAME));

						break;
					}
				}
			}
		}

		cl->download = 0;

		// We open the file here
		if ( !sv_allowDownload->integer ||
			idPack || unreferenced ||
			( cl->downloadSize = FS_SV_FOpenFileRead( cl->downloadName, &cl->download ) ) < 0 ) {
			// cannot auto-download file
			if(unreferenced)
			{
				Com_Printf("clientDownload: %d : \"%s\" is not referenced and cannot be downloaded.\n", (int) (cl - svs.clients), cl->downloadName);
				Com_sprintf(errorMessage, sizeof(errorMessage), "File \"%s\" is not referenced and cannot be downloaded.", cl->downloadName);
			}
			else if (idPack) {
				Com_Printf("clientDownload: %d : \"%s\" cannot download id pk3 files\n", (int) (cl - svs.clients), cl->downloadName);
				if(missionPack)
				{
					Com_sprintf(errorMessage, sizeof(errorMessage), "Cannot autodownload Team Arena file \"%s\"\n"
									"The Team Arena mission pack can be found in your local game store.", cl->downloadName);
				}
				else
				{
					Com_sprintf(errorMessage, sizeof(errorMessage), "Cannot autodownload id pk3 file \"%s\"", cl->downloadName);
				}
			}
			else if ( !sv_allowDownload->integer ) {
				Com_Printf("clientDownload: %d : \"%s\" download disabled\n", (int) (cl - svs.clients), cl->downloadName);
				if (sv_pure->integer) {
					Com_sprintf(errorMessage, sizeof(errorMessage), "Could not download \"%s\" because autodownloading is disabled on the server.\n\n"
										"You will need to get this file elsewhere before you "
										"can connect to this pure server.\n", cl->downloadName);
				} else {
					Com_sprintf(errorMessage, sizeof(errorMessage), "Could not download \"%s\" because autodownloading is disabled on the server.\n\n"
					"The server you are connecting to is not a pure server, "
					"set autodownload to No in your settings and you might be "
					"able to join the game anyway.\n", cl->downloadName);
				}
			} else {
				// NOTE TTimo this is NOT supposed to happen unless bug in our filesystem scheme?
				//	if the pk3 is referenced, it must have been found somewhere in the filesystem
				Com_Printf("clientDownload: %d : \"%s\" file not found on server\n", (int) (cl - svs.clients), cl->downloadName);
				Com_sprintf(errorMessage, sizeof(errorMessage), "File \"%s\" not found on server for autodownloading.\n", cl->downloadName);
			}
			MSG_WriteByte( msg, svc_download );
			MSG_WriteShort( msg, 0 ); // client is expecting block zero
			MSG_WriteLong( msg, -1 ); // illegal file size
			MSG_WriteString( msg, errorMessage );

			*cl->downloadName = 0;

			if(cl->download)
				FS_FCloseFile(cl->download);

			return;
		}

		Com_Printf( "clientDownload: %d : beginning \"%s\"\n", (int) (cl - svs.clients), cl->downloadName );

		// Init
		cl->downloadCurrentBlock = cl->downloadClientBlock = cl->downloadXmitBlock = 0;
		cl->downloadCount = 0;
		cl->downloadEOF = qfalse;
	}

	// Perform any reads that we need to
	while (cl->downloadCurrentBlock - cl->downloadClientBlock < MAX_DOWNLOAD_WINDOW &&
		cl->downloadSize != cl->downloadCount) {

		curindex = (cl->downloadCurrentBlock % MAX_DOWNLOAD_WINDOW);

		if (!cl->downloadBlocks[curindex])
			cl->downloadBlocks[curindex] = (unsigned char *)Z_Malloc( MAX_DOWNLOAD_BLKSIZE, TAG_DOWNLOAD, qtrue );

		cl->downloadBlockSize[curindex] = FS_Read( cl->downloadBlocks[curindex], MAX_DOWNLOAD_BLKSIZE, cl->download );

		if (cl->downloadBlockSize[curindex] < 0) {
			// EOF right now
			cl->downloadCount = cl->downloadSize;
			break;
		}

		cl->downloadCount += cl->downloadBlockSize[curindex];

		// Load in next block
		cl->downloadCurrentBlock++;
	}

	// Check to see if we have eof condition and add the EOF block
	if (cl->downloadCount == cl->downloadSize &&
		!cl->downloadEOF &&
		cl->downloadCurrentBlock - cl->downloadClientBlock < MAX_DOWNLOAD_WINDOW) {

		cl->downloadBlockSize[cl->downloadCurrentBlock % MAX_DOWNLOAD_WINDOW] = 0;
		cl->downloadCurrentBlock++;

		cl->downloadEOF = qtrue;  // We have added the EOF block
	}

	// Loop up to window size times based on how many blocks we can fit in the
	// client snapMsec and rate

	// based on the rate, how many bytes can we fit in the snapMsec time of the client
	// normal rate / snapshotMsec calculation
	rate = cl->rate;
	if ( sv_maxRate->integer ) {
		if ( sv_maxRate->integer < 1000 ) {
			Cvar_Set( "sv_MaxRate", "1000" );
		}
		if ( sv_maxRate->integer < rate ) {
			rate = sv_maxRate->integer;
		}
	}

	if (!rate) {
		blockspersnap = 1;
	} else {
		blockspersnap = ( (rate * cl->snapshotMsec) / 1000 + MAX_DOWNLOAD_BLKSIZE ) /
			MAX_DOWNLOAD_BLKSIZE;
	}

	if (blockspersnap < 0)
		blockspersnap = 1;

	while (blockspersnap--) {

		// Write out the next section of the file, if we have already reached our window,
		// automatically start retransmitting

		if (cl->downloadClientBlock == cl->downloadCurrentBlock)
			return; // Nothing to transmit

		if (cl->downloadXmitBlock == cl->downloadCurrentBlock) {
			// We have transmitted the complete window, should we start resending?

			//FIXME:  This uses a hardcoded one second timeout for lost blocks
			//the timeout should be based on client rate somehow
			if (svs.time - cl->downloadSendTime > 1000)
				cl->downloadXmitBlock = cl->downloadClientBlock;
			else
				return;
		}

		// Send current block
		curindex = (cl->downloadXmitBlock % MAX_DOWNLOAD_WINDOW);

		MSG_WriteByte( msg, svc_download );
		MSG_WriteShort( msg, cl->downloadXmitBlock );

		// block zero is special, contains file size
		if ( cl->downloadXmitBlock == 0 )
			MSG_WriteLong( msg, cl->downloadSize );

		MSG_WriteShort( msg, cl->downloadBlockSize[curindex] );

		// Write the block
		if ( cl->downloadBlockSize[curindex] ) {
			MSG_WriteData( msg, cl->downloadBlocks[curindex], cl->downloadBlockSize[curindex] );
		}

		Com_DPrintf( "clientDownload: %d : writing block %d\n", (int) (cl - svs.clients), cl->downloadXmitBlock );

		// Move on to the next block
		// It will get sent with next snap shot.  The rate will keep us in line.
		cl->downloadXmitBlock++;

		cl->downloadSendTime = svs.time;
	}
}

/*
=================
SV_Disconnect_f

The client is going to disconnect, so remove the connection immediately  FIXME: move to game?
=================
*/
const char *SV_GetStringEdString(char *refSection, char *refName);
static void SV_Disconnect_f( client_t *cl ) {
//	SV_DropClient( cl, "disconnected" );
	SV_DropClient( cl, SV_GetStringEdString("MP_SVGAME","DISCONNECTED") );
}

/*
=================
SV_VerifyPaks_f

If we are pure, disconnect the client if they do no meet the following conditions:

1. the first two checksums match our view of cgame and ui
2. there are no any additional checksums that we do not have

This routine would be a bit simpler with a goto but i abstained

=================
*/
static void SV_VerifyPaks_f( client_t *cl ) {
	int nChkSum1, nChkSum2, nClientPaks, nServerPaks, i, j, nCurArg;
	int nClientChkSum[1024];
	int nServerChkSum[1024];
	const char *pPaks, *pArg;
	qboolean bGood = qtrue;

	// if we are pure, we "expect" the client to load certain things from
	// certain pk3 files, namely we want the client to have loaded the
	// ui and cgame that we think should be loaded based on the pure setting
	//
	if ( sv_pure->integer != 0 ) {

		bGood = qtrue;
		nChkSum1 = nChkSum2 = 0;
		// we run the game, so determine which cgame and ui the client "should" be running
		//dlls are valid too now -rww
		bGood = (qboolean)(FS_FileIsInPAK("cgamex86.dll", &nChkSum1) == 1);

		if (bGood)
			bGood = (qboolean)(FS_FileIsInPAK("uix86.dll", &nChkSum2) == 1);

		nClientPaks = Cmd_Argc();

		// start at arg 1 ( skip cl_paks )
		nCurArg = 1;

		// we basically use this while loop to avoid using 'goto' :)
		while (bGood) {

			// must be at least 6: "cl_paks cgame ui @ firstref ... numChecksums"
			// numChecksums is encoded
			if (nClientPaks < 6) {
				bGood = qfalse;
				break;
			}
			// verify first to be the cgame checksum
			pArg = Cmd_Argv(nCurArg++);
			if (!pArg || *pArg == '@' || atoi(pArg) != nChkSum1 ) {
				bGood = qfalse;
				break;
			}
			// verify the second to be the ui checksum
			pArg = Cmd_Argv(nCurArg++);
			if (!pArg || *pArg == '@' || atoi(pArg) != nChkSum2 ) {
				bGood = qfalse;
				break;
			}
			// should be sitting at the delimeter now
			pArg = Cmd_Argv(nCurArg++);
			if (*pArg != '@') {
				bGood = qfalse;
				break;
			}
			// store checksums since tokenization is not re-entrant
			for (i = 0; nCurArg < nClientPaks; i++) {
				nClientChkSum[i] = atoi(Cmd_Argv(nCurArg++));
			}

			// store number to compare against (minus one cause the last is the number of checksums)
			nClientPaks = i - 1;

			// make sure none of the client check sums are the same
			// so the client can't send 5 the same checksums
			for (i = 0; i < nClientPaks; i++) {
				for (j = 0; j < nClientPaks; j++) {
					if (i == j)
						continue;
					if (nClientChkSum[i] == nClientChkSum[j]) {
						bGood = qfalse;
						break;
					}
				}
				if (bGood == qfalse)
					break;
			}
			if (bGood == qfalse)
				break;

			// get the pure checksums of the pk3 files loaded by the server
			pPaks = FS_LoadedPakPureChecksums();
			Cmd_TokenizeString( pPaks );
			nServerPaks = Cmd_Argc();
			if (nServerPaks > 1024)
				nServerPaks = 1024;

			for (i = 0; i < nServerPaks; i++) {
				nServerChkSum[i] = atoi(Cmd_Argv(i));
			}

			// check if the client has provided any pure checksums of pk3 files not loaded by the server
			for (i = 0; i < nClientPaks; i++) {
				for (j = 0; j < nServerPaks; j++) {
					if (nClientChkSum[i] == nServerChkSum[j]) {
						break;
					}
				}
				if (j >= nServerPaks) {
					bGood = qfalse;
					break;
				}
			}
			if ( bGood == qfalse ) {
				break;
			}

			// check if the number of checksums was correct
			nChkSum1 = sv.checksumFeed;
			for (i = 0; i < nClientPaks; i++) {
				nChkSum1 ^= nClientChkSum[i];
			}
			nChkSum1 ^= nClientPaks;
			if (nChkSum1 != nClientChkSum[nClientPaks]) {
				bGood = qfalse;
				break;
			}

			// break out
			break;
		}

		cl->gotCP = qtrue;

		if (bGood) {
			cl->pureAuthentic = 1;
		}
		else {
			cl->pureAuthentic = 0;
			cl->nextSnapshotTime = -1;
			cl->state = CS_ACTIVE;
			SV_SendClientSnapshot( cl );
			SV_DropClient( cl, "Unpure client detected. Invalid .PK3 files referenced!" );
		}
	}
}

/*
=================
SV_ResetPureClient_f
=================
*/
static void SV_ResetPureClient_f( client_t *cl ) {
	cl->pureAuthentic = 0;
	cl->gotCP = qfalse;
}

/*
===========
SV_ClientCleanName

Gamecode to engine port (from OpenJK)
============
*/
static void SV_ClientCleanName(const char* in, char* out, int outSize)
{
	int outpos = 0, colorlessLen = 0;

	// discard leading spaces
	for (; *in == ' '; in++);

	// discard leading asterisk's (fail raven for using * as a skipnotify)
	// apparently .* causes the issue too so... derp
	if (svs.servermod == SVMOD_BASEJKA)
		for(; *in == '*'; in++);

	for (; *in && outpos < outSize - 1; in++)
	{
		out[outpos] = *in;

		if (*(in+1) && *(in+1) != '\0' && *(in+2) && *(in+2) != '\0')
		{
			if (*in == ' ' && *(in+1) == ' ' && *(in+2) == ' ') // don't allow more than 3 consecutive spaces
				continue;
		
			if (*in == '@' && *(in+1) == '@' && *(in+2) == '@') // don't allow too many consecutive @ signs
				continue;
		}
		
		if ((byte)* in < 0x20)
			continue;

		switch ((byte)* in)
		{
			default:
				break;
			case 0x81:
			case 0x8D:
			case 0x8F:
			case 0x90:
			case 0x9D:
			case 0xA0:
			case 0xAD:
				continue;
				break;
		}
		
		if (outpos > 0 && out[outpos - 1] == Q_COLOR_ESCAPE)
		{
			if (Q_IsColorStringExt(&out[outpos - 1]))
			{
				colorlessLen--;
			}
			else
			{
				//spaces = ats = 0;
				colorlessLen++;
			}
		}
		else
		{
			//spaces = ats = 0;
			colorlessLen++;
		}
		outpos++;
	}

	out[outpos] = '\0';

	// don't allow empty names
	if (*out == '\0' || colorlessLen == 0)
		Q_strncpyz(out, "Padawan", outSize);
}

/*
=================
SV_UserinfoChanged

Pull specific info from a newly changed userinfo string
into a more C friendly form.
=================
*/
void SV_UserinfoChanged( client_t *cl ) {
	char	*val=NULL, *ip=NULL;
	int		i=0, len=0;
	
	if (sv_legacyFixes->integer && !(sv_legacyFixes->integer & SVFIXES_ALLOW_INVALID_PLAYER_NAMES) &&
		svs.servermod != SVMOD_JAPLUS && svs.servermod != SVMOD_MBII && svs.servermod != SVMOD_JAPRO)
	{
		char	cleanName[64];
		
		val = Info_ValueForKey(cl->userinfo, "name");

		SV_ClientCleanName(val, cleanName, sizeof(cleanName));
		Info_SetValueForKey(cl->userinfo, "name", cleanName);
		Q_strncpyz(cl->name, cleanName, sizeof(cl->name));
	}
	else
	{
		// name for C code
		Q_strncpyz( cl->name, Info_ValueForKey (cl->userinfo, "name"), sizeof(cl->name) );
	}

	// rate command

	// if the client is on the same subnet as the server and we aren't running an
	// internet public server, assume they don't need a rate choke
	if ( Sys_IsLANAddress( cl->netchan.remoteAddress ) && com_dedicated->integer != 2 && sv_lanForceRate->integer == 1 ) {
		cl->rate = 100000;	// lans should not rate limit
	} else {
		val = Info_ValueForKey (cl->userinfo, "rate");
		if (sv_ratePolicy->integer == 1)
		{
			// NOTE: what if server sets some dumb sv_clientRate value?
			cl->rate = sv_clientRate->integer;
		}
		else if( sv_ratePolicy->integer == 2)
		{
			i = atoi(val);
			if (!i) {
				i = sv_maxRate->integer; //FIXME old code was 3000 here, should increase to 5000 instead or maxRate?
			}
			i = Com_Clampi(1000, 100000, i);
			i = Com_Clampi( sv_minRate->integer, sv_maxRate->integer, i );
			if (i != cl->rate) {
				cl->rate = i;
			}
		}
	}

	// snaps command
	//Note: cl->snapshotMsec is also validated in sv_main.cpp -> SV_CheckCvars if sv_fps, sv_snapsMin or sv_snapsMax is changed
	int minSnaps = sv_snapsMin->integer > 0 ? Com_Clampi(1, sv_snapsMax->integer, sv_snapsMin->integer) : 1; // between 1 and sv_snapsMax ( 1 <-> 40 )
	int maxSnaps = sv_snapsMax->integer > 0 ? Q_min(sv_fps->integer, sv_snapsMax->integer) : sv_fps->integer; // can't produce more than sv_fps snapshots/sec, but can send less than sv_fps snapshots/sec
	val = Info_ValueForKey(cl->userinfo, "snaps");
	cl->wishSnaps = atoi(val);
	if (!cl->wishSnaps)
		cl->wishSnaps = maxSnaps;
	if (sv_fps && sv_fps->integer && sv_snapsPolicy->integer == 1)
	{
		cl->wishSnaps = sv_fps->integer;
		i = 1000 / sv_fps->integer;
		if (i != cl->snapshotMsec) {
			// Reset next snapshot so we avoid desync between server frame time and snapshot send time
			cl->nextSnapshotTime = -1;
			cl->snapshotMsec = i;
		}
	}
	else if (sv_snapsPolicy->integer == 2)
	{
		i = 1000 / Com_Clampi(minSnaps, maxSnaps, cl->wishSnaps);
		if (i != cl->snapshotMsec) {
			// Reset next snapshot so we avoid desync between server frame time and snapshot send time
			cl->nextSnapshotTime = -1;
			cl->snapshotMsec = i;
		}
	}

	// TTimo
	// maintain the IP information
	// the banning code relies on this being consistently present
	if( NET_IsLocalAddress(cl->netchan.remoteAddress) )
		ip = "localhost";
	else
		ip = (char*)NET_AdrToString( cl->netchan.remoteAddress );

	val = Info_ValueForKey( cl->userinfo, "ip" );
	if( val[0] )
		len = strlen( ip ) - strlen( val ) + strlen( cl->userinfo );
	else
		len = strlen( ip ) + 4 + strlen( cl->userinfo );

	if( len >= MAX_INFO_STRING )
		SV_DropClient( cl, "userinfo string length exceeded" );
	else
		Info_SetValueForKey( cl->userinfo, "ip", ip );

	val = Info_ValueForKey(cl->userinfo, "model");
#ifdef DEDICATED
	if (val)
	{
		if (!Q_stricmpn(val, "darksidetools", 13) && cl->netchan.remoteAddress.type != NA_LOOPBACK) {
			Com_Printf("%sDetected DST injection from client %s%s\n", S_COLOR_RED, S_COLOR_WHITE, cl->name);
			if (sv_antiDST->integer) {
				//SV_DropClient(cl, "was dropped by TnG!");
				SV_DropClient(cl, "was kicked for cheating by JKA.io");
				cl->lastPacketTime = svs.time;
			}
		}

		// Fix: Don't allow bugged models
		if (sv_legacyFixes->integer && !(sv_legacyFixes->integer & SVFIXES_ALLOW_BROKEN_MODELS) && svs.servermod != SVMOD_MBII)
		{
			len = (int)strlen(val);
			qboolean badModel = qfalse;

			if (!Q_stricmpn(val, "jedi_", len) && (!Q_stricmpn(val, "jedi_/red", len) || !Q_stricmpn(val, "jedi_/blue", len)))
				badModel = qtrue;
			else if (!Q_stricmpn(val, "rancor", 6))
				badModel = qtrue;
			else if (!Q_stricmpn(val, "wampa", 5))
				badModel = qtrue;

			if (badModel)
				Info_SetValueForKey(cl->userinfo, "model", "kyle");
		}
	}
#endif

	if (sv_legacyFixes->integer && !(sv_legacyFixes->integer & SVFIXES_ALLOW_INVALID_FORCEPOWERS))
	{
		char forcePowers[30];
		Q_strncpyz(forcePowers, Info_ValueForKey(cl->userinfo, "forcepowers"), sizeof(forcePowers));

		int len = (int)strlen(forcePowers);
		qboolean badForce = qfalse;
		if (len >= 22 && len <= 24) {
			byte seps = 0;

			for (int i = 0; i < len; i++) {
				if (forcePowers[i] != '-' && (forcePowers[i] < '0' || forcePowers[i] > '9')) {
					badForce = qtrue;
					break;
				}

				if (forcePowers[i] == '-' && (i < 1 || i > 5)) {
					badForce = qtrue;
					break;
				}

				if (i && forcePowers[i - 1] == '-' && forcePowers[i] == '-') {
					badForce = qtrue;
					break;
				}

				if (forcePowers[i] == '-') {
					seps++;
				}
			}

			if (seps != 2) {
				badForce = qtrue;
			}
		} else {
			badForce = qtrue;
		}

		if (badForce)
			Q_strncpyz(forcePowers, "7-1-030000000000003332", sizeof(forcePowers));

		Info_SetValueForKey(cl->userinfo, "forcepowers", forcePowers);
	}

#ifdef DEDICATED
	cl->disableDuelCull = qfalse;
	cl->jpPlugin = qfalse;
	if (svs.servermod == SVMOD_JAPLUS || svs.servermod == SVMOD_JAPRO) { //allow JA+ clients to configure duel isolation on JA+ servers using /pluginDisable
		val = Info_ValueForKey(cl->userinfo, "cjp_client");
		if (val && strlen(val) >= 3)
		{ //make sure they have some version of the plugin
			cl->jpPlugin = qtrue;
			val = Info_ValueForKey(cl->userinfo, "cp_pluginDisable");
			if (svs.servermod == SVMOD_JAPRO && (atoi(val) & (1 << 1))) { //JAPRO_PLUGIN_DUELSEEOTHERS
				cl->disableDuelCull = qtrue;
			}
		}
	}
#endif
}

#define INFO_CHANGE_MIN_INTERVAL	6000 //6 seconds is reasonable I suppose
#define INFO_CHANGE_MAX_COUNT		3 //only allow 3 changes within the 6 seconds

/*
==================
SV_UpdateUserinfo_f
==================
*/
static void SV_UpdateUserinfo_f( client_t *cl ) {
	char *arg = Cmd_Argv(1);

	// Stop random empty /userinfo calls without hurting anything
	if( !arg || !*arg )
		return;

#ifdef FINAL_BUILD
	if (cl->lastUserInfoChange > svs.time)
	{
		cl->lastUserInfoCount++;

		if (cl->lastUserInfoCount >= INFO_CHANGE_MAX_COUNT)
		{
		//	SV_SendServerCommand(cl, "print \"Warning: Too many info changes, last info ignored\n\"\n");
		//	SV_SendServerCommand(cl, "print \"@@@TOO_MANY_INFO\n\"\n");
			Q_strncpyz( cl->userinfoPostponed, arg, sizeof(cl->userinfoPostponed) );
			SV_SendServerCommand(cl, "print \"Warning: Too many info changes, last info postponed\n\"\n");
			return;
		}
	}
	else
#endif
	{
		cl->userinfoPostponed[0] = 0;
		cl->lastUserInfoCount = 0;
		cl->lastUserInfoChange = svs.time + INFO_CHANGE_MIN_INTERVAL;
	}

	Q_strncpyz(cl->userinfo, arg, sizeof(cl->userinfo));
	SV_UserinfoChanged( cl );
	// call prog code to allow overrides
	GVM_ClientUserinfoChanged( cl - svs.clients );
}

typedef struct ucmd_s {
	const char	*name;
	void	(*func)( client_t *cl );
} ucmd_t;

static ucmd_t ucmds[] = {
	{"userinfo", SV_UpdateUserinfo_f},
	{"disconnect", SV_Disconnect_f},
	{"cp", SV_VerifyPaks_f},
	{"vdr", SV_ResetPureClient_f},
	{"download", SV_BeginDownload_f},
	{"nextdl", SV_NextDownload_f},
	{"stopdl", SV_StopDownload_f},
	{"donedl", SV_DoneDownload_f},

	{NULL, NULL}
};

/*
==================
SV_ExecuteClientCommand

Also called by bot code
==================
*/
void SV_ExecuteClientCommand( client_t *cl, const char *s, qboolean clientOK ) {
	const ucmd_t *u;
	const char *cmd;
	const char *arg1;
	const char *arg2;
	qboolean bProcessed = qfalse;
	qboolean sayCmd = qfalse;

	Cmd_TokenizeString(s);

	cmd = Cmd_Argv(0);
	arg1 = Cmd_Argv(1);
	arg2 = Cmd_Argv(2);

	// see if it is a server level command
	for (u = ucmds; u->name; u++)
	{
		if (!strcmp(cmd, u->name))
		{
			u->func(cl);
			bProcessed = qtrue;
			
			break;
		}
	}

#ifdef DEDICATED
	if (!Q_stricmpn(cmd, "jkaDST_", 7) && cl->netchan.remoteAddress.type != NA_LOOPBACK) { //typo'd a mistyped DST setting
		Com_Printf("%sDetected DST command from client %s%s\n", S_COLOR_RED, S_COLOR_WHITE, cl->name);
		if (sv_antiDST->integer) {
			//SV_DropClient(cl, "was dropped by TnG!");
			SV_DropClient(cl, "was kicked for cheating by JKA.io");
			cl->lastPacketTime = svs.time;
		}
	}
#endif

	if (!Q_stricmpn(cmd, "say", 3) || !Q_stricmpn(cmd, "say_team", 8) || !Q_stricmpn(cmd, "tell", 4))
	{
		sayCmd = qtrue;

		// 256 because we don't need more, the chat can handle 150 max char
		// and allowing 256 prevent a message to not be sent instead of being truncated
		// if this is a bit more than 150
		if (svs.gvmIsLegacy && sv_legacyFixes->integer && strlen(Cmd_Args()) > 256)
		{
			clientOK = qfalse;
		}
	}

	if (sv_legacyFixes->integer && svs.servermod != SVMOD_MBII)
	{
		if (!(sv_legacyFixes->integer & SVFIXES_DISABLE_GC_CRASHFIX) && !Q_stricmpn(cmd, "gc", 2) && atoi(arg1) >= sv_maxclients->integer)
			clientOK = qfalse;

		if (!(sv_legacyFixes->integer & SVFIXES_DISABLE_NPC_CRASHFIX) && svs.servermod != SVMOD_JAPRO &&
			!Q_stricmpn(cmd, "npc", 3) && !Q_stricmpn(arg1, "spawn", 5) && (!Q_stricmpn(arg2, "ragnos", 6) || !Q_stricmpn(arg2, "saber_droid", 6)))
			clientOK = qfalse;

		// Fix: team crash
		if (!(sv_legacyFixes->integer & SVFIXES_DISABLE_TEAM_CRASHFIX)
			&& !Q_stricmpn(cmd, "team", 4) && (!Q_stricmpn(arg1, "follow1", 7) || !Q_stricmpn(arg1, "follow2", 7)))
			clientOK = qfalse;

		// Disable: callteamvote, useless in basejka and can lead to a bugged UI on custom client
		if (!(sv_legacyFixes->integer & SVFIXES_ALLOW_CALLTEAMVOTE) && svs.servermod == SVMOD_BASEJKA && !Q_stricmpn(cmd, "callteamvote", 12))
			clientOK = qfalse;

		// Fix: callvote fraglimit/timelimit with negative value
		if (!(sv_legacyFixes->integer & SVFIXES_ALLOW_NEGATIVE_CALLVOTES) && svs.servermod == SVMOD_BASEJKA && !Q_stricmpn(cmd, "callvote", 8) && (!Q_stricmpn(arg1, "fraglimit", 9) || !Q_stricmpn(arg1, "timelimit", 9)) && atoi(arg2) < 0)
			clientOK = qfalse;
	}

	if (clientOK) {
		// pass unknown strings to the game
		if (!u->name && sv.state == SS_GAME && (cl->state == CS_ACTIVE || cl->state == CS_PRIMED)) {
			// strip \r \n and ;
			if ( sv_filterCommands->integer ) {
				Cmd_Args_Sanitize( MAX_CVAR_VALUE_STRING, "\n\r", "  " );
				if (sv_filterCommands->integer == 2 && !sayCmd) {
					// also strip ';' for callvote
					Cmd_Args_Sanitize( MAX_CVAR_VALUE_STRING, ";", " " );
				}
			}
			GVM_ClientCommand( cl - svs.clients );
		}
	}
	else if (!bProcessed)
	{
		Com_DPrintf( "client text ignored for %s: %s\n", cl->name, cmd);
	}
}

/*
===============
SV_ClientCommand
===============
*/
static qboolean SV_ClientCommand( client_t *cl, msg_t *msg ) {
	int		seq;
	const char	*s;
	qboolean clientOk = qtrue;

	seq = MSG_ReadLong( msg );
	s = MSG_ReadString( msg );

	// see if we have already executed it
	if ( cl->lastClientCommand >= seq ) {
		return qtrue;
	}

	Com_DPrintf( "clientCommand: %s : %i : %s\n", cl->name, seq, s );

	// drop the connection if we have somehow lost commands
	if ( seq > cl->lastClientCommand + 1 ) {
		Com_Printf( "Client %s lost %i clientCommands\n", cl->name,
			seq - cl->lastClientCommand + 1 );
		SV_DropClient( cl, "Lost reliable commands" );
		return qfalse;
	}

	// malicious users may try using too many string commands
	// to lag other players.  If we decide that we want to stall
	// the command, we will stop processing the rest of the packet,
	// including the usercmd.  This causes flooders to lag themselves
	// but not other people
	// We don't do this when the client hasn't been active yet since its
	// normal to spam a lot of commands when downloading
	if ( !com_cl_running->integer &&
		cl->state >= CS_ACTIVE &&
		sv_floodProtect->integer )
	{
		const int floodTime = (sv_floodProtect->integer == 1) ? 1000 : sv_floodProtect->integer;
		if ( svs.time < (cl->lastReliableTime + floodTime) ) {
			// ignore any other text messages from this client but let them keep playing
			// TTimo - moved the ignored verbose to the actual processing in SV_ExecuteClientCommand, only printing if the core doesn't intercept
			clientOk = qfalse;
		}
		else {
			cl->lastReliableTime = svs.time;
		}
		if ( sv_floodProtectSlow->integer ) {
			cl->lastReliableTime = svs.time;
		}
	}

	SV_ExecuteClientCommand( cl, s, clientOk );

	cl->lastClientCommand = seq;
	Com_sprintf(cl->lastClientCommandString, sizeof(cl->lastClientCommandString), "%s", s);

	return qtrue;		// continue procesing
}


//==================================================================================


/*
==================
SV_ClientThink

Also called by bot code
==================
*/
void SV_ClientThink (client_t *cl, usercmd_t *cmd) {
#ifdef DEDICATED
	playerState_t *ps = NULL;

	if ( cl->state != CS_ACTIVE ) {
		cl->lastUsercmd = *cmd;
		return; // may have been kicked during the last usercmd
	}

	ps = SV_GameClientNum(cl - svs.clients);
	if (sv_legacyFixes->integer && !(sv_legacyFixes->integer & SVFIXES_DISABLE_SPEC_ALTFIRE_FOLLOWPREV)
		&& (svs.servermod == SVMOD_BASEJKA || svs.servermod == SVMOD_JAPLUS) && ps && (ps->pm_flags & PMF_FOLLOW)
		&& (cmd->buttons & BUTTON_ALT_ATTACK) && !(cmd->buttons & BUTTON_ATTACK) && !(cl->lastUsercmd.buttons & BUTTON_ALT_ATTACK))
	{ //allow alt attack to go back one player in spectator
		SV_ExecuteClientCommand(cl, "followPrev", qtrue);
	}
	cl->lastUsercmd = *cmd;
#else
	cl->lastUsercmd = *cmd;

	if ( cl->state != CS_ACTIVE ) {
		return;		// may have been kicked during the last usercmd
	}
#endif

	if ( cl->lastUserInfoCount >= INFO_CHANGE_MAX_COUNT && cl->lastUserInfoChange < svs.time && cl->userinfoPostponed[0] )
	{ // Update postponed userinfo changes now
		char info[MAX_INFO_STRING];

		Q_strncpyz( cl->userinfo, cl->userinfoPostponed, sizeof(cl->userinfo) );
		SV_UserinfoChanged( cl );

		// call prog code to allow overrides
		GVM_ClientUserinfoChanged(cl - svs.clients);

		// get the name out of the game and set it in the engine
		SV_GetConfigstring(CS_PLAYERS + (cl - svs.clients), info, sizeof(info));
		Info_SetValueForKey(cl->userinfo, "name", Info_ValueForKey(info, "n"));
		Q_strncpyz(cl->name, Info_ValueForKey(info, "n"), sizeof(cl->name));

		// clear it
		cl->userinfoPostponed[0] = 0;
		cl->lastUserInfoCount = 0;
		cl->lastUserInfoChange = svs.time + INFO_CHANGE_MIN_INTERVAL;
	}

	GVM_ClientThink( cl - svs.clients, NULL );
}

/*
==================
SV_UserMove

The message usually contains all the movement commands
that were in the last three packets, so that the information
in dropped packets can be recovered.

On very fast clients, there may be multiple usercmd packed into
each of the backup packets.
==================
*/
#ifdef DEDICATED
static unsigned short previousPacketDeltas[MAX_CLIENTS][PACKET_BACKUP];
static unsigned short previousPacketDeltasIndex[MAX_CLIENTS];
#endif
static void SV_UserMove( client_t *cl, msg_t *msg, qboolean delta ) {
	int			i, key;
	int			cmdCount;
	usercmd_t	nullcmd;
	usercmd_t	cmds[MAX_PACKET_USERCMDS];
	usercmd_t	*cmd, *oldcmd;
	qboolean	fixPing = (qboolean)sv_pingFix->integer;
#ifdef DEDICATED
	int			oldServerTime = 0, firstServerTime = 0, lastServerTime = 0;
#endif

	if ( delta ) {
		cl->deltaMessage = cl->messageAcknowledge;
	} else {
		cl->deltaMessage = -1;
	}

	cmdCount = MSG_ReadByte( msg );

	if ( cmdCount < 1 ) {
		Com_Printf( "cmdCount < 1\n" );
		return;
	}

	if ( cmdCount > MAX_PACKET_USERCMDS ) {
		Com_Printf( "cmdCount > MAX_PACKET_USERCMDS\n" );
		return;
	}

#ifdef DEDICATED
	if (cl->lastUsercmd.serverTime)
		oldServerTime = cl->lastUsercmd.serverTime;

	if (cl->unfixPing) {
		if (sv_pingFix->integer != 2)
			cl->unfixPing = qfalse;
		else if (fixPing && cl->unfixPing)
			fixPing = qfalse;
	}
#endif

	// use the checksum feed in the key
	key = sv.checksumFeed;
	// also use the message acknowledge
	key ^= cl->messageAcknowledge;
	// also use the last acknowledged server command in the key
	key ^= Com_HashKey(cl->reliableCommands[ cl->reliableAcknowledge & (MAX_RELIABLE_COMMANDS-1) ], 32);

	Com_Memset( &nullcmd, 0, sizeof(nullcmd) );
	oldcmd = &nullcmd;
	for ( i = 0 ; i < cmdCount ; i++ ) {
		cmd = &cmds[i];
		MSG_ReadDeltaUsercmdKey( msg, key, oldcmd, cmd );
		if ( sv_legacyFixes->integer )
		{
			if (!(sv_legacyFixes->integer & SVFIXES_ALLOW_INVALID_FORCESEL) && (cmd->forcesel == FP_LEVITATION || cmd->forcesel >= NUM_FORCE_POWERS))
			{ // block "charge jump" and other nonsense
				cmd->forcesel = 0xFFu;
			}

			if (!(sv_legacyFixes->integer & SVFIXES_ALLOW_INVALID_VIEWANGLES))
			{ // affects speed calculation
				cmd->angles[ROLL] = 0;
			}
		}

		if ( sv_strictPacketTimestamp->integer && cl->state == CS_ACTIVE ) {
			if ( cmd->serverTime < (sv.time - 1000) ) {
				static int lastWarnTime = 0;
				if ( lastWarnTime < sv.time - 5000 ) {
					lastWarnTime = sv.time;
					Com_DPrintf(
						"client %i(%i) serverTime too low (%i < %i: %.2fs)\n",
						cl - svs.clients,
						cl->state,
						cmd->serverTime,
						sv.time - 1000,
						((sv.time - 1000) - cmd->serverTime) / 1000.0f
					);
				}
				cmd->serverTime = sv.time - 1000;
			}
			else if ( cmd->serverTime > (sv.time + 200) ) {
				static int lastWarnTime = 0;
				if ( lastWarnTime < sv.time - 5000 ) {
					lastWarnTime = sv.time;
					Com_DPrintf(
						"client %i:%i serverTime in future (%i > %i: %.2fs)\n",
						cl - svs.clients,
						cl->state,
						cmd->serverTime,
						sv.time + 200,
						((sv.time + 200) - cmd->serverTime) / 1000.0f
					);
				}
				cmd->serverTime = sv.time + 200;
			}
}
		oldcmd = cmd;
	}

	// save time for ping calculation
	// With sv_pingFix enabled we store the time of the first acknowledge, instead of the last. And we use a time value that is not limited by sv_fps.
	if (!fixPing || cl->frames[cl->messageAcknowledge & PACKET_MASK].messageAcked == -1)
		cl->frames[cl->messageAcknowledge & PACKET_MASK].messageAcked = (fixPing ? Sys_Milliseconds() : svs.time);

	// TTimo
	// catch the no-cp-yet situation before SV_ClientEnterWorld
	// if CS_ACTIVE, then it's time to trigger a new gamestate emission
	// if not, then we are getting remaining parasite usermove commands, which we should ignore
	if (sv_pure->integer != 0 && cl->pureAuthentic == 0 && !cl->gotCP) {
		if (cl->state == CS_ACTIVE)
		{
			// we didn't get a cp yet, don't assume anything and just send the gamestate all over again
			Com_DPrintf( "%s: didn't get cp command, resending gamestate\n", cl->name);
			SV_SendClientGameState( cl );
		}
		return;
	}

	// if this is the first usercmd we have received
	// this gamestate, put the client into the world
	if ( cl->state == CS_PRIMED ) {
		SV_ClientEnterWorld( cl, &cmds[0] );
		// the moves can be processed normaly
#ifdef DEDICATED //triggers message after loading in
		if (!cl->chatLogPolicySent && svs.servermod == SVMOD_MBII) {
			SV_SendClientChatLogPolicy(cl);
		}
#endif
	}

	// a bad cp command was sent, drop the client
	if (sv_pure->integer != 0 && cl->pureAuthentic == 0) {
		SV_DropClient( cl, "Cannot validate pure client!");
		return;
	}

	if ( cl->state != CS_ACTIVE ) {
		cl->deltaMessage = -1;
		return;
	}

	// usually, the first couple commands will be duplicates
	// of ones we have previously received, but the servertimes
	// in the commands will cause them to be immediately discarded
	for ( i =  0 ; i < cmdCount ; i++ ) {
		// if this is a cmd from before a map_restart ignore it
		if ( cmds[i].serverTime > cmds[cmdCount-1].serverTime ) {
			continue;
		}
		// extremely lagged or cmd from before a map_restart
		//if ( cmds[i].serverTime > svs.time + 3000 ) {
		//	continue;
		//}
		// don't execute if this is an old cmd which is already executed
		// these old cmds are included when cl_packetdup > 0
		if ( cmds[i].serverTime <= cl->lastUsercmd.serverTime ) {
			continue;
		}
#ifdef DEDICATED
		else if (!firstServerTime) {
			firstServerTime = cmds[i].serverTime;
		}
		else if (cmds[i].serverTime > lastServerTime) {
			lastServerTime = cmds[i].serverTime;
		}
#endif
		SV_ClientThink (cl, &cmds[ i ]);
	}

#ifdef DEDICATED
	if (lastServerTime <= 0) {//lastServerTime is always 0 if client is sending 1 cmd per packet
		lastServerTime = firstServerTime;
	}

	if (sv_pingFix->integer == 2 && oldServerTime > 0 && firstServerTime > 0 && lastServerTime > 0)
	{
		//int serverFrameMsec = (1000 / sv_fps->integer);
		int packetDelta = lastServerTime - oldServerTime;

		if (packetDelta > 0) 
		{
			int w;
			int clientNum = cl - svs.clients;
			int total = 0, average = 0;

			previousPacketDeltas[clientNum][previousPacketDeltasIndex[clientNum] % PACKET_BACKUP] = packetDelta;
			previousPacketDeltasIndex[clientNum]++;
			for (w = 0; w < PACKET_BACKUP; w++) { //smooth out packetDelta
				total += previousPacketDeltas[clientNum][w];
			}

			if (!total) {//shouldn't happen, but don't divide by 0...
				total = packetDelta; //1
			}
			average = Round(total / PACKET_BACKUP);

			//allowing for some leeway but is supposed to use old ping calculation if their packet rate is less than 55-60
			//cl->unfixPing = (qboolean)(packetDelta > 20);// serverFrameMsec)
			cl->unfixPing = (qboolean)(average > 20);

			if (cl->unfixPing && com_developer->integer > 3) { //debug spew...
				char buf[MAX_STRING_CHARS] = { 0 };
				Com_sprintf(buf, sizeof(buf),
					S_COLOR_MAGENTA "Packet delta too low -  using old ping calc on client %i (delta %i average %i count %i)\n", packetDelta, average, cmdCount, cl - svs.clients);
				Com_Printf(buf);
				SV_SendServerCommand(cl, "print \"%s\"", buf);
			}
		}
	}
#endif
}


/*
===========================================================================

USER CMD EXECUTION

===========================================================================
*/

/*
===================
SV_ExecuteClientMessage

Parse a client packet
===================
*/
void SV_ExecuteClientMessage( client_t *cl, msg_t *msg ) {
	int			c;
	int			serverId;

	MSG_Bitstream(msg);

	serverId = MSG_ReadLong( msg );
	cl->messageAcknowledge = MSG_ReadLong( msg );

	if (cl->messageAcknowledge < 0) {
		// usually only hackers create messages like this
		// it is more annoying for them to let them hanging
		//SV_DropClient( cl, "illegible client message" );
		return;
	}

	cl->reliableAcknowledge = MSG_ReadLong( msg );

	// NOTE: when the client message is fux0red the acknowledgement numbers
	// can be out of range, this could cause the server to send thousands of server
	// commands which the server thinks are not yet acknowledged in SV_UpdateServerCommandsToClient
	if (cl->reliableAcknowledge < cl->reliableSequence - MAX_RELIABLE_COMMANDS) {
		// usually only hackers create messages like this
		// it is more annoying for them to let them hanging
		//SV_DropClient( cl, "illegible client message" );
		cl->reliableAcknowledge = cl->reliableSequence;
		return;
	}
	// if this is a usercmd from a previous gamestate,
	// ignore it or retransmit the current gamestate
	//
	// if the client was downloading, let it stay at whatever serverId and
	// gamestate it was at.  This allows it to keep downloading even when
	// the gamestate changes.  After the download is finished, we'll
	// notice and send it a new game state
	//
	// https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=536
	// don't drop as long as previous command was a nextdl, after a dl is done, downloadName is set back to ""
	// but we still need to read the next message to move to next download or send gamestate
	// I don't like this hack though, it must have been working fine at some point, suspecting the fix is somewhere else
	if ( serverId != sv.serverId && !*cl->downloadName && !strstr(cl->lastClientCommandString, "nextdl") ) {
		if ( serverId >= sv.restartedServerId && serverId < sv.serverId ) { // TTimo - use a comparison here to catch multiple map_restart
			// they just haven't caught the map_restart yet
			Com_DPrintf("%s : ignoring pre map_restart / outdated client message\n", cl->name);
			return;
		}
		// if we can tell that the client has dropped the last
		// gamestate we sent them, resend it
		// Fix for https://bugzilla.icculus.org/show_bug.cgi?id=6324
		if ( cl->state != CS_ACTIVE && cl->messageAcknowledge > cl->gamestateMessageNum ) {
			Com_DPrintf( "%s : dropped gamestate, resending\n", cl->name );
			SV_SendClientGameState( cl );
		}
		return;
	}

	// this client has acknowledged the new gamestate so it's
	// safe to start sending it the real time again
	if( cl->oldServerTime && serverId == sv.serverId ) {
		Com_DPrintf( "%s acknowledged gamestate\n", cl->name );
		cl->oldServerTime = 0;
	}

	// read optional clientCommand strings
	do {
		c = MSG_ReadByte( msg );
		if ( c == clc_EOF ) {
			break;
		}
		if ( c != clc_clientCommand ) {
			break;
		}
		if ( !SV_ClientCommand( cl, msg ) ) {
			return;	// we couldn't execute it because of the flood protection
		}
		if (cl->state == CS_ZOMBIE) {
			return;	// disconnect command
		}
	} while ( 1 );

	// read the usercmd_t
	if ( c == clc_move ) {
		SV_UserMove( cl, msg, qtrue );
	} else if ( c == clc_moveNoDelta ) {
		SV_UserMove( cl, msg, qfalse );
	} else if ( c != clc_EOF ) {
		Com_Printf( "WARNING: bad command byte for client %i\n", cl - svs.clients );
	}
//	if ( msg->readcount != msg->cursize ) {
//		Com_Printf( "WARNING: Junk at end of packet for client %i\n", cl - svs.clients );
//	}
}

