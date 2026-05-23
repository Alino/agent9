/*
 * Unit tests for celldiff.c.
 */
#include "../compat.h"
#include "../cells.h"
#include "../celldiff.h"
#include <stdio.h>
#include <string.h>

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name(void); \
                   static void test_##name##_runner(void) { \
                       tests_run++; \
                       printf("  test_%s ... ", #name); \
                       fflush(stdout); \
                       test_##name(); \
                       printf("ok\n"); \
                   } \
                   static void test_##name(void)

#define ASSERT_EQ(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        printf("FAIL\n    %s == %s\n    got %lld, expected %lld\n    %s:%d\n", \
               #a, #b, _a, _b, __FILE__, __LINE__); \
        tests_failed++; \
        return; \
    } \
} while(0)

/* Decoders for assertions */
static int
read_u16(uchar *p)
{
    return p[0] | (p[1] << 8);
}

static uint
read_u32(uchar *p)
{
    return (uint)p[0] | ((uint)p[1] << 8) | ((uint)p[2] << 16) | ((uint)p[3] << 24);
}

TEST(header_layout) {
    Buffer b;
    cellbuf_init(&b, 24, 80);
    /* On a freshly-init'd buffer, all_dirty=1 — first encode reports
     * every cell. */
    int need = CD_HEADER_BYTES + 24*80*CD_CELL_BYTES;
    uchar *out = (uchar*)malloc(need);
    int n = celldiff_encode(&b, out, need);
    ASSERT_EQ(read_u32(out + 0), CD_MAGIC);
    ASSERT_EQ(read_u16(out + 4), CD_VERSION);
    ASSERT_EQ(read_u16(out + 6), 24);
    ASSERT_EQ(read_u16(out + 8), 80);
    ASSERT_EQ(read_u32(out + 10), 24u*80u);
    ASSERT_EQ(read_u16(out + 14), 0);  /* cur_row */
    ASSERT_EQ(read_u16(out + 16), 0);  /* cur_col */
    ASSERT_EQ(out[18], 1);              /* cur_visible */
    ASSERT_EQ(n, need);
    free(out);
    cellbuf_free(&b);
}

TEST(empty_dirty_emits_header_only) {
    Buffer b;
    cellbuf_init(&b, 4, 4);
    uchar out[CD_HEADER_BYTES + 16*CD_CELL_BYTES];
    /* First drain: full screen */
    celldiff_encode(&b, out, sizeof(out));
    /* Second drain: nothing dirty */
    int n = celldiff_encode(&b, out, sizeof(out));
    ASSERT_EQ(n, CD_HEADER_BYTES);
    ASSERT_EQ(read_u32(out + 10), 0u);
    cellbuf_free(&b);
}

TEST(one_cell_diff) {
    Buffer b;
    cellbuf_init(&b, 4, 4);
    uchar buf[256];
    /* drain initial */
    celldiff_encode(&b, buf, sizeof(buf));

    cellbuf_goto(&b, 1, 2);
    cellbuf_set_attrs(&b, 9, 4, 1);
    cellbuf_put(&b, 'X');

    int n = celldiff_encode(&b, buf, sizeof(buf));
    ASSERT_EQ(read_u32(buf + 10), 1u);   /* ncells */
    int row = read_u16(buf + CD_HEADER_BYTES + 0);
    int col = read_u16(buf + CD_HEADER_BYTES + 2);
    uint r = read_u32(buf + CD_HEADER_BYTES + 4);
    int fg = buf[CD_HEADER_BYTES + 8];
    int bg = buf[CD_HEADER_BYTES + 9];
    int attrs = buf[CD_HEADER_BYTES + 10];
    ASSERT_EQ(row, 1);
    ASSERT_EQ(col, 2);
    ASSERT_EQ(r, (uint)'X');
    ASSERT_EQ(fg, 9);
    ASSERT_EQ(bg, 4);
    ASSERT_EQ(attrs, 1);
    ASSERT_EQ(n, CD_HEADER_BYTES + CD_CELL_BYTES);
    cellbuf_free(&b);
}

TEST(small_buffer_partial_emit_preserves_rest) {
    Buffer b;
    cellbuf_init(&b, 2, 4);
    uchar buf[CD_HEADER_BYTES + 3*CD_CELL_BYTES];  /* only fits 3 cells */
    /* First drain reports 8 cells dirty but only 3 fit */
    int n = celldiff_encode(&b, buf, sizeof(buf));
    ASSERT_EQ(read_u32(buf + 10), 3u);
    ASSERT_EQ(n, CD_HEADER_BYTES + 3*CD_CELL_BYTES);
    /* Next drain should report the remaining 5 */
    n = celldiff_encode(&b, buf, sizeof(buf));
    ASSERT_EQ(read_u32(buf + 10), 3u);
    /* Third drain: 2 left */
    n = celldiff_encode(&b, buf, sizeof(buf));
    ASSERT_EQ(read_u32(buf + 10), 2u);
    /* Fourth: empty */
    n = celldiff_encode(&b, buf, sizeof(buf));
    ASSERT_EQ(read_u32(buf + 10), 0u);
    cellbuf_free(&b);
}

TEST(cursor_position_in_header) {
    Buffer b;
    cellbuf_init(&b, 24, 80);
    uchar buf[512];
    /* drain init */
    while (celldiff_encode(&b, buf, sizeof(buf)) > CD_HEADER_BYTES) {}

    cellbuf_goto(&b, 7, 12);
    int n = celldiff_encode(&b, buf, sizeof(buf));
    (void)n;
    ASSERT_EQ(read_u16(buf + 14), 7);
    ASSERT_EQ(read_u16(buf + 16), 12);
    cellbuf_free(&b);
}

TEST(cursor_visible_flag) {
    Buffer b;
    cellbuf_init(&b, 24, 80);
    uchar buf[64];
    /* drain */
    while (celldiff_encode(&b, buf, sizeof(buf)) > CD_HEADER_BYTES) {}
    b.cur_visible = 0;
    celldiff_encode(&b, buf, sizeof(buf));
    ASSERT_EQ(buf[18], 0);
    b.cur_visible = 1;
    celldiff_encode(&b, buf, sizeof(buf));
    ASSERT_EQ(buf[18], 1);
    cellbuf_free(&b);
}

int main(void) {
    printf("=== test_celldiff ===\n");
    test_header_layout_runner();
    test_empty_dirty_emits_header_only_runner();
    test_one_cell_diff_runner();
    test_small_buffer_partial_emit_preserves_rest_runner();
    test_cursor_position_in_header_runner();
    test_cursor_visible_flag_runner();
    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
