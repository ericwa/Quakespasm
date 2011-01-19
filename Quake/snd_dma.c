/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske

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
// snd_dma.c -- main control for any streaming sound output device

#include "quakedef.h"
#include "snd_codec.h"

void S_Play(void);
void S_PlayVol(void);
void S_SoundList(void);
void S_Update_();
void S_StopAllSounds(qboolean clear);
void S_StopAllSoundsC(void);

snd_stream_t	*s_backgroundStream = NULL;
static qboolean	s_backgroundPaused = false;
static char		s_backgroundLoop[MAX_QPATH];
static S_BackgroundTrackFinishedCallback s_backgroundFinishedCallback;
static void		*s_backgroundFinishedCallbackUserData;

// =======================================================================
// Internal sound data & structures
// =======================================================================

channel_t	snd_channels[MAX_CHANNELS];
int		total_channels;

int		snd_blocked = 0;
qboolean	snd_initialized = false;

volatile dma_t	sn;
volatile dma_t	*shm = NULL;

vec3_t		listener_origin;
vec3_t		listener_forward;
vec3_t		listener_right;
vec3_t		listener_up;

#define	sound_nominal_clip_dist	1000.0

int		soundtime;	// sample PAIRS
int		paintedtime;	// sample PAIRS


#define	MAX_SFX		512
sfx_t		*known_sfx;		// hunk allocated [MAX_SFX]
int		num_sfx;

sfx_t		*ambient_sfx[NUM_AMBIENTS];

qboolean	sound_started = false;

int						s_rawend[MAX_RAW_STREAMS];
portable_samplepair_t s_rawsamples[MAX_RAW_STREAMS][MAX_RAW_SAMPLES];
static void				*s_rawresampler[MAX_RAW_STREAMS];

cvar_t bgmvolume = {"bgmvolume", "1", true};
cvar_t sfxvolume = {"volume", "0.7", true};

cvar_t nosound = {"nosound", "0"};
cvar_t precache = {"precache", "1"};
cvar_t loadas8bit = {"loadas8bit", "0"};
cvar_t bgmbuffer = {"bgmbuffer", "4096"};
cvar_t ambient_level = {"ambient_level", "0.3"};
cvar_t ambient_fade = {"ambient_fade", "100"};
cvar_t snd_noextraupdate = {"snd_noextraupdate", "0"};
cvar_t snd_show = {"snd_show", "0"};
cvar_t _snd_mixahead = {"_snd_mixahead", "0.1", true};
cvar_t sndspeed = {"sndspeed", "44100"};


void S_SoundInfo_f(void)
{
	if (!sound_started || !shm)
	{
		Con_Printf ("sound system not started\n");
		return;
	}

	Con_Printf("%d bit, %s, %d Hz\n", shm->samplebits,
			(shm->channels == 2) ? "stereo" : "mono", shm->speed);
	Con_Printf("%5d samples\n", shm->samples);
	Con_Printf("%5d samplepos\n", shm->samplepos);
	Con_Printf("%5d submission_chunk\n", shm->submission_chunk);
	Con_Printf("%5d total_channels\n", total_channels);
	Con_Printf("%p dma buffer\n", shm->buffer);
}


/*
================
S_Startup
================
*/
void S_Startup (void)
{
	if (!snd_initialized)
		return;

	sound_started = SNDDMA_Init();

	if (!sound_started)
	{
		Con_Printf("Failed initializing sound\n");
	}
	else
	{
		Con_Printf("Audio: %d bit, %s, %d Hz\n",
				shm->samplebits,
				(shm->channels == 2) ? "stereo" : "mono",
				shm->speed);
	}
}


