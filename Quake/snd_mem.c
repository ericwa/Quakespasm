/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// snd_mem.c: sound caching

#include "quakedef.h"
#include "snd_codec.h"

/*
==============
S_LoadSound
==============
*/
sfxcache_t *S_LoadSound (sfx_t *s)
{
	snd_info_t	info;
	char	namebuffer[256];
	byte	*data;
	int		len;
	sfxcache_t	*sc;

// see if still in memory
	sc = (sfxcache_t *) Cache_Check (&s->cache);
	if (sc)
		return sc;

//Con_Printf ("S_LoadSound: %x\n", (int)stackbuf);
// load it in
    Q_strcpy(namebuffer, "sound/");
    Q_strcat(namebuffer, s->name);

//	Con_Printf ("loading %s\n",namebuffer);

	// load it in
	data = S_CodecLoad(namebuffer, &info);
	if (!data)
	{
		Con_Printf ("Couldn't load %s\n", namebuffer);
		return NULL;
	}
	
	if (info.channels != 1)
	{
		Con_Printf ("%s is a stereo sample\n",s->name);
		return NULL;
	}

	int resampledNumSamples;
	void *resampled = Snd_Resample(info.rate, info.width, info.samples, info.channels, data, shm->speed, shm->samplebits/8, &resampledNumSamples);
	
	len = resampledNumSamples * (shm->samplebits/8) * info.channels;

	sc = (sfxcache_t *) Cache_Alloc ( &s->cache, len + sizeof(sfxcache_t), s->name);
	if (!sc)
		return NULL;

	float ratio = (float)info.rate / (float)shm->speed;
	
	sc->length = resampledNumSamples;
	sc->loopstart = (info.loopstart == -1 ? -1 : info.loopstart / ratio); // reposition loop marker to take resampling into account
	sc->speed = shm->speed;
	sc->width = (shm->samplebits/8);
	sc->stereo = info.channels;
    memcpy(sc->data, resampled, len);
	
	free(resampled);
	Z_Free(data);
	
	return sc;
}

