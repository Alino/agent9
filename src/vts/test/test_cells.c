/*
 * Unit tests for cells.c — compiled and run on the host (macOS/Linux), not
 * on Plan 9. Pure C. No frameworks.
 *
 * Run via test/run_tests.sh.
 */
#include "../compat.h"
#include "../cells.h"
#include <assert.h>
#include <stdio.h>

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

TEST(init_default_palette) {
    Buffer b;
    cellbuf_init(&b, 24, 80);
    ASSERT_EQ(b.rows, 24);
    ASSERT_EQ(b.cols, 80);
    ASSERT_EQ(b.ncells, 24*80);
    ASSERT_EQ(b.cur_row, 0);
    ASSERT_EQ(b.cur_col, 0);
    /* every cell should be ' ' with default fg/bg */
    Cell c = cellbuf_get(&b, 0, 0);
    ASSERT_EQ(c.rune, ' ');
    ASSERT_EQ(c.fg, 7);
    ASSERT_EQ(c.bg, 0);
    cellbuf_free(&b);
}

TEST(put_single_char) {
    Buffer b;
    cellbuf_init(&b, 24, 80);
    cellbuf_put(&b, 'X');
    Cell c = cellbuf_get(&b, 0, 0);
    ASSERT_EQ(c.rune, 'X');
    cellbuf_free(&b);
}

TEST(put_then_advance) {
    Buffer b;
    cellbuf_init(&b, 24, 80);
    cellbuf_put(&b, 'A');
    cellbuf_advance(&b);
    cellbuf_put(&b, 'B');
    cellbuf_advance(&b);
    cellbuf_put(&b, 'C');
    cellbuf_advance(&b);
    ASSERT_EQ(cellbuf_get(&b, 0, 0).rune, 'A');
    ASSERT_EQ(cellbuf_get(&b, 0, 1).rune, 'B');
    ASSERT_EQ(cellbuf_get(&b, 0, 2).rune, 'C');
    ASSERT_EQ(b.cur_row, 0);
    ASSERT_EQ(b.cur_col, 3);
    cellbuf_free(&b);
}

TEST(carriage_return) {
    Buffer b;
    cellbuf_init(&b, 24, 80);
    cellbuf_put(&b, 'A');
    cellbuf_advance(&b);
    cellbuf_put(&b, 'B');
    cellbuf_advance(&b);
    cellbuf_carriage_return(&b);
    ASSERT_EQ(b.cur_col, 0);
    cellbuf_put(&b, 'X');
    ASSERT_EQ(cellbuf_get(&b, 0, 0).rune, 'X');  /* overwrote 'A' */
    ASSERT_EQ(cellbuf_get(&b, 0, 1).rune, 'B');
    cellbuf_free(&b);
}

TEST(newline_then_cr) {
    Buffer b;
    cellbuf_init(&b, 24, 80);
    cellbuf_put(&b, 'A');
    cellbuf_advance(&b);
    cellbuf_newline(&b);
    cellbuf_carriage_return(&b);
    cellbuf_put(&b, 'B');
    ASSERT_EQ(cellbuf_get(&b, 0, 0).rune, 'A');
    ASSERT_EQ(cellbuf_get(&b, 1, 0).rune, 'B');
    cellbuf_free(&b);
}

TEST(backspace) {
    Buffer b;
    cellbuf_init(&b, 24, 80);
    cellbuf_put(&b, 'A');
    cellbuf_advance(&b);
    cellbuf_put(&b, 'B');
    cellbuf_advance(&b);
    cellbuf_backspace(&b);
    cellbuf_put(&b, 'X');
    ASSERT_EQ(cellbuf_get(&b, 0, 0).rune, 'A');
    ASSERT_EQ(cellbuf_get(&b, 0, 1).rune, 'X');
    cellbuf_free(&b);
}

TEST(tab_to_8col_stop) {
    Buffer b;
    cellbuf_init(&b, 24, 80);
    cellbuf_tab(&b);
    ASSERT_EQ(b.cur_col, 8);
    cellbuf_tab(&b);
    ASSERT_EQ(b.cur_col, 16);
    b.cur_col = 5;
    cellbuf_tab(&b);
    ASSERT_EQ(b.cur_col, 8);
    cellbuf_free(&b);
}

TEST(wrap_at_end_of_line) {
    Buffer b;
    cellbuf_init(&b, 24, 80);
    b.cur_row = 0;
    b.cur_col = 79;
    cellbuf_put(&b, 'Z');
    cellbuf_advance(&b);
    ASSERT_EQ(cellbuf_get(&b, 0, 79).rune, 'Z');
    ASSERT_EQ(b.cur_row, 1);
    ASSERT_EQ(b.cur_col, 0);
    cellbuf_free(&b);
}

