/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
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

// cl_cgame.c  -- client system interaction with client game
#include "client.h"
#include "cl_cgameapi.h"
#include "botlib/botlib.h"
#include "FXExport.h"
#include "FxUtil.h"
#include "qcommon/RoffSystem.h"
#include "qcommon/stringed_ingame.h"
#include "ghoul2/G2_gore.h"

extern IHeapAllocator *G2VertSpaceClient;

#include "snd_ambient.h"
#include "qcommon/timing.h"

/*
Ghoul2 Insert End
*/

extern	botlib_export_t	*botlib_export;

extern qboolean loadCamera(const char *name);
extern void startCamera(int time);
extern qboolean getCameraInfo(int time, vec3_t *origin, vec3_t *angles);

/*
====================
CL_GetUserCmd
====================
*/
qboolean CL_GetUserCmd( int cmdNumber, usercmd_t *ucmd ) {
	// cmds[cmdNumber] is the last properly generated command
	const int REAL_CMD_MASK = (cl_commandsize->integer >= 4 && cl_commandsize->integer <= 512) ? (cl_commandsize->integer - 1) : (CMD_MASK);//Loda - FPS UNLOCK ENGINE

	// can't return anything that we haven't created yet
	if ( cmdNumber > cl.cmdNumber ) {
		Com_Error( ERR_DROP, "CL_GetUserCmd: %i >= %i", cmdNumber, cl.cmdNumber );
	}

	// the usercmd has been overwritten in the wrapping
	// buffer because it is too far out of date
	if ( cmdNumber <= cl.cmdNumber - (REAL_CMD_MASK +1) ) { //Originaly CMD_BACKUP, but CMD_BACKUP is just CMD_MASK+1, //Loda - FPS UNLOCK ENGINE
		return qfalse;
	}

	*ucmd = cl.cmds[ cmdNumber & REAL_CMD_MASK ];//Loda - FPS UNLOCK ENGINE

	return qtrue;
}

/*
====================
CL_GetParseEntityState
====================
*/
qboolean	CL_GetParseEntityState( int parseEntityNumber, entityState_t *state ) {
	// can't return anything that hasn't been parsed yet
	if ( parseEntityNumber >= cl.parseEntitiesNum ) {
		Com_Error( ERR_DROP, "CL_GetParseEntityState: %i >= %i",
			parseEntityNumber, cl.parseEntitiesNum );
	}

	// can't return anything that has been overwritten in the circular buffer
	if ( parseEntityNumber <= cl.parseEntitiesNum - MAX_PARSE_ENTITIES ) {
		return qfalse;
	}

	*state = cl.parseEntities[ parseEntityNumber & ( MAX_PARSE_ENTITIES - 1 ) ];
	return qtrue;
}

/*
====================
CL_GetSnapshot
====================
*/
qboolean CL_GetSnapshot( int snapshotNumber, snapshot_t *snapshot ) {
	clSnapshot_t	*clSnap;
	int				i, count;

	if ( snapshotNumber > cl.snap.messageNum ) {
		Com_Error( ERR_DROP, "CL_GetSnapshot: snapshotNumber > cl.snapshot.messageNum" );
	}

	// if the frame has fallen out of the circular buffer, we can't return it
	if ( cl.snap.messageNum - snapshotNumber >= PACKET_BACKUP ) {
		return qfalse;
	}

	// if the frame is not valid, we can't return it
	clSnap = &cl.snapshots[snapshotNumber & PACKET_MASK];
	if ( !clSnap->valid ) {
		return qfalse;
	}

	// if the entities in the frame have fallen out of their
	// circular buffer, we can't return it
	if ( cl.parseEntitiesNum - clSnap->parseEntitiesNum >= MAX_PARSE_ENTITIES ) {
		return qfalse;
	}

	// write the snapshot
	snapshot->snapFlags = clSnap->snapFlags;
	snapshot->serverCommandSequence = clSnap->serverCommandNum;
	snapshot->ping = clSnap->ping;
	snapshot->serverTime = clSnap->serverTime;
	Com_Memcpy( snapshot->areamask, clSnap->areamask, sizeof( snapshot->areamask ) );
	snapshot->ps = clSnap->ps;
	snapshot->vps = clSnap->vps; //get the vehicle ps
	count = clSnap->numEntities;
	if ( count > MAX_ENTITIES_IN_SNAPSHOT ) {
		Com_DPrintf( "CL_GetSnapshot: truncated %i entities to %i\n", count, MAX_ENTITIES_IN_SNAPSHOT );
		count = MAX_ENTITIES_IN_SNAPSHOT;
	}
	snapshot->numEntities = count;

 	for ( i = 0 ; i < count ; i++ ) {

		int entNum =  ( clSnap->parseEntitiesNum + i ) & (MAX_PARSE_ENTITIES-1) ;

		// copy everything but the ghoul2 pointer
		memcpy(&snapshot->entities[i], &cl.parseEntities[ entNum ], sizeof(entityState_t));
	}

	// FIXME: configstring changes and server commands!!!

	return qtrue;
}

