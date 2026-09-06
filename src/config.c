/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/config.c — wacki.cfg: persisted player display/input preferences.
 *
 * Dodane klucze:
 *   aspect_mode=stretch|4:3   — tryb proporcji ekranu
 *   touch_mode=absolute|relative|off — tryb ekranu dotykowego
 *
 * g_aspect_mode i g_touch_mode są zdefiniowane tutaj (nie w video_sdl.c /
 * platform_sdl.c) aby były dostępne dla WSZYSTKICH targetów przez linkowanie
 * config.c — zapobiega undefined reference na PS2 i innych platformach. */

#include "wacki.h"
#include "wacki/log.h"
#include <stdio.h>
#include <string.h>

#define WACKI_CFG_PATH "wacki.cfg"
#define CFG_SCALE_MAX  8

extern int  g_scale_factor;
extern int  g_fullscreen;

/* Zdefiniowane tutaj — dostępne dla wszystkich targetów. */
char g_aspect_mode[16] = "stretch";
char g_touch_mode[16]  = "absolute";

int g_config_first_run = 0;

void ConfigSave(void);

static void normalize_aspect(const char *raw)
{
    if (!raw) return;
    if (strncmp(raw,"4:3",3)==0 || strncmp(raw,"4x3",3)==0 || strncmp(raw,"43",2)==0)
        strncpy(g_aspect_mode, "4:3", 15);
    else if (strncmp(raw,"stretch",7)==0)
        strncpy(g_aspect_mode, "stretch", 15);
    g_aspect_mode[15] = '\0';
}

static void normalize_touch(const char *raw)
{
    if (!raw) return;
    if      (strncmp(raw,"absolute",8)==0) strncpy(g_touch_mode,"absolute",15);
    else if (strncmp(raw,"relative",8)==0) strncpy(g_touch_mode,"relative",15);
    else if (strncmp(raw,"off",3)==0)      strncpy(g_touch_mode,"off",15);
    g_touch_mode[15] = '\0';
}

void ConfigLoad(void)
{
    FILE *fp = fopen(WACKI_CFG_PATH, "r");
    if (!fp) {
        g_config_first_run = 1;
        ConfigSave();
        return;
    }
    char line[128], sv[16];
    int v;
    while (fgets(line, sizeof line, fp)) {
        if      (sscanf(line,"fullscreen=%d",&v)==1) g_fullscreen = v?1:0;
        else if (sscanf(line,"scale=%d",&v)==1 && v>=1 && v<=CFG_SCALE_MAX)
            g_scale_factor = v;
        else if (sscanf(line,"aspect_mode=%15s",sv)==1) normalize_aspect(sv);
        else if (sscanf(line,"touch_mode=%15s",sv)==1)  normalize_touch(sv);
    }
    fclose(fp);
    LOG_INFO("config","loaded: fullscreen=%d scale=%d aspect=%s touch=%s",
             g_fullscreen,g_scale_factor,g_aspect_mode,g_touch_mode);
}

void ConfigSave(void)
{
    FILE *fp = fopen(WACKI_CFG_PATH, "w");
    if (!fp) { LOG_INFO("config","cannot write %s",WACKI_CFG_PATH); return; }
    fprintf(fp,"# Wacki display/input preferences\n");
    fprintf(fp,"fullscreen=%d\n", g_fullscreen?1:0);
    fprintf(fp,"scale=%d\n",      g_scale_factor>0?g_scale_factor:1);
    fprintf(fp,"# aspect_mode: stretch (pelny ekran) | 4:3 (czarne pasy)\n");
    fprintf(fp,"aspect_mode=%s\n", g_aspect_mode[0]?g_aspect_mode:"stretch");
    fprintf(fp,"# touch_mode: absolute | relative | off\n");
    fprintf(fp,"touch_mode=%s\n",  g_touch_mode[0]?g_touch_mode:"absolute");
    fclose(fp);
    LOG_INFO("config","saved: fullscreen=%d scale=%d aspect=%s touch=%s",
             g_fullscreen,g_scale_factor?g_scale_factor:1,g_aspect_mode,g_touch_mode);
}