/*
================
S_Init
================
*/
void S_Init (void)
{
	if (snd_initialized)
	{
		Con_Printf("Sound is already initialized\n");
		return;
	}

	S_CodecInit();
	
	Cvar_RegisterVariable(&nosound, NULL);
	Cvar_RegisterVariable(&sfxvolume, NULL);
	Cvar_RegisterVariable(&precache, NULL);
	Cvar_RegisterVariable(&loadas8bit, NULL);
	Cvar_RegisterVariable(&bgmvolume, NULL);
	Cvar_RegisterVariable(&bgmbuffer, NULL);
	Cvar_RegisterVariable(&ambient_level, NULL);
	Cvar_RegisterVariable(&ambient_fade, NULL);
	Cvar_RegisterVariable(&snd_noextraupdate, NULL);
	Cvar_RegisterVariable(&snd_show, NULL);
	Cvar_RegisterVariable(&_snd_mixahead, NULL);
	Cvar_RegisterVariable(&sndspeed, NULL);

	if (safemode || COM_CheckParm("-nosound"))
		return;

	Con_Printf("Sound Initialization\n");

	Cmd_AddCommand("play", S_Play);
	Cmd_AddCommand("playvol", S_PlayVol);
	Cmd_AddCommand("stopsound", S_StopAllSoundsC);
	Cmd_AddCommand("soundlist", S_SoundList);
	Cmd_AddCommand("soundinfo", S_SoundInfo_f);

	if (COM_CheckParm("-sndspeed"))
	{
		sndspeed.value = Q_atoi(com_argv[COM_CheckParm("-sndspeed")+1]);
	}

	if (host_parms.memsize < 0x800000)
	{
		Cvar_Set ("loadas8bit", "1");
		Con_Printf ("loading all sounds as 8bit\n");
	}

	known_sfx = (sfx_t *) Hunk_AllocName (MAX_SFX*sizeof(sfx_t), "sfx_t");
	num_sfx = 0;

	snd_initialized = true;

	S_Startup ();
	if (sound_started == 0)
		return;

// provides a tick sound until washed clean
//	if (shm->buffer)
//		shm->buffer[4] = shm->buffer[5] = 0x7f;	// force a pop for debugging

	ambient_sfx[AMBIENT_WATER] = S_PrecacheSound ("ambience/water1.wav");
	ambient_sfx[AMBIENT_SKY] = S_PrecacheSound ("ambience/wind2.wav");

	S_StopAllSounds (true);
}


// =======================================================================
// Shutdown sound engine
// =======================================================================
void S_Shutdown (void)
{
	if (!sound_started)
		return;

	S_CodecShutdown();
	
	if (shm)
		shm->gamealive = 0;
	sound_started = 0;
	snd_blocked = 0;

	SNDDMA_Shutdown();
	shm = NULL;
	
	int i;
	for (i = 0; i<MAX_RAW_STREAMS; i++)
	{
		if (s_rawresampler[i] != NULL)
			Snd_ResamplerClose(s_rawresampler[i]);
	}
}


// =======================================================================
// Load a sound
// =======================================================================

/*
==================
S_FindName

==================
*/
sfx_t *S_FindName (const char *name)
{
	int		i;
	sfx_t	*sfx;

	if (!name)
		Sys_Error ("S_FindName: NULL");

	if (Q_strlen(name) >= MAX_QPATH)
		Sys_Error ("Sound name too long: %s", name);

// see if already loaded
	for (i = 0; i < num_sfx; i++)
	{
		if (!Q_strcmp(known_sfx[i].name, name))
		{
			return &known_sfx[i];
		}
	}

	if (num_sfx == MAX_SFX)
		Sys_Error ("S_FindName: out of sfx_t");

	sfx = &known_sfx[i];
	strcpy (sfx->name, name);

	num_sfx++;

	return sfx;
}


/*
==================
S_TouchSound

==================
*/
void S_TouchSound (const char *name)
{
	sfx_t	*sfx;

	if (!sound_started)
		return;

	sfx = S_FindName (name);
	Cache_Check (&sfx->cache);
}

/*
==================
S_PrecacheSound

==================
*/
sfx_t *S_PrecacheSound (const char *name)
{
	sfx_t	*sfx;

	if (!sound_started || nosound.value)
		return NULL;

	sfx = S_FindName (name);

// cache it in
	if (precache.value)
		S_LoadSound (sfx);

	return sfx;
}


//=============================================================================

