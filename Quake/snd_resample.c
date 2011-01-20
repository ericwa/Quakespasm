/*
 Copyright (C) 2011 Eric Wasylishen
 
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

#include "quakedef.h"
#include "sound.h"
#include "speex_resampler.h"

#define Snd_ResamplerQuality 0

struct resampler {
	int channels;
	SpeexResamplerState *st;
};

void *Snd_ResamplerInit()
{
	struct resampler *data = malloc(sizeof(struct resampler));
	data->channels = 1;
	data->st = speex_resampler_init(1, 44100, 44100, Snd_ResamplerQuality, NULL);
	return data;
}

void Snd_ResamplerClose(void *handle)
{
	struct resampler *data = (struct resampler *)handle;
	speex_resampler_destroy(data->st);
	free(data);
}

void Snd_ResamplerReset(void *handle)
{
	struct resampler *data = (struct resampler *)handle;
	speex_resampler_reset_mem(data->st);
}

void *Snd_Resample(void *handle,
						 int inrate, int inwidth, int innumsamples, int channels, const void *indata,
						 int outrate, int outwidth, int *outnumsamples)
{
	// check params
	if ( !(inwidth == 1 || inwidth == 2) || !(outwidth == 1 || outwidth == 2) )
	{
		Sys_Error("Snd_ResampleStream only supports 1 or 2 bytes per sample");
	}
	
	const float frac = ((float)inrate) / ((float)outrate);	
	const int maxsamples = (innumsamples / frac) + 10;
	short *in16bit;
	short *out16bit = malloc(maxsamples * channels * 2);
	
	// Convert input to 16-bit if necessary
	
	if (inwidth == 2)
	{
		in16bit = (short*)indata;
	}
	else if (inwidth == 1)
	{
		in16bit = malloc(innumsamples * 2 * channels);
		int i;
		for (i=0; i<innumsamples; i++)
		{
			unsigned char sample = ((unsigned char *)indata)[i];
			in16bit[i] = (((short)sample) - 128) << 8;
		}
	}

	// Call the resampler
	
	if (inrate == outrate)
	{
		memcpy(out16bit, in16bit, innumsamples * channels * 2);
		*outnumsamples = innumsamples;
	}
	else
	{
		struct resampler *data = (struct resampler *)handle;
		
		unsigned int old_inrate, old_outrate;
		speex_resampler_get_rate(data->st, &old_inrate, &old_outrate);
		if (data->channels != channels)
		{
			speex_resampler_destroy(data->st);
			data->st = speex_resampler_init(channels, inrate, outrate, Snd_ResamplerQuality, NULL);
			data->channels = channels;
		}
		else
		{
			speex_resampler_set_rate(data->st, inrate, outrate);
		}
		
		unsigned int consumedtotal = 0;
		unsigned int outputtotal = 0;
		
		while (consumedtotal < innumsamples)
		{
			unsigned int consumed = innumsamples - consumedtotal;
			unsigned int output = maxsamples - outputtotal;
			speex_resampler_process_interleaved_int(data->st, in16bit + consumedtotal, &consumed, out16bit + outputtotal, &output);
			consumedtotal += consumed;
			outputtotal += output;
		}
		
		*outnumsamples = outputtotal;
	}
	
	
	// Prepare to return
	
	if (in16bit != indata)
	{
		free(in16bit);
	}
	
	void *outdata;
	if (outwidth == 2)
	{
		outdata = out16bit;
	}
	else // (outputwidth == 1)
	{
		int i;
		int len = (*outnumsamples) * channels;
		outdata = malloc(len);
		for (i = 0; i<len; i++)
		{
			int s16sample = out16bit[i];
			unsigned char u8sample = ((s16sample + 32768) >> 8);
			((unsigned char *)outdata)[i] = u8sample;
		}
		free(out16bit);
	}

	return outdata;
}