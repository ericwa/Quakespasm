/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2010-2011 O. Sezer <sezero@users.sourceforge.net>

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
// snd_mix.c -- portable code to mix sounds for snd_dma.c

#include "quakedef.h"

#define	PAINTBUFFER_SIZE	2048
portable_samplepair_t paintbuffer[PAINTBUFFER_SIZE];
int		snd_scaletable[32][256];
int		*snd_p, snd_linear_count;
short		*snd_out;

static int	snd_vol;

static void Snd_WriteLinearBlastStereo16 (void)
{
	int		i;
	int		val;

	for (i = 0; i < snd_linear_count; i += 2)
	{
		val = snd_p[i] >> 8;
		if (val > 0x7fff)
			snd_out[i] = 0x7fff;
		else if (val < (short)0x8000)
			snd_out[i] = (short)0x8000;
		else
			snd_out[i] = val;

		val = snd_p[i+1] >> 8;
		if (val > 0x7fff)
			snd_out[i+1] = 0x7fff;
		else if (val < (short)0x8000)
			snd_out[i+1] = (short)0x8000;
		else
			snd_out[i+1] = val;
	}
}

static void S_TransferStereo16 (int endtime)
{
	int		lpos;
	int		lpaintedtime;

	snd_p = (int *) paintbuffer;
	lpaintedtime = paintedtime;

	while (lpaintedtime < endtime)
	{
	// handle recirculating buffer issues
		lpos = lpaintedtime & ((shm->samples >> 1) - 1);

		snd_out = (short *)shm->buffer + (lpos << 1);

		snd_linear_count = (shm->samples >> 1) - lpos;
		if (lpaintedtime + snd_linear_count > endtime)
			snd_linear_count = endtime - lpaintedtime;

		snd_linear_count <<= 1;

	// write a linear blast of samples
		Snd_WriteLinearBlastStereo16 ();

		snd_p += snd_linear_count;
		lpaintedtime += (snd_linear_count >> 1);
	}
}

static void S_TransferPaintBuffer (int endtime)
{
	int	out_idx, out_mask;
	int	count, step, val;
	int	*p;

	if (shm->samplebits == 16 && shm->channels == 2)
	{
		S_TransferStereo16 (endtime);
		return;
	}

	p = (int *) paintbuffer;
	count = (endtime - paintedtime) * shm->channels;
	out_mask = shm->samples - 1;
	out_idx = paintedtime * shm->channels & out_mask;
	step = 3 - shm->channels;

	if (shm->samplebits == 16)
	{
		short *out = (short *)shm->buffer;
		while (count--)
		{
			val = *p >> 8;
			p+= step;
			if (val > 0x7fff)
				val = 0x7fff;
			else if (val < (short)0x8000)
				val = (short)0x8000;
			out[out_idx] = val;
			out_idx = (out_idx + 1) & out_mask;
		}
	}
	else if (shm->samplebits == 8 && !shm->signed8)
	{
		unsigned char *out = shm->buffer;
		while (count--)
		{
			val = *p >> 8;
			p+= step;
			if (val > 0x7fff)
				val = 0x7fff;
			else if (val < (short)0x8000)
				val = (short)0x8000;
			out[out_idx] = (val >> 8) + 128;
			out_idx = (out_idx + 1) & out_mask;
		}
	}
	else if (shm->samplebits == 8)	/* S8 format, e.g. with Amiga AHI */
	{
		signed char *out = (signed char *) shm->buffer;
		while (count--)
		{
			val = *p >> 8;
			p+= step;
			if (val > 0x7fff)
				val = 0x7fff;
			else if (val < (short)0x8000)
				val = (short)0x8000;
			out[out_idx] = (val >> 8);
			out_idx = (out_idx + 1) & out_mask;
		}
	}
}

/*
==============
S_MakeBlackmanWindowKernel

Based on equation 16-4 from
"The Scientist and Engineer's Guide to Digital Signal Processing"

kernel has room for M+1 floats,
f_c is the filter cutoff frequency, as a fraction of the samplerate
==============
*/
static void S_MakeBlackmanWindowKernel(float *kernel, int M, float f_c)
{
	int i;
	for (i = 0; i <= M; i++)
	{
		if (i == M/2)
		{
			kernel[i] = 2 * M_PI * f_c;
		}
		else
		{
			kernel[i] = ( sin(2 * M_PI * f_c * (i - M/2.0)) / (i - (M/2.0)) )
				* (0.42 - 0.5*cos(2 * M_PI * i / (double)M)
				   + 0.08*cos(4 * M_PI * i / (double)M) );
		}
	}
	
// normalize the kernel so all of the values sum to 1
	{
		float sum = 0;
		for (i = 0; i <= M; i++)
		{
			sum += kernel[i];
		}
		
		for (i = 0; i <= M; i++)
		{
			kernel[i] /= sum;
		}
	}
}