/*
=================
SND_PickChannel

picks a channel based on priorities, empty slots, number of channels
=================
*/
channel_t *SND_PickChannel (int entnum, int entchannel)
{
	int	ch_idx;
	int	first_to_die;
	int	life_left;

// Check for replacement sound, or find the best one to replace
	first_to_die = -1;
	life_left = 0x7fffffff;
	for (ch_idx = NUM_AMBIENTS; ch_idx < NUM_AMBIENTS + MAX_DYNAMIC_CHANNELS; ch_idx++)
	{
		if (entchannel != 0		// channel 0 never overrides
			&& snd_channels[ch_idx].entnum == entnum
			&& (snd_channels[ch_idx].entchannel == entchannel || entchannel == -1) )
		{	// always override sound from same entity
			first_to_die = ch_idx;
			break;
		}

		// don't let monster sounds override player sounds
		if (snd_channels[ch_idx].entnum == cl.viewentity && entnum != cl.viewentity && snd_channels[ch_idx].sfx)
			continue;

		if (snd_channels[ch_idx].end - paintedtime < life_left)
		{
			life_left = snd_channels[ch_idx].end - paintedtime;
			first_to_die = ch_idx;
		}
	}

	if (first_to_die == -1)
		return NULL;

	if (snd_channels[first_to_die].sfx)
		snd_channels[first_to_die].sfx = NULL;

	return &snd_channels[first_to_die];
}

/*
=================
SND_Spatialize

spatializes a channel
=================
*/
void SND_Spatialize (channel_t *ch)
{
	vec_t	dot;
	vec_t	dist;
	vec_t	lscale, rscale, scale;
	vec3_t	source_vec;

// anything coming from the view entity will always be full volume
	if (ch->entnum == cl.viewentity)
	{
		ch->leftvol = ch->master_vol;
		ch->rightvol = ch->master_vol;
		return;
	}

// calculate stereo seperation and distance attenuation
	VectorSubtract(ch->origin, listener_origin, source_vec);
	dist = VectorNormalize(source_vec) * ch->dist_mult;
	dot = DotProduct(listener_right, source_vec);

	if (shm->channels == 1)
	{
		rscale = 1.0;
		lscale = 1.0;
	}
	else
	{
		rscale = 1.0 + dot;
		lscale = 1.0 - dot;
	}

// add in distance effect
	scale = (1.0 - dist) * rscale;
	ch->rightvol = (int) (ch->master_vol * scale);
	if (ch->rightvol < 0)
		ch->rightvol = 0;

	scale = (1.0 - dist) * lscale;
	ch->leftvol = (int) (ch->master_vol * scale);
	if (ch->leftvol < 0)
		ch->leftvol = 0;
}


// =======================================================================
// Start a sound effect
// =======================================================================

void S_StartSound (int entnum, int entchannel, sfx_t *sfx, vec3_t origin, float fvol, float attenuation)
{
	channel_t	*target_chan, *check;
	sfxcache_t	*sc;
	int		ch_idx;
	int		skip;

	if (!sound_started)
		return;

	if (!sfx)
		return;

	if (nosound.value)
		return;

// pick a channel to play on
	target_chan = SND_PickChannel(entnum, entchannel);
	if (!target_chan)
		return;

// spatialize
	memset (target_chan, 0, sizeof(*target_chan));
	VectorCopy(origin, target_chan->origin);
	target_chan->dist_mult = attenuation / sound_nominal_clip_dist;
	target_chan->master_vol = (int) (fvol * 255);
	target_chan->entnum = entnum;
	target_chan->entchannel = entchannel;
	SND_Spatialize(target_chan);

	if (!target_chan->leftvol && !target_chan->rightvol)
		return;		// not audible at all

// new channel
	sc = S_LoadSound (sfx);
	if (!sc)
	{
		target_chan->sfx = NULL;
		return;		// couldn't load the sound's data
	}

	target_chan->sfx = sfx;
	target_chan->pos = 0.0;
	target_chan->end = paintedtime + sc->length;

// if an identical sound has also been started this frame, offset the pos
// a bit to keep it from just making the first one louder
	check = &snd_channels[NUM_AMBIENTS];
	for (ch_idx = NUM_AMBIENTS; ch_idx < NUM_AMBIENTS + MAX_DYNAMIC_CHANNELS; ch_idx++, check++)
	{
		if (check == target_chan)
			continue;
		if (check->sfx == sfx && !check->pos)
		{
			skip = rand () % (int)(0.1*shm->speed);
			if (skip >= target_chan->end)
				skip = target_chan->end - 1;
			target_chan->pos += skip;
			target_chan->end -= skip;
			break;
		}
	}
}