qboolean CL_GetDefaultState(int index, entityState_t *state)
{
	if (index < 0 || index >= MAX_GENTITIES)
	{
		return qfalse;
	}

	if (!(cl.entityBaselines[index].eFlags & EF_PERMANENT))
	{
		return qfalse;
	}

	*state = cl.entityBaselines[index];

	return qtrue;
}

/*
=====================
CL_SetUserCmdValue
=====================
*/
extern float cl_mPitchOverride;
extern float cl_mYawOverride;
extern float cl_mSensitivityOverride;
void CL_SetUserCmdValue( int userCmdValue, float sensitivityScale, float mPitchOverride, float mYawOverride, float mSensitivityOverride, int fpSel, int invenSel ) {
	cl.cgameUserCmdValue = userCmdValue;
	cl.cgameSensitivity = sensitivityScale;
	cl_mPitchOverride = mPitchOverride;
	cl_mYawOverride = mYawOverride;
	cl_mSensitivityOverride = mSensitivityOverride;
	cl.cgameForceSelection = fpSel;
	cl.cgameInvenSelection = invenSel;
}

int gCLTotalClientNum = 0;
//keep track of the total number of clients
extern cvar_t	*cl_autolodscale;
//if we want to do autolodscaling

void CL_DoAutoLODScale(void)
{
	float finalLODScaleFactor = 0;

	if ( gCLTotalClientNum >= 8 )
	{
		finalLODScaleFactor = (gCLTotalClientNum/-8.0f);
	}


	Cvar_Set( "r_autolodscalevalue", va("%f", finalLODScaleFactor) );
}

#if defined(DISCORD) && !defined(_DEBUG)
static void CL_UpdateDiscordServerInfo(const char *info)
{
	char *value = NULL;

	cl.discord.needPassword = qfalse;

	value = Info_ValueForKey(info, "sv_hostname");
	Q_strncpyz(cl.discord.hostName, value, sizeof(cl.discord.hostName));

	cl.discord.maxPlayers = atoi(Info_ValueForKey(info, "sv_maxclients"));
	cl.discord.timelimit = atoi(Info_ValueForKey(info, "timelimit"));
	cl.discord.gametype = atoi(Info_ValueForKey(info, "g_gametype"));

	value = Info_ValueForKey(info, "g_needpass");
	if (atoi(value))
		cl.discord.needPassword = qtrue;

	value = Info_ValueForKey(info, "mapname");
	Q_strncpyz(cl.discord.mapName, value, sizeof(cl.discord.mapName));
}
#endif

static void CL_ParsePlayerInfo(int start, int end)
{
	int clientCount = 0, botCount = 0, redTeam = 0, blueTeam = 0, specTeam = 0;
	int i = start;

	while (i < end)
	{
		char *s = cl.gameState.stringData + cl.gameState.stringOffsets[i];
		int team = atoi(Info_ValueForKey(s, "t"));
		char *bot = Info_ValueForKey(s, "skill");

		switch (team)
		{
		default:
		case 0:
			break;
		case 1:
			redTeam++;
			break;
		case 2:
			blueTeam++;
			break;
		case 3:
			specTeam++;
			break;
		}

		if (bot && bot[0])
		{
			botCount++;
		}

		if (s && s[0])
		{
			clientCount++;
		}

		i++;
	}

	gCLTotalClientNum = clientCount;

#ifdef _DEBUG
	Com_DPrintf("%i clients\n", gCLTotalClientNum);
#endif

#if defined(DISCORD) && !defined(_DEBUG)
	cl.discord.playerCount = clientCount;
	cl.discord.redTeam = redTeam;
	cl.discord.blueTeam = blueTeam;
	cl.discord.specCount = specTeam;
	cl.discord.botCount = botCount;
#endif

	if (cl_autolodscale && cl_autolodscale->integer)
		CL_DoAutoLODScale();
}

