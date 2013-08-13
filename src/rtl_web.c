/*
 * rtl-sdr, turns your Realtek RTL2832 based DVB dongle into a SDR receiver
 * Copyright (C) 2012 by Steve Markgraf <steve@steve-m.de>
 * Copyright (C) 2012 by Hoernchen <la@tfc-server.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef _WIN32
#error "Win32 unsupported"
#endif

#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <math.h>

#include <pthread.h>
#include <libwebsockets.h>
#include <fftw3.h>
#include <jansson.h>
#include "rtl-sdr.h"

#define DEFAULT_FFT_SIZE		(65536)
static rtlsdr_dev_t *dev = NULL;
static int do_exit = 0;
static char* cwd;

enum rtl_web_protocols {
        /* always first */
        PROTOCOL_HTTP = 0,

        PROTOCOL_PLOT_DATA,

        PROTOCOL_RTL,

        /* always last */
        PROTOCOL_COUNT
};

static struct libwebsocket_protocols protocols[];


void usage(void)
{
	printf("rtl_web, an I/Q spectrum server for RTL2832 based DVB-T receivers\n\n"
		"Usage:\t[-a listen address]\n"
		"\t[-p listen port (default: 1234)]\n"
		"\t[-f frequency to tune to [Hz]]\n"
		"\t[-g gain (default: 0 for auto)]\n"
		"\t[-s samplerate in Hz (default: 2048000 Hz)]\n"
		"\t[-b fftsize (default: 65536)]\n"
		"\t[-d device index (default: 0)]\n"
		"\t[-n number of averages (default: 100)]\n");
	exit(1);
}

static void sighandler(int signum)
{
	fprintf(stderr, "Signal caught, exiting!\n");
	if (!do_exit) {
		rtlsdr_cancel_async(dev);
		do_exit = 1;
	}
}

struct serveable {
	const char *urlpath;
	const char *mimetype;
};

static const struct serveable whitelist[] = {
	{ "/favicon.ico", "image/x-icon" },
	/* last one is the default served if no match */
	{ "/xplot.js", "text/javascript" },
	{ "/rtl_web.html", "text/html" },
};

static char* a404_error = "HTTP/1.0 404 OK\x0d\x0aServer: libwebsockets\x0d\x0a";

static int callback_http(
		struct libwebsocket_context *context,
		struct libwebsocket *wsi,
		enum libwebsocket_callback_reasons reason,
		void *user,
		void *in,
		size_t len)
{
	unsigned int n;
	char client_name[128];
	char client_ip[128];
	char *local_path = calloc(strlen(cwd) + 512, sizeof(char));

	switch (reason) {
	case LWS_CALLBACK_HTTP:
		if (in) {
			fprintf(stderr, "serving HTTP URI %s\n", (char *)in);

			for (n = 0; n < (sizeof(whitelist) / sizeof(whitelist[0]) - 1); n++)
				if (in && strcmp((const char *)in, whitelist[n].urlpath) == 0)
					break;
			sprintf(local_path, "%s/www/%s", cwd, whitelist[n].urlpath);
			fprintf(stderr, "serving file %s\n", local_path);
			if (libwebsockets_serve_http_file(wsi, local_path, "text/html"))
				fprintf(stderr, "Failed to send HTTP file\n");
		}
		break;

		/*
		 * callback for confirming to continue with client IP appear in
		 * protocol 0 callback since no websocket protocol has been agreed
		 * yet.  You can just ignore this if you won't filter on client IP
		 * since the default uhandled callback return is 0 meaning let the
		 * connection continue.
		 */

	case LWS_CALLBACK_FILTER_NETWORK_CONNECTION:

		libwebsockets_get_peer_addresses(
				(int)(long)user,
				client_name,
				sizeof(client_name),
				client_ip,
				sizeof(client_ip));

		fprintf(stderr, "Received network connect from %s (%s)\n",
				client_name, client_ip);

		/* if we returned non-zero from here, we kill the connection */
		break;

	default:
		break;
	}

	return 0;
}