void S_StopSound (int entnum, int entchannel)
{
	int	i;

	for (i = 0; i < MAX_DYNAMIC_CHANNELS; i++)
	{
		if (snd_channels[i].entnum == entnum
			&& snd_channels[i].entchannel == entchannel)
		{
			snd_channels[i].end = 0;
			snd_channels[i].sfx = NULL;
			return;
		}
	}
}

void S_StopAllSounds (qboolean clear)
{
	int		i;

	if (!sound_started)
		return;

	total_channels = MAX_DYNAMIC_CHANNELS + NUM_AMBIENTS;	// no statics

	for (i = 0; i < MAX_CHANNELS; i++)
	{
		if (snd_channels[i].sfx)
			snd_channels[i].sfx = NULL;
	}

	memset(snd_channels, 0, MAX_CHANNELS * sizeof(channel_t));

	if (clear)
		S_ClearBuffer ();
}

void S_StopAllSoundsC (void)
{
	S_StopAllSounds (true);
}

void S_ClearBuffer (void)
{
	int		clear;

	if (!sound_started || !shm)
		return;

	SNDDMA_LockBuffer ();
	if (! shm->buffer)
		return;

	if (shm->samplebits == 8)
		clear = 0x80;
	else
		clear = 0;

	memset(shm->buffer, clear, shm->samples * shm->samplebits / 8);

	SNDDMA_Submit ();
}


/*
=================
S_StaticSound
=================
*/
void S_StaticSound (sfx_t *sfx, vec3_t origin, float vol, float attenuation)
{
	channel_t	*ss;
	sfxcache_t		*sc;

	if (!sfx)
		return;

	if (total_channels == MAX_CHANNELS)
	{
		Con_Printf ("total_channels == MAX_CHANNELS\n");
		return;
	}

	ss = &snd_channels[total_channels];
	total_channels++;

	sc = S_LoadSound (sfx);
	if (!sc)
		return;

	if (sc->loopstart == -1)
	{
		Con_Printf ("Sound %s not looped\n", sfx->name);
		return;
	}

	ss->sfx = sfx;
	VectorCopy (origin, ss->origin);
	ss->master_vol = (int)vol;
	ss->dist_mult = (attenuation / 64) / sound_nominal_clip_dist;
	ss->end = paintedtime + sc->length;

	SND_Spatialize (ss);
}


//=============================================================================


/*
 ============
 S_Base_RawSamples
 
 Music streaming
 ============
 */