/*
=====================
CL_ConfigstringModified
=====================
*/
void CL_ConfigstringModified( void ) {
	char		*old, *s;
	int			i, index;
	char		*dup;
	gameState_t	oldGs;
	int			len;

	index = atoi( Cmd_Argv(1) );
	if ( index < 0 || index >= MAX_CONFIGSTRINGS ) {
		Com_Error( ERR_DROP, "CL_ConfigstringModified: bad index %i", index );
	}
	// get everything after "cs <num>"
	s = Cmd_ArgsFrom(2);

	old = cl.gameState.stringData + cl.gameState.stringOffsets[ index ];
	if ( !strcmp( old, s ) ) {
		return;		// unchanged
	}

	// build the new gameState_t
	oldGs = cl.gameState;

	Com_Memset( &cl.gameState, 0, sizeof( cl.gameState ) );

	// leave the first 0 for uninitialized strings
	cl.gameState.dataCount = 1;

	for ( i = 0 ; i < MAX_CONFIGSTRINGS ; i++ ) {
		if ( i == index ) {
			dup = s;
		} else {
			dup = oldGs.stringData + oldGs.stringOffsets[ i ];
		}
		if ( !dup[0] ) {
			continue;		// leave with the default empty string
		}

		len = strlen( dup );

		if ( len + 1 + cl.gameState.dataCount > MAX_GAMESTATE_CHARS ) {
			Com_Error( ERR_DROP, "MAX_GAMESTATE_CHARS exceeded" );
		}

		// append it to the gameState string buffer
		cl.gameState.stringOffsets[ i ] = cl.gameState.dataCount;
		Com_Memcpy( cl.gameState.stringData + cl.gameState.dataCount, dup, len + 1 );
		cl.gameState.dataCount += len + 1;
	}

	if (index >= CS_PLAYERS &&
		index < CS_G2BONES)
	{ //this means that a client was updated in some way. Go through and count the clients.
		CL_ParsePlayerInfo(CS_PLAYERS, CS_G2BONES);
	}

#if defined(DISCORD) && !defined(_DEBUG)
	if (index == CS_SERVERINFO)
	{
		s = cl.gameState.stringData + cl.gameState.stringOffsets[CS_SERVERINFO];

		if (s && s[0]) {
			CL_UpdateDiscordServerInfo(s);
		}
	}
#endif

	if ( index == CS_SYSTEMINFO ) {
		// parse serverId and other cvars
		CL_SystemInfoChanged();
	}

}
#ifndef MAX_STRINGED_SV_STRING
	#define MAX_STRINGED_SV_STRING 1024
#endif
// just copied it from CG_CheckSVStringEdRef(
void CL_CheckSVStringEdRef(char *buf, const char *str)
{ //I don't really like doing this. But it utilizes the system that was already in place.
	int i = 0;
	int b = 0;
	int strLen = 0;
	qboolean gotStrip = qfalse;

	if (!str || !str[0])
	{
		if (str)
		{
			strcpy(buf, str);
		}
		return;
	}

	strcpy(buf, str);

	strLen = strlen(str);

	if (strLen >= MAX_STRINGED_SV_STRING)
	{
		return;
	}

	while (i < strLen && str[i])
	{
		gotStrip = qfalse;

		if (str[i] == '@' && (i+1) < strLen)
		{
			if (str[i+1] == '@' && (i+2) < strLen)
			{
				if (str[i+2] == '@' && (i+3) < strLen)
				{ //@@@ should mean to insert a StringEd reference here, so insert it into buf at the current place
					char stringRef[MAX_STRINGED_SV_STRING];
					int r = 0;

					while (i < strLen && str[i] == '@')
					{
						i++;
					}

					while (i < strLen && str[i] && str[i] != ' ' && str[i] != ':' && str[i] != '.' && str[i] != '\n')
					{
						stringRef[r] = str[i];
						r++;
						i++;
					}
					stringRef[r] = 0;

					buf[b] = 0;
					Q_strcat(buf, MAX_STRINGED_SV_STRING, SE_GetString("MP_SVGAME", stringRef));
					b = strlen(buf);
				}
			}
		}

		if (!gotStrip)
		{
			buf[b] = str[i];
			b++;
		}
		i++;
	}

	buf[b] = 0;
}
/*
===================
CL_GetServerCommand

Set up argc/argv for the given command
===================
*/

