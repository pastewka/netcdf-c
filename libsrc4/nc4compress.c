#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <bzlib.h>
#include "netcdf.h"
#include "hdf5.h"
#include "nc4compress.h"

#ifdef BZIP2_COMPRESSION

/*forward*/
/* declare a filter function */
static size_t H5Z_filter_bzip2(unsigned flags,size_t cd_nelmts,const unsigned cd_values[],
                    size_t nbytes,size_t *buf_size,void**buf);

static const H5Z_class2_t H5Z_BZIP2[1] = {{
    H5Z_CLASS_T_VERS,       /* H5Z_class_t version */
    (H5Z_filter_t)H5Z_FILTER_BZIP2,         /* Filter id number             */
    1,              /* encoder_present flag (set to true) */
    1,              /* decoder_present flag (set to true) */
    "bzip2",                  /* Filter name for debugging    */
    NULL,                       /* The "can apply" callback     */
    NULL,                       /* The "set local" callback     */
    (H5Z_func_t)H5Z_filter_bzip2,         /* The actual filter function   */
}};

#endif

/*
Turn on bzip for a variable's plist
*/
int
nccompress_set(int algorithm, hid_t plistid, const nc_compression_t* parms)
{
    int status;
    unsigned int cd_values[1];
    switch (algorithm) {
    case NC_COMPRESS_DEFLATE:
        cd_values[0] = parms->level;
        status = H5Pset_deflate(plistid, parms->level);
	break;
    case NC_COMPRESS_BZIP2:
#ifdef BZIP2_COMPRESSION
        cd_values[0] = parms->level;
        status = H5Pset_filter(plistid, (H5Z_filter_t)H5Z_FILTER_BZIP2, H5Z_FLAG_MANDATORY, (size_t)1, cd_values);
#else
        status = NC_EHDFERR;
#endif
	break;
    case NC_COMPRESS_SZIP:
	status = NC_EHDFERR;
	break;
    default:
	status = NC_EHDFERR;
	break;
    }
    return status;
}

/* 
Register filter with the library
*/
int
nccompress_register(int algorithm, const nc_compression_t* parms)
{
    herr_t status;
    htri_t avail;
    unsigned int filter_info;

    switch (algorithm) {
    case NC_COMPRESS_DEFLATE: /* already registered */
	break;
    case NC_COMPRESS_BZIP2:
#ifdef BZIP2_COMPRESSION
        status = H5Zregister(H5Z_BZIP2);
#else
	status = NC_EHDFERR;
#endif
	break;
    case NC_COMPRESS_SZIP:
	status = NC_EHDFERR;
	break;
    default:
	status = NC_EHDFERR;
	break;
    }
    if(status < 0) {
	return status;
    }

    /*
     * Check if compression is available and can be used for both
     * compression and decompression.  Normally we do not perform error
     * checking in these examples for the sake of clarity, but in this
     * case we will make an exception because this filter is an
     * optional part of the hdf5 library.
     */
    avail = H5Zfilter_avail((H5Z_filter_t)algorithm);
    if(!avail) {
        fprintf(stderr,"Filter not available: %d.\n",algorithm);
        return 1;
    }
    status = H5Zget_filter_info((H5Z_filter_t)algorithm, &filter_info);
    if(!(filter_info & H5Z_FILTER_CONFIG_ENCODE_ENABLED) ||
	!(filter_info & H5Z_FILTER_CONFIG_DECODE_ENABLED) ) {
        fprintf(stderr,"Filter not available for encoding and decoding: %d.\n",algorithm);
        return 1;
    }
    return 0;
}

