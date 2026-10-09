/* Shared single-line editor; included by ui.c and the host fixture. */
static int field_min(int a, int b) { return a < b ? a : b; }
static int field_max(int a, int b) { return a > b ? a : b; }
static void field_range(struct field *f)
{
    f->sel = f->anchor == f->cursor ? 0 :
        f->anchor == 0 && f->cursor == f->len ? 1 : 2;
}
static void field_normalize(struct field *f)
{
    /* Existing callers set sel=1 when Tab/F2 should select all. */
    if (f->sel == 1) { f->anchor = 0; f->cursor = f->len; }
    if (f->cursor < 0) f->cursor = 0;
    if (f->cursor > f->len) f->cursor = f->len;
    if (f->anchor < 0) f->anchor = 0;
    if (f->anchor > f->len) f->anchor = f->len;
    if (!f->sel) f->anchor = f->cursor;
    if (f->scroll < 0) f->scroll = 0;
    if (f->scroll > f->len) f->scroll = f->len;
    field_range(f);
}
static int field_class(int c)
{
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') return 0;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || (unsigned char)c >= 128) return 1;
    return 2;
}
static int field_left(struct field *f, int p)
{
    int c;
    while (p && field_class(f->text[p - 1]) == 0) p--;
    if (!p) return p;
    c = field_class(f->text[p - 1]);
    while (p && field_class(f->text[p - 1]) == c) p--;
    return p;
}
static int field_right(struct field *f, int p)
{
    int c;
    if (p < f->len) {
        c = field_class(f->text[p]);
        while (p < f->len && field_class(f->text[p]) == c) p++;
    }
    while (p < f->len && field_class(f->text[p]) == 0) p++;
    return p;
}
static void field_word(struct field *f, int p, int *a, int *b)
{
    int c;
    if (p == f->len && p) p--;
    *a = *b = p;
    if (p == f->len) return;
    c = field_class(f->text[p]);
    while (*a && field_class(f->text[*a - 1]) == c) (*a)--;
    while (*b < f->len && field_class(f->text[*b]) == c) (*b)++;
}
static void field_delete(struct field *f, int a, int b)
{
    mem_move(f->text + a, f->text + b, f->len - b + 1);
    f->len -= b - a;
    f->cursor = f->anchor = a;
    f->sel = 0;
}
void field_set(struct field *f, char *buffer, int max, const char *init)
{
    f->text = buffer; f->max = max;
    str_ncopy(buffer, init, max);
    f->len = str_len(buffer);
    f->cursor = f->anchor = f->len;
    f->sel = f->scroll = f->drag = f->click_valid = f->password = 0;
    f->click_pos = f->click_x = f->word_a = f->word_b = 0; f->click_tick = 0;
}
int field_insert(struct field *f, const char *text)
{
    int a, b, n = str_len(text);
    field_normalize(f);
    a = field_min(f->cursor, f->anchor); b = field_max(f->cursor, f->anchor);
    if (n >= f->max - f->len + b - a) return 0;
    if (a != b) field_delete(f, a, b);
    mem_move(f->text + a + n, f->text + a, f->len - a + 1);
    mem_move(f->text + a, text, n);
    f->len += n; f->cursor = f->anchor = a + n; f->sel = 0;
    f->drag = f->click_valid = 0;
    return 1;
}
int field_key(struct field *f, int key, int shift)
{
    int s = KEY_SCAN(key), c = KEY_CHAR(key), a, b, p, ext = shift & SH_SHIFT;
    int ctrl = (shift & SH_CTRL) || s == 0x73 || s == 0x74 || s == 0x77 || s == 0x75;
    field_normalize(f); f->drag = f->click_valid = 0;
    a = field_min(f->cursor, f->anchor); b = field_max(f->cursor, f->anchor);
    if (c == 1) { f->anchor = 0; f->cursor = f->len; field_range(f); return 1; }
    if (c == 8 || ((!c || c == 0xE0) && s == K_DEL)) {
        if (a != b) field_delete(f, a, b);
        else if (c == 8 && f->cursor) field_delete(f, ctrl ? field_left(f, a) : a - 1, a);
        else if (c != 8 && f->cursor < f->len) field_delete(f, a, ctrl ? field_right(f, a) : a + 1);
        else return 0;
        return 1;
    }
    if (c >= ' ' && c < 127 && !(shift & (SH_CTRL | SH_ALT))) {
        char insert[2]; insert[0] = (char)c; insert[1] = 0;
        return field_insert(f, insert);
    }
    if (c && c != 0xE0) return 0;
    p = f->cursor;
    if (s == K_LEFT || s == 0x73) p = !ext && a != b ? a : ctrl ? field_left(f, p) : p > 0 ? p - 1 : 0;
    else if (s == K_RIGHT || s == 0x74) p = !ext && a != b ? b : ctrl ? field_right(f, p) : p < f->len ? p + 1 : f->len;
    else if (s == K_HOME || s == 0x77) p = 0;
    else if (s == K_END || s == 0x75) p = f->len;
    else return 0;
    f->cursor = p;
    if (!ext) f->anchor = p;
    field_range(f); return 1;
}
static int field_glyph(struct field *f, int p)
{
    char ch[2]; int w;
    ch[0] = f->password ? '*' : f->text[p]; ch[1] = 0;
    w = ui_measure(ch); return w > 0 ? w : 1;
}
static int field_width(struct field *f, int a, int b)
{
    int w = 0;
    while (a < b) w += field_glyph(f, a++);
    return w;
}
static void field_view(struct field *f, int w)
{
    int width;
    if (f->cursor < f->scroll) f->scroll = f->cursor;
    width = field_width(f, f->scroll, f->cursor);
    while (f->scroll < f->cursor && width > w - 10) width -= field_glyph(f, f->scroll++);
}
void field_draw(struct field *f, int x, int y, int w, int focused)
{
    char text[258]; int p, start, end, px, q, a, b, fg, n;
    field_normalize(f); field_view(f, w);
    ui_inset(x, y, w, 22);
    start = f->scroll; end = start; px = 0;
    while (end < f->len && end - start < 257) {
        q = field_glyph(f, end);
        if (px + q > w - 8) break;
        px += q; end++;
    }
    a = field_min(f->anchor, f->cursor); b = field_max(f->anchor, f->cursor);
    px = x + 4; p = start;
    while (p < end) {
        int selected = p >= a && p < b;
        n = 0; q = 0;
        do {
            text[n++] = f->password ? '*' : f->text[p];
            q += field_glyph(f, p++);
        } while (p < end && (p >= a && p < b) == selected);
        text[n] = 0; fg = C_INK;
        if (selected && focused) { ui_rect(px, y + 3, q, 16, C_TITLE); fg = C_PAPER; }
        ui_text(px, y + 3, text, fg); px += q;
    }
    if (focused && !((HOST.ticks / 9) & 1)) {
        px = x + 4 + field_width(f, f->scroll, f->cursor);
        if (px < x + w - 3) ui_rect(px, y + 4, 1, 15, C_INK);
    }
}
static int field_hit(struct field *f, int x, int w, int mx)
{
    int p = f->scroll, px = x + 4, q;
    if (mx < px) return p;
    while (p < f->len && px < x + w - 4) {
        q = field_glyph(f, p);
        if (mx < px + (q + 1) / 2) break;
        px += q; p++;
    }
    return p;
}
/* Position-based input also serves scaled, multiline text on the paint canvas. */
int field_select(struct field *f, int kind, int p, int shift)
{
    int a, b, d;
    field_normalize(f);
    if (p < 0) p = 0;
    if (p > f->len) p = f->len;
    if (kind == MOUSE_DOWN) {
        d = p - f->click_pos; if (d < 0) d = -d;
        if (f->click_valid && d <= 1 && !(shift & SH_SHIFT) &&
            (unsigned short)(HOST.ticks - f->click_tick) <= (unsigned)(HOST.dblclick > 0 ? HOST.dblclick : 9)) {
            field_word(f, p, &f->word_a, &f->word_b);
            f->anchor = f->word_a; f->cursor = f->word_b;
            f->drag = 2; f->click_valid = 0;
        } else {
            if (!(shift & SH_SHIFT)) f->anchor = p;
            f->cursor = p; f->drag = 1;
            f->click_pos = p; f->click_tick = HOST.ticks; f->click_valid = 1;
        }
        field_range(f); return 1;
    }
    if ((kind != MOUSE_MOVE && kind != MOUSE_UP) || !f->drag) return 0;
    if (p != f->click_pos) f->click_valid = 0;
    if (f->drag == 2) {
        field_word(f, p, &a, &b);
        if (p < f->word_a) { f->anchor = f->word_b; f->cursor = a; }
        else if (p >= f->word_b) { f->anchor = f->word_a; f->cursor = b; }
        else { f->anchor = f->word_a; f->cursor = f->word_b; }
    } else f->cursor = p;
    field_range(f);
    if (kind == MOUSE_UP) f->drag = 0;
    return 1;
}
int field_mouse(struct field *f, int kind, int x, int w, int mx, int shift)
{
    int p, d, result;
    field_normalize(f);
    p = field_hit(f, x, w, mx);
    if (kind == MOUSE_DOWN) {
        d = mx - f->click_x; if (d < 0) d = -d;
        if (d > 4) f->click_valid = 0;
        f->click_x = mx;
    } else if (kind == MOUSE_MOVE && f->drag) {
        if (mx < x + 4 && f->scroll) p = --f->scroll;
        if (mx >= x + w - 4 && p < f->len) p++;
    }
    result = field_select(f, kind, p, shift);
    if (result) field_view(f, w);
    return result;
}
void field_click(struct field *f, int x, int w, int mx)
{
    field_mouse(f, MOUSE_DOWN, x, w, mx, HOST.shift);
}
