/* SPDX-License-Identifier: MIT */
#include "model.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

int ide_relative_path(const char *p)
{
    if (!*p || *p == '/' || *p == '-' || strlen(p) >= IDE_NAME) return 0;
    const char *start = p;
    for (;;) {
        unsigned char c = (unsigned char)*p;
        if (!c || c == '/') {
            size_t n = (size_t)(p - start);
            if (!n || (n == 1 && *start == '.') || (n == 2 && start[0] == '.' && start[1] == '.')) return 0;
            if (!c) return 1;
            start = ++p;
        } else {
            if (!(isalnum(c) || c == '_' || c == '-' || c == '.')) return 0;
            ++p;
        }
    }
}
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') ++s;
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r')) s[--n] = 0;
    return s;
}
static int same_path(const char *a,const char *b)
{
    while (*a && *b) if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return 0;
    return !*a && !*b;
}
static int parse_config(struct ide_project *out, char *buf)
{
    struct ide_project p = {0}; unsigned seen = 0;
    for (char *line = buf; line && *line;) {
        char *next = strchr(line, '\n'); if (next) *next++ = 0;
        char *key = trim(line); line = next;
        if (!*key || *key == '#') continue;
        char *eq = strchr(key, '='); if (!eq) return -EINVAL;
        *eq++ = 0; key = trim(key); char *value = trim(eq);
        unsigned bit = !strcmp(key, "sources") ? 1 : !strcmp(key, "output") ? 2 : !strcmp(key, "libraries") ? 4 : 0;
        if (!bit || (seen & bit)) return -EINVAL;
        seen |= bit;
        if (bit == 2) {
            if (!ide_relative_path(value) || strlen(value) >= sizeof(p.output) || same_path(value,"project.cfg")) return -EINVAL;
            strcpy(p.output, value); continue;
        }
        while (*value) {
            char *end = value; while (*end && *end != ' ' && *end != '\t') ++end;
            char *more = end; if (*more) *more++ = 0;
            if (bit == 1) {
                if (!ide_relative_path(value) || p.source_count == IDE_SOURCES) return -EINVAL;
                size_t n = strlen(value);
                if (n < 3 || strcmp(value + n - 2, ".c")) return -EINVAL;
                for (unsigned i=0; i<p.source_count; ++i) if (same_path(p.sources[i],value)) return -EINVAL;
                strcpy(p.sources[p.source_count++], value);
            } else {
                if (!strncmp(value, "-l", 2)) value += 2;
                if (!*value || strlen(value) >= 64 || p.library_count == IDE_LIBRARIES) return -EINVAL;
                for (const char *c=value; *c; ++c) if (!(isalnum((unsigned char)*c) || *c == '_' || *c == '-')) return -EINVAL;
                strcpy(p.libraries[p.library_count++], value);
            }
            value = trim(more);
        }
    }
    if ((seen & 3) != 3 || !p.source_count) return -EINVAL;
    for (unsigned i=0; i<p.source_count; ++i) if (same_path(p.output,p.sources[i])) return -EINVAL;
    *out = p; return 0;
}
int ide_project_parse(struct ide_project *out, const char *data, size_t size)
{
    if (!size || size > IDE_CFG || memchr(data, 0, size)) return -EINVAL;
    char *buf=malloc(size+1); if (!buf) return -ENOMEM;
    memcpy(buf,data,size); buf[size]=0;
    int r=parse_config(out,buf); free(buf); return r;
}
static int number(const char **p, unsigned *v)
{
    const char *s=*p; unsigned n=0;
    if (!isdigit((unsigned char)*s)) return 0;
    while (isdigit((unsigned char)*s)) {
        unsigned digit=(unsigned)(*s++-'0');
        if (n > (UINT_MAX-digit)/10) return 0;
        n=n*10+digit;
    }
    if (!n || *s != ':') return 0;
    *v=n; *p=s+1; return 1;
}
int ide_diagnostic(const char *s, struct ide_error *out)
{
    const char *colon=strchr(s, ':'); if (!colon || colon == s || (size_t)(colon-s) >= IDE_NAME) return 0;
    struct ide_error e={0}; memcpy(e.file,s,(size_t)(colon-s));
    const char *p=colon+1;
    if (!number(&p,&e.line)) return 0;
    if (isdigit((unsigned char)*p) && !number(&p,&e.column)) return 0;
    while (*p == ' ' || *p == '\t') ++p;
    if (!strncmp(p,"error:",6)) p+=6;
    else if (!strncmp(p,"warning:",8)) { e.warning=1; p+=8; }
    else return 0;
    while (*p == ' ' || *p == '\t') ++p;
    size_t n=strlen(p); if (n>=sizeof(e.message)) n=sizeof(e.message)-1;
    memcpy(e.message,p,n); *out=e; return 1;
}
void ide_changed(struct ide_model *m)
{
    ++m->revision; m->built=0;
    if (m->state != IDE_BUILDING && m->state != IDE_LAUNCHING) m->state=IDE_IDLE;
}
int ide_build_begin(struct ide_model *m)
{
    if (m->state == IDE_BUILDING || m->state == IDE_LAUNCHING) return -EBUSY;
    if (!m->project.source_count) return -EINVAL;
    m->build_revision=m->revision; m->built=0; m->state=IDE_BUILDING;
    m->errors=m->warnings=m->dropped=0; m->log_len=0; m->log[0]=0; m->log_truncated=0;
    memset(m->partial_len,0,sizeof(m->partial_len)); memset(m->overflow,0,sizeof(m->overflow));
    return 0;
}
static void diagnostic_line(struct ide_model *m, unsigned stream)
{
    struct ide_error e;
    m->partial[stream][m->partial_len[stream]]=0;
    if (m->overflow[stream]) ++m->dropped;
    else if (ide_diagnostic(m->partial[stream],&e)) {
        if (m->errors+m->warnings == IDE_ERRORS) ++m->dropped;
        else { m->error[m->errors+m->warnings]=e; if (e.warning) ++m->warnings; else ++m->errors; }
    }
    m->partial_len[stream]=0; m->overflow[stream]=0;
}
void ide_output(struct ide_model *m, unsigned stream, const char *s, size_t n)
{
    if (stream > 1) return;
    size_t keep=n; if (keep>IDE_LOG-m->log_len) { keep=IDE_LOG-m->log_len; m->log_truncated=1; }
    memcpy(m->log+m->log_len,s,keep); m->log_len+=keep; m->log[m->log_len]=0;
    for (size_t i=0; i<n; ++i) {
        unsigned char c=(unsigned char)s[i];
        if (c == '\n') diagnostic_line(m,stream);
        else if (c != '\r') {
            if (!c || m->partial_len[stream] == IDE_LINE-1) m->overflow[stream]=1;
            else m->partial[stream][m->partial_len[stream]++]=(char)c;
        }
    }
}
void ide_output_end(struct ide_model *m, unsigned stream)
{ if (stream<2 && (m->partial_len[stream] || m->overflow[stream])) diagnostic_line(m,stream); }
void ide_build_end(struct ide_model *m, int code)
{
    if (m->state != IDE_BUILDING) return;
    ide_output_end(m,0); ide_output_end(m,1); m->exit_code=code;
    m->built=!code && !m->errors && m->revision == m->build_revision;
    m->state=m->built ? IDE_READY : IDE_FAILED;
}
int ide_run_begin(struct ide_model *m)
{
    if (!m->built || (m->state != IDE_READY && m->state != IDE_RUNNING)) return -EINVAL;
    m->state=IDE_LAUNCHING; return 0;
}
void ide_run_result(struct ide_model *m, int result)
{ if (m->state == IDE_LAUNCHING) m->state=result>0 ? IDE_RUNNING : (m->built ? IDE_READY : IDE_IDLE); }

