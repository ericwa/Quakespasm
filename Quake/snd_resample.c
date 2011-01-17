#include "quakedef.h"
#include "sound.h"

// FIXME: call a real resampler :-)

void *Snd_Resample(int inrate, int inwidth, int innumsamples, int channels, const void *indata,
				   int outrate, int outwidth, int *outnumsamples)
{
	char *outdata;
	int		i;
	int		sample, samplefrac, fracstep;
	
	float stepscale = ((float)inrate) / ((float)outrate);	
	*outnumsamples = innumsamples / stepscale;	

	outdata = malloc((*outnumsamples) * channels * outwidth);
	
	// resample / decimate to the current source rate

	samplefrac = 0;
	fracstep = stepscale*256;
	for (i=0 ; i<(*outnumsamples) ; i++)
	{
		int srcsample = samplefrac >> 8;
		samplefrac += fracstep;
		if (inwidth == 2)
			sample = LittleShort ( ((short *)indata)[srcsample] );
		else
			sample = (int)( (((unsigned char *)indata)[srcsample]) - 128) << 8;
		if (outwidth == 2)
			((short *)outdata)[i] = sample;
		else
			((signed char *)outdata)[i] = sample >> 8;
	}
	
	return outdata;
}

void *Snd_ResamplerInit(int inrate, int inwidth, int outrate, int outwidth, int channels) { return NULL; }

void Snd_ResamplerClose(void *resampler) {}

void Snd_ResampleStream(void *resampler,
						int *innumsamples, void *indata,
						int *outnumsamples, void *outdata) {}
