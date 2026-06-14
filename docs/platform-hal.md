# Warstwa abstrakcji platformy (HAL)

Kod zależny od platformy jest oddzielony od silnika **interfejsami
per-podsystem**, a nie rozsianymi `#ifdef`. Rdzeń silnika — `game.c`, `vm/`,
`scene/`, mikser w `audio.c`, `flic.c`, `save.c`, `data_root.c` i reszta — woła
te interfejsy i nie wie, na czym działa. Dodanie platformy sprowadza się do
nowego katalogu z implementacjami plus jednego pliku `mk/<target>.mk`, bez
dotykania rdzenia.

## Zasady

1. **Rdzeń woła interfejsy, nigdy `#ifdef <platforma>`.** Pliki silnika są
   platform-agnostyczne; cała wiedza o sprzęcie siedzi za HAL-em.
2. **HAL jest per-podsystem, nie monolityczny.** Platformy mieszają backendy —
   np. PS2 = wejście przez SDL + wideo gsKit + dźwięk audsrv + I/O fileXio.
   Abstrakcja jest więc pocięta wzdłuż osi, które realnie się różnią, a każda
   platforma wybiera implementację na każdej osi z osobna.
3. **Wybór na etapie kompilacji, nie w runtime.** Targety są cross-kompilowane
   (osobne buildy), więc właściwe pliki `.c` wybiera linker — bez narzutu vtable
   czy wskaźników na funkcje. Każdy interfejs to zwykłe deklaracje funkcji;
   dokładnie jedna platforma dostarcza definicje.
4. **Dodanie platformy = nowy katalog + wpis w buildzie.** Zero zmian w
   rdzeniu, zero nowych `#ifdef` w plikach współdzielonych.

## Struktura katalogów

```
include/wacki/platform/
    storage.h   wykrywanie katalogu danych + I/O plików + zapisy + reader FLIC
    audio.h     urządzenie audio miksera + dźwięk cutscenek (AVI)
    video.h     init + present(shadow, paleta) + okno / fullscreen / message box
    input.h     zdolności wejścia (klawiatura?) + hooki pada + nawigacja menu
    system.h    cykl życia procesu + dcache + trace + głośność firmware

src/platform/
    sdl/   platform_sdl.c (Platform* + pompa wejścia/zdarzeń) · video_sdl.c
           audio_sdl.c · system_sdl.c · save_host.c · file_host.c · flic_host.c
           data_root_desktop.c · data_root_handheld.c
           gamepad_sdl.c (glue SDL_GameController) · hooks_desktop.c
    ps2/   system_ps2.c · storage_ps2.c · audio_ps2.c · video_ps2.c
           (+ ps2_internal.h — deklaracje międzyplikowe, font8x8.inc)
    miyoo/ miyoo.c        portmaster/ portmaster.c        macos/ macos.m
```

Pliki w `sdl/` są **czystym SDL2 — bez `#ifdef WACKI_MIYOO` /
`WACKI_PORTMASTER` / `WACKI_PS2`**. Jedyne `#ifdef`-y platformowe żyją wewnątrz
`src/platform/`: warianty rodziny SDL (`WACKI_MIYOO` / `WACKI_HANDHELD` /
`__APPLE__` / `_WIN32` / `__linux__`) w `sdl/`, oraz `WACKI_PS2` w `ps2/`.

## Interfejsy (co się różni)

- **`storage.h`** — `plat_data_roots()` (kandydaci na katalog danych per
  platforma) i `plat_prompt_data_folder()` (natywny picker folderu, desktop);
  shim plikowy `CygFile` (`fopen_cyg`/…: stdio na desktopie, fileXio na PS2);
  `plat_save_read/write` (plik vs karta pamięci PS2); oraz strumieniowy reader
  FLIC (`plat_flic_*`: setvbuf-owane stdio vs wątek read-ahead na PS2).
- **`audio.h`** — urządzenie miksera: `plat_audio_open/close/is_open/lock/
  unlock`. Mikser w `audio.c` jest czystym mikserem kanałów — oddaje callback
  `mixer_pull`, a platforma go napędza (callback SDL albo wątek audsrv). Osobno
  dźwięk cutscenek AVI jako urządzenie push: `plat_avi_audio_begin/push/end` +
  `_is_open/_below_cushion/_flush/_needs_pump`.