extern cvar_t	*con_notifyconnect;
extern cvar_t	*con_notifywords;
extern cvar_t	*con_notifyvote;

#define	MAX_NOTIFYWORDS 8
extern char	notifyWords[MAX_NOTIFYWORDS][32];
extern int stampColor;

extern void QDECL CL_LogPrintf(fileHandle_t fileHandle, const char *fmt, ...);
qboolean CL_GetServerCommand( int serverCommandNumber ) {
	const char *s;
	const char *cmd;
	static char bigConfigString[BIG_INFO_STRING];

	// if we have irretrievably lost a reliable command, drop the connection
	if ( serverCommandNumber <= clc.serverCommandSequence - MAX_RELIABLE_COMMANDS )
	{
		int i = 0;

		// when a demo record was started after the client got a whole bunch of
		// reliable commands then the client never got those first reliable commands
		if ( clc.demoplaying )
			return qfalse;

		while (i < MAX_RELIABLE_COMMANDS)
		{ //spew out the reliable command buffer
			if (clc.reliableCommands[i][0])
			{
				Com_Printf("%i: %s\n", i, clc.reliableCommands[i]);
			}
			i++;
		}
		Com_Error( ERR_DROP, "CL_GetServerCommand: a reliable command was cycled out" );
		return qfalse;
	}

	if ( serverCommandNumber > clc.serverCommandSequence ) {
		Com_Error( ERR_DROP, "CL_GetServerCommand: requested a command not received" );
		return qfalse;
	}

	s = clc.serverCommands[ serverCommandNumber & ( MAX_RELIABLE_COMMANDS - 1 ) ];
	clc.lastExecutedServerCommand = serverCommandNumber;

	Com_DPrintf( "serverCommand: %i : %s\n", serverCommandNumber, s );

rescan:
	Cmd_TokenizeString( s );
	cmd = Cmd_Argv(0);

	if ( !strcmp( cmd, "disconnect" ) ) {
		char strEd[MAX_STRINGED_SV_STRING];
		CL_CheckSVStringEdRef(strEd, Cmd_Argv(1));
		Com_Error (ERR_SERVERDISCONNECT, "%s: %s\n", SE_GetString("MP_SVGAME_SERVER_DISCONNECTED"), strEd );
	}

	if ( !strcmp( cmd, "bcs0" ) ) {
		Com_sprintf( bigConfigString, BIG_INFO_STRING, "cs %s \"%s", Cmd_Argv(1), Cmd_Argv(2) );
		return qfalse;
	}

	if ( !strcmp( cmd, "bcs1" ) ) {
		s = Cmd_Argv(2);
		if( strlen(bigConfigString) + strlen(s) >= BIG_INFO_STRING ) {
			Com_Error( ERR_DROP, "bcs exceeded BIG_INFO_STRING" );
		}
		strcat( bigConfigString, s );
		return qfalse;
	}

	if ( !strcmp( cmd, "bcs2" ) ) {
		s = Cmd_Argv(2);
		if( strlen(bigConfigString) + strlen(s) + 1 >= BIG_INFO_STRING ) {
			Com_Error( ERR_DROP, "bcs exceeded BIG_INFO_STRING" );
		}
		strcat( bigConfigString, s );
		strcat( bigConfigString, "\"" );
		s = bigConfigString;
		goto rescan;
	}

	if ( !strcmp( cmd, "cs" ) ) {
		CL_ConfigstringModified();
		// reparse the string, because CL_ConfigstringModified may have done another Cmd_TokenizeString()
		Cmd_TokenizeString( s );
		return qtrue;
	}

	if ( !strcmp( cmd, "map_restart" ) ) {
		// clear notify lines and outgoing commands before passing
		// the restart to the cgame
		Con_ClearNotify();
		// reparse the string, because Con_ClearNotify() may have done another Cmd_TokenizeString()
		Cmd_TokenizeString( s );
		Com_Memset( cl.cmds, 0, sizeof( cl.cmds ) );
		return qtrue;
	}

	// the clientLevelShot command is used during development
	// to generate 128*128 screenshots from the intermission
	// point of levels for the menu system to use
	// we pass it along to the cgame to make appropriate adjustments,
	// but we also clear the console and notify lines here
	if ( !strcmp( cmd, "clientLevelShot" ) ) {
		// don't do it if we aren't running the server locally,
		// otherwise malicious remote servers could overwrite
		// the existing thumbnails
		if ( !com_sv_running->integer ) {
			return qfalse;
		}
		// close the console
		Con_Close();
		// take a special screenshot next frame
		Cbuf_AddText( "wait ; wait ; wait ; wait ; screenshot levelshot\n" );
		return qtrue;
	}

	if (!Q_stricmp(cmd, "chat") || !Q_stricmp(cmd, "tchat") || !Q_stricmp(cmd, "lchat") || !Q_stricmp(cmd, "ltchat"))
	{
		if (cl_logChat->integer) {
			char chat[MAX_NETNAME + MAX_SAY_TEXT + 12];
			int i, l;

			s = Cmd_Argv(1);
			Q_strncpyz(chat, s, sizeof(chat));
			Q_StripColor(chat);
		
			//Remove escape char from name
			l = 0;
			for (i = 0; chat[i]; i++) {
				if (chat[i] == '\x19')
					continue;
				chat[l++] = chat[i];
			}
			chat[l] = '\0';

			CL_LogPrintf(cls.log.file, chat);
		}

		stampColor = COLOR_WHITE;

#ifdef _WIN32
		if (con_notifywords->integer == -1) {
			con_alert = qtrue;
		}
		else
#endif
		if (Q_stricmp(con_notifywords->string, "0")) {
			char *text = Q_strrchr(s, ':');
			if (text) {
				int i;
				for (i=0; i<MAX_NOTIFYWORDS; i++) {
					if (strcmp(notifyWords[i], "") && Q_stristr(text, notifyWords[i])) {
						stampColor = COLOR_CYAN;
#ifdef _WIN32
						con_alert = qtrue;
#endif
						break;
					}
				}
			}
		}

		return qtrue;
	}

	if (!Q_stricmp(cmd, "print")) {
		s = Cmd_Argv(1);
		if (Q_stristr(s, "@@@PLCONNECT") || Q_stristr(s, "@@@DISCONNECT") || Q_stristr(s, "@@@WAS_KICKED") || Q_stristr(s, "timed out")) {
			stampColor = COLOR_YELLOW;
#ifdef _WIN32
		if (con_notifyconnect->integer)
			con_alert = qtrue;
#endif
		}
		if (Q_stristr(s, "@@@PLCALLEDVOTE")) {
			stampColor = COLOR_ORANGE;
#ifdef _WIN32
		if (con_notifyvote->integer)
			con_alert = qtrue;
#endif
		}
		return qtrue;
	}

	// we may want to put a "connect to other server" command here

	// cgame can now act on the command
	return qtrue;
}