void ide_highlight(const char *s, size_t n, unsigned char *out)
{
    static const char *const keys[]={"auto","break","case","char","const","continue","default","do","double","else",
        "enum","extern","float","for","goto","if","inline","int","long","register","restrict","return","short",
        "signed","sizeof","static","struct","switch","typedef","union","unsigned","void","volatile","while","_Bool"};
    memset(out,IDE_C_TEXT,n);
    int line_start=1;
    for (size_t i=0;i<n;) {
        size_t start=i; unsigned kind=IDE_C_TEXT;
        if (s[i]=='\n') { line_start=1; ++i; continue; }
        if (line_start && (s[i]==' ' || s[i]=='\t')) { ++i; continue; }
        if (line_start && s[i]=='#') { kind=IDE_C_PREPROCESSOR; while (i<n && s[i]!='\n') ++i; }
        else if (i+1<n && s[i]=='/' && s[i+1]=='*') {
            kind=IDE_C_COMMENT; i+=2;
            while (i<n && !(i+1<n && s[i]=='*' && s[i+1]=='/')) ++i;
            if (i<n) i+=2;
        } else if (i+1<n && s[i]=='/' && s[i+1]=='/') {
            kind=IDE_C_COMMENT; while (i<n && s[i]!='\n') ++i;
        } else if (s[i]=='"' || s[i]=='\'') {
            char quote=s[i++]; kind=IDE_C_STRING;
            while (i<n) { if (s[i]=='\\' && i+1<n) { i+=2; continue; } if (s[i++]==quote) break; }
        } else if (isalpha((unsigned char)s[i]) || s[i]=='_') {
            while (i<n && (isalnum((unsigned char)s[i]) || s[i]=='_')) ++i;
            for (unsigned k=0;k<sizeof(keys)/sizeof(keys[0]);++k)
                if (strlen(keys[k])==i-start && !memcmp(s+start,keys[k],i-start)) { kind=IDE_C_KEYWORD; break; }
        } else if (isdigit((unsigned char)s[i])) {
            kind=IDE_C_NUMBER; while (i<n && (isalnum((unsigned char)s[i]) || s[i]=='.')) ++i;
        } else ++i;
        memset(out+start,(int)kind,i-start); line_start=0;
    }
}
