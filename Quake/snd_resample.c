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
			in16bit[i] = (((unsigned char *)indata)[i] - 128) << 6; // FIXME: should be << 8, but causes clipping
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
