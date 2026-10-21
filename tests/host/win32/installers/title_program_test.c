/* f5-15C host test of the guest operator program (title_program.h) and the
 * kernel observer's inventory policy (title_controller.h). With a file
 * argument it parses that compiled program and prints a JSON summary for the
 * Python round-trip test; without one it runs the built-in cases.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../../src/probes/win32_installers/title_program.h"
#include "../../../../src/probes/win32_installers/title_controller.h"

static unsigned checks;
#define CHECK(c)                                                                  \
    do {                                                                          \
        checks++;                                                                 \
        if (!(c)) {                                                               \
            fprintf(stderr, "title program line %u: %s\n", __LINE__, #c);         \
            exit(1);                                                              \
        }                                                                         \
    } while (0)
static struct tp_program program;
static unsigned parse(const char *text) { return tp_parse(&program, text, (unsigned)strlen(text)); }
static const char *base =
    "CIUKIOP 1\nworkload aoe-trial\nchoices default\nentry MSAoE.exe\ntimeout 480\n"
    "defaults 1 idle=4000 max=40\nrestart accept\nuninstall 1 timeout=240\nshot 1500\n";
static unsigned with(const char *steps)
{
    static char text[4096];
    snprintf(text, sizeof(text), "%s%send\n", base, steps);
    return parse(text);
}
static const char *json_text(const char *in)
{
    static char pool[8][700];
    static unsigned next;
    char *out = pool[next++ % 8];
    unsigned n = 0;
    for (; *in && n + 3 < sizeof(pool[0]); in++) {
        if (*in == '\\' || *in == '"') out[n++] = '\\';
        out[n++] = *in;
    }
    out[n] = 0;
    return out;
}
static void summary(void)
{
    printf("{\"workload\":\"%s\",\"entry\":\"%s\",\"arguments\":\"%s\",\"timeout\":%u,\"defaults\":%u,"
           "\"idle_ms\":%u,\"max_defaults\":%u,\"restart_accept\":%u,\"uninstall\":%u,\"uninstall_timeout\":%u,"
           "\"shot_ms\":%u,\"install_steps\":%u,\"uninstall_steps\":%u,\"steps\":[",
           json_text(program.workload), json_text(program.entry), json_text(program.arguments), program.timeout_s, program.defaults, program.idle_ms,
           program.max_defaults, program.restart_accept, program.uninstall, program.uninstall_timeout_s,
           program.shot_ms, program.install_steps, program.uninstall_steps);
    for (unsigned i = 0; i < program.step_count; i++) {
        const struct tp_step *s = &program.steps[i];
        printf("%s{\"segment\":%u,\"action\":%u,\"flags\":%u,\"timeout\":%u,\"settle\":%u,\"repeat\":%u,\"x\":%d,\"y\":%d,"
               "\"keys\":[", i ? "," : "", s->segment, s->action, s->flags, s->timeout_s, s->settle_ms, s->repeat, s->x, s->y);
        for (unsigned k = 0; k < s->key_count; k++) printf("%s%u", k ? "," : "", s->keys[k]);
        printf("],\"title\":\"%s\",\"class\":\"%s\",\"button\":\"%s\",\"text\":\"%s\",\"value\":\"%s\"}",
               json_text(s->title), json_text(s->cls), json_text(s->button), json_text(s->text), json_text(s->value));
    }
    printf("]}\n");
}
static struct tp_window win(uint32_t hwnd, uint32_t pid, uint32_t parent, uint32_t style, const char *cls, const char *title)
{
    struct tp_window w = {hwnd, pid, parent, style, "", ""};
    snprintf(w.cls, sizeof(w.cls), "%s", cls);
    snprintf(w.title, sizeof(w.title), "%s", title);
    return w;
}
int main(int argc, char **argv)
{
    if (argc == 2) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) return 2;
        static char text[65536];
        size_t n = fread(text, 1, sizeof(text) - 1, f);
        fclose(f);
        unsigned line = tp_parse(&program, text, (unsigned)n);
        if (line) { printf("{\"error_line\":%u}\n", line); return 1; }
        summary();
        return 0;
    }
    /* Header, defaults and required fields. */
    CHECK(!with(""));
    CHECK(!strcmp(program.workload, "aoe-trial") && !strcmp(program.entry, "MSAoE.exe") && program.choices_default);
    CHECK(program.timeout_s == 480 && program.defaults == 1 && program.idle_ms == 4000 && program.max_defaults == 40);
    CHECK(program.restart_accept == 1 && program.uninstall == 1 && program.uninstall_timeout_s == 240 && program.shot_ms == 1500);
    CHECK(parse("CIUKIOP 2\nend\n") == 1);
    CHECK(parse("CIUKIOP 1\nworkload x\nentry a.exe\nend\n") != 0);           /* choices default required */
    CHECK(parse("CIUKIOP 1\nworkload x\nchoices default\nentry ..\\a.exe\nend\n") == 4);
    CHECK(parse("CIUKIOP 1\nworkload x\nchoices default\nentry C:a.exe\nend\n") == 4);
    CHECK(parse("CIUKIOP 1\nworkload x\nchoices default\nentry \\a.exe\nend\n") == 4);
    CHECK(parse("CIUKIOP 1\nworkload x\nchoices custom\nentry a.exe\nend\n") == 3);
    CHECK(parse("CIUKIOP 1\nworkload x\nchoices default\nentry a.exe\nend\nstep install default\n") == 6);
    CHECK(parse("CIUKIOP 1\nworkload x\nchoices default\nentry Total%20Annihilation.exe\nend\n") == 0 &&
          !strcmp(program.entry, "Total Annihilation.exe"));
    CHECK(parse("CIUKIOP 1\nworkload x\nchoices default\nentry a%2.exe\nend\n") == 4);
    CHECK(parse("CIUKIOP 1\nworkload x\nchoices default\nentry a%00.exe\nend\n") == 4);
    /* Steps and their per-action rules. */
    CHECK(!with("step install button button=Next* title=Setup*\n") && program.steps[0].action == TP_BUTTON &&
          !strcmp(program.steps[0].button, "Next*") && program.steps[0].timeout_s == 60 && program.steps[0].settle_ms == 500);
    CHECK(!with("step install keys keys=alt+n,tab,shift+tab,space,enter,f10,ctrl+a\n") && program.steps[0].key_count == 7);
    CHECK(program.steps[0].keys[0] == (0x31 | TP_ALT) && program.steps[0].keys[1] == 0x0f &&
          program.steps[0].keys[2] == (0x0f | TP_SHIFT) && program.steps[0].keys[3] == 0x39 &&
          program.steps[0].keys[4] == 0x1c && program.steps[0].keys[5] == 0x44 && program.steps[0].keys[6] == (0x1e | TP_CTRL));
    CHECK(with("step install keys keys=N\n"));        /* a shifted character is shift+<base> */
    CHECK(with("step install keys keys=hyper+a\n"));
    CHECK(!with("step install text value=C:%5CProgram%20Files%5CGame replace=1\n") &&
          !strcmp(program.steps[0].value, "C:\\Program Files\\Game") && (program.steps[0].flags & TP_REPLACE));
    CHECK(!with("step install click title=Setup x=120 y=40\n") && program.steps[0].x == 120 && program.steps[0].y == 40);
    CHECK(with("step install click title=Setup x=120\n"));
    CHECK(with("step install click x=1 y=1\n"));       /* click needs a window match */
    CHECK(with("step install default x=1 y=1\n"));
    CHECK(with("step install button title=Setup\n"));  /* button needs button= */
    CHECK(with("step install expect title=Setup\n"));   /* expect needs text= */
    CHECK(!with("step install expect class=#32770 text=C:%5CProgram%20Files* timeout=30 optional=1\n") &&
          program.steps[0].timeout_s == 30 && (program.steps[0].flags & TP_OPTIONAL));
    CHECK(with("step install wait\n"));
    CHECK(with("step install default title=a title=b\n"));
    CHECK(with("step install default bogus=1\n"));
    CHECK(with("step install default timeout=0\n") && with("step install default timeout=601\n"));
    CHECK(!with("step install default repeat=3\nstep uninstall default\n") && program.install_steps == 1 &&
          program.uninstall_steps == 1 && program.steps[0].repeat == 3);
    CHECK(with("step uninstall default\nstep install default\n"));
    CHECK(parse("CIUKIOP 1\nworkload x\nchoices default\nentry a.exe\nuninstall 0\nstep uninstall default\nend\n") != 0);
    /* Globs, captions, mnemonics. */
    CHECK(tp_glob("next*", "Next >") && tp_glob("*agree", "I Agree") && tp_glob("i ?gree", "I Agree") && !tp_glob("next", "Next >"));
    char caption[64];
    tp_caption("  &Next >", caption, sizeof(caption));
    CHECK(!strcmp(caption, "Next >"));
    tp_caption("Save && &Exit", caption, sizeof(caption));
    CHECK(!strcmp(caption, "Save & Exit"));
    CHECK(tp_mnemonic("&Next >") == 'n' && tp_mnemonic("I &Agree") == 'a' && tp_mnemonic("A && B") == 0 && !tp_mnemonic("OK"));
    CHECK(tp_chord("alt+n") == (0x31 | TP_ALT) && tp_char_key('C') == (0x2e | TP_SHIFT) && tp_char_key(':') == (0x27 | TP_SHIFT) &&
          tp_char_key('\\') == 0x2b && tp_char_key(' ') == 0x39 && !tp_char_key('\t'));
    /* Window matching on a directory snapshot (newest first). */
    const uint32_t pids[] = {7, 9};
    struct tp_window w[] = {
        win(30, 9, 0, TP_WS_VISIBLE, "#32770", "Setup - Age of Empires"),
        win(31, 9, 30, TP_WS_VISIBLE, "Button", "&Next >"),
        win(32, 9, 30, TP_WS_VISIBLE | TP_WS_DISABLED, "Button", "&Back"),
        win(33, 9, 30, TP_WS_VISIBLE, "Button", "Cancel"),
        win(34, 9, 30, TP_WS_VISIBLE, "Edit", "C:\\Program Files\\Microsoft Games\\Age of Empires Trial"),
        win(40, 5, 0, TP_WS_VISIBLE, "#32770", "Setup - Other process"),
        win(41, 5, 40, TP_WS_VISIBLE, "Button", "&Finish"),
        win(50, 7, 0, 0, "#32770", "Hidden setup"),
    };
    unsigned count = sizeof(w) / sizeof(w[0]);
    struct tp_step s = {0};
    int child;
    snprintf(s.title, sizeof(s.title), "Setup*");
    CHECK(tp_match(&s, w, count, pids, 2, &child) == 0 && child == -1);
    snprintf(s.button, sizeof(s.button), "Next*");
    CHECK(tp_match(&s, w, count, pids, 2, &child) == 0 && child == 1);
    snprintf(s.button, sizeof(s.button), "Back");
    CHECK(tp_match(&s, w, count, pids, 2, &child) == -1);   /* disabled */
    snprintf(s.button, sizeof(s.button), "Finish");
    CHECK(tp_match(&s, w, count, pids, 2, &child) == -1);   /* untracked process */
    s.button[0] = 0;
    snprintf(s.text, sizeof(s.text), "C:\\Program Files\\*");
    CHECK(tp_match(&s, w, count, pids, 2, &child) == 0 && child == 4);
    s.text[0] = 0;
    snprintf(s.title, sizeof(s.title), "Hidden*");
    CHECK(tp_match(&s, w, count, pids, 2, &child) == -1);   /* invisible */
    CHECK(tp_default_button(w, count, 0) == 1);               /* Next, never Cancel/Back */
    struct tp_window progress[] = {win(60, 7, 0, TP_WS_VISIBLE, "#32770", "Copying"),
                                   win(61, 7, 60, TP_WS_VISIBLE, "Button", "Cancel")};
    CHECK(tp_default_button(progress, 2, 0) == -1);
    struct tp_window finish[] = {win(70, 7, 0, TP_WS_VISIBLE, "#32770", "Done"),
                                 win(71, 7, 70, TP_WS_VISIBLE, "Button", "&Next >"),
                                 win(72, 7, 70, TP_WS_VISIBLE, "Button", "&Finish")};
    CHECK(tp_default_button(finish, 3, 0) == 2);
    CHECK(tp_top(w, count, 4) == 0 && tp_top(w, count, 0) == 0);
    /* Kernel inventory policy. */
    CHECK(title_path_class("C:\\WINDOWS\\SYSTEM.CRG.0") == TITLE_PATH_EXCLUDED);
    CHECK(title_path_class("c:\\windows\\user.crg.1") == TITLE_PATH_EXCLUDED);
    CHECK(title_path_class("C:\\WINDOWS\\TEMP\\_ISTMP0.DIR") == TITLE_PATH_TEMP);
    CHECK(title_path_class("C:\\System\\Win32\\x.dll") == TITLE_PATH_SYSTEM && title_path_class("C:\\SYSTEM") == TITLE_PATH_SYSTEM);
    CHECK(title_path_class("C:\\WINDOWS\\SYSTEM\\DSOUND.VXD") == TITLE_PATH_ORDINARY);
    CHECK(title_path_class("C:\\SystemTools\\a") == TITLE_PATH_ORDINARY);
    CHECK(title_menu_link("C:\\WINDOWS\\Start Menu\\Programs\\Diablo\\Diablo.lnk") &&
          !title_menu_link("C:\\WINDOWS\\Desktop\\Diablo.lnk") && title_link("C:\\WINDOWS\\Desktop\\Diablo.LNK") &&
          !title_link("C:\\Games\\a.lnk"));
    CHECK(title_path_hash("C:\\A\\b") == title_path_hash("c:\\a\\B") && title_path_hash("C:\\A") != title_path_hash("C:\\B"));
    struct title_entry entries[64];
    for (unsigned i = 0; i < 64; i++) entries[i] = (struct title_entry){title_path_hash((char[]){'C', ':', '\\', (char)('A' + i % 26), (char)('0' + i / 26), 0}), i, 0, 0, 0};
    title_sort(entries, 64);
    for (unsigned i = 1; i < 64; i++) CHECK(entries[i - 1].hash <= entries[i].hash);
    CHECK(title_find(entries, 64, title_path_hash("C:\\C1")) >= 0 && title_find(entries, 64, title_path_hash("C:\\Z9")) < 0);
    uint8_t link[128] = "CIUKLNK3";
    link[8] = 1;
    const char *fields[] = {"Diablo", "C:\\Diablo\\Diablo.exe", "", "C:\\Diablo", ""};
    unsigned at = 40;
    for (unsigned i = 0; i < 5; i++) {
        unsigned n = (unsigned)strlen(fields[i]);
        link[16 + i * 4] = (uint8_t)n;
        memcpy(link + at, fields[i], n);
        at += n;
    }
    const uint8_t *target;
    unsigned length;
    CHECK(title_link_target(link, at, &target, &length) && length == 20 && !memcmp(target, "C:\\Diablo\\Diablo.exe", 20));
    CHECK(!title_link_target(link, at - 1, &target, &length));
    struct title_result r = {.magic = TITLE_MAGIC, .version = 1, .phase = TITLE_READY};
    CHECK(title_valid(&r));
    r.phase = TITLE_PHASES;
    CHECK(!title_valid(&r));
    printf("PASS title program checks=%u\n", checks);
    return 0;
}