TEST(scroll_drops_top_row) {
    Buffer b;
    cellbuf_init(&b, 3, 4);
    /* row 0 = "ABCD", row 1 = "EFGH", row 2 = "IJKL" */
    const char *rows[] = {"ABCD", "EFGH", "IJKL"};
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++) {
            b.cur_row = r; b.cur_col = c;
            cellbuf_put(&b, rows[r][c]);
        }
    }
    cellbuf_scroll(&b, 1);
    ASSERT_EQ(cellbuf_get(&b, 0, 0).rune, 'E');
    ASSERT_EQ(cellbuf_get(&b, 0, 3).rune, 'H');
    ASSERT_EQ(cellbuf_get(&b, 1, 0).rune, 'I');
    ASSERT_EQ(cellbuf_get(&b, 1, 3).rune, 'L');
    ASSERT_EQ(cellbuf_get(&b, 2, 0).rune, ' ');  /* blank */
    cellbuf_free(&b);
}

TEST(newline_at_bottom_scrolls) {
    Buffer b;
    cellbuf_init(&b, 3, 4);
    b.cur_row = 2;
    b.cur_col = 0;
    cellbuf_put(&b, 'A');
    cellbuf_advance(&b);
    cellbuf_newline(&b);
    ASSERT_EQ(b.cur_row, 2);
    /* Row 2 should be blank after scroll; the 'A' we wrote moved to row 1 */
    ASSERT_EQ(cellbuf_get(&b, 1, 0).rune, 'A');
    cellbuf_free(&b);
}

TEST(goto_clamps) {
    Buffer b;
    cellbuf_init(&b, 24, 80);
    cellbuf_goto(&b, -5, -10);
    ASSERT_EQ(b.cur_row, 0);
    ASSERT_EQ(b.cur_col, 0);
    cellbuf_goto(&b, 100, 100);
    ASSERT_EQ(b.cur_row, 23);
    ASSERT_EQ(b.cur_col, 79);
    cellbuf_free(&b);
}

TEST(erase_line_modes) {
    Buffer b;
    cellbuf_init(&b, 1, 6);
    /* fill row */
    for (int c = 0; c < 6; c++) {
        b.cur_col = c;
        cellbuf_put(&b, 'A' + c);
    }
    /* cursor at col 3 */
    b.cur_col = 3;
    cellbuf_erase_line(&b, 0);  /* erase right of (incl) cursor */
    ASSERT_EQ(cellbuf_get(&b, 0, 0).rune, 'A');
    ASSERT_EQ(cellbuf_get(&b, 0, 2).rune, 'C');
    ASSERT_EQ(cellbuf_get(&b, 0, 3).rune, ' ');
    ASSERT_EQ(cellbuf_get(&b, 0, 5).rune, ' ');
    cellbuf_free(&b);

    cellbuf_init(&b, 1, 6);
    for (int c = 0; c < 6; c++) {
        b.cur_col = c;
        cellbuf_put(&b, 'A' + c);
    }
    b.cur_col = 3;
    cellbuf_erase_line(&b, 1);  /* erase left of (incl) cursor */
    ASSERT_EQ(cellbuf_get(&b, 0, 0).rune, ' ');
    ASSERT_EQ(cellbuf_get(&b, 0, 3).rune, ' ');
    ASSERT_EQ(cellbuf_get(&b, 0, 4).rune, 'E');
    ASSERT_EQ(cellbuf_get(&b, 0, 5).rune, 'F');
    cellbuf_free(&b);

    cellbuf_init(&b, 1, 6);
    for (int c = 0; c < 6; c++) {
        b.cur_col = c;
        cellbuf_put(&b, 'A' + c);
    }
    cellbuf_erase_line(&b, 2);  /* erase all */
    for (int c = 0; c < 6; c++)
        ASSERT_EQ(cellbuf_get(&b, 0, c).rune, ' ');
    cellbuf_free(&b);
}

TEST(save_restore_cursor) {
    Buffer b;
    cellbuf_init(&b, 24, 80);
    b.cur_row = 5;
    b.cur_col = 10;
    cellbuf_set_attrs(&b, 1, 2, ATTR_BOLD);
    cellbuf_save_cursor(&b);
    b.cur_row = 0;
    b.cur_col = 0;
    cellbuf_set_attrs(&b, 7, 0, 0);
    cellbuf_restore_cursor(&b);
    ASSERT_EQ(b.cur_row, 5);
    ASSERT_EQ(b.cur_col, 10);
    ASSERT_EQ(b.cur_fg, 1);
    ASSERT_EQ(b.cur_bg, 2);
    ASSERT_EQ(b.cur_attrs, ATTR_BOLD);
    cellbuf_free(&b);
}