static int
callback_rtl(
		struct libwebsocket_context *context,
		struct libwebsocket *wsi,
		enum libwebsocket_callback_reasons reason,
		void *user,
		void *in,
		size_t len)
{
	int n;
	json_error_t json_error;
	json_t *root;

	unsigned char buf[LWS_SEND_BUFFER_PRE_PADDING + 512 + LWS_SEND_BUFFER_POST_PADDING];
	unsigned char *message = &buf[LWS_SEND_BUFFER_PRE_PADDING];
	
	switch (reason) {

	case LWS_CALLBACK_ESTABLISHED:
		fprintf(stderr, "callback_plot_data: LWS_CALLBACK_ESTABLISHED\n");

		//
		// TODO
		// {
		//  tuner_type: " ",
		//  frequency_hz: 0.0
		// }	
		n = sprintf((char *)message, "test");
		n = libwebsocket_write(wsi, message, n, LWS_WRITE_TEXT);
		if (n < 0) {
			fprintf(stderr, "ERROR writing to socket");
			return 1;
		}
		break;

	case LWS_CALLBACK_BROADCAST:
		n = libwebsocket_write(wsi, in, len, LWS_WRITE_BINARY);
		if (n < 0) {
			fprintf(stderr, "ERROR writing to socket");
			return 1;
		}
		break;

	case LWS_CALLBACK_RECEIVE:
		root = json_loadb(in, len, 0, &json_error);
		if (root == NULL) {
			fprintf(stderr, "invalid rx %s\n", in);
			break;
		}
		if (!json_is_object(root)) {
			fprintf(stderr, "invalid message %s\n", in);
			break;
		}
		json_t *freq = json_object_get(root, "frequency_hz");
		if (json_is_number(freq)) {
			int frequency = json_integer_value(freq);
			int r;
			r = rtlsdr_set_center_freq(dev, frequency);
			if (r < 0)
				fprintf(stderr, "WARNING: Failed to set center freq.\n");
			else
				fprintf(stderr, "Tuned to %i Hz.\n", frequency);
			json_decref(freq);
		}
		json_decref(root);

		libwebsockets_broadcast(
			&protocols[PROTOCOL_PLOT_DATA],
			NULL,
			0);
		break;
	default:
		break;
	}

	return 0;
}

static int
callback_plot_data(
		struct libwebsocket_context *context,
		struct libwebsocket *wsi,
		enum libwebsocket_callback_reasons reason,
		void *user,
		void *in,
		size_t len)
{
	int n;
	json_error_t json_error;
	json_t *root;

	unsigned char buf[LWS_SEND_BUFFER_PRE_PADDING + 512 + LWS_SEND_BUFFER_POST_PADDING];
	unsigned char *message = &buf[LWS_SEND_BUFFER_PRE_PADDING];
	
	switch (reason) {

	case LWS_CALLBACK_ESTABLISHED:
		fprintf(stderr, "callback_plot_data: LWS_CALLBACK_ESTABLISHED\n");

		//
		// TODO
		// {
		//  xstart: 0.0,
		//  xdelta: 0.0
		// }	 
		uint32_t frequency_hz = rtlsdr_get_center_freq(dev);
		uint32_t samprate_hz = rtlsdr_get_sample_rate(dev); 
		double xdelta = (((double) samprate_hz) / 65536); // TODO use fftsize
		double xstart = frequency_hz - (((double) samprate_hz) / 2); // TODO use fftsize

		n = sprintf((char *)message, "{ \"xstart\": %f, \"xdelta\": %f }", xstart, xdelta); // TODO use jansson
		n = libwebsocket_write(wsi, message, n, LWS_WRITE_TEXT);
		if (n < 0) {
			fprintf(stderr, "ERROR writing to socket");
			return 1;
		}
		break;

	case LWS_CALLBACK_BROADCAST:
		if (len > 0) {
			n = libwebsocket_write(wsi, in, len, LWS_WRITE_BINARY);
			if (n < 0) {
				fprintf(stderr, "ERROR writing to socket");
				return 1;
			}
		} else {
			uint32_t frequency_hz = rtlsdr_get_center_freq(dev);
			uint32_t samprate_hz = rtlsdr_get_sample_rate(dev); 
			double xdelta = (((double) samprate_hz) / 65536); // TODO use fftsize
			double xstart = frequency_hz - (((double) samprate_hz) / 2); // TODO use fftsize

			n = sprintf((char *)message, "{ \"xstart\": %f, \"xdelta\": %f }", xstart, xdelta); // TODO use jansson
			n = libwebsocket_write(wsi, message, n, LWS_WRITE_TEXT);
			if (n < 0) {
				fprintf(stderr, "ERROR writing to socket");
				return 1;
			}
		}
		break;

	default:
		break;
	}

	return 0;
}

static struct libwebsocket_protocols protocols[] = {
	/* first protocol must always be HTTP handler */
	{
		"http-only",            /* name */
		callback_http,          /* callback */
		0                       /* per_session_data_size */
	},
	{
		"plot-data",
		callback_plot_data,
		0,
	},
	{
		"rtl",
		callback_rtl,
		0,
	},
	{
		NULL, NULL, 0           /* End of list */
	}
};

