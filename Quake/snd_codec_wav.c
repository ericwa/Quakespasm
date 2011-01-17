/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2005 Stuart Dalton (badcdev@gmail.com)

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "quakedef.h"
#include "snd_codec.h"

#define PAD(x,y) (((x)+(y)-1) & ~((y)-1))

/*
=================
FGetLittleLong
=================
*/
static int FGetLittleLong( int handle ) {
	int		v;

	Sys_FileRead( handle, &v, sizeof(v) );

	return LittleLong( v);
}

/*
=================
FGetLittleShort
=================
*/
static short FGetLittleShort( int handle ) {
	short	v;

	Sys_FileRead( handle, &v, sizeof(v) );

	return LittleShort( v);
}

/*
=================
S_ReadChunkInfo
=================
*/
static int S_ReadChunkInfo(int handle, char *name)
{
	int len, r;

	name[4] = 0;

	r = Sys_FileRead(handle, name, 4);
	if(r != 4)
		return -1;

	len = FGetLittleLong(handle);
	if( len < 0 ) {
		//Con_Printf( "WARNING: Negative chunk length\n" );
		return -1;
	}
	

	return len;
}

/*
=================
S_FindRIFFChunk

Returns the length of the data in the chunk, or -1 if not found
=================
*/
static int S_FindRIFFChunk( int handle, char *chunk ) {
	char	name[5];
	int		len;

	while( ( len = S_ReadChunkInfo(handle, name) ) >= 0 )
	{
		// If this is the right chunk, return
		if( !Q_strncmp( name, chunk, 4 ) )
			return len;

		len = PAD( len, 2 );

		// Not the right chunk - skip it
		Sys_FileSeekRelative( handle, len );
	}

	return -1;
}

/*
=================
S_ByteSwapRawSamples
=================
*/
static void S_ByteSwapRawSamples( int samples, int width, int s_channels, const byte *data ) {
	int		i;

	if ( width != 2 ) {
		return;
	}
	if ( LittleShort( 256 ) == 256 ) {
		return;
	}

	if ( s_channels == 2 ) {
		samples <<= 1;
	}
	for ( i = 0 ; i < samples ; i++ ) {
		((short *)data)[i] = LittleShort( ((short *)data)[i] );
	}
}

/*
=================
S_ReadRIFFHeader
=================
*/
static qboolean S_ReadRIFFHeader( int file, const char *filename, snd_info_t *info)
{
	char dump[16];
	int wav_format;
	int bits;
	int fmtlen = 0;
	
	// skip the riff wav header
	Sys_FileRead(file, dump, 12);

	int wavstart = Sys_FileTell(file);	
	
	// Scan for the format chunk
	if((fmtlen = S_FindRIFFChunk(file, "fmt ")) < 0)
	{
		Con_Printf( "ERROR: Couldn't find \"fmt\" chunk\n");
		return false;
	}

	// Save the parameters
	wav_format = FGetLittleShort(file);
	info->channels = FGetLittleShort(file);
	info->rate = FGetLittleLong(file);
	FGetLittleLong(file);
	FGetLittleShort(file);
	bits = FGetLittleShort(file);

	if( bits < 8 )
	{
	  Con_Printf( "ERROR: Less than 8 bit sound is not supported\n");
	  return false;
	}

	info->width = bits / 8;
	info->dataofs = 0;
	info->samples = 0;
	
	// get cue chunk
	Sys_FileSeek(file, wavstart);
	if( S_FindRIFFChunk(file, "cue ") > 0)
	{
		Sys_FileSeekRelative(file, 24);
		info->loopstart = FGetLittleLong(file);
		
		// if the next chunk is a LIST chunk, look for a cue length marker
		if ( S_FindRIFFChunk(file, "LIST") > 0)
		{
			Sys_FileSeekRelative(file, 20);
			char marker[4];
			Sys_FileRead(file, marker, 4);
			
			if (!strncmp (marker, "mark", 4))
			{	// this is not a proper parse, but it works with cooledit...
				Sys_FileSeekRelative(file, -8);
				int i = FGetLittleLong(file);	// samples in loop
				info->samples = info->loopstart + i;
			}
		}
	}
	else
	{
		info->loopstart = -1;
	}

	// Scan for the data chunk
	Sys_FileSeek(file, wavstart);
	if( (info->size = S_FindRIFFChunk(file, "data")) < 0)
	{
		Con_Printf( "ERROR: Couldn't find \"data\" chunk\n");
		return false;
	}

	int samples = (info->size / info->width) / info->channels;
	
	if (info->samples)
	{
		if (samples < info->samples)
		{
			Con_Printf ("Sound has a bad loop length \"%s\"\n", filename);
			info->samples = samples;
		}
	}
	else
		info->samples = samples;
	
	info->dataofs = Sys_FileTell(file) - (wavstart - 12);
	
	return true;
}

// WAV codec
snd_codec_t wav_codec =
{
	".wav",
	S_WAV_CodecLoad,
	S_WAV_CodecOpenStream,
	S_WAV_CodecReadStream,
	S_WAV_CodecCloseStream,
	NULL
};

/*
=================
S_WAV_CodecLoad
=================
*/
void *S_WAV_CodecLoad(const char *filename, snd_info_t *info)
{
	int file;
	void *buffer;

	// Try to open the file
	COM_OpenFile(filename, &file);
	if(file == -1)
	{
		Con_Printf( "ERROR: Could not open \"%s\"\n",
				filename);
		return NULL;
	}

	// Read the RIFF header
	if(!S_ReadRIFFHeader(file, filename, info))
	{
		COM_CloseFile(file);
		Con_Printf( "ERROR: Incorrect/unsupported format in \"%s\"\n",
				filename);
		return NULL;
	}

	// Allocate some memory
	buffer = Z_Malloc(info->size);
	if(!buffer)
	{
		COM_CloseFile(file);
		Con_Printf( "ERROR: Out of memory reading \"%s\"\n",
				filename);
		return NULL;
	}

	// Read, byteswap
	Sys_FileRead(file, buffer, info->size);
	S_ByteSwapRawSamples(info->samples, info->width, info->channels, (byte *)buffer);

	// Close and return
	COM_CloseFile(file);
	return buffer;
}

/*
=================
S_WAV_CodecOpenStream
=================
*/
snd_stream_t *S_WAV_CodecOpenStream(const char *filename)
{
	snd_stream_t *rv;

	// Open
	rv = S_CodecUtilOpen(filename, &wav_codec);
	if(!rv)
		return NULL;

	// Read the RIFF header
	if(!S_ReadRIFFHeader(rv->file, filename, &rv->info))
	{
		S_CodecUtilClose(&rv);
		return NULL;
	}

	return rv;
}

/*
=================
S_WAV_CodecCloseStream
=================
*/
void S_WAV_CodecCloseStream(snd_stream_t *stream)
{
	S_CodecUtilClose(&stream);
}

/*
=================
S_WAV_CodecReadStream
=================
*/
int S_WAV_CodecReadStream(snd_stream_t *stream, int bytes, void *buffer)
{
	int remaining = stream->info.size - stream->pos;
	int samples;

	if(remaining <= 0)
		return 0;
	if(bytes > remaining)
		bytes = remaining;
	stream->pos += bytes;
	samples = (bytes / stream->info.width) / stream->info.channels;
	Sys_FileRead(stream->file, buffer, bytes);
	S_ByteSwapRawSamples(samples, stream->info.width, stream->info.channels, buffer);
	return bytes;
}