void QDECL CL_LogPrintf(fileHandle_t fileHandle, const char *fmt, ...) {
	va_list argptr;
	char string[1024] = { 0 };
	size_t len;
	time_t rawtime;
	time(&rawtime);

	if (clc.demoplaying)
		return;
	
	if (!cls.log.started) {
		cls.log.started = qtrue;
		CL_LogPrintf(fileHandle, "Start log\n--------------------------------------------------------------\n\n");
	}

	strftime(string, sizeof(string), "[%Y-%m-%d] [%H:%M:%S] ", localtime(&rawtime));

	len = strlen(string);

	va_start(argptr, fmt);
	Q_vsnprintf(string + len, sizeof(string) - len, fmt, argptr);
	va_end(argptr);

	if (!fileHandle)
		return;

	FS_Write(string, strlen(string), fileHandle);
}

static void CL_OpenLog(const char *filename, fileHandle_t *f, qboolean sync) {
	FS_FOpenFileByMode(filename, f, sync ? FS_APPEND_SYNC : FS_APPEND);
	if (*f)
		Com_Printf("Logging to %s\n", filename);
	else
		Com_Printf("^3WARNING: Couldn't open logfile: %s\n", filename);

	cls.log.started = qfalse;
}

static void CL_CloseLog(fileHandle_t *f) {
	if (!*f)
		return;

	FS_FCloseFile(*f);
	*f = NULL_FILE;

	cls.log.started = qfalse;
}

