/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform_macos.m — macOS menu-bar polish.
 *
 *   1. Polish-localise SDL's stock menu bar. SDL builds the default
 *      Cocoa menu (App / Window / View) with hardcoded English titles
 *      in SDL_cocoaapp.m; Wacki is a Polish-only game, so we walk
 *      [NSApp mainMenu] and rename every standard item. Items are
 *      matched by action selector (robust across SDL versions), not by
 *      the English string.
 *
 *   2. Add a "Gra" menu exposing the in-game shortcuts as clickable
 *      items: Szybki zapis (F5), Szybki odczyt (F9), Zrzut ekranu,
 *      Pełny ekran (F11), Pauza (F12). Each item calls a C bridge in
 *      src/platform_sdl.c that performs the exact same action as the
 *      keyboard shortcut.
 *
 * Compiled and linked only on Darwin desktop builds (see Makefile);
 * never reached on Linux, Windows or the Miyoo handheld. No-op when
 * there's no menu (headless / no window).
 */

#import <Cocoa/Cocoa.h>
#include <unistd.h>

/* When launched from Finder, a .app bundle's working directory is "/"
 * (read-only), so the engine's cwd-relative Wacki.sav / wacki.cfg /
 * screenshot writes have nowhere to land. Relocate the process to
 * ~/Library/Application Support/Wacki/ — the standard per-user data
 * spot on macOS — before any file IO. No-op (returns 0, cwd kept) for a
 * bare-binary run: a dev launching dist/wacki expects saves next to it
 * and FindDataRoot's cwd probe to keep working. Called once at the very
 * top of WackiMain, before ConfigLoad. */
int PlatformMacUseAppSupportDir(void)
{
    @autoreleasepool {
        NSString *bp = [[NSBundle mainBundle] bundlePath];
        if (![bp hasSuffix:@".app"]) return 0;

        NSFileManager *fm = [NSFileManager defaultManager];
        NSURL *base = [fm URLForDirectory:NSApplicationSupportDirectory
                                 inDomain:NSUserDomainMask
                        appropriateForURL:nil
                                   create:YES
                                    error:NULL];
        if (!base) return 0;

        NSURL *dir = [base URLByAppendingPathComponent:@"Wacki"
                                           isDirectory:YES];
        if (![fm createDirectoryAtURL:dir
          withIntermediateDirectories:YES
                           attributes:nil
                                error:NULL]) return 0;

        const char *path = [[dir path] fileSystemRepresentation];
        if (!path || chdir(path) != 0) return 0;
        return 1;
    }
}

/* C bridges implemented in src/platform_sdl.c. */
extern void PlatformMenuQuickSave(void);
extern void PlatformMenuQuickLoad(void);
extern void PlatformMenuScreenshot(void);
extern void PlatformMenuToggleFull(void);
extern void PlatformMenuPause(void);

/* Item tags → which bridge to fire. */
enum {
    WACKI_ACT_QUICKSAVE = 1,
    WACKI_ACT_QUICKLOAD,
    WACKI_ACT_SCREENSHOT,
    WACKI_ACT_FULLSCREEN,
    WACKI_ACT_PAUSE
};

/* Menu target. One action method dispatches on the item's tag. Held in
 * a static so it outlives the call (NSMenuItem keeps only a weak
 * reference to its target); never released — lives for the app. */
@interface WackiMenuTarget : NSObject
- (void)fire:(id)sender;
@end

@implementation WackiMenuTarget
- (void)fire:(id)sender
{
    switch ([(NSMenuItem *)sender tag]) {
        case WACKI_ACT_QUICKSAVE:  PlatformMenuQuickSave();  break;
        case WACKI_ACT_QUICKLOAD:  PlatformMenuQuickLoad();  break;
        case WACKI_ACT_SCREENSHOT: PlatformMenuScreenshot(); break;
        case WACKI_ACT_FULLSCREEN: PlatformMenuToggleFull(); break;
        case WACKI_ACT_PAUSE:      PlatformMenuPause();      break;
        default: break;
    }
}
@end

static WackiMenuTarget *s_menu_target = nil;

