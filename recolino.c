/*
    Recolino - a real-time command line annotator

    Copyright (C) 2009 Jan Weil

    based on JACK's capture_client.c
    Copyright (C) 2001 Paul Davis
    Copyright (C) 2003 Jack O'Quin
    and midisine.c
    Copyright (C) 2004 Ian Esten
    
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include <stdio.h>
#include <string.h>
#include <libgen.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>
#include <pthread.h>
#include <math.h>
#include <sndfile.h>
#include <getopt.h>
#include <jack/jack.h>
#include <jack/ringbuffer.h>
#include <jack/thread.h>
#include <jack/midiport.h>

#define RECOLINO_JCLIENT "recolino"
#define RECOLINO_JPORT_MONITOR "monitor"
#define RECOLINO_JPORT_MIDI "midi_in"
#define RECOLINO_RB_SIZE_AUDIO 16384
#define RECOLINO_RB_SIZE_EVENT 64
#define RECOLINO_TEST_TEMPO 120
#define RECOLINO_OUTPUT_KEY(tsec, value) "%.3f %c\n", tsec, value
#define RECOLINO_OUTPUT_MIDI(tsec, value) "%.3f %u\n", tsec, value

#define MIN(a, b) ((a) < (b) ? (a) : (b))

typedef struct {
	enum {RECOLINO_EVENT_KEY, RECOLINO_EVENT_MIDI} evtype;
	jack_nframes_t jtstamp;
	int value;
} recolino_event_t;

typedef struct {
	FILE *annout;
	FILE *errout;
	SNDFILE *sndfile;
	SF_INFO sfinfo;
	jack_client_t *jclient;
	jack_port_t *jportaudio;
	jack_port_t *jportmidi;
	jack_ringbuffer_t *rbaudio;
	jack_ringbuffer_t *rbevent;
	jack_nframes_t startjtime;
	pthread_t disk_thread_id;
	pthread_mutex_t disk_thread_lock;
	pthread_cond_t data_needed;
	pthread_t output_thread_id;
	pthread_mutex_t output_thread_lock;
	pthread_cond_t key_available;
	volatile int running;
	struct termios stattr;
	int sfdflags;
	int echo;
} recolino_info_t;

static int sigcaught = 0;
static jack_default_audio_sample_t *click = NULL;

void usage(char *progname)
{
	fprintf(stderr, "usage: %s [-a | --audioport jack_monitor_port] [-m | --midiport jack_midi_input_port] audiofile\n", basename(progname));
}

void signal_handler(int sig)
{
	sigcaught = 1;
}

void cleanup(int status, void *arg)
{
	recolino_info_t *ri = (recolino_info_t*) arg;

	fprintf(ri->errout, "exiting...\n");

	fflush(ri->annout);

	free(click);

	if (0 != ri->output_thread_id) {
		pthread_cancel(ri->output_thread_id);
	}

	jack_ringbuffer_free(ri->rbaudio);
	jack_ringbuffer_free(ri->rbevent);

	if (NULL != ri->jclient) {
		if (0 != jack_client_close(ri->jclient)) {
			fprintf(ri->errout, "failed to close jack client '%s'\n", RECOLINO_JCLIENT);
		}
	}

	if (NULL != ri->sndfile && 0 != sf_close(ri->sndfile)) {
		fprintf(ri->errout, "failed to close sound file handle\n");
	}

	tcsetattr(STDIN_FILENO, TCSANOW, &ri->stattr);
	fcntl(STDIN_FILENO, F_SETFL, ri->sfdflags);

	exit(status);
}

void *disk_thread(void *arg)
{
	recolino_info_t *ri = (recolino_info_t*) arg;
	float *readbuf; 
	sf_count_t nread;

	readbuf = (float*) malloc(ri->sfinfo.channels * RECOLINO_RB_SIZE_AUDIO * sizeof(float));
	if (NULL == readbuf) {
		fprintf(ri->errout, "memory allocation failed!");
		exit(1);
	}

	while (1) {
		int fi;

		nread = jack_ringbuffer_write_space(ri->rbaudio) / sizeof(jack_default_audio_sample_t);
		nread = sf_readf_float(ri->sndfile, readbuf, nread);

		if (0 == nread || 1 == sigcaught) {
			goto eof;
		}

		for (fi = 0; fi < nread; fi++) {
			jack_default_audio_sample_t sum = .0;
			int ci;

			for (ci = 0; ci < ri->sfinfo.channels; ci++) {
				sum += readbuf[fi * ri->sfinfo.channels + ci];
			}

			jack_ringbuffer_write(ri->rbaudio, (void*) &sum, sizeof(jack_default_audio_sample_t));
		}

		/* ready to rumble */
		if (!ri->running) {
			ri->running = 1;
		}

		pthread_cond_wait(&ri->data_needed, &ri->disk_thread_lock);
	}

