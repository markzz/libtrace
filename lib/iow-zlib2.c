#include <zlib.h>
#include "wandio.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

enum err_t {
	ERR_OK	= 1,
	ERR_EOF = 0,
	ERR_ERROR = -1
};

struct zlibw_t {
	z_stream strm;
	Bytef outbuff[1024*1024];
	int inoffset;
	iow_t *child;
	enum err_t err;
};


extern iow_source_t zlib_wsource; 

#define DATA(iow) ((struct zlibw_t *)((iow)->data))
#define min(a,b) ((a)<(b) ? (a) : (b))

iow_t *zlib_wopen(iow_t *child, int compress_level)
{
	iow_t *iow;
	if (!child)
		return NULL;
	iow = malloc(sizeof(iow_t));
	iow->source = &zlib_wsource;
	iow->data = malloc(sizeof(struct zlibw_t));

	DATA(iow)->child = child;

	DATA(iow)->strm.next_in = NULL;
	DATA(iow)->strm.avail_in = 0;
	DATA(iow)->strm.next_out = DATA(iow)->outbuff;
	DATA(iow)->strm.avail_out = sizeof(DATA(iow)->outbuff);
	DATA(iow)->strm.zalloc = Z_NULL;
	DATA(iow)->strm.zfree = Z_NULL;
	DATA(iow)->strm.opaque = NULL;
	DATA(iow)->err = ERR_OK;

	deflateInit2(&DATA(iow)->strm, 
			compress_level,	/* Level */
			Z_DEFLATED, 	/* Method */
			15 | 16, 	/* 15 bits of windowsize, 16 == use gzip header */
			9,		/* Use maximum (fastest) amount of memory usage */
			Z_DEFAULT_STRATEGY
		);

	return iow;
}


static off_t zlib_wwrite(iow_t *iow, const char *buffer, off_t len)
{
	if (DATA(iow)->err == ERR_EOF) {
		return 0; /* EOF */
	}
	if (DATA(iow)->err == ERR_ERROR) {
		return -1; /* ERROR! */
	}

	DATA(iow)->strm.next_in = (Bytef*)buffer; /* This casts away const, but it's really const 
						   * anyway 
						   */
	DATA(iow)->strm.avail_in = len;

	while (DATA(iow)->err == ERR_OK && DATA(iow)->strm.avail_in > 0) {
		while (DATA(iow)->strm.avail_out <= 0) {
			int bytes_written = wandio_wwrite(DATA(iow)->child, 
				(char *)DATA(iow)->outbuff,
				sizeof(DATA(iow)->outbuff));
			if (bytes_written <= 0) { /* Error */
				DATA(iow)->err = ERR_ERROR;
				/* Return how much data we managed to write ok */
				if (DATA(iow)->strm.avail_in != len) {
					return len-DATA(iow)->strm.avail_in;
				}
				/* Now return error */
				return -1;
			}
			DATA(iow)->strm.next_out = DATA(iow)->outbuff;
			DATA(iow)->strm.avail_out = sizeof(DATA(iow)->outbuff);
		}
		/* Decompress some data into the output buffer */
		int err=deflate(&DATA(iow)->strm, 0);
		switch(err) {
			case Z_OK:
				DATA(iow)->err = ERR_OK;
				break;
			default:
				DATA(iow)->err = ERR_ERROR;
		}
	}
	/* Return the number of bytes decompressed */
	return len-DATA(iow)->strm.avail_in;
}

static void zlib_wclose(iow_t *iow)
{
	while (deflate(&DATA(iow)->strm, Z_FINISH) == Z_OK) {
		/* Need to flush the output buffer */
		wandio_wwrite(DATA(iow)->child, 
				(char*)DATA(iow)->outbuff,
				sizeof(DATA(iow)->outbuff)-DATA(iow)->strm.avail_out);
		DATA(iow)->strm.next_out = DATA(iow)->outbuff;
		DATA(iow)->strm.avail_out = sizeof(DATA(iow)->outbuff);
	}
	deflateEnd(&DATA(iow)->strm);
	wandio_wwrite(DATA(iow)->child, 
			(char *)DATA(iow)->outbuff,
			sizeof(DATA(iow)->outbuff)-DATA(iow)->strm.avail_out);
	wandio_wdestroy(DATA(iow)->child);
	free(iow->data);
	free(iow);
}

iow_source_t zlib_wsource = {
	"zlibw",
	zlib_wwrite,
	zlib_wclose
};
