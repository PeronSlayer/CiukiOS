/* MCI command-string interface: tokenizer, per-device command tables,
 * parameter blocks and return-string formatting (Platform SDK "MCI Command
 * Strings", "Command Shortcuts and Variations"; return formats as described
 * in docs/validation/f5-12b-media.md). SPDX-License-Identifier:
 * LGPL-2.1-or-later */
#include "audio.h"
#include "mci.h"
#include <stddef.h>
uint32_t mci_execute(uint32_t, uint32_t, uint32_t, void *, uint32_t *,
                     const char *);
uint32_t mci_auto_open(const char *, uint32_t *);
void mci_auto_close(uint32_t);
uint32_t mci_device_type(uint32_t);
uint32_t mci_lookup(const char *, uint32_t *);
const char *mci_type_for_element(const char *);
int mci_is_type(const char *);

#define W 1u /* waveaudio */
#define S 2u /* sequencer */
#define A 4u /* avivideo */
#define C 8u /* cdaudio */
#define ANY 15u
enum { K_FLAG, K_INT, K_STR, K_RECT, K_CONST, K_ITEM, K_HWND, K_CINT };
struct constant {
  const char *words;
  uint32_t value;
};
struct keyword {
  const char *words; /* space separated, matched token by token */
  uint8_t kind, mask;
  uint16_t offset;
  uint32_t flag, value;
  const struct constant *constants;
};
enum { R_NONE, R_INT, R_STR, R_RECT, R_ID, R_QUANTITY };
struct command {
  const char *verb;
  uint32_t msg;
  uint8_t ret, mask;
  const struct keyword *keywords;
};
#define O(type, field) ((uint16_t)offsetof(struct type, field))
#define SET_EXTRA(i) ((uint16_t)(offsetof(struct mci_set, extra) + 4 * (i)))
static const struct constant time_formats[] = {
    {"milliseconds", MCI_FORMAT_MILLISECONDS},
    {"ms", MCI_FORMAT_MILLISECONDS},
    {"hms", MCI_FORMAT_HMS},
    {"msf", MCI_FORMAT_MSF},
    {"frames", MCI_FORMAT_FRAMES},
    {"smpte 30 drop", MCI_FORMAT_SMPTE_30DROP},
    {"smpte 24", MCI_FORMAT_SMPTE_24},
    {"smpte 25", MCI_FORMAT_SMPTE_25},
    {"smpte 30", MCI_FORMAT_SMPTE_30},
    {"bytes", MCI_FORMAT_BYTES},
    {"samples", MCI_FORMAT_SAMPLES},
    {"tmsf", MCI_FORMAT_TMSF},
    {"song pointer", MCI_SEQ_FORMAT_SONGPTR},
    {0, 0}};
static const struct constant channels[] = {
    {"all", 0}, {"left", 1}, {"right", 2}, {0, 0}};
static const struct constant styles[] = {{"overlapped", 0x00CF0000u},
                                         {"popup", 0x80000000u},
                                         {"child", 0x40000000u},
                                         {0, 0}};
static const struct constant format_tags[] = {{"pcm", 1}, {0, 0}};
static const struct constant ports[] = {
    {"none", MCI_SEQ_NONE}, {"mapper", MCI_SEQ_MAPPER}, {0, 0}};
static const struct constant sync_modes[] = {{"file", MCI_SEQ_FILE},
                                             {"midi", MCI_SEQ_MIDI},
                                             {"smpte", MCI_SEQ_SMPTE},
                                             {"none", MCI_SEQ_NONE},
                                             {0, 0}};
static const struct constant show[] = {{"show min noactive", 7},
                                       {"show minimized", 2},
                                       {"show maximized", 3},
                                       {"show noactivate", 4},
                                       {"show normal", 1},
                                       {"show na", 8},
                                       {"show", 5},
                                       {"hide", 0},
                                       {"minimize", 6},
                                       {"restore", 9},
                                       {0, 0}};
#define NOTIFY_WAIT                                                            \
  {"notify", K_FLAG, ANY, 0, MCI_NOTIFY, 0, 0},                                \
      {"wait", K_FLAG, ANY, 0, MCI_WAIT, 0, 0}
static const struct keyword kw_open[] = {
    NOTIFY_WAIT,
    {"type", K_STR, ANY, O(mci_open, type), MCI_OPEN_TYPE, 0, 0},
    {"alias", K_STR, ANY, O(mci_open, alias), MCI_OPEN_ALIAS, 0, 0},
    {"shareable", K_FLAG, ANY, 0, MCI_OPEN_SHAREABLE, 0, 0},
    {"sharable", K_FLAG, ANY, 0, MCI_OPEN_SHAREABLE, 0, 0},
    {"buffer", K_INT, W, O(mci_open, style), MCI_WAVE_OPEN_BUFFER, 0, 0},
    {"parent", K_HWND, A, O(mci_open, parent), MCI_DGV_OPEN_PARENT, 0, 0},
    {"style", K_CINT, A, O(mci_open, style), MCI_DGV_OPEN_WS, 0, styles},
    {"nostatic", K_FLAG, A, 0, MCI_DGV_OPEN_NOSTATIC, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}};