eof:
	ri->running = 0;
	free(readbuf);

	return NULL;
}

void *output_thread(void *arg)
{
	recolino_info_t *ri = (recolino_info_t*) arg;

	while (1) {
		while (jack_ringbuffer_read_space(ri->rbevent) >= sizeof(recolino_event_t)) {
			recolino_event_t event;
			float tsec;

			jack_ringbuffer_read(ri->rbevent, (void*) &event, sizeof(recolino_event_t));

			/* this is a feature: we do not consider jack's sampling rate so that you can adapt the speed */
			tsec = (float) (event.jtstamp - ri->startjtime) / ri->sfinfo.samplerate;

			switch (event.evtype) {
				case RECOLINO_EVENT_KEY:
					fprintf(ri->annout, RECOLINO_OUTPUT_KEY(tsec, event.value));
					if (ri->echo) {
						fprintf(ri->errout, RECOLINO_OUTPUT_KEY(tsec, event.value));
					}
					break;
				case RECOLINO_EVENT_MIDI:
					fprintf(ri->annout, RECOLINO_OUTPUT_MIDI(tsec, event.value));
					if (ri->echo) {
						fprintf(ri->errout, RECOLINO_OUTPUT_MIDI(tsec, event.value));
					}
					break;
				default:
					fprintf(ri->errout, "unknown event type!\n");
					exit(1);
			}
			fflush(ri->annout);
		} 
		pthread_cond_wait(&ri->key_available, &ri->output_thread_lock);
	}

	return NULL;
}