// must be divisible by 4
#define FILTER_KERNEL_SIZE 48

/*
==============
S_LowpassFilter

lowpass filters 24-bit integer samples in 'data' (stored in 32-bit ints).

f_c is the filter cutoff frequency, as a fraction of the samplerate
memory must be an array of FILTER_KERNEL_SIZE floats
==============
*/
static void S_LowpassFilter(float f_c, int *data, int stride, int count,
							float *memory)
{
	int i;
	
// M is the "kernel size" parameter for makekernel() - must be even.
// FILTER_KERNEL_SIZE size is M+1, rounded up to be divisible by 4
	const int M = FILTER_KERNEL_SIZE - 2;
	
	float input[FILTER_KERNEL_SIZE + count];
	
	static float kernel[FILTER_KERNEL_SIZE];
	static float kernel_fc;
	
	if (f_c <= 0 || f_c > 0.5)
		return;
	
	if (count < FILTER_KERNEL_SIZE)
	{
		Con_Warning("S_LowpassFilter: not enough samples");
		return;
	}

// prepare the kernel
	
	if (kernel_fc != f_c)
	{
		S_MakeBlackmanWindowKernel(kernel, M, f_c);
		kernel_fc = f_c;
	}
	
// set up the input buffer
// memory holds the previous FILTER_KERNEL_SIZE samples of input.

	for (i=0; i<FILTER_KERNEL_SIZE; i++)
	{
		input[i] = memory[i];
	}
	for (i=0; i<count; i++)
	{
		input[FILTER_KERNEL_SIZE+i] = data[i * stride] / (32768.0 * 256.0);
	}
	
// copy out the last FILTER_KERNEL_SIZE samples to 'memory' for next time
	
	memcpy(memory, input + count, FILTER_KERNEL_SIZE * sizeof(float));
	
// apply the filter
	
	for (i=0; i<count; i++)
	{
		float val[4] = {0, 0, 0, 0};
		
		int j;
		for (j=0; j<=M; j+=4)
		{
			val[0] += kernel[j] * input[M + i - j];
			val[1] += kernel[j+1] * input[M + i - (j+1)];
			val[2] += kernel[j+2] * input[M + i - (j+2)];
			val[3] += kernel[j+3] * input[M + i - (j+3)];
		}
		
		data[i * stride] = (val[0] + val[1] + val[2] + val[3])
		* (32768.0 * 256.0);
	}
}

/*
===============================================================================

CHANNEL MIXING

===============================================================================
*/

static void SND_PaintChannelFrom8 (channel_t *ch, sfxcache_t *sc, int endtime, int paintbufferstart);
static void SND_PaintChannelFrom16 (channel_t *ch, sfxcache_t *sc, int endtime, int paintbufferstart);