static const struct keyword kw_basic[] = {NOTIFY_WAIT, {0, 0, 0, 0, 0, 0, 0}};
static const struct keyword kw_play[] = {
    NOTIFY_WAIT,
    {"from", K_INT, ANY, O(mci_play, from), MCI_FROM, 0, 0},
    {"to", K_INT, ANY, O(mci_play, to), MCI_TO, 0, 0},
    {"fullscreen", K_FLAG, A, 0, MCI_MCIAVI_PLAY_FULLSCREEN, 0, 0},
    {"window", K_FLAG, A, 0, MCI_MCIAVI_PLAY_WINDOW, 0, 0},
    {"repeat", K_FLAG, A, 0, MCI_DGV_PLAY_REPEAT, 0, 0},
    {"reverse", K_FLAG, A, 0, MCI_DGV_PLAY_REVERSE, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}};
static const struct keyword kw_seek[] = {
    NOTIFY_WAIT,
    {"to start", K_FLAG, ANY, 0, MCI_SEEK_TO_START, 0, 0},
    {"to end", K_FLAG, ANY, 0, MCI_SEEK_TO_END, 0, 0},
    {"to", K_INT, ANY, O(mci_seek, to), MCI_TO, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}};
#define ITEM(words, mask, value)                                               \
  { words, K_ITEM, mask, O(mci_status, item), MCI_STATUS_ITEM, value, 0 }
static const struct keyword kw_status[] = {
    NOTIFY_WAIT,
    ITEM("length", ANY, MCI_STATUS_LENGTH),
    ITEM("position", ANY, MCI_STATUS_POSITION),
    ITEM("number of tracks", ANY, MCI_STATUS_NUMBER_OF_TRACKS),
    ITEM("mode", ANY, MCI_STATUS_MODE),
    ITEM("media present", ANY, MCI_STATUS_MEDIA_PRESENT),
    ITEM("time format", ANY, MCI_STATUS_TIME_FORMAT),
    ITEM("ready", ANY, MCI_STATUS_READY),
    ITEM("current track", ANY, MCI_STATUS_CURRENT_TRACK),
    ITEM("format tag", W, MCI_WAVE_STATUS_FORMATTAG),
    ITEM("channels", W, MCI_WAVE_STATUS_CHANNELS),
    ITEM("samplespersec", W, MCI_WAVE_STATUS_SAMPLESPERSEC),
    ITEM("bytespersec", W, MCI_WAVE_STATUS_AVGBYTESPERSEC),
    ITEM("alignment", W, MCI_WAVE_STATUS_BLOCKALIGN),
    ITEM("bitspersample", W, MCI_WAVE_STATUS_BITSPERSAMPLE),
    ITEM("level", W, MCI_WAVE_STATUS_LEVEL),
    ITEM("input", W, MCI_WAVE_INPUT),
    ITEM("output", W, MCI_WAVE_OUTPUT),
    ITEM("tempo", S, MCI_SEQ_STATUS_TEMPO),
    ITEM("port", S, MCI_SEQ_STATUS_PORT),
    ITEM("slave", S, MCI_SEQ_STATUS_SLAVE),
    ITEM("master", S, MCI_SEQ_STATUS_MASTER),
    ITEM("offset", S, MCI_SEQ_STATUS_OFFSET),
    ITEM("division type", S, MCI_SEQ_STATUS_DIVTYPE),
    ITEM("type", C, MCI_CDA_STATUS_TYPE_TRACK),
    ITEM("window handle", A, MCI_DGV_STATUS_HWND),
    ITEM("speed", A, MCI_DGV_STATUS_SPEED),
    ITEM("frame rate", A, MCI_DGV_STATUS_FRAME_RATE),
    ITEM("audio", A, MCI_DGV_STATUS_AUDIO),
    ITEM("video", A, MCI_DGV_STATUS_VIDEO),
    ITEM("window visible", A, MCI_DGV_STATUS_WINDOW_VISIBLE),
    ITEM("window minimized", A, MCI_DGV_STATUS_WINDOW_MINIMIZED),
    ITEM("window maximized", A, MCI_DGV_STATUS_WINDOW_MAXIMIZED),
    ITEM("frames skipped", A, MCI_AVI_STATUS_FRAMES_SKIPPED),
    {"track", K_INT, ANY, O(mci_status, track), MCI_TRACK, 0, 0},
    {"start", K_FLAG, ANY, 0, MCI_STATUS_START, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}};