void S_Base_RawSamples( int stream, int samples, int rate, int width, int s_channels, const byte *data, float volume ) {
	int		i;
	int		dst;
	int		intVolume;
	portable_samplepair_t *rawsamples;
	void	*resampler;
	int resampledNumSamples;
	
	if ( !sound_started ) {
		return;
	}
	
	if ( (stream < 0) || (stream >= MAX_RAW_STREAMS) ) {
		return;
	}
	rawsamples = s_rawsamples[stream];
	
	// Set up resampler
	if (s_rawresampler[stream] == NULL)
	{
		s_rawresampler[stream] = Snd_ResamplerInit();
	}
	resampler = s_rawresampler[stream];
	
	intVolume = 256 * volume;
	
	if ( s_rawend[stream] < soundtime ) {
		Con_DPrintf( "S_Base_RawSamples: resetting minimum: %i < %i\n", s_rawend[stream], soundtime );
		s_rawend[stream] = soundtime;
	}
	
	void *resampled = Snd_Resample(resampler, 
								   rate, width, samples, s_channels, data,
								   shm->speed, 2, &resampledNumSamples);
	
	// old:
	
	//Con_Printf ("%i < %i < %i\n", soundtime, s_paintedtime, s_rawend[stream]);
	
	if (s_channels == 2)
	{
		for (i=0 ; i<resampledNumSamples ; i++)
		{
			dst = s_rawend[stream]&(MAX_RAW_SAMPLES-1);
			s_rawend[stream]++;
			rawsamples[dst].left = ((short *)resampled)[i*2] * intVolume;
			rawsamples[dst].right = ((short *)resampled)[i*2+1] * intVolume;
		}
	}
	else if (s_channels == 1)
	{
		for (i=0 ; i<resampledNumSamples; i++)
		{
			dst = s_rawend[stream]&(MAX_RAW_SAMPLES-1);
			s_rawend[stream]++;
			rawsamples[dst].left = ((short *)resampled)[i] * intVolume;
			rawsamples[dst].right = ((short *)resampled)[i] * intVolume;
		}
	}
	else
	{
		Con_Printf( "S_Base_RawSamples: unsupported number of channels %d\n", s_channels );
	}

	
	if ( s_rawend[stream] > soundtime + MAX_RAW_SAMPLES ) {
		Con_DPrintf( "S_Base_RawSamples: overflowed %i > %i\n", s_rawend[stream], soundtime );
	}
}

//=============================================================================

/*
===================
S_UpdateAmbientSounds
===================
*/
void S_UpdateAmbientSounds (void)
{
	mleaf_t		*l;
	int		vol, ambient_channel;
	channel_t	*chan;

	//johnfitz -- no ambients when disconnected
	if (cls.state != ca_connected)
		return;
	//johnfitz

// calc ambient sound levels
	if (!cl.worldmodel)
		return;

	l = Mod_PointInLeaf (listener_origin, cl.worldmodel);
	if (!l || !ambient_level.value)
	{
		for (ambient_channel = 0; ambient_channel < NUM_AMBIENTS; ambient_channel++)
			snd_channels[ambient_channel].sfx = NULL;
		return;
	}

	for (ambient_channel = 0; ambient_channel < NUM_AMBIENTS; ambient_channel++)
	{
		chan = &snd_channels[ambient_channel];
		chan->sfx = ambient_sfx[ambient_channel];

		vol = (int) (ambient_level.value * l->ambient_sound_level[ambient_channel]);
		if (vol < 8)
			vol = 0;

	// don't adjust volume too fast
		if (chan->master_vol < vol)
		{
			chan->master_vol += (int) (host_frametime * ambient_fade.value);
			if (chan->master_vol > vol)
				chan->master_vol = vol;
		}
		else if (chan->master_vol > vol)
		{
			chan->master_vol -= (int) (host_frametime * ambient_fade.value);
			if (chan->master_vol < vol)
				chan->master_vol = vol;
		}

		chan->leftvol = chan->rightvol = chan->master_vol;
	}
}