void S_PaintChannels (int endtime)
{
	int		i;
	int		end, ltime, count;
	channel_t	*ch;
	sfxcache_t	*sc;

	snd_vol = sfxvolume.value * 256;

	while (paintedtime < endtime)
	{
	// if paintbuffer is smaller than DMA buffer
		end = endtime;
		if (endtime - paintedtime > PAINTBUFFER_SIZE)
			end = paintedtime + PAINTBUFFER_SIZE;

	// clear the paint buffer
		memset(paintbuffer, 0, (end - paintedtime) * sizeof(portable_samplepair_t));

	// paint in the channels.
		ch = snd_channels;
		for (i = 0; i < total_channels; i++, ch++)
		{
			if (!ch->sfx)
				continue;
			if (!ch->leftvol && !ch->rightvol)
				continue;
			sc = S_LoadSound (ch->sfx);
			if (!sc)
				continue;

			ltime = paintedtime;

			while (ltime < end)
			{	// paint up to end
				if (ch->end < end)
					count = ch->end - ltime;
				else
					count = end - ltime;

				if (count > 0)
				{
					// the last param to SND_PaintChannelFrom is the index
					// to start painting to in the paintbuffer, usually 0.
					if (sc->width == 1)
						SND_PaintChannelFrom8(ch, sc, count, ltime - paintedtime);
					else
						SND_PaintChannelFrom16(ch, sc, count, ltime - paintedtime);

					ltime += count;
				}

			// if at end of loop, restart
				if (ltime >= ch->end)
				{
					if (sc->loopstart >= 0)
					{
						ch->pos = sc->loopstart;
						ch->end = ltime + sc->length - ch->pos;
					}
					else
					{	// channel just stopped
						ch->sfx = NULL;
						break;
					}
				}
			}
		}

	// clip each sample to 0dB, then reduce by 6dB (to leave some headroom for
	// the lowpass filter and the music). the lowpass will smooth out the
	// clipping
		for (i=0; i<end-paintedtime; i++)
		{
			paintbuffer[i].left = CLAMP(-32768 << 8, paintbuffer[i].left, 32767 << 8) >> 1;
			paintbuffer[i].right = CLAMP(-32768 << 8, paintbuffer[i].right, 32767 << 8) >> 1;
		}
		
	// apply a lowpass filter
		if (sndspeed.value < shm->speed)
		{
			static float memory_l[FILTER_KERNEL_SIZE];
			static float memory_r[FILTER_KERNEL_SIZE];
			
			const float cutoff_freq = (sndspeed.value * 0.45) / shm->speed;
			
			S_LowpassFilter(cutoff_freq, (int *)paintbuffer,       2, end - paintedtime, memory_l);
			S_LowpassFilter(cutoff_freq, ((int *)paintbuffer) + 1, 2, end - paintedtime, memory_r);
		}
		
	// paint in the music
		if (s_rawend >= paintedtime)
		{	// copy from the streaming sound source
			int		s;
			int		stop;
			
			stop = (end < s_rawend) ? end : s_rawend;
			
			for (i = paintedtime; i < stop; i++)
			{
				s = i & (MAX_RAW_SAMPLES - 1);
			// lower music by 6db to match sfx
				paintbuffer[i - paintedtime].left += s_rawsamples[s].left >> 1;
				paintbuffer[i - paintedtime].right += s_rawsamples[s].right >> 1;
			}
			//	if (i != end)
			//		Con_Printf ("partial stream\n");
			//	else
			//		Con_Printf ("full stream\n");
		}
		
	// transfer out according to DMA format
		S_TransferPaintBuffer(end);
		paintedtime = end;
	}
}

void SND_InitScaletable (void)
{
	int		i, j;
	int		scale;

	for (i = 0; i < 32; i++)
	{
		scale = i * 8 * 256 * sfxvolume.value;
		for (j = 0; j < 256; j++)
		{
		/* When compiling with gcc-4.1.0 at optimisations O1 and
		   higher, the tricky signed char type conversion is not
		   guaranteed. Therefore we explicity calculate the signed
		   value from the index as required. From Kevin Shanahan.
		   See: http://gcc.gnu.org/bugzilla/show_bug.cgi?id=26719
		*/
		//	snd_scaletable[i][j] = ((signed char)j) * scale;
			snd_scaletable[i][j] = ((j < 128) ?  j : j - 256) * scale;
		}
	}
}


static void SND_PaintChannelFrom8 (channel_t *ch, sfxcache_t *sc, int count, int paintbufferstart)
{
	int	data;
	int		*lscale, *rscale;
	unsigned char	*sfx;
	int		i;

	if (ch->leftvol > 255)
		ch->leftvol = 255;
	if (ch->rightvol > 255)
		ch->rightvol = 255;

	lscale = snd_scaletable[ch->leftvol >> 3];
	rscale = snd_scaletable[ch->rightvol >> 3];
	sfx = (unsigned char *)sc->data + ch->pos;

	for (i = 0; i < count; i++)
	{
		data = sfx[i];
		paintbuffer[paintbufferstart + i].left += lscale[data];
		paintbuffer[paintbufferstart + i].right += rscale[data];
	}

	ch->pos += count;
}

static void SND_PaintChannelFrom16 (channel_t *ch, sfxcache_t *sc, int count, int paintbufferstart)
{
	int	data;
	int	left, right;
	int	leftvol, rightvol;
	signed short	*sfx;
	int	i;

	leftvol = ch->leftvol * snd_vol;
	rightvol = ch->rightvol * snd_vol;
	leftvol >>= 8;
	rightvol >>= 8;
	sfx = (signed short *)sc->data + ch->pos;

	for (i = 0; i < count; i++)
	{
		data = sfx[i];
	// this was causing integer overflow as observed in quakespasm
	// with the warpspasm mod moved >>8 to left/right volume above.
	//	left = (data * leftvol) >> 8;
	//	right = (data * rightvol) >> 8;
		left = data * leftvol;
		right = data * rightvol;
		paintbuffer[paintbufferstart + i].left += left;
		paintbuffer[paintbufferstart + i].right += right;
	}

	ch->pos += count;
}

