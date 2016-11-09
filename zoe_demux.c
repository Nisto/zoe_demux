#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>

uint32_t get_u32_be(uint8_t *buf)
{
    return (buf[0]<<24) | (buf[1]<<16) | (buf[2]<<8) | buf[3];
}

uint16_t get_u16_be(uint8_t *buf)
{
    return (buf[0]<<8) | buf[1];
}

void put_u32_le(uint8_t *buf, uint32_t n)
{
    buf[0] = (uint8_t)(n & 0xFF);
    buf[1] = (uint8_t)((n >> 8) & 0xFF);
    buf[2] = (uint8_t)((n >> 16) & 0xFF);
    buf[3] = (uint8_t)((n >> 24) & 0xFF);
}

void efopen(FILE **stream, const char *filename, const char *mode)
{
    *stream = fopen(filename, mode);
    if (*stream == NULL) {
        printf(
            "%s: could not open file for %s (%s)\n",
            filename, mode[0]=='r'?"reading":"writing", strerror(errno)
        );
        exit(EXIT_FAILURE);
    }
}

void efseek(FILE *stream, long int offset, int whence, char *name)
{
    if (0 != fseek(stream, offset, whence)) {
        printf(
            "%s: 0x%08lX: seek error (%ld, mode=%d) (%s)\n",
            name, ftell(stream), offset, whence, strerror(errno)
        );
        exit(EXIT_FAILURE);
    }
}

