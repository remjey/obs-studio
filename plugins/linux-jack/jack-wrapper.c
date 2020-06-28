/*
Copyright (C) 2015 by Bernd Buschinski <b.buschinski@gmail.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "jack-wrapper.h"

#include <util/threading.h>
#include <stdio.h>

#include <util/platform.h>

#define blog(level, msg, ...) blog(level, "jack-input: " msg, ##__VA_ARGS__)

/**
 * Get obs speaker layout from number of channels
 *
 * @param channels number of channels reported by jack
 *
 * @return obs speaker_layout id
 *
 * @note This *might* not work for some rather unusual setups, but should work
 *       fine for the majority of cases.
 */
static enum speaker_layout jack_channels_to_obs_speakers(uint_fast32_t channels)
{
	switch (channels) {
	case 1:
		return SPEAKERS_MONO;
	case 2:
		return SPEAKERS_STEREO;
	case 3:
		return SPEAKERS_2POINT1;
	case 4:
		return SPEAKERS_4POINT0;
	case 5:
		return SPEAKERS_4POINT1;
	case 6:
		return SPEAKERS_5POINT1;
	/* What should we do with 7 channels? */
	/* case 7: return SPEAKERS_...; */
	case 8:
		return SPEAKERS_7POINT1;
	}

	return SPEAKERS_UNKNOWN;
}

/**
 * Creates the ring buffer that will receive that data from JACK that
 * jack_transfer_samples() will send to OBS eventually.
 */
static void create_rb(struct jack_data *data)
{
	data->rb_buffer_size = jack_get_buffer_size(data->jack_client);
	jack_nframes_t sample_rate = jack_get_sample_rate(data->jack_client);

	/* The ring buffer is about one second long */
	data->rb_items = sample_rate / data->rb_buffer_size;

	data->rb =
		bzalloc(data->rb_items * sizeof(struct jack_ring_buffer_item));

	for (size_t i = 0; i < data->rb_items; ++i) {
		data->rb[i].buffer = bzalloc(
			data->channels * sizeof(jack_default_audio_sample_t *));
		for (size_t j = 0; j < data->channels; ++j) {
			data->rb[i].buffer[j] =
				bzalloc(data->rb_buffer_size *
					sizeof(jack_default_audio_sample_t));
		}
	}

	data->rb_read = 0;
	data->rb_write = 0;
}

/**
 * Destroys the ring buffer.
 */
static void destroy_rb(struct jack_data *data)
{
	if (!data->rb)
		return;

	os_atomic_set_long(&data->rb_write, 0);
	os_atomic_set_long(&data->rb_read, 0);

	for (size_t i = 0; i < data->rb_items; ++i) {
		for (size_t j = 0; j < data->channels; ++j) {
			bfree(data->rb[i].buffer[j]);
		}
		bfree(data->rb[i].buffer);
	}
	bfree(data->rb);
	data->rb = NULL;
}

/**
 * Continuously checks if samples are available in the ring buffer and send
 * them to OBS.
 *
 * @param arg a struct jack_data
 *
 * @return NULL
 *
 * @note this function only reads the ring buffer when it is not being
 * redimensioned by jack_buffer_size_callback() and it only reads parts
 * that are not being currently written by jack_process_callback().
 */
void *jack_transfer_worker(void *arg)
{
	struct jack_data *data = arg;

	while (data->activated) {
		long rb_read = os_atomic_load_long(&data->rb_read);
		long rb_write = os_atomic_load_long(&data->rb_write);
		if (rb_read >= rb_write) {
			os_sleep_ms(20);
			continue;
		}

		pthread_mutex_lock(&data->rb_mutex);

		struct jack_ring_buffer_item *rb_item =
			data->rb + (rb_read % data->rb_items);

		struct obs_source_audio out;
		out.speakers = jack_channels_to_obs_speakers(data->channels);
		out.samples_per_sec = jack_get_sample_rate(data->jack_client);
		/* format is always 32 bit float for jack */
		out.format = AUDIO_FORMAT_FLOAT_PLANAR;

		for (unsigned int i = 0; i < data->channels; ++i) {
			out.data[i] = (uint8_t *)rb_item->buffer[i];
		}

		out.frames = rb_item->nframes;
		out.timestamp = rb_item->timestamp * 1000;

		obs_source_output_audio(data->source, &out);

		pthread_mutex_unlock(&data->rb_mutex);
		os_atomic_inc_long(&data->rb_read);
	}

	return NULL;
}

/**
 * Called by JACK whenever the maximum size of a buffer changes. Resizes the
 * ring buffer.
 *
 * @param nframes the number of frames available in the buffers
 * @param arg a strurct jack_data
 *
 * @return 0
 */
