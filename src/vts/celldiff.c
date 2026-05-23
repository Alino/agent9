/*
 * vts cell-diff wire format encoder. See celldiff.h.
 */
#include "compat.h"
#include "celldiff.h"

static void
put_u16(uchar *p, int v)
{
	p[0] = v & 0xFF;
	p[1] = (v >> 8) & 0xFF;
}

static void
put_u32(uchar *p, uint v)
{
	p[0] = v & 0xFF;
	p[1] = (v >> 8) & 0xFF;
	p[2] = (v >> 16) & 0xFF;
	p[3] = (v >> 24) & 0xFF;
}

int
celldiff_encode(Buffer *b, uchar *out, int max)
{
	int max_cells, n_emitted, i, idx;
	uchar *cellp;
	Cell c;
	uint ncells_field;
	int all_dirty_at_start = b->all_dirty;

	if(max < CD_HEADER_BYTES)
		return 0;

	max_cells = (max - CD_HEADER_BYTES) / CD_CELL_BYTES;

	n_emitted = 0;
	cellp = out + CD_HEADER_BYTES;

	/* If all_dirty, iterate all cells and emit up to max_cells. */
	if(all_dirty_at_start){
		for(i = 0; i < b->ncells && n_emitted < max_cells; i++){
			c = b->cells[i];
			put_u16(cellp + 0, i / b->cols);
			put_u16(cellp + 2, i % b->cols);
			put_u32(cellp + 4, c.rune);
			cellp[8] = c.fg;
			cellp[9] = c.bg;
			cellp[10] = c.attrs;
			cellp[11] = 0;
			cellp += CD_CELL_BYTES;
			n_emitted++;
		}
		/* Did we emit them all? */
		if(n_emitted == b->ncells){
			b->all_dirty = 0;
		} else {
			/* Mark the un-emitted ones individually dirty. */
			for(i = n_emitted; i < b->ncells; i++)
				b->dirty[i >> 3] |= (1 << (i & 7));
			b->all_dirty = 0;
		}
	} else {
		/* Walk the dirty bitmap. */
		for(i = 0; i < b->ncells && n_emitted < max_cells; i++){
			if(!(b->dirty[i >> 3] & (1 << (i & 7))))
				continue;
			c = b->cells[i];
			put_u16(cellp + 0, i / b->cols);
			put_u16(cellp + 2, i % b->cols);
			put_u32(cellp + 4, c.rune);
			cellp[8] = c.fg;
			cellp[9] = c.bg;
			cellp[10] = c.attrs;
			cellp[11] = 0;
			cellp += CD_CELL_BYTES;
			n_emitted++;
			/* Clear this bit so it isn't re-emitted next call. */
			b->dirty[i >> 3] &= ~(1 << (i & 7));
		}
	}

	/* Fill header */
	ncells_field = (uint)n_emitted;
	put_u32(out + 0, CD_MAGIC);
	put_u16(out + 4, CD_VERSION);
	put_u16(out + 6, b->rows);
	put_u16(out + 8, b->cols);
	put_u32(out + 10, ncells_field);
	put_u16(out + 14, b->cur_row);
	put_u16(out + 16, b->cur_col);
	out[18] = b->cur_visible;
	out[19] = 0;
	out[20] = 0;
	out[21] = 0;

	idx = CD_HEADER_BYTES + n_emitted * CD_CELL_BYTES;
	return idx;
}