static const struct keyword kw_set[] = {
    NOTIFY_WAIT,
    {"time format", K_CONST, ANY, O(mci_set, time_format), MCI_SET_TIME_FORMAT,
     0, time_formats},
    {"door open", K_FLAG, C, 0, MCI_SET_DOOR_OPEN, 0, 0},
    {"door closed", K_FLAG, C, 0, MCI_SET_DOOR_CLOSED, 0, 0},
    {"audio", K_CONST, ANY, O(mci_set, audio), MCI_SET_AUDIO, 0, channels},
    {"video", K_FLAG, A, 0, MCI_SET_VIDEO, 0, 0},
    {"on", K_FLAG, ANY, 0, MCI_SET_ON, 0, 0},
    {"off", K_FLAG, ANY, 0, MCI_SET_OFF, 0, 0},
    {"any input", K_FLAG, W, 0, MCI_WAVE_SET_ANYINPUT, 0, 0},
    {"any output", K_FLAG, W, 0, MCI_WAVE_SET_ANYOUTPUT, 0, 0},
    {"input", K_INT, W, SET_EXTRA(0), MCI_WAVE_INPUT, 0, 0},
    {"output", K_INT, W, SET_EXTRA(1), MCI_WAVE_OUTPUT, 0, 0},
    {"format tag", K_CINT, W, SET_EXTRA(2), MCI_WAVE_SET_FORMATTAG, 0,
     format_tags},
    {"channels", K_INT, W, SET_EXTRA(3), MCI_WAVE_SET_CHANNELS, 0, 0},
    {"samplespersec", K_INT, W, SET_EXTRA(4), MCI_WAVE_SET_SAMPLESPERSEC, 0,
     0},
    {"bytespersec", K_INT, W, SET_EXTRA(5), MCI_WAVE_SET_AVGBYTESPERSEC, 0, 0},
    {"alignment", K_INT, W, SET_EXTRA(6), MCI_WAVE_SET_BLOCKALIGN, 0, 0},
    {"bitspersample", K_INT, W, SET_EXTRA(7), MCI_WAVE_SET_BITSPERSAMPLE, 0,
     0},
    {"tempo", K_INT, S, SET_EXTRA(0), MCI_SEQ_SET_TEMPO, 0, 0},
    {"port", K_CINT, S, SET_EXTRA(1), MCI_SEQ_SET_PORT, 0, ports},
    {"slave", K_CONST, S, SET_EXTRA(2), MCI_SEQ_SET_SLAVE, 0, sync_modes},
    {"master", K_CONST, S, SET_EXTRA(3), MCI_SEQ_SET_MASTER, 0, sync_modes},
    {"offset", K_INT, S, SET_EXTRA(4), MCI_SEQ_SET_OFFSET, 0, 0},
    {"seek exactly", K_FLAG, A, 0, MCI_DGV_SET_SEEK_EXACTLY, 0, 0},
    {"speed", K_INT, A, SET_EXTRA(1), MCI_DGV_SET_SPEED, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}};
static const struct keyword kw_info[] = {
    NOTIFY_WAIT,
    {"product", K_FLAG, ANY, 0, MCI_INFO_PRODUCT, 0, 0},
    {"file", K_FLAG, W | S | A, 0, MCI_INFO_FILE, 0, 0},
    {"name", K_FLAG, ANY, 0, MCI_INFO_NAME, 0, 0},
    {"copyright", K_FLAG, ANY, 0, MCI_INFO_COPYRIGHT, 0, 0},
    {"identity", K_FLAG, C, 0, MCI_INFO_MEDIA_IDENTITY, 0, 0},
    {"upc", K_FLAG, C, 0, MCI_INFO_MEDIA_UPC, 0, 0},
    {"window text", K_FLAG, A, 0, MCI_DGV_INFO_TEXT, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}};
#define CAP(words, mask, value)                                                \
  { words, K_ITEM, mask, O(mci_caps, item), MCI_GETDEVCAPS_ITEM, value, 0 }