/* Append one item to `menu`, tagged so -fire: knows what to run. A
 * function-key equivalent (when fkey != 0, e.g. NSF5FunctionKey) makes
 * the shortcut both visible and live; pass 0 for no key equivalent
 * (the in-game letter key still works, it just isn't shown here). */
static void add_game_item(NSMenu *menu, NSString *title, NSInteger tag,
                          unichar fkey)
{
    NSMenuItem *it = [menu addItemWithTitle:title
                                     action:@selector(fire:)
                              keyEquivalent:@""];
    [it setTag:tag];
    [it setTarget:s_menu_target];
    if (fkey) {
        [it setKeyEquivalent:[NSString stringWithCharacters:&fkey length:1]];
        [it setKeyEquivalentModifierMask:NSEventModifierFlagFunction];
    }
}

static void localize_standard_menus(NSMenu *bar)
{
    for (NSMenuItem *top in [bar itemArray]) {
        NSMenu *sub = [top submenu];
        if (!sub) continue;

        /* Window menu — SDL registers it via -setWindowsMenu:. */
        if (sub == [NSApp windowsMenu]) {
            [top setTitle:@"Okno"];
            [sub setTitle:@"Okno"];
        }

        for (NSMenuItem *it in [sub itemArray]) {
            SEL a = [it action];
            if (a == @selector(orderFrontStandardAboutPanel:)) {
                [it setTitle:@"O programie Wacki"];
            } else if (a == @selector(hide:)) {
                [it setTitle:@"Ukryj Wacki"];
            } else if (a == @selector(hideOtherApplications:)) {
                [it setTitle:@"Ukryj pozostałe"];
            } else if (a == @selector(unhideAllApplications:)) {
                [it setTitle:@"Pokaż wszystko"];
            } else if (a == @selector(terminate:)) {
                [it setTitle:@"Zakończ Wacki"];
            } else if (a == @selector(performClose:)) {
                [it setTitle:@"Zamknij"];
            } else if (a == @selector(performMiniaturize:)) {
                [it setTitle:@"Zminimalizuj"];
            } else if (a == @selector(performZoom:)) {
                [it setTitle:@"Powiększ"];
            } else if (a == @selector(toggleFullScreen:)) {
                [it setTitle:@"Przełącz pełny ekran"];
                [top setTitle:@"Widok"];   /* SDL titles this "View" */
            } else if ([it submenu] == [NSApp servicesMenu]) {
                [it setTitle:@"Usługi"];
            }
        }
    }
}

/* Build the "Gra" menu and insert it right after the app menu. */
static void add_game_menu(NSMenu *bar)
{
    NSMenu *game = [[NSMenu alloc] initWithTitle:@"Gra"];

    add_game_item(game, @"Szybki zapis",  WACKI_ACT_QUICKSAVE,  NSF5FunctionKey);
    add_game_item(game, @"Szybki odczyt", WACKI_ACT_QUICKLOAD,  NSF9FunctionKey);
    [game addItem:[NSMenuItem separatorItem]];
    add_game_item(game, @"Zrzut ekranu",  WACKI_ACT_SCREENSHOT, 0);
    [game addItem:[NSMenuItem separatorItem]];
    add_game_item(game, @"Pełny ekran",   WACKI_ACT_FULLSCREEN, NSF11FunctionKey);
    add_game_item(game, @"Pauza",         WACKI_ACT_PAUSE,      NSF12FunctionKey);

    NSMenuItem *top = [[NSMenuItem alloc] initWithTitle:@"Gra"
                                                 action:nil
                                          keyEquivalent:@""];
    [top setSubmenu:game];
    /* Index 0 is the app menu; slot "Gra" right after it. */
    [bar insertItem:top atIndex:([bar numberOfItems] > 0 ? 1 : 0)];
}

void PlatformSetupMacMenu(void)
{
    @autoreleasepool {
        NSMenu *bar = [NSApp mainMenu];
        if (!bar) return;

        if (!s_menu_target) s_menu_target = [[WackiMenuTarget alloc] init];

        localize_standard_menus(bar);
        add_game_menu(bar);
    }
}