#ifdef BZIP2_COMPRESSION
static size_t
H5Z_filter_bzip2(unsigned int flags, size_t cd_nelmts,
                     const unsigned int cd_values[], size_t nbytes,
                     size_t *buf_size, void **buf)
{
  char *outbuf = NULL;
  size_t outbuflen, outdatalen;
  int ret;

  if(flags & H5Z_FLAG_REVERSE) {

    /** Decompress data.
     **
     ** This process is troublesome since the size of uncompressed data
     ** is unknown, so the low-level interface must be used.
     ** Data is decompressed to the output buffer (which is sized
     ** for the average case); if it gets full, its size is doubled
     ** and decompression continues.  This avoids repeatedly trying to
     ** decompress the whole block, which could be really inefficient.
     **/

    bz_stream stream;
    char *newbuf = NULL;
    size_t newbuflen;

    /* Prepare the output buffer. */
    outbuflen = nbytes * 3 + 1;  /* average bzip2 compression ratio is 3:1 */
    outbuf = malloc(outbuflen);
    if(outbuf == NULL) {
      fprintf(stderr, "memory allocation failed for bzip2 decompression\n");
      goto cleanupAndFail;
    }

    /* Use standard malloc()/free() for internal memory handling. */
    stream.bzalloc = NULL;
    stream.bzfree = NULL;
    stream.opaque = NULL;

    /* Start decompression. */
    ret = BZ2_bzDecompressInit(&stream, 0, 0);
    if(ret != BZ_OK) {
      fprintf(stderr, "bzip2 decompression start failed with error %d\n", ret);
      goto cleanupAndFail;
    }

    /* Feed data to the decompression process and get decompressed data. */
    stream.next_out = outbuf;
    stream.avail_out = outbuflen;
    stream.next_in = *buf;
    stream.avail_in = nbytes;
    do {
      ret = BZ2_bzDecompress(&stream);
      if(ret < 0) {
	fprintf(stderr, "BUG: bzip2 decompression failed with error %d\n", ret);
	goto cleanupAndFail;
      }

      if(ret != BZ_STREAM_END && stream.avail_out == 0) {
        /* Grow the output buffer. */
        newbuflen = outbuflen * 2;
        newbuf = realloc(outbuf, newbuflen);
        if(newbuf == NULL) {
          fprintf(stderr, "memory allocation failed for bzip2 decompression\n");
          goto cleanupAndFail;
        }
        stream.next_out = newbuf + outbuflen;  /* half the new buffer behind */
        stream.avail_out = outbuflen;  /* half the new buffer ahead */
        outbuf = newbuf;
        outbuflen = newbuflen;
      }
    } while (ret != BZ_STREAM_END);

    /* End compression. */
    outdatalen = stream.total_out_lo32;
    ret = BZ2_bzDecompressEnd(&stream);
    if(ret != BZ_OK) {
      fprintf(stderr, "bzip2 compression end failed with error %d\n", ret);
      goto cleanupAndFail;
    }

  } else {

    /** Compress data.
     **
     ** This is quite simple, since the size of compressed data in the worst
     ** case is known and it is not much bigger than the size of uncompressed
     ** data.  This allows us to use the simplified one-shot interface to
     ** compression.
     **/

    unsigned int odatalen;  /* maybe not the same size as outdatalen */
    int blockSize100k = 9;

    /* Get compression block size if present. */
    if(cd_nelmts > 0) {
      blockSize100k = cd_values[0];
      if(blockSize100k < 1 || blockSize100k > 9) {
	fprintf(stderr, "invalid compression block size: %d\n", blockSize100k);
	goto cleanupAndFail;
      }
    }

    /* Prepare the output buffer. */
    outbuflen = nbytes + nbytes / 100 + 600;  /* worst case (bzip2 docs) */
    outbuf = malloc(outbuflen);
    if(outbuf == NULL) {
      fprintf(stderr, "memory allocation failed for bzip2 compression\n");
      goto cleanupAndFail;
    }

    /* Compress data. */
    odatalen = outbuflen;
    ret = BZ2_bzBuffToBuffCompress(outbuf, &odatalen, *buf, nbytes,
                                   blockSize100k, 0, 0);
    outdatalen = odatalen;
    if(ret != BZ_OK) {
      fprintf(stderr, "bzip2 compression failed with error %d\n", ret);
      goto cleanupAndFail;
    }
  }

  /* Always replace the input buffer with the output buffer. */
  free(*buf);
  *buf = outbuf;
  *buf_size = outbuflen;
  return outdatalen;

 cleanupAndFail:
  if(outbuf)
    free(outbuf);
  return 0;
}
#endif /*BZIP2_COMPRESSION*/