/*
============
S_Update

Called once each time through the main loop
============
*/
void S_Update (vec3_t origin, vec3_t forward, vec3_t right, vec3_t up)
{
	int			i, j;
	int			total;
	channel_t	*ch;
	channel_t	*combine;

	if (!sound_started || (snd_blocked > 0))
		return;

	VectorCopy(origin, listener_origin);
	VectorCopy(forward, listener_forward);
	VectorCopy(right, listener_right);
	VectorCopy(up, listener_up);

// update general area ambient sound sources
	S_UpdateAmbientSounds ();

	combine = NULL;

// update spatialization for static and dynamic sounds
	ch = snd_channels + NUM_AMBIENTS;
	for (i = NUM_AMBIENTS; i < total_channels; i++, ch++)
	{
		if (!ch->sfx)
			continue;
		SND_Spatialize(ch);	// respatialize channel
		if (!ch->leftvol && !ch->rightvol)
			continue;

	// try to combine static sounds with a previous channel of the same
	// sound effect so we don't mix five torches every frame

		if (i >= MAX_DYNAMIC_CHANNELS + NUM_AMBIENTS)
		{
		// see if it can just use the last one
			if (combine && combine->sfx == ch->sfx)
			{
				combine->leftvol += ch->leftvol;
				combine->rightvol += ch->rightvol;
				ch->leftvol = ch->rightvol = 0;
				continue;
			}
		// search for one
			combine = snd_channels + MAX_DYNAMIC_CHANNELS + NUM_AMBIENTS;
			for (j = MAX_DYNAMIC_CHANNELS + NUM_AMBIENTS; j < i; j++, combine++)
			{
				if (combine->sfx == ch->sfx)
					break;
			}

			if (j == total_channels)
			{
				combine = NULL;
			}
			else
			{
				if (combine != ch)
				{
					combine->leftvol += ch->leftvol;
					combine->rightvol += ch->rightvol;
					ch->leftvol = ch->rightvol = 0;
				}
				continue;
			}
		}
	}

//
// debugging output
//
	if (snd_show.value)
	{
		total = 0;
		ch = snd_channels;
		for (i = 0; i < total_channels; i++, ch++)
		{
			if (ch->sfx && (ch->leftvol || ch->rightvol) )
			{
			//	Con_Printf ("%3i %3i %s\n", ch->leftvol, ch->rightvol, ch->sfx->name);
				total++;
			}
		}

		Con_Printf ("----(%i)----\n", total);
	}

	// add raw data from streamed samples
	S_UpdateBackgroundTrack();	
	
// mix some sound
	S_Update_();
}

void GetSoundtime (void)
{
	int		samplepos;
	static	int		buffers;
	static	int		oldsamplepos;
	int		fullsamples;

	fullsamples = shm->samples / shm->channels;

// it is possible to miscount buffers if it has wrapped twice between
// calls to S_Update.  Oh well.
#ifdef __sun__
	soundtime = SNDDMA_GetSamples();
#else
	samplepos = SNDDMA_GetDMAPos();


	if (samplepos < oldsamplepos)
	{
		buffers++;	// buffer wrapped

		if (paintedtime > 0x40000000)
		{	// time to chop things off to avoid 32 bit limits
			buffers = 0;
			paintedtime = fullsamples;
			S_StopAllSounds (true);
		}
	}
	oldsamplepos = samplepos;

	soundtime = buffers*fullsamples + samplepos/shm->channels;
#endif
}

void S_ExtraUpdate (void)
{

	if (snd_noextraupdate.value)
		return;		// don't pollute timings
	S_Update_();
}

void S_Update_(void)
{
#if 1
	unsigned int	endtime;
	int		samps;

	if (!sound_started || (snd_blocked > 0))
		return;

	SNDDMA_LockBuffer ();
	if (! shm->buffer)
		return;

// Updates DMA time
	GetSoundtime();

// check to make sure that we haven't overshot
	if (paintedtime < soundtime)
	{
	//	Con_Printf ("S_Update_ : overflow\n");
		paintedtime = soundtime;
	}

// mix ahead of current position
	endtime = soundtime + (unsigned int)(_snd_mixahead.value * shm->speed);
	samps = shm->samples >> (shm->channels - 1);
	endtime = min(endtime, (unsigned int)(soundtime + samps));

	S_PaintChannels (endtime);

	SNDDMA_Submit ();
#endif
}



/*
 ===============================================================================
 
 background music functions
 from ioquake3
 
 ===============================================================================
 */

/*
 ======================
 S_PauseBackgroundTrack
 ======================
 */
void S_PauseBackgroundTrack( void ) {
	s_backgroundPaused = true;
}

/*
 ======================
 S_ResumeBackgroundTrack
 ======================
 */
void S_ResumeBackgroundTrack( void ) {
	s_backgroundPaused = false;
}

/*
 ======================
 S_StopBackgroundTrack
 ======================
 */
void S_Base_StopBackgroundTrack( void ) {
	if(!s_backgroundStream)
		return;
	S_CodecCloseStream(s_backgroundStream);
	s_backgroundStream = NULL;
	s_rawend[0] = 0;
}