TEST(dirty_tracking) {
    Buffer b;
    cellbuf_init(&b, 4, 4);
    /* After init, all_dirty = 1; drain should report ncells */
    int idxs[100];
    Cell out[100];
    int n = cellbuf_drain_dirty(&b, idxs, out, 100);
    ASSERT_EQ(n, 16);
    /* After draining, no more dirty */
    ASSERT_EQ(cellbuf_dirty_count(&b), 0);
    /* Write one cell — only that one should be dirty */
    b.cur_row = 1;
    b.cur_col = 2;
    cellbuf_put(&b, 'X');
    n = cellbuf_drain_dirty(&b, idxs, out, 100);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(idxs[0], 1 * 4 + 2);  /* row 1, col 2 */
    ASSERT_EQ(out[0].rune, 'X');
    cellbuf_free(&b);
}

TEST(set_attrs_writes_color) {
    Buffer b;
    cellbuf_init(&b, 24, 80);
    cellbuf_set_attrs(&b, 1, 4, ATTR_BOLD | ATTR_UNDERLINE);
    cellbuf_put(&b, 'R');
    Cell c = cellbuf_get(&b, 0, 0);
    ASSERT_EQ(c.rune, 'R');
    ASSERT_EQ(c.fg, 1);
    ASSERT_EQ(c.bg, 4);
    ASSERT_EQ(c.attrs, ATTR_BOLD | ATTR_UNDERLINE);
    cellbuf_free(&b);
}

TEST(scrollback_captures_discarded_rows) {
    Buffer b;
    cellbuf_init(&b, 3, 5);
    /* Fill row 0 with "ABCDE", scroll up by 1 -> row 0 goes to scrollback */
    for (int c = 0; c < 5; c++) {
        b.cur_row = 0; b.cur_col = c;
        cellbuf_put(&b, 'A' + c);
    }
    cellbuf_scroll(&b, 1);
    ASSERT_EQ(cellbuf_scrollback_count(&b), 1);
    char buf[SCROLL_LINE_BYTES];
    int n = cellbuf_scrollback_lines(&b, 0, 1, buf);
    ASSERT_EQ(n, 1);
    /* String should be "ABCDE" */
    ASSERT_EQ(strncmp(buf, "ABCDE", 5), 0);
    cellbuf_free(&b);
}

TEST(scrollback_drops_trailing_spaces) {
    Buffer b;
    cellbuf_init(&b, 3, 10);
    /* Write "Hi" at start of row 0 */
    b.cur_row = 0; b.cur_col = 0;
    cellbuf_put(&b, 'H');
    b.cur_col = 1;
    cellbuf_put(&b, 'i');
    cellbuf_scroll(&b, 1);
    char buf[SCROLL_LINE_BYTES];
    cellbuf_scrollback_lines(&b, 0, 1, buf);
    /* Should be "Hi" not "Hi        " */
    ASSERT_EQ(strlen(buf), 2);
    ASSERT_EQ(strcmp(buf, "Hi"), 0);
    cellbuf_free(&b);
}

TEST(scrollback_oldest_first) {
    Buffer b;
    cellbuf_init(&b, 2, 5);
    /* Push three lines: "AAAA", "BBBB", "CCCC" */
    int i;
    for (i = 0; i < 3; i++) {
        b.cur_row = 0; b.cur_col = 0;
        for (int c = 0; c < 4; c++) {
            b.cur_col = c;
            cellbuf_put(&b, 'A' + i);
        }
        cellbuf_scroll(&b, 1);
    }
    ASSERT_EQ(cellbuf_scrollback_count(&b), 3);
    char buf[3 * SCROLL_LINE_BYTES];
    int n = cellbuf_scrollback_lines(&b, 0, 3, buf);
    ASSERT_EQ(n, 3);
    ASSERT_EQ(strcmp(buf + 0 * SCROLL_LINE_BYTES, "AAAA"), 0);
    ASSERT_EQ(strcmp(buf + 1 * SCROLL_LINE_BYTES, "BBBB"), 0);
    ASSERT_EQ(strcmp(buf + 2 * SCROLL_LINE_BYTES, "CCCC"), 0);
    cellbuf_free(&b);
}

int main(void) {
    printf("=== test_cells ===\n");
    test_init_default_palette_runner();
    test_put_single_char_runner();
    test_put_then_advance_runner();
    test_carriage_return_runner();
    test_newline_then_cr_runner();
    test_backspace_runner();
    test_tab_to_8col_stop_runner();
    test_wrap_at_end_of_line_runner();
    test_scroll_drops_top_row_runner();
    test_newline_at_bottom_scrolls_runner();
    test_goto_clamps_runner();
    test_erase_line_modes_runner();
    test_save_restore_cursor_runner();
    test_dirty_tracking_runner();
    test_set_attrs_writes_color_runner();
    test_scrollback_captures_discarded_rows_runner();
    test_scrollback_drops_trailing_spaces_runner();
    test_scrollback_oldest_first_runner();
    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
