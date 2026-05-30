/* src/vm/script_obj.c — .scr file loading + tag-based section lookup.
 *
 * Wacki ships its scripts as text-tagged files (.scr extension). A
 * file is a flat blob of mixed UTF-8 prose tags and binary bytecode:
 *
 * [etap]N ← stage N start
 * [komnata]M ← room M start (binary bytecode follows)
 * <bytecode>
 * [komnata]M2
 * ...
 * [etap]N2
 * ...
 * [rozmowa]<name> ← dialog by name (Gadki.scr)
 * [sampl]<asset> ← per-asset sample table (Wacky.scr)
 *
 * The loader (LoadScriptFile) reads the file once. The lookup helpers
 * (FindScriptByStageAndRoom, ScriptObjFindSection) advance the
 * ScriptObj's start/end pointers to a specific section. The script VM
 * then steps through `start..end` interpreting bytecode.
 *
 * NOTE: tags are linear ASCII text — there's no index, every lookup
 * scans the whole file. Wacky.scr is small (~50 KB) so this is fine.
 */

#include "wacki.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ScriptObj {
    const uint8_t *start;
    const uint8_t *end;
    uint32_t       size;
    uint8_t       *buf;
} ScriptObj;

/* Locate the FIRST byte after `tag` in [base..base+size), starting
 * from `p`. Returns NULL if not found. */
static const uint8_t *scan_text_tag(const uint8_t *base, uint32_t size,
                                    const uint8_t *p, const char *tag)
{
    size_t         tlen = strlen(tag);
    const uint8_t *end  = base + size;
    while (p && p < end) {
        if ((size_t)(end - p) >= tlen && memcmp(p, tag, tlen) == 0) {
            return p + tlen;
        }
        ++p;
    }
    return NULL;
}

int LoadScriptFile(void *self_, const char *name)
{
    ScriptObj *self = (ScriptObj *)self_;

    /* First try the host filesystem (for testing / development overrides). */
    FILE *fp = fopen(name, "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        self->size = (uint32_t)ftell(fp);
        fseek(fp, 0, SEEK_SET);
        self->buf = (uint8_t *)malloc(self->size);
        if (self->buf) fread(self->buf, 1, self->size, fp);
        fclose(fp);
        return self->buf != NULL;
    }

    /* Fall back to the shipped DTA archive. */
    void    *buf = NULL;
    uint32_t sz  = 0;
    if (!LoadFileFromDta(name, &buf, &sz)) return 0;
    self->buf  = (uint8_t *)buf;
    self->size = sz;
    return 1;
}

/* Look for `tag` followed by a parameter that matches `param` (e.g.
 * `[komnata]` `3`). On success sets self->start/end to the section
 * range (from after-param up to the next occurrence of the same tag,
 * or up to `altparam` if it appears first, or to end-of-file).
 *
 * The altparam path supports section ranges delimited by a DIFFERENT
 * tag — e.g. `[etap]N` body ends at either `[etap]N+1` OR at the next
 * `[komnata]`, whichever comes first. */
static int find_tag_in_script(ScriptObj *self, const char *tag,
                              const char *param, const char *altparam)
{
    if (!self->buf) return 0;
    size_t         plen = strlen(param);
    const uint8_t *p    = scan_text_tag(self->buf, self->size, self->buf, tag);

    while (p) {
        while (p < self->buf + self->size && isspace(*p)) ++p;

        if ((size_t)(self->buf + self->size - p) >= plen &&
            memcmp(p, param, plen) == 0)
        {
            self->start = p + plen;
            const uint8_t *next = scan_text_tag(self->buf, self->size,
                                                self->start, tag);
            if (altparam) {
                const uint8_t *alt = scan_text_tag(self->buf, self->size,
                                                   self->start, altparam);
                if (alt && (!next || alt < next)) next = alt;
            }
            self->end = next ? next : self->buf + self->size;
            return 1;
        }
        p = scan_text_tag(self->buf, self->size, p, tag);
    }
    return 0;
}

int FindScriptByStageAndRoom(void *self_, const char *etap, const char *komnata)
{
    ScriptObj *self = (ScriptObj *)self_;
    if (!find_tag_in_script(self, "[etap]",    etap,    NULL)) return 0;
    if (!find_tag_in_script(self, "[komnata]", komnata, NULL)) return 0;
    return 1;
}

int ScriptObjFindSection(void *self_, const char *tag,
                         const char *param, const char *altparam)
{
    return find_tag_in_script((ScriptObj *)self_, tag, param, altparam);
}

/* Accessors used by Wacky.scr [sampl] table parser (audio.c) — exposes
 * the section pointers without leaking the ScriptObj layout. */
const uint8_t *ScriptObjGetSectionStart(void *self_)
{
    return self_ ? ((ScriptObj *)self_)->start : NULL;
}

const uint8_t *ScriptObjGetSectionEnd(void *self_)
{
    return self_ ? ((ScriptObj *)self_)->end : NULL;
}