/*
 ======================
 S_StartBackgroundTrack
 ======================
 */
qboolean S_Base_StartBackgroundTrack( const char *trackname, qboolean loop, S_BackgroundTrackFinishedCallback callback, void *userdata ) {
	Con_DPrintf( "S_StartBackgroundTrack( %s, %s )\n", trackname, (loop ? "looped" : "not looped") );
	
	if(!*trackname)
	{
		S_Base_StopBackgroundTrack();
		return false;
	}
	
	if( !loop ) {
		s_backgroundLoop[0] = 0;
	} else {
		strncpy( s_backgroundLoop, trackname, MAX_QPATH );
		s_backgroundLoop[MAX_QPATH-1] = '\0';
	}
	
	s_backgroundPaused = false;
	
	// close the background track, but DON'T reset s_rawend
	// if restarting the same back ground track
	if(s_backgroundStream)
	{
		S_CodecCloseStream(s_backgroundStream);
		s_backgroundStream = NULL;
	}
	
	// Open stream
	s_backgroundStream = S_CodecOpenStream(trackname);
	if(!s_backgroundStream) {
		return false;
	}

	if (!loop) {
		s_backgroundFinishedCallback = callback;
		s_backgroundFinishedCallbackUserData = userdata;
	}

	return true;
}

/*
 ======================
 S_UpdateBackgroundTrack
 ======================
 */
void S_UpdateBackgroundTrack( void ) {
	int		bufferSamples;
	int		fileSamples;
	byte	raw[30000];		// just enough to fit in a mac stack frame
	int		fileBytes;
	int		r;
	
	if(!s_backgroundStream) {
		return;
	}
	
	// FIXME: this means setting the music volume to 0 pauses it.. not sure if that's great
	// don't bother playing anything if musicvolume is 0
	if ( bgmvolume.value <= 0 ) {
		return;
	}
	
	if ( s_backgroundPaused ) {
		return;
	}
	
	// see how many samples should be copied into the raw buffer
	if ( s_rawend[0] < soundtime ) {
		s_rawend[0] = soundtime;
	}
	
	while ( s_rawend[0] < soundtime + MAX_RAW_SAMPLES ) {
		bufferSamples = MAX_RAW_SAMPLES - (s_rawend[0] - soundtime);
		
		// decide how much data needs to be read from the file
		fileSamples = bufferSamples * s_backgroundStream->info.rate / shm->speed;
		
		if (!fileSamples)
			return;
		
		// our max buffer size
		fileBytes = fileSamples * (s_backgroundStream->info.width * s_backgroundStream->info.channels);
		if ( fileBytes > sizeof(raw) ) {
			fileBytes = sizeof(raw);
			fileSamples = fileBytes / (s_backgroundStream->info.width * s_backgroundStream->info.channels);
		}
		
		// Read
		r = S_CodecReadStream(s_backgroundStream, fileBytes, raw);
		if(r < fileBytes)
		{
			fileBytes = r;
			fileSamples = r / (s_backgroundStream->info.width * s_backgroundStream->info.channels);
		}
		
		if(r > 0)
		{
			// add to raw buffer
			S_Base_RawSamples( 0, fileSamples, s_backgroundStream->info.rate,
							  s_backgroundStream->info.width, s_backgroundStream->info.channels, raw, bgmvolume.value );
		}
		else
		{
			// loop
			if(s_backgroundLoop[0])
			{
				S_CodecCloseStream(s_backgroundStream);
				s_backgroundStream = NULL;
				S_Base_StartBackgroundTrack( s_backgroundLoop, true, NULL, NULL );
				if(!s_backgroundStream)
					return;
			}
			else
			{
				S_Base_StopBackgroundTrack();
				
				if (s_backgroundFinishedCallback)
				{
					(*s_backgroundFinishedCallback)(s_backgroundFinishedCallbackUserData);
				}
				return;
			}
		}
		
	}
}