static const struct keyword kw_caps[] = {
    NOTIFY_WAIT,
    CAP("can record", ANY, MCI_GETDEVCAPS_CAN_RECORD),
    CAP("has audio", ANY, MCI_GETDEVCAPS_HAS_AUDIO),
    CAP("has video", ANY, MCI_GETDEVCAPS_HAS_VIDEO),
    CAP("device type", ANY, MCI_GETDEVCAPS_DEVICE_TYPE),
    CAP("uses files", ANY, MCI_GETDEVCAPS_USES_FILES),
    CAP("compound device", ANY, MCI_GETDEVCAPS_COMPOUND_DEVICE),
    CAP("can eject", ANY, MCI_GETDEVCAPS_CAN_EJECT),
    CAP("can play", ANY, MCI_GETDEVCAPS_CAN_PLAY),
    CAP("can save", ANY, MCI_GETDEVCAPS_CAN_SAVE),
    CAP("inputs", W, MCI_WAVE_GETDEVCAPS_INPUTS),
    CAP("outputs", W, MCI_WAVE_GETDEVCAPS_OUTPUTS),
    CAP("can lock", A, MCI_DGV_GETDEVCAPS_CAN_LOCK),
    CAP("can stretch", A, MCI_DGV_GETDEVCAPS_CAN_STRETCH),
    CAP("can freeze", A, MCI_DGV_GETDEVCAPS_CAN_FREEZE),
    CAP("maximum windows", A, MCI_DGV_GETDEVCAPS_MAX_WINDOWS),
    CAP("can reverse", A, MCI_DGV_GETDEVCAPS_CAN_REVERSE),
    CAP("has still", A, MCI_DGV_GETDEVCAPS_HAS_STILL),
    CAP("maximum play rate", A, MCI_DGV_GETDEVCAPS_MAXIMUM_RATE),
    CAP("minimum play rate", A, MCI_DGV_GETDEVCAPS_MINIMUM_RATE),
    {0, 0, 0, 0, 0, 0, 0}};
static const struct keyword kw_sysinfo[] = {
    NOTIFY_WAIT,
    {"quantity", K_FLAG, ANY, 0, MCI_SYSINFO_QUANTITY, 0, 0},
    {"open", K_FLAG, ANY, 0, MCI_SYSINFO_OPEN, 0, 0},
    {"name", K_INT, ANY, O(mci_sysinfo, number), MCI_SYSINFO_NAME, 0, 0},
    {"installname", K_FLAG, ANY, 0, MCI_SYSINFO_INSTALLNAME, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}};
static const struct keyword kw_break[] = {
    NOTIFY_WAIT,
    {"on", K_INT, ANY, O(mci_break, key), MCI_BREAK_KEY, 0, 0},
    {"off", K_FLAG, ANY, 0, MCI_BREAK_OFF, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}};
static const struct keyword kw_window[] = {
    NOTIFY_WAIT,
    {"handle", K_HWND, A, O(mci_window, window), MCI_DGV_WINDOW_HWND, 0, 0},
    {"state", K_CONST, A, O(mci_window, show), MCI_DGV_WINDOW_STATE, 0, show},
    {"text", K_STR, A, O(mci_window, text), MCI_DGV_WINDOW_TEXT, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}};
static const struct keyword kw_put[] = {
    NOTIFY_WAIT,
    {"destination", K_FLAG, A, 0, MCI_DGV_PUT_DESTINATION, 0, 0},
    {"source", K_FLAG, A, 0, MCI_DGV_PUT_SOURCE, 0, 0},
    {"window", K_FLAG, A, 0, MCI_DGV_PUT_WINDOW, 0, 0},
    {"frame", K_FLAG, A, 0, MCI_DGV_PUT_FRAME, 0, 0},
    {"video", K_FLAG, A, 0, MCI_DGV_PUT_VIDEO, 0, 0},
    {"client", K_FLAG, A, 0, MCI_DGV_PUT_CLIENT, 0, 0},
    {"at", K_RECT, A, O(mci_put, rect), MCI_DGV_RECT, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}};
static const struct keyword kw_where[] = {
    NOTIFY_WAIT,
    {"destination", K_FLAG, A, 0, MCI_DGV_WHERE_DESTINATION, 0, 0},
    {"source", K_FLAG, A, 0, MCI_DGV_WHERE_SOURCE, 0, 0},
    {"window", K_FLAG, A, 0, MCI_DGV_WHERE_WINDOW, 0, 0},
    {"frame", K_FLAG, A, 0, MCI_DGV_WHERE_FRAME, 0, 0},
    {"video", K_FLAG, A, 0, MCI_DGV_WHERE_VIDEO, 0, 0},
    {"max", K_FLAG, A, 0, MCI_DGV_WHERE_MAX, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}};
static const struct keyword kw_step[] = {
    NOTIFY_WAIT,
    {"by", K_INT, A, O(mci_step, frames), MCI_DGV_STEP_FRAMES, 0, 0},
    {"reverse", K_FLAG, A, 0, MCI_DGV_STEP_REVERSE, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}};
static const struct keyword kw_update[] = {
    NOTIFY_WAIT,
    {"hdc", K_HWND, A, O(mci_update, dc), MCI_DGV_UPDATE_HDC, 0, 0},
    {"at", K_RECT, A, O(mci_update, rect), MCI_DGV_RECT, 0, 0},
    {"paint", K_FLAG, A, 0, MCI_DGV_UPDATE_PAINT, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}};
static const struct keyword kw_realize[] = {
    NOTIFY_WAIT,
    {"normal", K_FLAG, A, 0, 0x10000u, 0, 0},
    {"background", K_FLAG, A, 0, 0x20000u, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}};