- **`video.h`** — `plat_video_init/present/shutdown` (present dostaje 8-bpp
  shadow + paletę), `plat_video_toggle_fullscreen`, `plat_video_message_box`,
  `plat_video_sdl_init_flags` (które podsystemy SDL podnieść). Na PS2 present
  idzie przez gsKit ze sprzętową paletą; na desktopie/handheldach przez
  streaming-teksturę SDL.
- **`input.h`** — *zdolności* wejścia, nie sam pump (ten jest współdzielony w
  `platform_sdl.c`): `plat_input_has_keyboard` (czy jest realna klawiatura —
  gameplay nie zakłada, że tak), `plat_handle_platform_key` (przyciski sprzętowe
  jako keysymy — Miyoo), `plat_pad_read_extra` (na PS2: tryb analog DualShocka +
  mysz USB), oraz nawigacja prostego menu `plat_pad_menu_nav` / `plat_input_flush`
  (używane przez ekran wyboru trybu wideo PS2).
- **`system.h`** — cykl życia procesu: `plat_system_early_init` /
  `plat_system_exit` (bring-up stosu IOP na PS2, stderr→log na Win32, parkowanie
  EE przy wyjściu PS2…), `plat_dcache_flush` (no-op na targetach cache-spójnych),
  `plat_trace_mark` (breadcrumby bring-upu, czytane po PINE), oraz hook
  `plat_restore_system_volume` (re-aplikacja głośności firmware — Miyoo).

## Model „hooków" per-target

Pliki `sdl/` są wspólne dla desktopu i handheldów i nie mają `#ifdef`-ów
urządzeń. Garść zachowań, które realnie różnią się per urządzenie — głośność
firmware, mapowanie przycisków na keysymy, domyślny fullscreen, analog + mysz na
PS2 — to **hooki** (`plat_restore_system_volume`, `plat_handle_platform_key`,
`plat_apply_video_prefs`, `plat_pad_read_extra`, `plat_input_has_keyboard`).
Każdy target linkuje **dokładnie jednego dostawcę hooków**:

| Target      | Dostawca hooków             |
|-------------|-----------------------------|
| desktop     | `sdl/hooks_desktop.c` (no-opy) |
| Miyoo       | `miyoo/miyoo.c` (MI_AO + keysymy) |
| PortMaster  | `portmaster/portmaster.c` (fullscreen) |
| PS2         | `ps2/system_ps2.c` (analog + mysz USB) |

Reguła „jeden dostawca na target" usuwa `#ifdef`-y bez kolizji symboli.
`gamepad_sdl.c` (glue `SDL_GameController`) jest linkowany na każdym targecie z
SDL — jest no-opem, gdy pada nie ma — więc i bramka pada znika.

## Wybór platformy w buildzie

Główny `Makefile` trzyma tylko rzeczy wspólne i robi `include mk/$(TGT).mk`
(gdzie `TGT` = `$(TARGET)`, a domyślnie `desktop`). Każdy `mk/<target>.mk`
zbiera całą wiedzę swojego targetu: flagi `CFLAGS`, nazwę binarki, opcje
rozmiaru i listę `PLATFORM_SRCS` (implementacje HAL + dostawca hooków).
Rdzeniowa lista źródeł (`ENGINE_SRCS`) nigdy nie nazywa platformy. Nieznany
`TARGET` failuje od razu z czytelnym komunikatem.

## Dodanie nowej platformy

1. Utwórz `src/platform/<plat>/` z implementacjami pięciu interfejsów. Jeśli
   platforma ma SDL2 — reużyj plików `sdl/` i dopisz tylko to, co odbiega.
2. Dostarcz jednego dostawcę hooków dla targetu (albo reużyj odpowiedniego).
3. Dodaj `mk/<plat>.mk` (`CFLAGS` / `BIN_NAME` / `PLATFORM_SRCS`).

Rdzeń zostaje nietknięty.
