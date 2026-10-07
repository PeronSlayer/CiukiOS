#ifndef CIUKIOS_ICON_LABEL_H
#define CIUKIOS_ICON_LABEL_H

/* Fixed-cell icon labels: wrap on spaces and preserve valid UTF-8 sequences
 * when an unbroken filename must continue on the next line. */
#ifndef CIUKI_ICON_LABEL_MEASURE
#define CIUKI_ICON_LABEL_MEASURE ui_measure
#endif

static int ciuki_label_char_bytes(const unsigned char *s)
{
    int n, i;
    if (s[0] < 0x80) return 1;
    if (s[0] >= 0xC2 && s[0] <= 0xDF) n = 2;
    else if (s[0] >= 0xE0 && s[0] <= 0xEF) n = 3;
    else if (s[0] >= 0xF0 && s[0] <= 0xF4) n = 4;
    else return 1;
    for (i = 1; i < n; i++)
        if (!s[i] || (s[i] & 0xC0) != 0x80) return 1;
    return n;
}

/* Return the source offset after one visible line and its measured width. */
static int ciuki_label_next_line(const char *text, int start, char *line,
                                 int max_width, int *line_width)
{
    char probe[LFN_NAME];
    int p = start, fit = start, break_at = -1, end, next, i;
    while (text[p] == ' ') p++;
    start = p;
    while (text[p]) {
        int bytes = ciuki_label_char_bytes((const unsigned char *)text + p);
        int length = p + bytes - start;
        for (i = 0; i < length; i++) probe[i] = text[start + i];
        probe[length] = 0;
        if (CIUKI_ICON_LABEL_MEASURE(probe) > max_width) break;
        if (text[p] == ' ') break_at = p;
        p += bytes;
        fit = p;
    }
    if (text[p] && break_at > start) {
        end = break_at;
        next = break_at + 1;
        while (text[next] == ' ') next++;
    } else {
        end = fit;
        next = fit;
    }
    if (end == start && text[end]) {
        end += ciuki_label_char_bytes((const unsigned char *)text + end);
        next = end;
    }
    for (i = 0; i < end - start; i++) line[i] = text[start + i];
    line[end - start] = 0;
    *line_width = CIUKI_ICON_LABEL_MEASURE(line);
    return next;
}

/* Measure a complete label once when the item list is built or sorted. */
static int ciuki_label_layout(const char *text, int max_width,
                              int *max_line_width)
{
    char line[LFN_NAME];
    int pos = 0, lines = 0, width = 0, line_width;
    while (text[pos]) {
        int next = ciuki_label_next_line(text, pos, line, max_width,
                                         &line_width);
        if (line_width > width) width = line_width;
        if (next <= pos) break;
        pos = next;
        lines++;
    }
    *max_line_width = width;
    return lines ? lines : 1;
}

#endif