static const struct command commands[] = {
    {"open", MCI_OPEN, R_ID, ANY, kw_open},
    {"close", MCI_CLOSE, R_NONE, ANY, kw_basic},
    {"play", MCI_PLAY, R_NONE, ANY, kw_play},
    {"seek", MCI_SEEK, R_NONE, ANY, kw_seek},
    {"stop", MCI_STOP, R_NONE, ANY, kw_basic},
    {"pause", MCI_PAUSE, R_NONE, ANY, kw_basic},
    {"resume", MCI_RESUME, R_NONE, ANY, kw_basic},
    {"status", MCI_STATUS, R_INT, ANY, kw_status},
    {"set", MCI_SET, R_NONE, ANY, kw_set},
    {"info", MCI_INFO, R_STR, ANY, kw_info},
    {"capability", MCI_GETDEVCAPS, R_INT, ANY, kw_caps},
    {"sysinfo", MCI_SYSINFO, R_STR, ANY, kw_sysinfo},
    {"break", MCI_BREAK, R_NONE, ANY, kw_break},
    {"window", MCI_WINDOW, R_NONE, A, kw_window},
    {"put", MCI_PUT, R_NONE, A, kw_put},
    {"where", MCI_WHERE, R_RECT, A, kw_where},
    {"step", MCI_STEP, R_NONE, A, kw_step},
    {"update", MCI_UPDATE, R_NONE, A, kw_update},
    {"realize", MCI_REALIZE, R_NONE, A, kw_realize},
    {"record", MCI_RECORD, R_NONE, ANY, kw_basic},
    {"save", MCI_SAVE, R_NONE, ANY, kw_basic},
    {"load", MCI_LOAD, R_NONE, ANY, kw_basic},
    {"cue", MCI_CUE, R_NONE, ANY, kw_basic},
    {"freeze", MCI_FREEZE, R_NONE, A, kw_basic},
    {"unfreeze", MCI_UNFREEZE, R_NONE, A, kw_basic},
    {"configure", MCI_CONFIGURE, R_NONE, A, kw_basic},
    {0, 0, 0, 0, 0}};

#define TOKENS 48u
struct tokens {
  char buffer[1024];
  char *word[TOKENS];
  uint32_t count;
};
static uint32_t tokenize(const char *s, struct tokens *t) {
  uint32_t at = 0;
  t->count = 0;
  for (;;) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
      s++;
    if (!*s)
      return 0;
    if (t->count == TOKENS)
      return MCIERR_PARSER_INTERNAL;
    t->word[t->count++] = t->buffer + at;
    if (*s == '"') {
      s++;
      while (*s && *s != '"') {
        if (at + 2 >= sizeof(t->buffer))
          return MCIERR_PARAM_OVERFLOW;
        t->buffer[at++] = *s++;
      }
      if (*s != '"')
        return MCIERR_NO_CLOSING_QUOTE;
      s++;
      if (*s && *s != ' ' && *s != '\t')
        return MCIERR_EXTRA_CHARACTERS;
    } else
      while (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') {
        if (at + 2 >= sizeof(t->buffer))
          return MCIERR_PARAM_OVERFLOW;
        t->buffer[at++] = *s++;
      }
    t->buffer[at++] = 0;
  }
}
/* Matches the space-separated `words` against tokens from `at`; returns the
 * number of tokens consumed (0 = no match). */
static uint32_t match(const char *words, const struct tokens *t, uint32_t at) {
  uint32_t used = 0;
  while (*words) {
    if (at + used >= t->count)
      return 0;
    const char *token = t->word[at + used];
    unsigned i = 0;
    for (; words[i] && words[i] != ' '; i++) {
      unsigned x = (uint8_t)words[i], y = (uint8_t)token[i];
      if (y >= 'A' && y <= 'Z')
        y += 32;
      if (x != y)
        return 0;
    }
    if (token[i])
      return 0;
    used++;
    words += i;
    while (*words == ' ')
      words++;
  }
  return used;
}
/* Decimal integers; "a:b:c:d" packs up to four bytes, least significant
 * first (MCI_MAKE_TMSF/MSF/HMS layout). */