int process(jack_nframes_t nframes, void *arg)
{
	recolino_info_t *ri = (recolino_info_t*) arg;
	jack_default_audio_sample_t *out;
	size_t bavail;
	int c;

	if (!ri->running) {
		return 0;
	}

	if (ri->startjtime == 0) {
		/* first frame */
		ri->startjtime = jack_last_frame_time(ri->jclient);
	}

	out = jack_port_get_buffer(ri->jportaudio, nframes);

	if ((bavail = jack_ringbuffer_read_space(ri->rbaudio)) < nframes) {
		fprintf(ri->errout, "audio buffer underrun!\n");
	}

	jack_ringbuffer_read(ri->rbaudio, (void*) out, MIN(bavail, nframes * sizeof(jack_default_audio_sample_t)));

	/* tell disk thread we need more data */
	if (0 == pthread_mutex_trylock(&ri->disk_thread_lock)) {
		pthread_cond_signal(&ri->data_needed);
		pthread_mutex_unlock(&ri->disk_thread_lock);
	}

	/* poll for keyboard input */
	while (-1 != read(STDIN_FILENO, &c, 1)) {
		if (jack_ringbuffer_write_space(ri->rbevent) < sizeof(recolino_event_t)) {
			fprintf(ri->errout, "keyboard buffer overrun!\n");
		} else {
			recolino_event_t event;
			event.evtype = RECOLINO_EVENT_KEY;
			event.jtstamp = jack_last_frame_time(ri->jclient);
			event.value = c;

			jack_ringbuffer_write(ri->rbevent, (void*) &event, sizeof(recolino_event_t));

			/* tell output thread there is data available */
			if (0 == pthread_mutex_trylock(&ri->output_thread_lock)) {
				pthread_cond_signal(&ri->key_available);
				pthread_mutex_unlock(&ri->output_thread_lock);
			}
		}
	}

	/* poll for midi input */
	if (NULL != ri->jportmidi) {
		void *port_buf;
		jack_nframes_t event_count;
		int i;

		port_buf = jack_port_get_buffer(ri->jportmidi, nframes);
		event_count = jack_midi_get_event_count(port_buf);

		if (jack_ringbuffer_write_space(ri->rbevent) < event_count * sizeof(recolino_event_t)) {
			fprintf(ri->errout, "keyboard buffer overrun!\n");
		} else {
			for (i = 0; i < event_count; i++) {
				jack_midi_event_t jmidievent;
				recolino_event_t event;

				jack_midi_event_get(&jmidievent, port_buf, i);

				/* note on event? */
				if ( (*(jmidievent.buffer) & 0xf0) == 0x90 ) {
					event.evtype = RECOLINO_EVENT_MIDI;
					event.jtstamp = jack_last_frame_time(ri->jclient) + jmidievent.time;
					event.value = *(jmidievent.buffer + 1);

					jack_ringbuffer_write(ri->rbevent, (void*) &event, sizeof(recolino_event_t));

					/* tell output thread there is data available */
					if (0 == pthread_mutex_trylock(&ri->output_thread_lock)) {
						pthread_cond_signal(&ri->key_available);
						pthread_mutex_unlock(&ri->output_thread_lock);
					}
				}
			}
		}
	}

	return 0;
}