/*
====================
CL_ShutdonwCGame

====================
*/
void CL_ShutdownCGame( void ) {
	Key_SetCatcher( Key_GetCatcher( ) & ~KEYCATCH_CGAME );

	if ( !cls.cgameStarted )
		return;

	cls.cgameStarted = qfalse;

	CL_UnbindCGame();

	if (cl_logChat->integer) {
		if (cls.log.started) CL_LogPrintf(cls.log.file, "End log\n----------------------------------------------------------------\n\n");
		CL_CloseLog(&cls.log.file);
	}
}

/*
====================
CL_InitCGame

Should only be called by CL_StartHunkUsers
====================
*/

void CL_InitCGame( void ) {
	const char			*info;
	const char			*mapname;
	int					t1, t2;

	t1 = Sys_Milliseconds();

	// put away the console
	Con_Close();

	// find the current mapname
	info = cl.gameState.stringData + cl.gameState.stringOffsets[ CS_SERVERINFO ];
	mapname = Info_ValueForKey( info, "mapname" );
	Com_sprintf( cl.mapname, sizeof( cl.mapname ), "maps/%s.bsp", mapname );

#if defined(DISCORD) && !defined(_DEBUG)
	CL_UpdateDiscordServerInfo(info);
#endif

	// load the dll
	CL_BindCGame();

	cls.state = CA_LOADING;

	// init for this gamestate
	// use the lastExecutedServerCommand instead of the serverCommandSequence
	// otherwise server commands sent just before a gamestate are dropped
	CGVM_Init( clc.serverMessageSequence, clc.lastExecutedServerCommand, clc.clientNum );

	int clRate = Cvar_VariableIntegerValue( "rate" );
	if ( clRate == 4000 ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: Old default /rate value detected (4000). Suggest typing /rate 25000 into console for a smoother connection!\n" );
	}

	// reset any CVAR_CHEAT cvars registered by cgame
	if ( !clc.demoplaying && !cl_connectedToCheatServer )
		Cvar_SetCheatState();

	// we will send a usercmd this frame, which
	// will cause the server to send us the first snapshot
	cls.state = CA_PRIMED;

	t2 = Sys_Milliseconds();

	Com_Printf( "CL_InitCGame: %5.2f seconds\n", (t2-t1)/1000.0 );

	// have the renderer touch all its images, so they are present
	// on the card even if the driver does deferred loading
	re->EndRegistration();

	// make sure everything is paged in
//	if (!Sys_LowPhysicalMemory())
	{
		Com_TouchMemory();
	}

	// clear anything that got printed
	Con_ClearNotify ();

	if (cl_logChat->integer) {
		struct tm		*newtime;
		time_t			rawtime;
		char			logname[32];

		time(&rawtime);
		newtime = localtime(&rawtime);
		strftime(logname, sizeof(logname), "chatlogs/cl_%y-%b.log", newtime);

		CL_OpenLog(logname, &cls.log.file, (cl_logChat->integer == 2 ? qtrue : qfalse));
	}
	else {
		Com_DPrintf("Not logging chat to disk.\n");
	}
}