static uint32_t integer(const char *s, int allow_sign, uint32_t *out) {
  uint32_t fields[4] = {0}, n = 0, negative = 0;
  if (allow_sign && *s == '-') {
    negative = 1;
    s++;
  }
  if (!*s)
    return MCIERR_BAD_INTEGER;
  for (;;) {
    uint64_t v = 0;
    if (*s < '0' || *s > '9')
      return MCIERR_BAD_INTEGER;
    while (*s >= '0' && *s <= '9') {
      v = v * 10 + (uint32_t)(*s++ - '0');
      if (v > 0xffffffffu)
        return MCIERR_BAD_INTEGER;
    }
    fields[n++] = (uint32_t)v;
    if (!*s)
      break;
    if (*s != ':' || n == 4 || negative)
      return MCIERR_BAD_INTEGER;
    s++;
  }
  if (n == 1) {
    *out = negative ? (uint32_t)-(int64_t)fields[0] : fields[0];
    return 0;
  }
  uint32_t v = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (fields[i] > 255)
      return MCIERR_BAD_INTEGER;
    v |= fields[i] << (8 * i);
  }
  *out = v;
  return 0;
}
static int mask_of(uint32_t devtype) {
  return devtype == MCI_DEVTYPE_WAVEFORM_AUDIO ? W
         : devtype == MCI_DEVTYPE_SEQUENCER    ? S
         : devtype == MCI_DEVTYPE_DIGITAL_VIDEO ? A
         : devtype == MCI_DEVTYPE_CD_AUDIO      ? C
                                                : 0;
}
static int mask_of_name(const char *type) {
  return mci_strieq(type, "waveaudio")   ? W
         : mci_strieq(type, "sequencer") ? S
         : mci_strieq(type, "avivideo")  ? A
         : mci_strieq(type, "cdaudio")   ? C
                                         : ANY;
}
static uint32_t parse_keywords(const struct command *c, struct tokens *t,
                               uint32_t at, int mask, uint32_t *flags,
                               uint8_t *block) {
  while (at < t->count) {
    const struct keyword *best = 0;
    uint32_t used = 0;
    for (const struct keyword *k = c->keywords; k->words; k++) {
      uint32_t n = (k->mask & mask) ? match(k->words, t, at) : 0;
      if (n > used) {
        used = n;
        best = k;
      }
    }
    if (!best)
      return MCIERR_UNRECOGNIZED_KEYWORD;
    at += used;
    if ((*flags & best->flag) && best->flag)
      return MCIERR_DUPLICATE_FLAGS;
    *flags |= best->flag;
    uint32_t value = 0, e;
    switch (best->kind) {
    case K_FLAG:
      break;
    case K_ITEM:
      w32_put32(block + best->offset, best->value);
      break;
    case K_INT:
    case K_HWND:
      if (at >= t->count)
        return MCIERR_MISSING_PARAMETER;
      if (best->kind == K_HWND && match("default", t, at))
        value = 0;
      else if ((e = integer(t->word[at], 0, &value)))
        return e;
      at++;
      if (best->kind == K_HWND)
        *(uintptr_t *)(block + best->offset) = value;
      else
        w32_put32(block + best->offset, value);
      break;
    case K_STR:
      if (at >= t->count)
        return MCIERR_MISSING_STRING_ARGUMENT;
      *(uintptr_t *)(block + best->offset) = (uintptr_t)t->word[at++];
      break;
    case K_RECT:
      if (at + 4 > t->count)
        return MCIERR_MISSING_PARAMETER;
      for (unsigned i = 0; i < 4; i++) {
        if ((e = integer(t->word[at++], 1, &value)))
          return e;
        w32_put32(block + best->offset + 4 * i, value);
      }
      break;
    case K_CONST:
    case K_CINT: {
      const struct constant *hit = 0;
      uint32_t n = 0;
      if (at >= t->count)
        return MCIERR_MISSING_PARAMETER;
      for (const struct constant *k = best->constants; k->words; k++) {
        uint32_t m = match(k->words, t, at);
        if (m > n) {
          n = m;
          hit = k;
        }
      }
      if (hit) {
        value = hit->value;
        at += n;
      } else if (best->kind == K_CINT && !integer(t->word[at], 0, &value))
        at++;
      else
        return MCIERR_BAD_CONSTANT;
      w32_put32(block + best->offset, value);
      break;
    }
    }
  }
  return 0;
}
static const char *resource(uint32_t id) {
  static const char *const general[] = {
      "vcr",          "videodisc", "overlay",  "cdaudio",   "dat",
      "scanner",      "animation", "digitalvideo", "other", "waveaudio",
      "sequencer",    "not ready", "stopped",  "playing",   "recording",
      "seeking",      "paused",    "open",     "true",      "false",
      "milliseconds", "hms",       "msf",      "frames",    "smpte 24",
      "smpte 25",     "smpte 30",  "smpte 30 drop", "bytes", "samples",
      "tmsf"};
  if (id >= 513 && id <= 543)
    return general[id - 513];
  switch (id) {
  case MCI_CDA_TRACK_AUDIO:
    return "audio";
  case MCI_CDA_TRACK_OTHER:
    return "other";
  case MCI_WAVE_PCM:
    return "pcm";
  case MCI_WAVE_MAPPER:
    return "mapper";
  case MCI_SEQ_DIV_PPQN:
    return "PPQN";
  case MCI_SEQ_DIV_SMPTE_24:
    return "SMPTE 24 Frame";
  case MCI_SEQ_DIV_SMPTE_25:
    return "SMPTE 25 Frame";
  case MCI_SEQ_DIV_SMPTE_30DROP:
    return "SMPTE 30 Drop Frame";
  case MCI_SEQ_DIV_SMPTE_30:
    return "SMPTE 30 Frame";
  case MCI_SEQ_MAPPER_S:
    return "mapper";
  case MCI_SEQ_SONGPTR_S:
    return "song pointer";
  case MCI_SEQ_NONE_S:
    return "none";
  }
  return 0;
}
static uint32_t append(char *out, uint32_t cap, uint32_t *at, const char *s) {
  while (*s) {
    if (*at + 1 >= cap)
      return MCIERR_PARAM_OVERFLOW;
    out[(*at)++] = *s++;
  }
  out[*at] = 0;
  return 0;
}
static uint32_t append_number(char *out, uint32_t cap, uint32_t *at,
                              int64_t v, unsigned width) {
  char digits[24];
  unsigned n = 0;
  int negative = v < 0;
  uint64_t u = negative ? (uint64_t)-v : (uint64_t)v;
  do {
    digits[n++] = (char)('0' + u % 10);
    u /= 10;
  } while (u);
  while (n < width)
    digits[n++] = '0';
  if (negative)
    digits[n++] = '-';
  char text[24];
  for (unsigned i = 0; i < n; i++)
    text[i] = digits[n - 1 - i];
  text[n] = 0;
  return append(out, cap, at, text);
}
static uint32_t format_value(uint32_t value, uint32_t kind, char *out,
                             uint32_t cap) {
  uint32_t at = 0, e = 0;
  if (!out || !cap)
    return 0;
  out[0] = 0;
  if (kind & MCI_RESOURCE_RETURNED) {
    const char *s = resource(value >> 16);
    return s ? append(out, cap, &at, s) : MCIERR_PARSER_INTERNAL;
  }
  if (kind & (MCI_COLONIZED3_RETURN | MCI_COLONIZED4_RETURN)) {
    unsigned n = (kind & MCI_COLONIZED4_RETURN) ? 4 : 3;
    for (unsigned i = 0; i < n && !e; i++) {
      if (i)
        e = append(out, cap, &at, ":");
      if (!e)
        e = append_number(out, cap, &at, (value >> (8 * i)) & 255, 2);
    }
    return e;
  }
  return append_number(out, cap, &at, value, 0);
}
/* Runs without the MCI lock: mci_execute takes it and must be able to drop
 * it completely while it waits. Device ids are revalidated there. */
