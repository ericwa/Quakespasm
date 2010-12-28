/*
	cd_shared.c

	Copyright (C) 1996-1997  Id Software, Inc.

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public License
	as published by the Free Software Foundation; either version 2
	of the License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

	See the GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to:

		Free Software Foundation, Inc.
		51 Franklin St, Fifth Floor,
		Boston, MA  02110-1301  USA
*/


#include "quakedef.h"

static qboolean	playing = false;
static qboolean	wasPlaying = false;
static qboolean	enabled = true;
static qboolean playLooping = false;
static byte	remap[100];
static char	playTrackName[MAX_QPATH];
static double old_cdvolume;

static void CDAudio_Eject(void)
{
	// FIXME: call backend
	;
}

static qboolean CDAudio_IsNumberedTrack(const char *trackName)
{
    char numberAsString[10];
    sprintf(numberAsString, "%d", (int)atoi(trackName));
    return 0 == strcmp(trackName, numberAsString);
}

void CDAudio_PlayNamed(const char *name, qboolean looping)
{
	if (!enabled)
		return;

    // already playing the correct track?
	if (playing && (0 == strcmp(name, playTrackName)))
	{
        return;
	}

    // copy the track name to playTrackName
    if (CDAudio_IsNumberedTrack(name))
    {
        int track = atoi(name);
        if (track < 100)
        {
            track = remap[track];
        }
        sprintf(playTrackName, "%d", track);
    }
    else
    {
        if ((strlen(name) + 1) > MAX_QPATH)
        {
            return;
        }
        strcpy(playTrackName, name);
    }

	if (playing)
	{
		CDAudio_Stop();
	}

    // FIXME: check for backend error

	playLooping = looping;
	playing = true;

    // FIXME: make backend play
}

void CDAudio_Play(byte track, qboolean looping)
{
    char name[MAX_QPATH];
    sprintf(name, "%d", (int)track);
    CDAudio_PlayNamed(name, looping);
}

void CDAudio_Stop(void)
{
	if (!enabled)
		return;

	if (!playing)
		return;

    // FIXME: stop backend

	wasPlaying = false;
	playing = false;
}

static void CDAudio_Next(void)
{
	byte track;

	if (!enabled)
		return;

	if (!playing)
		return;

    if (!CDAudio_IsNumberedTrack(playTrackName))
        return;

	track = atoi(playTrackName) + 1;

	CDAudio_Play (track, playLooping);
}

static void CDAudio_Prev(void)
{
	byte track;

	if (!enabled)
		return;

	if (!playing)
		return;

    if (!CDAudio_IsNumberedTrack(playTrackName))
        return;

	track = atoi(playTrackName) - 1;
    if (track < 1)
        track = 1;

	CDAudio_Play (track, playLooping);
}

void CDAudio_Pause(void)
{
	if (!enabled)
		return;

	if (!playing)
		return;

	// FIXME: pause in backend

	wasPlaying = playing;
	playing = false;
}

void CDAudio_Resume(void)
{
	if (!enabled)
		return;

	if (!wasPlaying)
		return;

    // FIXME: resume in backend

	playing = true;
}

static void CD_f (void)
{
	const char	*command,*arg2;
	int		ret, n;

	if (Cmd_Argc() < 2)
	{
		Con_Printf("commands:\n");
		Con_Printf("  on, off, reset, remap, \n");
		Con_Printf("  play, stop, next, prev, loop,\n");
		Con_Printf("  pause, resume, eject, info\n");
		return;
	}

	command = Cmd_Argv (1);

	if (Q_strcasecmp(command, "on") == 0)
	{
		enabled = true;
		return;
	}

	if (Q_strcasecmp(command, "off") == 0)
	{
		if (playing)
			CDAudio_Stop();
		enabled = false;
		return;
	}

	if (Q_strcasecmp(command, "reset") == 0)
	{
		enabled = true;
		if (playing)
			CDAudio_Stop();
		for (n = 0; n < 100; n++)
			remap[n] = n;

		// FIXME: backend get disc info

		return;
	}

	if (Q_strcasecmp(command, "remap") == 0)
	{
		ret = Cmd_Argc () - 2;
		if (ret <= 0)
		{
			for (n = 1; n < 100; n++)
				if (remap[n] != n)
					Con_Printf ("  %u -> %u\n", n, remap[n]);
			return;
		}
		for (n = 1; n <= ret; n++)
			remap[n] = atoi(Cmd_Argv (n + 1));
		return;
	}

	if (Q_strcasecmp(command, "play") == 0)
	{
		arg2 = Cmd_Argv (2);
        if (*arg2)
        {
            CDAudio_PlayNamed(Cmd_Argv(2), false);
        }
		else
		{
		    CDAudio_Play((byte)1, false);
		}
		return;
	}

	if (Q_strcasecmp(command, "loop") == 0)
	{
		arg2 = Cmd_Argv (2);
        if (*arg2)
        {
            CDAudio_PlayNamed(Cmd_Argv(2), true);
        }
		else
		{
		    CDAudio_Play((byte)1, true);
		}
		return;
	}

	if (Q_strcasecmp(command, "stop") == 0)
	{
		CDAudio_Stop();
		return;
	}

	if (Q_strcasecmp(command, "pause") == 0)
	{
		CDAudio_Pause();
		return;
	}

	if (Q_strcasecmp(command, "resume") == 0)
	{
		CDAudio_Resume();
		return;
	}

	if (Q_strcasecmp(command, "next") == 0)
	{
		CDAudio_Next();
		return;
	}

	if (Q_strcasecmp(command, "prev") == 0)
	{
		CDAudio_Prev();
		return;
	}

	if (Q_strcasecmp(command, "eject") == 0)
	{
		if (playing)
			CDAudio_Stop();
		CDAudio_Eject();
		return;
	}

	if (Q_strcasecmp(command, "info") == 0)
	{
		if (playing)
			Con_Printf ("Currently %s track %s\n", playLooping ? "looping" : "playing", playTrackName);
		else if (wasPlaying)
			Con_Printf ("Paused %s track %s\n", playLooping ? "looping" : "playing", playTrackName);

		Con_Printf ("Volume is %f\n", bgmvolume.value);

		return;
	}
	Con_Printf ("cd: no such command. Use \"cd\" for help.\n");
}

static qboolean CD_GetVolume (void *unused)
{
/* FIXME: write proper code in here when SDL
   supports cdrom volume control some day. */
	return false;
}

static qboolean CD_SetVolume (void *unused)
{
/* FIXME: write proper code in here when SDL
   supports cdrom volume control some day. */
	return false;
}

static qboolean CDAudio_SetVolume (cvar_t *var)
{
	if (!enabled)
		return false;

	if (var->value < 0.0)
		Cvar_SetValue (var->name, 0.0);
	else if (var->value > 1.0)
		Cvar_SetValue (var->name, 1.0);
	old_cdvolume = var->value;

	//FIXME:
	return true;
}

void CDAudio_Update(void)
{
	if (!enabled)
		return;

	if (old_cdvolume != bgmvolume.value)
		CDAudio_SetVolume (&bgmvolume);

	// FIXME: update backend
}


int CDAudio_Init(void)
{
	int	i;

    // FIXME: try to init backend

	for (i = 0; i < 100; i++)
		remap[i] = i;

	enabled = true;

	// FIXME: check if cd in drive

	Cmd_AddCommand ("cd", CD_f);

	return 0;
}

void CDAudio_Shutdown(void)
{
	CDAudio_Stop();

	// FIXME: shutdown backend
}