int jack_buffer_size_callback(jack_nframes_t nframes, void *arg)
{
	struct jack_data *data = (struct jack_data *)arg;
	if (nframes == data->rb_buffer_size)
		return 0;
	blog(LOG_INFO, "bufsize went from %d to %d", data->rb_buffer_size,
	     nframes);

	pthread_mutex_lock(&data->rb_mutex);

	destroy_rb(data);
	data->rb_buffer_size = nframes;
	create_rb(data);

	pthread_mutex_unlock(&data->rb_mutex);
	return 0;
}

/**
 * Called by JACK to process samples. Received samples are copied into
 * the ring buffer. JACK’s documentation states that this code must be suitable
 * for real-time execution, hence it must finish as fast as possible and
 * long or blocking calls and syscalls are forbidden.
 *
 * The use of a ring buffer and atomic integer operations enables the
 * delegation of processing to another thread, see transfer_worker().
 *
 * @param nframes the size of the available buffers
 * @param arg a struct jack_data
 *
 * @return 0
 *
 * @note this function doesn’t run when JACK calls
 * jack_buffer_size_callback() so it is safe for it to not use the rb_mutex
 * when modifying the ring buffer.
 */
int jack_process_callback(jack_nframes_t nframes, void *arg)
{
	struct jack_data *data = (struct jack_data *)arg;
	if (data == 0)
		return 0;

	long rb_write = os_atomic_load_long(&data->rb_write);
	struct jack_ring_buffer_item *rb_item =
		data->rb + (rb_write % data->rb_items);

	for (unsigned int i = 0; i < data->channels; ++i) {
		jack_default_audio_sample_t *jack_buffer =
			(jack_default_audio_sample_t *)jack_port_get_buffer(
				data->jack_ports[i], nframes);
		memcpy(rb_item->buffer[i], jack_buffer,
		       nframes * sizeof(jack_default_audio_sample_t));
	}

	rb_item->nframes = nframes;
	rb_item->timestamp = jack_frames_to_time(
		data->jack_client, jack_last_frame_time(data->jack_client));

	os_atomic_inc_long(&data->rb_write);
	return 0;
}

int_fast32_t jack_init(struct jack_data *data)
{
	if (data->jack_client != NULL)
		goto good;

	jack_options_t jack_option =
		data->start_jack_server ? JackNullOption : JackNoStartServer;

	data->jack_client = jack_client_open(data->device, jack_option, 0);
	if (data->jack_client == NULL) {
		blog(LOG_ERROR,
		     "jack_client_open Error:"
		     "Could not create JACK client! %s",
		     data->device);
		goto error;
	}

	data->jack_ports =
		(jack_port_t **)bzalloc(sizeof(jack_port_t *) * data->channels);
	for (unsigned int i = 0; i < data->channels; ++i) {
		char port_name[10] = {'\0'};
		snprintf(port_name, sizeof(port_name), "in_%u", i + 1);

		data->jack_ports[i] = jack_port_register(
			data->jack_client, port_name, JACK_DEFAULT_AUDIO_TYPE,
			JackPortIsInput, 0);
		if (data->jack_ports[i] == NULL) {
			blog(LOG_ERROR,
			     "jack_port_register Error:"
			     "Could not create JACK port! %s",
			     port_name);
			goto error;
		}
	}

	if (jack_set_buffer_size_callback(
		    data->jack_client, jack_buffer_size_callback, data) != 0) {
		blog(LOG_ERROR, "jack_set_buffer_size_callback Error");
		goto error;
	}

	if (jack_set_process_callback(data->jack_client, jack_process_callback,
				      data) != 0) {
		blog(LOG_ERROR, "jack_set_process_callback Error");
		goto error;
	}

	create_rb(data);

	if (jack_activate(data->jack_client) != 0) {
		blog(LOG_ERROR, "jack_activate Error:"
				"Could not activate JACK client!");
		goto error;
	}

	data->activated = true;

	if (pthread_create(&data->transfer_thread, NULL, jack_transfer_worker,
			   data) != 0) {
		blog(LOG_ERROR,
		     "pthread_create Error:"
		     "Could not create the samples transfer thread!");
		goto error;
	}

	data->transfer_thread_started = true;

good:
	return 0;

error:
	return 1;
}

/**
 * Unregisters the ports registered by jack_init().
 *
 * @param data a struct jack_data
 */
static void unregister_ports(struct jack_data *data)
{
	if (data->jack_ports != NULL) {
		for (int i = 0; i < data->channels; ++i) {
			if (data->jack_ports[i] != NULL)
				jack_port_unregister(data->jack_client,
						     data->jack_ports[i]);
		}
	}
	bfree(data->jack_ports);
	data->jack_ports = NULL;
}

void deactivate_jack(struct jack_data *data)
{
	if (data->jack_client) {
		if (data->activated) {
			jack_deactivate(data->jack_client);
			data->activated = false;
		}

		if (data->transfer_thread_started) {
			pthread_join(data->transfer_thread, NULL);
			data->transfer_thread_started = false;
		}

		unregister_ports(data);

		jack_client_close(data->jack_client);
		data->jack_client = NULL;

		destroy_rb(data);
	}
}
