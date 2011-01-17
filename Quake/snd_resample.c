#include "quakedef.h"
#include "sound.h"
#include "speex_resampler.h"

// FIXME: call a real resampler :-)

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
			in16bit[i] = (((unsigned char *)indata)[i] - 128) << 8;
		}
	}
	else
	{
		exit(5);
	}

	// Call the resampler
	SpeexResamplerState *resampler = speex_resampler_init(channels, inrate, outrate, 10, NULL);
	
	*outnumsamples = 0;
	unsigned int consumedtotal = 0;
	unsigned int loops = 0;
	unsigned int consumed, output;
	while (consumedtotal < innumsamples)
	{
		consumed = innumsamples - consumedtotal;
		output = maxsamples - (*outnumsamples);
		speex_resampler_process_interleaved_int(resampler, in16bit, &consumed, outdata, &output);
		consumedtotal += consumed;
		(*outnumsamples) += output;
		loops++;
		if (loops > 100)
		{
			Con_Printf("Infinite loop\n");
		}
	}
	
	speex_resampler_destroy(resampler);
	
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