/*
====================
CL_GameCommand

See if the current console command is claimed by the cgame
====================
*/
qboolean CL_GameCommand( void ) {
	if ( !cls.cgameStarted )
		return qfalse;

	return CGVM_ConsoleCommand();
}



/*
=====================
CL_CGameRendering
=====================
*/
void CL_CGameRendering( stereoFrame_t stereo ) {
	//rww - RAGDOLL_BEGIN
	if (!com_sv_running->integer)
	{ //set the server time to match the client time, if we don't have a server going.
		re->G2API_SetTime(cl.serverTime, 0);
	}
	re->G2API_SetTime(cl.serverTime, 1);
	//rww - RAGDOLL_END

	CGVM_DrawActiveFrame( cl.serverTime, stereo, clc.demoplaying );
}


/*
=================
CL_AdjustTimeDelta

Adjust the clients view of server time.

We attempt to have cl.serverTime exactly equal the server's view
of time plus the timeNudge, but with variable latencies over
the internet it will often need to drift a bit to match conditions.

Our ideal time would be to have the adjusted time approach, but not pass,
the very latest snapshot.

Adjustments are only made when a new snapshot arrives with a rational
latency, which keeps the adjustment process framerate independent and
prevents massive overadjustment during times of significant packet loss
or bursted delayed packets.
=================
*/

#define	RESET_TIME	500

void CL_AdjustTimeDelta( void ) {
	int		newDelta;
	int		deltaDelta;

	cl.newSnapshots = qfalse;

	// the delta never drifts when replaying a demo
	if ( clc.demoplaying ) {
		return;
	}

	newDelta = cl.snap.serverTime - cls.realtime;
	deltaDelta = abs( newDelta - cl.serverTimeDelta );

	if ( deltaDelta > RESET_TIME ) {
		cl.serverTimeDelta = newDelta;
		cl.oldServerTime = cl.snap.serverTime;	// FIXME: is this a problem for cgame?
		cl.serverTime = cl.snap.serverTime;
		if ( cl_showTimeDelta->integer ) {
			Com_Printf( "<RESET> " );
		}
	} else if ( deltaDelta > 100 ) {
		// fast adjust, cut the difference in half
		if ( cl_showTimeDelta->integer ) {
			Com_Printf( "<FAST> " );
		}
		cl.serverTimeDelta = ( cl.serverTimeDelta + newDelta ) >> 1;
	} else {
		// slow drift adjust, only move 1 or 2 msec

		// if any of the frames between this and the previous snapshot
		// had to be extrapolated, nudge our sense of time back a little
		// the granularity of +1 / -2 is too high for timescale modified frametimes
		if ( com_timescale->value == 0 || com_timescale->value == 1 ) {
			if ( cl.extrapolatedSnapshot ) {
				cl.extrapolatedSnapshot = qfalse;
				cl.serverTimeDelta -= 2;
			} else {
				// otherwise, move our sense of time forward to minimize total latency
				cl.serverTimeDelta++;
			}
		}
	}

	if ( cl_showTimeDelta->integer ) {
		Com_Printf( "%i ", cl.serverTimeDelta );
	}
}


/*
==================
CL_FirstSnapshot
==================
*/
void CL_FirstSnapshot( void ) {
	// ignore snapshots that don't have entities
	if ( cl.snap.snapFlags & SNAPFLAG_NOT_ACTIVE ) {
		return;
	}

	re->RegisterMedia_LevelLoadEnd();

	cls.state = CA_ACTIVE;

	// set the timedelta so we are exactly on this first frame
	cl.serverTimeDelta = cl.snap.serverTime - cls.realtime;
	cl.oldServerTime = cl.snap.serverTime;

	clc.timeDemoBaseTime = cl.snap.serverTime;

	// if this is the first frame of active play,
	// execute the contents of activeAction now
	// this is to allow scripting a timedemo to start right
	// after loading
	if ( cl_activeAction->string[0] ) {
		Cbuf_AddText( cl_activeAction->string );
		Cvar_Set( "activeAction", "" );
	}
}

