/*
 * vts cell-diff wire format encoder.
 *
 * Defines the binary layout that vts emits on /<session>/cells reads.
 * Same encoder is used by both vts (writes diffs) and any client
 * (mxio renderer, vts-attach) reading the stream.
 *
 * Wire format (little-endian):
 *
 *   u32  magic = 0x76746331 ('vtc1')
 *   u16  version = 1
 *   u16  rows
 *   u16  cols
 *   u32  ncells
 *   u16  cursor_row
 *   u16  cursor_col
 *   u8   cursor_visible
 *   u8   reserved[3]
 *
 *   for each cell:
 *     u16  row
 *     u16  col
 *     u32  rune (UTF-32 code point)
 *     u8   fg
 *     u8   bg
 *     u8   attrs
 *     u8   reserved
 *
 * Header size: 4+2+2+2+4+2+2+1+3 = 22 bytes
 * Per-cell:    2+2+4+1+1+1+1     = 12 bytes
 */
#ifndef VTS_CELLDIFF_H
#define VTS_CELLDIFF_H

#include "compat.h"
#include "cells.h"

enum {
	CD_MAGIC = 0x76746331,
	CD_VERSION = 1,
	CD_HEADER_BYTES = 22,
	CD_CELL_BYTES = 12,
};

/*
 * Encode a buffer's dirty cells into `out`. Returns the number of bytes
 * written. The buffer's dirty bitmap is cleared as a side effect.
 *
 * If `out` is too small, only the header + as many cells as fit are
 * encoded; the rest of the dirty bits are PRESERVED so a subsequent
 * call returns the remaining cells.
 *
 * If there are no dirty cells, only the header is emitted (ncells=0).
 */
int celldiff_encode(Buffer *b, uchar *out, int max);

#endif
