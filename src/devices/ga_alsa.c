#include "gorilla/ga.h"
#include "gorilla/ga_internal.h"

#include <string.h>
#include <unistd.h>
#include <alsa/asoundlib.h>

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

// TODO: consider using SND_PCM_ACCESS_MMAP_INTERLEAVED?
// it would require some restructuring elsewhere.  Probably not worth it,
// especially since most people aren't using straight alsa anymore

struct GaXDeviceImpl {
	snd_pcm_t *interface;
};

static ga_result gaX_open(GaDevice *dev) {
#define acheck(expr) if ((expr) < 0) goto cleanup
	dev->impl = gcX_ops->allocFunc(sizeof(GaXDeviceImpl));
	if (!dev->impl) return GA_ERR_GENERIC;

	if (snd_pcm_open(&dev->impl->interface, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
		gcX_ops->freeFunc(dev->impl);
		return GA_ERR_GENERIC;
	}

	snd_pcm_hw_params_t *params;
        snd_pcm_hw_params_alloca(&params);
        acheck(snd_pcm_hw_params_any(dev->impl->interface, params));

        acheck(snd_pcm_hw_params_set_access(dev->impl->interface, params, SND_PCM_ACCESS_RW_INTERLEAVED));

	snd_pcm_format_t fmt;
	switch (dev->format.bits_per_sample) {
		case  8: fmt = SND_PCM_FORMAT_U8; break;
		case 16: fmt = SND_PCM_FORMAT_S16_LE; break;
		case 24: fmt = SND_PCM_FORMAT_S24_LE; break;
		case 32: fmt = SND_PCM_FORMAT_S32_LE; break;
		default: goto cleanup;
	}
        acheck(snd_pcm_hw_params_set_format(dev->impl->interface, params, fmt));

        acheck(snd_pcm_hw_params_set_channels(dev->impl->interface, params, dev->format.num_channels));
        acheck(snd_pcm_hw_params_set_buffer_size(dev->impl->interface, params, dev->num_samples * ga_format_sample_size(&dev->format)));
	// this can transparently change the sample rate from under the user
	// TODO: should we let them pass an option to error if they can't get exactly the desired sample rate?
	acheck(snd_pcm_hw_params_set_rate_near(dev->impl->interface, params, &dev->format.sample_rate, NULL));
	acheck(snd_pcm_hw_params(dev->impl->interface, params));

	//todo latency

	return GA_OK;

cleanup:
	snd_pcm_drain(dev->impl->interface);
	snd_pcm_close(dev->impl->interface);
	gcX_ops->freeFunc(dev->impl);
	return GA_ERR_GENERIC;
}

static ga_result gaX_close(GaDevice *dev) {
	snd_pcm_drain(dev->impl->interface);
	snd_pcm_close(dev->impl->interface);
	gcX_ops->freeFunc(dev->impl);

	snd_config_update_free_global();
	// this just frees a global cache, that will be reinstated
	// transparently by alsa in the event that the library user creates and
	// then destroys multiple devices
	// but freeing it avoids false positives from valgrind/memtest
	return GA_OK;
}

static gc_int32 gaX_check(GaDevice *dev) {
	snd_pcm_sframes_t avail = snd_pcm_avail(dev->impl->interface);
	if (avail < 0) return 0;
	return avail / dev->num_samples;
}

static ga_result gaX_queue(GaDevice *dev, void *buf) {
	snd_pcm_sframes_t written = snd_pcm_writei(dev->impl->interface, buf, dev->num_samples);
	// TODO: handle the below
	if (written == -EBADFD) return GA_ERR_GENERIC; // PCM is not in the right state (SND_PCM_STATE_PREPARED or SND_PCM_STATE_RUNNING) 
	if (written == -EPIPE) return GA_ERR_GENERIC; // underrun
	if (written == -ESTRPIPE) return GA_ERR_GENERIC; // a suspend event occurred (stream is suspended and waiting for an application recovery)

	if (written != dev->num_samples) return GA_ERR_GENERIC; // underrun/signal
	return GA_OK;
}

GaXDeviceProcs gaX_deviceprocs_ALSA = { gaX_open, gaX_check, gaX_queue, gaX_close };