/*
==================
CL_SetCGameTime
==================
*/
void CL_SetCGameTime( void ) {

	// getting a valid frame message ends the connection process
	if ( cls.state != CA_ACTIVE ) {
		if ( cls.state != CA_PRIMED ) {
			return;
		}
		if ( clc.demoplaying ) {
			// we shouldn't get the first snapshot on the same frame
			// as the gamestate, because it causes a bad time skip
			if ( !clc.firstDemoFrameSkipped ) {
				clc.firstDemoFrameSkipped = qtrue;
				return;
			}
			CL_ReadDemoMessage();
		}
		if ( cl.newSnapshots ) {
			cl.newSnapshots = qfalse;
			CL_FirstSnapshot();
		}
		if ( cls.state != CA_ACTIVE ) {
			return;
		}
	}

	// if we have gotten to this point, cl.snap is guaranteed to be valid
	if ( !cl.snap.valid ) {
		Com_Error( ERR_DROP, "CL_SetCGameTime: !cl.snap.valid" );
	}

	// allow pause in single player
	if ( sv_paused->integer && CL_CheckPaused() && com_sv_running->integer ) {
		// paused
		return;
	}

	if ( cl.snap.serverTime < cl.oldFrameServerTime ) {
		Com_Error( ERR_DROP, "cl.snap.serverTime < cl.oldFrameServerTime" );
	}
	cl.oldFrameServerTime = cl.snap.serverTime;


	// get our current view of time

	if ( clc.demoplaying && (!com_timescale->value || cl_paused->integer) )
	{
		// timescale 0 is used to lock a demo in place for single frame advances
		cl.serverTimeDelta -= cls.frametime;
	}
	else
	{
		// cl_timeNudge is a user adjustable cvar that allows more
		// or less latency to be added in the interest of better
		// smoothness or better responsiveness.
		int tn;

		tn = cl_timeNudge->integer;

		if (tn < 0 && (cl.snap.ps.pm_type == PM_SPECTATOR || cl.snap.ps.pm_flags & PMF_FOLLOW || clc.demoplaying))
			tn = 0; // JAPRO ENGINE - disable negative timenudge when spectating
#ifdef _DEBUG
		if (tn<-900) {
			tn = -900;
		} else if (tn>900) {
			tn = 900;
		}
#else
		if (tn<-2000) {//JAPRO ENGINE
			tn = -2000;
		} else if (tn>2000) {
			tn = 2000;
		}
#endif

		cl.serverTime = cls.realtime + cl.serverTimeDelta - tn;

		// guarantee that time will never flow backwards, even if
		// serverTimeDelta made an adjustment or cl_timeNudge was changed
		if ( cl.serverTime < cl.oldServerTime ) {
			cl.serverTime = cl.oldServerTime;
		}
		cl.oldServerTime = cl.serverTime;

		// note if we are almost past the latest frame (without timeNudge),
		// so we will try and adjust back a bit when the next snapshot arrives
		if ( cls.realtime + cl.serverTimeDelta >= cl.snap.serverTime - 5 ) {
			cl.extrapolatedSnapshot = qtrue;
		}
	}

	// if we have gotten new snapshots, drift serverTimeDelta
	// don't do this every frame, or a period of packet loss would
	// make a huge adjustment
	if ( cl.newSnapshots ) {
		CL_AdjustTimeDelta();
	}

	if ( !clc.demoplaying ) {
		return;
	}

	// if we are playing a demo back, we can just keep reading
	// messages from the demo file until the cgame definately
	// has valid snapshots to interpolate between

	// a timedemo will always use a deterministic set of time samples
	// no matter what speed machine it is run on,
	// while a normal demo may have different time samples
	// each time it is played back
	if ( cl_timedemo->integer ) {
		if (!clc.timeDemoStart) {
			clc.timeDemoStart = Sys_Milliseconds();
		}
		clc.timeDemoFrames++;
		cl.serverTime = clc.timeDemoBaseTime + clc.timeDemoFrames * 50;
	}

	while ( cl.serverTime >= cl.snap.serverTime ) {
		// feed another messag, which should change
		// the contents of cl.snap
		CL_ReadDemoMessage();
		if ( cls.state != CA_ACTIVE ) {
			return;		// end of demo
		}
	}
}