uint32_t mci_string(const char *command, char *out, uint32_t cap,
                    uintptr_t window) {
  struct tokens t;
  union mci_parms block;
  char scratch[260];
  if (out && cap)
    out[0] = 0;
  if (!command)
    return MCIERR_MISSING_COMMAND_STRING;
  uint32_t e = tokenize(command, &t);
  if (!e && !t.count)
    e = MCIERR_MISSING_COMMAND_STRING;
  const struct command *c = 0;
  if (!e) {
    for (const struct command *k = commands; k->verb; k++)
      if (mci_strieq(k->verb, t.word[0]))
        c = k;
    if (!c)
      e = MCIERR_UNRECOGNIZED_COMMAND;
    else if (t.count < 2)
      e = MCIERR_MISSING_DEVICE_NAME;
  }
  if (e)
    return e;
  w32_fill(&block, 0, sizeof(block));
  uint32_t flags = 0, id = 0, kind = 0, opened = 0;
  const char *name = t.word[1];
  int mask = ANY;
  const char *sysinfo_type = 0;
  if (c->msg == MCI_OPEN) {
    /* Find the device type first: "type" keyword, "type!element", a type
     * name, or the element's extension. */
    const char *type = 0;
    for (uint32_t i = 2; i + 1 < t.count; i++)
      if (match("type", &t, i))
        type = t.word[i + 1];
    if (!type) {
      const char *bang = name;
      while (*bang && *bang != '!')
        bang++;
      if (*bang) {
        char head[32];
        uint32_t n = (uint32_t)(bang - name);
        if (n < sizeof(head)) {
          w32_copy(head, name, n);
          head[n] = 0;
          mask = mask_of_name(head);
        }
      } else if (mci_is_type(name))
        mask = mask_of_name(name);
      else {
        type = mci_type_for_element(name);
      }
    }
    if (type)
      mask = mask_of_name(type);
    if (mci_strieq(name, "new"))
      return MCIERR_UNSUPPORTED_FUNCTION; /* recording is not provided */
  } else if (c->msg == MCI_SYSINFO) {
    if (mci_strieq(name, "all"))
      id = MCI_ALL_DEVICE_ID;
    else
      sysinfo_type = name;
  } else if (mci_strieq(name, "all"))
    id = MCI_ALL_DEVICE_ID;
  else {
    uint32_t devtype = 0;
    id = mci_lookup(name, &devtype);
    if (id)
      mask = mask_of(devtype);
    else if (c->msg == MCI_CLOSE || c->msg == MCI_BREAK)
      e = MCIERR_INVALID_DEVICE_NAME;
    else
      opened = 1;
  }
  if (!e)
    e = parse_keywords(c, &t, 2, opened ? ANY : mask, &flags,
                       (uint8_t *)&block);
  if (!e && !(c->mask & mask))
    e = MCIERR_UNRECOGNIZED_COMMAND;
  if (e)
    return e;
  if (opened) {
    if (flags & MCI_NOTIFY)
      return MCIERR_NOTIFY_ON_AUTO_OPEN;
    e = mci_auto_open(name, &id);
    if (e)
      return e;
    mask = mask_of(mci_device_type(id));
    /* Re-parse with the opened device's keywords. */
    flags = 0;
    w32_fill(&block, 0, sizeof(block));
    e = parse_keywords(c, &t, 2, mask, &flags, (uint8_t *)&block);
    if (!e && !(c->mask & mask))
      e = MCIERR_UNRECOGNIZED_COMMAND;
    if (e) {
      mci_auto_close(id);
      return e;
    }
  }
  block.generic.callback = window;
  if (c->msg == MCI_OPEN) {
    if (mci_is_type(name) && !(flags & MCI_OPEN_TYPE)) {
      flags |= MCI_OPEN_TYPE;
      block.open.type = (uintptr_t)name;
    } else
      flags |= MCI_OPEN_ELEMENT;
    if (flags & MCI_OPEN_ELEMENT)
      block.open.element = (uintptr_t)name;
  }
  if (c->ret == R_STR) {
    if (c->msg == MCI_SYSINFO && (flags & MCI_SYSINFO_QUANTITY)) {
      block.sysinfo.text = (uintptr_t)scratch;
      block.sysinfo.size = 4;
    } else {
      block.info.text = (uintptr_t)(out && cap ? out : scratch);
      block.info.size = out && cap ? cap : sizeof(scratch);
    }
  }
  if ((c->msg == MCI_STATUS && !(flags & MCI_STATUS_ITEM)) ||
      (c->msg == MCI_GETDEVCAPS && !(flags & MCI_GETDEVCAPS_ITEM))) {
    e = MCIERR_MISSING_PARAMETER;
  } else
    e = mci_execute(id, c->msg, flags, &block, &kind, sysinfo_type);
  if (!e) {
    if (c->ret == R_INT)
      e = format_value(c->msg == MCI_STATUS ? (uint32_t)block.status.value
                                            : block.caps.value,
                       kind, out, cap);
    else if (c->ret == R_ID)
      e = format_value(block.open.device, 0, out, cap);
    else if (c->msg == MCI_SYSINFO && (flags & MCI_SYSINFO_QUANTITY))
      e = format_value(w32_u32(scratch), 0, out, cap);
    else if (c->ret == R_RECT && out && cap) {
      uint32_t at = 0;
      const struct mci_rect *r = &block.put.rect;
      int32_t v[4] = {r->left, r->top, r->right, r->bottom};
      out[0] = 0;
      for (unsigned i = 0; i < 4 && !e; i++) {
        if (i)
          e = append(out, cap, &at, " ");
        if (!e)
          e = append_number(out, cap, &at, v[i], 0);
      }
    }
  }
  if (opened)
    mci_auto_close(id);
  return e;
}
uint32_t W32API mciSendStringA(const char *command, char *out, uint32_t n,
                               uintptr_t window) {
  return mci_string(command, out, n, window);
}
uint32_t W32API mciSendStringW(const WCHAR *command, WCHAR *out, uint32_t n,
                               uintptr_t window) {
  char text[1024], result[512];
  uint32_t i = 0;
  if (!command)
    return MCIERR_MISSING_COMMAND_STRING;
  for (; command[i]; i++) {
    if (i + 1 >= sizeof(text))
      return MCIERR_PARAM_OVERFLOW;
    if (command[i] > 0xff)
      return MCIERR_INVALID_DEVICE_NAME;
    text[i] = (char)command[i];
  }
  text[i] = 0;
  uint32_t cap = n < sizeof(result) ? n : sizeof(result);
  uint32_t r = mci_string(text, out ? result : 0, out ? cap : 0, window);
  if (out && n) {
    for (i = 0; i + 1 < n && i < cap && result[i]; i++)
      out[i] = (uint8_t)result[i];
    out[i] = 0;
  }
  return r;
}
int32_t W32API mciExecute(const char *command) {
  return mci_string(command, 0, 0, 0) == 0;
}