void efread(void *ptr, size_t bytes, FILE *stream, char *name)
{
    if (bytes != fread(ptr, 1, bytes, stream)) {
        printf(
            "%s: 0x%08lX: read error ",
            name, ftell(stream)
        );
        if (feof(stream)) {
            printf("(unexpected end of file) ");
        }
        printf("(%s)\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
}

void efwrite(const void *ptr, size_t bytes, FILE *stream, char *name)
{
    if (bytes != fwrite(ptr, 1, bytes, stream)) {
        printf(
            "%s: 0x%08lX: write error: %s\n",
            name, ftell(stream), strerror(errno)
        );
        exit(EXIT_FAILURE);
    }
}

char *subext(char *path, const char *rep)
{
    //
    // substitutes extension of path with another string
    //
    int i, s=0;
    char *out;

    // scan from start up to basename
    for (i=0; path[i]; ++i)
    {
        if (path[i] == '/' || path[i] == '\\') {
            s = i + 1;
        }
    }

    // scan from basename up to extension
    for (i=s; path[i]; ++i)
    {
        if (path[i] == '.') {
            s = i;
        }
    }

    // no extension; get full path
    if (path[s] != '.') {
        s = i;
    }

    out = malloc(s + strlen(rep) + 1);

    if (out == NULL) {
        printf(
            "could not allocate memory for subext()\n"
            "arguments:\n0: %s\n1: %s\n", path, rep
        );
        exit(EXIT_FAILURE);
    }

    memcpy(out, path, s);

    strcpy(out + s, rep);

    return out;
}

const char *stream_names[] = {
    ".genh",
    ".m2a",
    ".m2v",
    ".bin",
    "_subs_en.bin",
    "_subs_fr.bin",
    "_subs_de.bin",
    "_subs_it.bin"
};

enum {
    STREAM_ADPCM   = 0,
    STREAM_AUDIO   = 1,
    STREAM_VIDEO   = 2,
    STREAM_BIN     = 3,
    STREAM_SUBS_EN = 4,
    STREAM_SUBS_FR = 5,
    STREAM_SUBS_DE = 6,
    STREAM_SUBS_IT = 7
};

typedef struct {
    FILE *f;
    char *path;
} stream_t;

#define STREAMS_MAX ((sizeof stream_names) / (sizeof *stream_names))

#define ZOE_SSID_ADPCM   0x01
#define ZOE_SSID_BIN     0x05
#define ZOE_SSID_SUBS_EN 0x07
#define ZOE_SSID_SUBS_FR 0x08
#define ZOE_SSID_SUBS_DE 0x09
#define ZOE_SSID_SUBS_IT 0x0A

void patch_zoe_adpcm(uint8_t *stop, uint8_t *ptr)
{
    const uint8_t end[14] = {
        0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
        0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77
    };

    // scan backwards
    for (ptr = ptr - 16; ptr >= stop; ptr -= 16) {
        // valid predictor?
        if (ptr[0] >> 4 <= 4) {
            // valid flags?
            if (ptr[1] <= 7) {
                // terminating block?
                if (memcmp(&ptr[2], end, 14) == 0) {
                    ptr[1] = 0x07;
                    return;
                }
            }
        }
    }
}

void finalize_zoe_genh(uint8_t *streambuf, stream_t *stream)
{
    long int genh_size;
    //uint32_t adpcm_size;
    uint16_t sample_rate;
    uint8_t  channels;

    genh_size = ftell(stream->f);

    if (genh_size % 16 == 0 && genh_size >= 0x1800) {
        efseek(stream->f, 0x1000, SEEK_SET, stream->path);
        efread(streambuf, 16, stream->f, stream->path);
        //adpcm_size = get_u32_be(streambuf + 0);
        sample_rate = get_u16_be(streambuf + 6);
        channels = streambuf[8];

        // 1ch min size: 4096 (GENH) + 2048 (ZOE) + 16 (at least one block)
        // 2ch min size: 4096 (GENH) + 2048 (ZOE) + (2 * 2048 (interleave))
        if ((channels == 1 && genh_size >= 0x1810) || (channels == 2 && genh_size >= 0x2800)) {
            //
            // the coding in ZOE has some extra blocks at the end which causes
            // players to pop, so we'll have to fix them up...
            //
            if (channels == 1 && genh_size < 0x2000) {
                //
                // 1 channel only (NOT required to be >=2048); check what's available
                //
                efseek(stream->f, 0x1800, SEEK_SET, stream->path);
                efread(streambuf, genh_size - 0x1800, stream->f, stream->path);
                patch_zoe_adpcm(&streambuf[0], &streambuf[genh_size - 0x1800]);
                efseek(stream->f, 0x1800, SEEK_SET, stream->path);
                efwrite(streambuf, genh_size - 0x1800, stream->f, stream->path);
            } else {
                //
                // 1 or 2 channels; check last 2048 bytes
                //
                efseek(stream->f, -(0x800 * channels), SEEK_END, stream->path);
                efread(streambuf, 0x800 * channels, stream->f, stream->path);
                patch_zoe_adpcm(&streambuf[0], &streambuf[0x800]);
                if (channels == 2) {
                    patch_zoe_adpcm(&streambuf[0x800], &streambuf[0x1000]);
                }
                efseek(stream->f, -(0x800 * channels), SEEK_END, stream->path);
                efwrite(streambuf, 0x800 * channels, stream->f, stream->path);
            }

            // write the header
            memset(streambuf, 0, 0x1000);
            memcpy(streambuf+0x00, "GENH", 4);                                     // 0x00 magic
            put_u32_le(streambuf+0x04, channels);                                  // 0x04 channels
            put_u32_le(streambuf+0x08, channels == 2 ? 0x800 : 0);                 // 0x08 interleave
            put_u32_le(streambuf+0x0C, sample_rate);                               // 0x0C sample rate
            put_u32_le(streambuf+0x10, 0xFFFFFFFF);                                // 0x10 loop start, samples (-1 == none)
            put_u32_le(streambuf+0x14, (genh_size - 0x1800) / 16 * 28 / channels); // 0x14 loop end, samples
            put_u32_le(streambuf+0x18, 0);                                         // 0x18 coding (PSX)
            put_u32_le(streambuf+0x1C, 0x1800);                                    // 0x1C audio start offset
            put_u32_le(streambuf+0x20, 0x1000);                                    // 0x20 GENH header size
            efseek(stream->f, 0, SEEK_SET, stream->path);
            efwrite(streambuf, 0x1000, stream->f, stream->path);
            return;
        }

        if (channels != 1 && channels != 2) {
            printf(
                "%s: invalid/unsupported channel count: %" PRIu8 "\n",
                stream->path, channels
            );
            exit(EXIT_FAILURE);
        }
    }

    printf("%s: malformed or no ADPCM data\n", stream->path);
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
    FILE     *pss                 = NULL;
    char     *pss_path            = NULL;
    long int pss_size             = 0;
    stream_t streams[STREAMS_MAX] = {0};
    uint8_t  stream               = 0;
    uint16_t streambufsize        = 4096;
    uint8_t  *streambuf           = NULL;
    uint32_t id                   = 0;
    uint8_t  ssid                 = 0;
    uint16_t packet_size          = 0;
    uint16_t payload_offset       = 0;

    if (argc != 2) {
        printf("usage: %s <pss>\n", argv[0]);
        return 1;
    }

    pss_path = argv[1];

    efopen(&pss, pss_path, "rb");

    efseek(pss, 0, SEEK_END, pss_path);

    pss_size = ftell(pss);

    if (pss_size == EOF) {
        printf("%s: cannot determine file size (too large?)\n", pss_path);
        exit(EXIT_FAILURE);
    }

    efseek(pss, 0, SEEK_SET, pss_path);

    streambuf = malloc(streambufsize);

    if (streambuf == NULL) {
        printf("%s: could not allocate stream buffer\n", pss_path);
        exit(EXIT_FAILURE);
    }

    while (ftell(pss) < pss_size) {
        efread(streambuf, 4, pss, pss_path);

        id = get_u32_be(streambuf);

        if (id >> 8 != 1) {
            printf(
                "%s: 0x%08lX: invalid MPEG start code: %06" PRIX32 "\n",
                pss_path, ftell(pss) - 4, id >> 8
            );
            exit(EXIT_FAILURE);
        }

        id &= 0xFF;

        if (id == 0xB9) {
            //
            // Program End
            //
            break;
        } else if (id == 0xBA) {
            //
            // Pack Header - skip
            //
            efread(streambuf, 1, pss, pss_path);
            if ((*streambuf & 0xF1) == 0x21) {
                // MPEG-1
                efseek(pss, 7L, SEEK_CUR, pss_path);
            } else {
                // MPEG-2
                efread(streambuf, 9, pss, pss_path);
                // stuffing bytes
                if (streambuf[8] & 7) {
                    efseek(pss, (long int)(streambuf[8] & 7), SEEK_CUR, pss_path);
                }
            }
        } else if (id == 0xBB) {
            //
            // Program Stream System Header - skip
            //
            efread(streambuf, 2, pss, pss_path);
            efseek(pss, (long int)get_u16_be(streambuf), SEEK_CUR, pss_path);
        } else if (id >= 0xBD && id <= 0xEF) {
            //
            // Packetized Elementary Streams (PES)
            //
            efread(streambuf, 2, pss, pss_path);
            packet_size = get_u16_be(streambuf);
            if (id == 0xBE) {
                //
                // Padding Stream - skip
                //
                efseek(pss, (long int)packet_size, SEEK_CUR, pss_path);
            } else {
                //
                // Audio/Video/Private streams
                //
                if (packet_size > streambufsize) {
                    streambuf = realloc(streambuf, packet_size);
                    if (streambuf == NULL) {
                        printf(
                            "%s: 0x%08lX: failed expanding stream buffer to %" PRIu16 " bytes\n",
                            pss_path, ftell(pss), packet_size
                        );
                        exit(EXIT_FAILURE);
                    }
                    streambufsize = packet_size;
                }

                efread(streambuf, packet_size, pss, pss_path);

                if (id == 0xBD || id == 0xBF) {
                    //
                    // Private streams
                    //

                    if (packet_size < 0x11) {
                        printf(
                            "%s: 0x%08lX: invalid private header/packet\n",
                            pss_path, ftell(pss) - packet_size
                        );
                        exit(EXIT_FAILURE);
                    }

                    // header size is fixed for PSS I guess
                    payload_offset = 0x11;

                    // first byte of payload is the substream ID
                    ssid = streambuf[0x10];

                    if (ssid == ZOE_SSID_ADPCM) {
                        stream = STREAM_ADPCM;
                    } else if (ssid == ZOE_SSID_BIN) {
                        stream = STREAM_BIN;
                    } else if (ssid == ZOE_SSID_SUBS_EN) {
                        stream = STREAM_SUBS_EN;
                    } else if (ssid == ZOE_SSID_SUBS_FR) {
                        stream = STREAM_SUBS_FR;
                    } else if (ssid == ZOE_SSID_SUBS_DE) {
                        stream = STREAM_SUBS_DE;
                    } else if (ssid == ZOE_SSID_SUBS_IT) {
                        stream = STREAM_SUBS_IT;
                    } else {
                        printf(
                            "%s: 0x%08lX: unexpected substream ID: %02" PRIX8 "\n",
                            pss_path, ftell(pss) - packet_size + 0x10, ssid
                        );
                        exit(EXIT_FAILURE);
                    }
                } else if (id >= 0xC0 && id <= 0xDF) {
                    //
                    // MPEG-1 / MPEG-2 audio streams
                    //
                    stream = STREAM_AUDIO;
                } else if (id >= 0xE0 && id <= 0xEF) {
                    //
                    // MPEG-1 / MPEG-2 video streams
                    //
                    stream = STREAM_VIDEO;
                }

                if (stream == STREAM_AUDIO || stream == STREAM_VIDEO) {
                    if (packet_size < 3) {
                        printf(
                            "%s: 0x%08lX: invalid PES header\n",
                            pss_path, ftell(pss) - packet_size
                        );
                        exit(EXIT_FAILURE);
                    }

                    payload_offset = 0x03 + streambuf[0x02];
                }

                if (streams[stream].f == NULL) {
                    streams[stream].path = subext(pss_path, stream_names[stream]);
                    efopen(&streams[stream].f, streams[stream].path, "w+b");
                    if (stream == STREAM_ADPCM) {
                        // reserve GENH space
                        efwrite(streambuf, 4096, streams[stream].f, streams[stream].path);
                    }
                }

                efwrite(streambuf + payload_offset, packet_size - payload_offset, streams[stream].f, streams[stream].path);
            }
        } else {
            printf(
                "%s: 0x%08lX: unexpected MPEG ID: %02" PRIX32 "\n",
                pss_path, ftell(pss) - 1, id
            );
            exit(EXIT_FAILURE);
        }
    }

    if (streams[STREAM_ADPCM].f) {
        finalize_zoe_genh(streambuf, &streams[STREAM_ADPCM]);
    }

    // clean up
    if (0 != fclose(pss)) {
        printf("%s: could not close PSS file\n", pss_path);
        exit(EXIT_FAILURE);
    }
    free(streambuf);
    for (stream=0; stream<STREAMS_MAX; ++stream) {
        if (streams[stream].f) {
            if (0 != fclose(streams[stream].f)) {
                printf(
                    "%s: could not close output file: %s\n",
                    pss_path, streams[stream].path
                );
                exit(EXIT_FAILURE);
            }
            free(streams[stream].path);
        }
    }

    return 0;
}