/*
 ==========================
 S_BackgroundTrackIsPlaying
 ==========================
 */
qboolean S_BackgroundTrackIsPlaying( void )
{
	return (NULL != s_backgroundStream) && (!s_backgroundPaused); 
}

/*
 ==========================
 S_BackgroundTrackIsPaused
 ==========================
 */
qboolean S_BackgroundTrackIsPaused( void )
{
	return (NULL != s_backgroundStream) && (s_backgroundPaused); 
}

/*
 ==========================
 S_BackgroundTrackIsLooping
 ==========================
 */
qboolean S_BackgroundTrackIsLooping( void )
{
	return s_backgroundLoop[0]; 
}


void S_BlockSound (void)
{
/* FIXME: do we really need the blocking at the
 * driver level?
 */
	if (sound_started && snd_blocked == 0)	/* ++snd_blocked == 1 */
	{
		snd_blocked  = 1;
		S_ClearBuffer ();
		if (shm)
			SNDDMA_BlockSound();
	}
}

void S_UnblockSound (void)
{
	if (!sound_started || !snd_blocked)
		return;
	if (snd_blocked == 1)			/* --snd_blocked == 0 */
	{
		snd_blocked  = 0;
		SNDDMA_UnblockSound();
		S_ClearBuffer ();
	}
}

/*
===============================================================================

console functions

===============================================================================
*/

void S_Play (void)
{
	static int hash = 345;
	int		i;
	char	name[256];
	sfx_t	*sfx;

	i = 1;
	while (i < Cmd_Argc())
	{
		if (!Q_strrchr(Cmd_Argv(i), '.'))
		{
			Q_strcpy(name, Cmd_Argv(i));
			Q_strcat(name, ".wav");
		}
		else
			Q_strcpy(name, Cmd_Argv(i));
		sfx = S_PrecacheSound(name);
		S_StartSound(hash++, 0, sfx, listener_origin, 1.0, 1.0);
		i++;
	}
}

void S_PlayVol (void)
{
	static int hash = 543;
	int		i;
	float	vol;
	char	name[256];
	sfx_t	*sfx;

	i = 1;
	while (i < Cmd_Argc())
	{
		if (!Q_strrchr(Cmd_Argv(i), '.'))
		{
			Q_strcpy(name, Cmd_Argv(i));
			Q_strcat(name, ".wav");
		}
		else
			Q_strcpy(name, Cmd_Argv(i));
		sfx = S_PrecacheSound(name);
		vol = Q_atof(Cmd_Argv(i + 1));
		S_StartSound(hash++, 0, sfx, listener_origin, vol, 1.0);
		i+=2;
	}
}

void S_SoundList (void)
{
	int		i;
	sfx_t	*sfx;
	sfxcache_t	*sc;
	int		size, total;

	total = 0;
	for (sfx = known_sfx, i = 0; i < num_sfx; i++, sfx++)
	{
		sc = (sfxcache_t *) Cache_Check (&sfx->cache);
		if (!sc)
			continue;
		size = sc->length*sc->width*(sc->stereo + 1);
		total += size;
		if (sc->loopstart >= 0)
			Con_SafePrintf ("L"); //johnfitz -- was Con_Printf
		else
			Con_SafePrintf (" "); //johnfitz -- was Con_Printf
		Con_SafePrintf("(%2db) %6i : %s\n",sc->width*8,  size, sfx->name); //johnfitz -- was Con_Printf
	}
	Con_Printf ("%i sounds, %i bytes\n", num_sfx, total); //johnfitz -- added count
}


void S_LocalSound (const char *name)
{
	sfx_t	*sfx;

	if (nosound.value)
		return;
	if (!sound_started)
		return;

	sfx = S_PrecacheSound (name);
	if (!sfx)
	{
		Con_Printf ("S_LocalSound: can't cache %s\n", name);
		return;
	}
	S_StartSound (cl.viewentity, -1, sfx, vec3_origin, 1, 1);
}


void S_ClearPrecache (void)
{
}


void S_BeginPrecaching (void)
{
}


void S_EndPrecaching (void)
{
}

