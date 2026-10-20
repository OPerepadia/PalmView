/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Link-time replacements for FFmpeg features YouTube's 144p streams never
 * use: H.274 film grain, Dolby Vision metadata and EXIF. Defining these here
 * keeps h274.o (692 KB of tables), dovi_rpudec.o and exif.o out of the
 * module. Each reports "not supported" so FFmpeg skips the feature. */

#include "libavcodec/avcodec.h"
#include "libavcodec/dovi_rpu.h"
#include "libavcodec/exif.h"
#include "libavcodec/h274.h"
#include "libavutil/error.h"
#include "libavutil/mem.h"

int ff_h274_apply_film_grain(AVFrame *out, const AVFrame *in,
                             const AVFilmGrainParams *params)
{
    (void)out; (void)in; (void)params;
    return AVERROR(ENOSYS);
}

int ff_dovi_rpu_parse(DOVIContext *s, const uint8_t *rpu, size_t rpu_size,
                      int err_recognition)
{
    (void)s; (void)rpu; (void)rpu_size; (void)err_recognition;
    return AVERROR(ENOSYS);
}

int ff_dovi_get_metadata(DOVIContext *s, AVDOVIMetadata **out_metadata)
{
    (void)s;
    *out_metadata = 0;
    return AVERROR(ENOSYS);
}

/* EXIF: parsing always fails, so nothing is ever attached to frames. */

int32_t av_exif_get_tag_id(const char *name)
{
    (void)name;
    return -1;
}

int av_exif_get_entry(void *logctx, AVExifMetadata *ifd, uint16_t id,
                      int flags, AVExifEntry **value)
{
    (void)logctx; (void)ifd; (void)id; (void)flags;
    *value = 0;
    return 0;
}

int av_exif_remove_entry(void *logctx, AVExifMetadata *ifd, uint16_t id,
                         int flags)
{
    (void)logctx; (void)ifd; (void)id; (void)flags;
    return 0;
}

int av_exif_parse_buffer(void *logctx, const uint8_t *data, size_t size,
                         AVExifMetadata *ifd, enum AVExifHeaderMode header_mode)
{
    (void)logctx; (void)data; (void)size; (void)ifd; (void)header_mode;
    return AVERROR(ENOSYS);
}

int av_exif_write(void *logctx, const AVExifMetadata *ifd,
                  AVBufferRef **buffer, enum AVExifHeaderMode header_mode)
{
    (void)logctx; (void)ifd; (void)header_mode;
    *buffer = 0;
    return AVERROR(ENOSYS);
}

void av_exif_free(AVExifMetadata *ifd)
{
    if (ifd == 0) return;
    av_freep(&ifd->entries);
    ifd->count = 0;
    ifd->size = 0;
}

int av_exif_ifd_to_dict(void *logctx, const AVExifMetadata *ifd,
                        AVDictionary **metadata)
{
    (void)logctx; (void)ifd; (void)metadata;
    return 0;
}

AVExifMetadata *av_exif_clone_ifd(const AVExifMetadata *ifd)
{
    (void)ifd;
    return 0;
}

int av_exif_orientation_to_matrix(int32_t *matrix, int orientation)
{
    (void)matrix; (void)orientation;
    return AVERROR(ENOSYS);
}
