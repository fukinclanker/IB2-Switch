#ifndef TEX_TRANSCODE_H
#define TEX_TRANSCODE_H

#include <stddef.h>
#include <stdint.h>

enum { TT_KIND_RGB = 0, TT_KIND_PUNCH = 1, TT_KIND_RGBA = 2 };

/* Block-level codecs (also used by the host-side tests). rgba = 16 texels,
 * row-major (p = y*4 + x), 4 bytes each. */
void tt_decode_etc2_rgb(const uint8_t *src8, uint8_t *rgba, int punchthrough);
void tt_decode_eac_alpha(const uint8_t *src8, uint8_t *rgba);
void tt_encode_bc1(const uint8_t *rgba, uint8_t *out8);
void tt_encode_bc3(const uint8_t *rgba, uint8_t *out16);

/* 1 if src_format is an ETC format this module converts. */
int tt_plan(unsigned src_format, unsigned *dst_format, int *src_block_bytes,
            int *dst_block_bytes, int *kind);
/* Returns how many blocks decoded to opaque near-black. */
unsigned tt_transcode_blocks(const uint8_t *src, uint8_t *dst, size_t nblocks,
                             int kind);

typedef struct {
  uint64_t live_count, live_bytes, peak_bytes, unmanaged_bytes;
  uint64_t transcoded, transcoded_in, transcoded_out, transcode_ms;
  uint64_t passthrough, black_uploads, gl_errors;
  int disabled;
} TexStats;

#ifdef __SWITCH__
#include <GLES2/gl2.h>
void glCompressedTexImage2D_hook(GLenum target, GLint level,
                                 GLenum internalformat, GLsizei width,
                                 GLsizei height, GLint border,
                                 GLsizei image_size, const void *data);
void glTexImage2D_hook(GLenum target, GLint level, GLint internalformat,
                       GLsizei width, GLsizei height, GLint border,
                       GLenum format, GLenum type, const void *pixels);
void glDeleteTextures_hook(GLsizei n, const GLuint *names);
void tt_stats(TexStats *st);
#endif

#endif