void window(double *buf, int size, const char *name) {
	double d[4];

	if (strcmp(name, "HANNING") == 0) {
		d[0] = 0.5;
		d[1] = -0.5;
		d[2] = 0.0;
		d[3] = 0.0;
	} else {
		fprintf(stderr, "Unknown window");
	}

	int ndo = size;
	if (ndo % 2 == 0) ndo = (ndo/2)+1;

	int i;
	double c;
	for (i=0; i<ndo; i++) {
		c = 2*M_PI*((float) i)/size;
		buf[i] = d[3]*cos(3*c) + d[2]*cos(2*c) + d[1]*cos(c) + d[0];
	}
	for (i=ndo; i<size; i++) {
		buf[i] = buf[size-i+1];
	}
}

int main(int argc, char **argv)
{
	int r, opt, i;
	char* addr = "127.0.0.1";
	int port = 1234;
	uint32_t frequency = 100000000, samp_rate = 2048000;
	struct sockaddr_in local, remote;
	int device_count;
	uint32_t dev_index = 0, buf_num = 0;
	int gain = 0;
	struct llist *curelem,*prev;
	
	uint32_t fft_size = DEFAULT_FFT_SIZE;
	uint8_t *buffer;
	uint8_t *websocket_buffer;
	int n_read;
	int num_avgs = 10;
	int ctr = 0;
	
	struct libwebsocket_context *context;
	const char *cert_path = NULL;
	const char *key_path = NULL;
	int opts = 0;

	double *win;
	fftw_complex *in, *out;
        fftw_plan p;

	struct sigaction sigact, sigign;

	while ((opt = getopt(argc, argv, "a:p:f:g:s:b:n:d:")) != -1) {
		switch (opt) {
		case 'd':
			dev_index = atoi(optarg);
			break;
		case 'f':
			frequency = (uint32_t)atof(optarg);
			break;
		case 'g':
			gain = (int)(atof(optarg) * 10); /* tenths of a dB */
			break;
		case 's':
			samp_rate = (uint32_t)atof(optarg);
			break;
		case 'a':
			addr = optarg;
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 'n':
			num_avgs = atoi(optarg);
			break;
		case 'b':
			fft_size = atoi(optarg);
			break;
		default:
			usage();
			break;
		}
	}

	if (argc < optind)
		usage();

	cwd = get_current_dir_name();

	buffer = malloc(fft_size * sizeof(uint8_t) * 2);
	websocket_buffer = malloc(LWS_SEND_BUFFER_PRE_PADDING + (fft_size * sizeof(float)) + LWS_SEND_BUFFER_POST_PADDING);

	device_count = rtlsdr_get_device_count();
	if (!device_count) {
		fprintf(stderr, "No supported devices found.\n");
		exit(1);
	}

	printf("Found %d device(s).\n", device_count);

	rtlsdr_open(&dev, dev_index);
	if (NULL == dev) {
	fprintf(stderr, "Failed to open rtlsdr device #%d.\n", dev_index);
		exit(1);
	}

	printf("Using %s\n", rtlsdr_get_device_name(dev_index));
	sigact.sa_handler = sighandler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigign.sa_handler = SIG_IGN;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGQUIT, &sigact, NULL);
	sigaction(SIGPIPE, &sigign, NULL);

	/* Set the sample rate */
	r = rtlsdr_set_sample_rate(dev, samp_rate);
	if (r < 0)
		fprintf(stderr, "WARNING: Failed to set sample rate.\n");

	/* Set the frequency */
	r = rtlsdr_set_center_freq(dev, frequency);
	if (r < 0)
		fprintf(stderr, "WARNING: Failed to set center freq.\n");
	else
		fprintf(stderr, "Tuned to %i Hz.\n", frequency);

	if (0 == gain) {
		 /* Enable automatic gain */
		r = rtlsdr_set_tuner_gain_mode(dev, 0);
		if (r < 0)
			fprintf(stderr, "WARNING: Failed to enable automatic gain.\n");
	} else {
		/* Enable manual gain */
		r = rtlsdr_set_tuner_gain_mode(dev, 1);
		if (r < 0)
			fprintf(stderr, "WARNING: Failed to enable manual gain.\n");

		/* Set the tuner gain */
		r = rtlsdr_set_tuner_gain(dev, gain);
		if (r < 0)
			fprintf(stderr, "WARNING: Failed to set tuner gain.\n");
		else
			fprintf(stderr, "Tuner gain set to %f dB.\n", gain/10.0);
	}

	/* Reset endpoint before we start reading from it (mandatory) */
	r = rtlsdr_reset_buffer(dev);
	if (r < 0)
		fprintf(stderr, "WARNING: Failed to reset buffers.\n");
        
	context = libwebsocket_create_context(
                        port,
                        addr,
                        protocols,
                        libwebsocket_internal_extensions,
                        cert_path,
                        key_path,
                        -1,
                        -1,
                        opts,
                        NULL);

	/* Prepare fftw */
	in = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * fft_size);
        out = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * fft_size);
	p = fftw_plan_dft_1d(fft_size, in, out, FFTW_FORWARD, FFTW_MEASURE);

	/* Prepare window */
	win = malloc(sizeof(double) * fft_size);
	window(win, fft_size, "HANNING");