int main(int argc, char **argv)
{
	struct option long_options[] = {
		{"audioport", required_argument, 0, 'a'},
		{"midiport", required_argument, 0, 'm'},
		{0, 0, 0, 0}
	};
	char *aportname = NULL;
	char *mportname = NULL;
	char *sfilename = NULL;
	/* static for on_exit */
	static recolino_info_t ri;
	struct termios tattr;
	int fdflags;

	tcgetattr(STDIN_FILENO, &ri.stattr);

	while (1) {
		int c = 0;
		int option_index = 0;

		c = getopt_long(argc, argv, "a:m:", long_options, &option_index);

		if (-1 == c) {
			break;
		}

		switch(c) {
			case 'a':
				aportname = optarg;
				break;
			case 'm':
				mportname = optarg;
				break;
			case '?':
				break;
			default:
				usage(argv[0]);
				exit(1);
		}
	}

	ri.errout = stderr;
	ri.annout = stdout;
	ri.sndfile = NULL;
	ri.jclient = NULL;
	ri.jportaudio = NULL;
	ri.jportmidi = NULL;
	ri.disk_thread_id = 0;
	ri.output_thread_id = 0;
	ri.running = 0;
	ri.startjtime = 0;

	ri.rbaudio = jack_ringbuffer_create(RECOLINO_RB_SIZE_AUDIO * sizeof(jack_default_audio_sample_t));
	ri.rbevent = jack_ringbuffer_create(RECOLINO_RB_SIZE_EVENT * sizeof(recolino_event_t));
	if (NULL == ri.rbaudio || NULL == ri.rbevent) {
		fprintf(ri.errout, "ring buffer allocation failed!\n");
		exit(1);
	}
	jack_ringbuffer_mlock(ri.rbaudio);
	jack_ringbuffer_reset(ri.rbaudio);
	jack_ringbuffer_mlock(ri.rbevent);
	jack_ringbuffer_reset(ri.rbevent);

	on_exit(cleanup, &ri);

	signal(SIGTERM, signal_handler);
	signal(SIGINT, signal_handler);

	if (argc - optind != 1) {
		usage(argv[0]);
		exit(1);
	}

	sfilename = argv[optind];

	if (!isatty(STDIN_FILENO)) { 
		fprintf(ri.errout, "stdin seems not to be a terminal. I don't dare to proceed. What are you trying to do anyway?\n"); 
		exit(1);
	}

	/* set non-blocking mode for stdin */
	fdflags = fcntl(STDIN_FILENO, F_GETFL, 0);
	if (fdflags == -1) {
		fprintf(ri.errout, "failed to get file descriptor flags\n");
		exit(1);
	}
	ri.sfdflags = fdflags;
	fdflags |= O_NONBLOCK;
	fcntl(STDIN_FILENO, F_SETFL, fdflags);

	/* set non-canonical mode for stdin */
	tcgetattr(STDIN_FILENO, &tattr);
	tattr.c_lflag &= ~(ICANON|ECHO); 
	tattr.c_cc[VMIN] = 1;
	tattr.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &tattr);

	/* is stdout piped? */
	ri.echo = isatty(fileno(ri.annout)) ? 0 : 1;

	ri.sndfile = sf_open(sfilename, SFM_READ, &ri.sfinfo);

	if (NULL == ri.sndfile) {
		fprintf(ri.errout, "failed to open sound file '%s' for reading\n", sfilename);
		exit(1);
	}

	ri.jclient = jack_client_open(RECOLINO_JCLIENT, JackNullOption, NULL);

	if (NULL == ri.jclient) {
		fprintf(ri.errout, "failed to open jack client '%s'\n", RECOLINO_JCLIENT);
		exit(1);
	}

	if (0 != jack_set_process_callback(ri.jclient, process, &ri)) {
		fprintf(ri.errout, "failed to set process callback\n");
		exit(1);
	}

	if (0 != jack_activate(ri.jclient)) {
		fprintf(ri.errout, "failed to active jack client '%s'\n", RECOLINO_JCLIENT);
		exit(1);
	}

	/* audio monitor port */
	ri.jportaudio = jack_port_register(ri.jclient, RECOLINO_JPORT_MONITOR, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

	if (NULL == ri.jportaudio) {
		fprintf(ri.errout, "failed to register jack port '%s'\n", RECOLINO_JPORT_MONITOR);
		exit(1);
	}

	if (NULL != aportname) {
		if (0 != jack_connect(ri.jclient, RECOLINO_JCLIENT":"RECOLINO_JPORT_MONITOR, aportname)) {
			fprintf(ri.errout, "failed to connect '%s' to '%s'\n", RECOLINO_JCLIENT":"RECOLINO_JPORT_MONITOR, aportname);
			exit(1);
		}
	}

	/* midi input port */
	ri.jportmidi = jack_port_register(ri.jclient, RECOLINO_JPORT_MIDI, JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);

	if (NULL == ri.jportmidi) {
		fprintf(ri.errout, "failed to register jack port '%s'\n", RECOLINO_JPORT_MONITOR);
		exit(1);
	}

	if (NULL != mportname) {
		if (0 != jack_connect(ri.jclient, mportname, RECOLINO_JCLIENT":"RECOLINO_JPORT_MIDI)) {
			fprintf(ri.errout, "failed to connect '%s' to '%s'\n", RECOLINO_JCLIENT":"RECOLINO_JPORT_MIDI, mportname);
			exit(1);
		}
	}

	pthread_mutex_init(&ri.disk_thread_lock, NULL);
	pthread_cond_init(&ri.data_needed, NULL);
	pthread_mutex_init(&ri.output_thread_lock, NULL);
	pthread_cond_init(&ri.key_available, NULL);

	pthread_create(&ri.disk_thread_id, NULL, disk_thread, &ri);

	fprintf(ri.errout, "playing '%s'\n", sfilename);
	fprintf(ri.errout, "playback speed: %.2f\n", (float) jack_get_sample_rate(ri.jclient) / ri.sfinfo.samplerate);

	pthread_create(&ri.output_thread_id, NULL, output_thread, &ri);

	pthread_join(ri.disk_thread_id, NULL);

	return 0;
}
