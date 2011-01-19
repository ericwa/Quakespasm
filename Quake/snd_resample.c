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

void *Snd_Resample(int inrate, int inwidth, int innumsamples, int channels, const void *indata,
				   int outrate, int outwidth, int *outnumsamples)
{
	const float frac = ((float)inrate) / ((float)outrate);	
	const int maxsamples = (innumsamples / frac) + 10;
	short *outdata = malloc(maxsamples * channels * outwidth);

	// Convert input to 16-bit if necessary
	short *in16bit;
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
			
			if (sample == 255)
			{
				//Con_Printf("8-bit clipping\n");
			}
			
			in16bit[i] = (((short)sample) - 128) << 8;
		}
	}
	else
	{
		exit(5);
	}

	// See if we need to resample
	if (inrate == outrate)
	{
		memcpy(outdata, in16bit, innumsamples * channels * 2);
		*outnumsamples = innumsamples;
	}
	else
	{
		
		// Call the resampler
		static SpeexResamplerState *st = NULL;
		if (st == NULL)
		{
			st = speex_resampler_init(channels, inrate, outrate, 0, NULL);
		}
		else
		{
			speex_resampler_reset_mem(st);
		}
		speex_resampler_set_rate(st, inrate, outrate);

		*outnumsamples = 0;
		unsigned int consumedtotal = 0;
		unsigned int outputtotal = 0;
		unsigned int loops = 0;
		unsigned int consumed, output;
		while (consumedtotal < innumsamples)
		{
			int roomToConsume, roomToOutput;
			
			consumed = innumsamples - consumedtotal;
			output = maxsamples - outputtotal;
			
			roomToConsume = consumed;
			roomToOutput = output;
			
			speex_resampler_process_interleaved_int(st, in16bit + consumedtotal, &consumed, outdata + outputtotal, &output);
			consumedtotal += consumed;
			outputtotal += output;
			
			loops++;
			if (loops > 100)
			{
				Con_Printf("Infinite loop\n");
			}
		}
		
		*outnumsamples = outputtotal;
		
		if (*outnumsamples != (innumsamples / frac))
		{
			Con_Printf("Output %d, predicted %d\n", *outnumsamples, (innumsamples / frac));
		}
		//speex_resampler_destroy(resampler);
	}
	
	// Check for clipping.
	{
		int i;
		for (i=0; i<*outnumsamples; i++)
		{
			short sample = outdata[i];
			
			if (sample == 32767)
			{
				//Con_Printf("16-bit clipping\n");
			}
		}
	}
	
	if (in16bit != indata)
	{
		free(in16bit);
	}
	
	if(outwidth != 2) exit(5);
	
	return outdata;
}

void *Snd_ResamplerInit(int inrate, int inwidth, int outrate, int outwidth, int channels) { return NULL; }

void Snd_ResamplerClose(void *resampler) {}

void Snd_ResampleStream(void *resampler,
						int *innumsamples, void *indata,
						int *outnumsamples, void *outdata) {}