//#define OUTPUT_DEBUG_FILES
#ifdef OUTPUT_DEBUG_FILES
	FILE *winf = fopen("win.sd", "wb");
	if (fwrite(win, sizeof(double), fft_size, winf) != fft_size) {
		fprintf(stderr, "Failed to write window file!\n");
	}

	FILE *raw = fopen("raw.cd", "wb");
	FILE *fft = fopen("fft.cd", "wb");
	FILE *psd = fopen("psd.cd", "wb");
#endif

	fprintf(stderr, "Starting serving\n");
	while (!do_exit) {
		// Read a block of data
		r = rtlsdr_read_sync(dev, &buffer[0], fft_size*2, &n_read);
		if (r < 0) {
			fprintf(stderr, "WARNING: sync read failed.\n");
			break;
		}
		if (n_read != fft_size*2) {
			fprintf(stderr, "WARNING: short read, samples lost, exiting!.\n");
			break;
		}

		// Convert the data to a Complex-Floating point
		// and apply the window
		size_t i=0;
	        for (i=0; i<fft_size; i++) {
			in[i][0] = (((double)buffer[(i*2)+0]) - 127.5) * win[i];
			in[i][1] = (((double)buffer[(i*2)+1]) - 127.5) * win[i];
		}

#ifdef OUTPUT_DEBUG_FILES
		if (fwrite(in, sizeof(fftw_complex), fft_size, raw) != fft_size) {
			fprintf(stderr, "Short raw write, samples lost, exiting!\n");
			break;
		}
#endif
		// Window the data	

		// Execute the FFT
		fftw_execute(p);

#ifdef OUTPUT_DEBUG_FILES
		if (fwrite(out, sizeof(fftw_complex), fft_size, fft) != fft_size) {
			fprintf(stderr, "Short fft write, samples lost, exiting!\n");
			break;
		}
#endif

		// Convert the complex numbers to magnitude, throwing away the imaginary part
		double *mag2 = out;
		for (i=0; i<fft_size; i++) {
			mag2[i] = (mag2[(i*2)+0] * mag2[(i*2)+0]) + (mag2[(i*2)+1] * mag2[(i*2)+1]);
		}


		// average the mag2 with the current output buffer
		float *output = &websocket_buffer[LWS_SEND_BUFFER_PRE_PADDING];
		// negative frequencies first
		int half_fft = fft_size/2;
		for (i=0; i<fft_size/2; i++) {
			output[i] = (output[i] + mag2[i+half_fft]) / 2.0;
		}
		// then positive frequencies
		for (i=0; i<fft_size/2; i++) {
			output[i+half_fft] = (output[i+half_fft] + mag2[i]) / 2.0;
		}

		ctr++;
		if (ctr >= num_avgs) {
			ctr = 0;

#ifdef OUTPUT_DEBUG_FILES
			if (fwrite(&websocket_buffer[LWS_SEND_BUFFER_PRE_PADDING], sizeof(float), fft_size, psd) != fft_size) {
				fprintf(stderr, "Short psd write, samples lost, exiting!\n");
				break;
			}
#endif

			// Send the data out
			libwebsockets_broadcast(
				&protocols[PROTOCOL_PLOT_DATA],
				&websocket_buffer[LWS_SEND_BUFFER_PRE_PADDING],
				sizeof(float)*fft_size);
	
			libwebsocket_service(context, 50);
	
			// reset the buffer
			memset(&websocket_buffer[LWS_SEND_BUFFER_PRE_PADDING], 0, sizeof(float)*fft_size);
#ifdef OUTPUT_DEBUG_FILES
			break;
#endif
		}
	}

out:
#ifdef OUTPUT_DEBUG_FILES
	fclose(raw);
	fclose(fft);
	fclose(psd);
#endif
	free(win);
	fftw_free(in);
	fftw_free(out);
	fftw_destroy_plan(p);
	libwebsocket_context_destroy(context);
	rtlsdr_close(dev);
	free(buffer);
	free(cwd);
	printf("bye!\n");
	return r >= 0 ? r : -r;
}
