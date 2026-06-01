# Wacki

A portable C/SDL2 reconstruction of **Wacki** (1997) — a Polish
point-and-click adventure written by Henryk Cygert. The game itself
is entirely in Polish, set in Poland, and made in Poland; for that
reason the rest of this README is in Polish too. The codebase, code
comments, and architectural docs are in English so the engine stays
hackable for anyone — but the user-facing material lives where its
audience does.

![Wacki](https://i.ytimg.com/vi/pVzsMB6r3hE/maxresdefault.jpg)

---

## Co to w ogóle jest

**Wacki** to point-and-click adventure z 1997 roku — szukasz przedmiotów,
gadasz z postaciami, łączysz rzeczy ze sobą, czasem ginie ci bohater i
ładujesz save. Oryginał chodził na DirectDraw + WaveOut pod Win9x, na
płycie CD-ROM, z autorską binarką `WACKI.EXE` (302 KB PE32).

To repo to **port silnika** zrekonstruowany z reverse-engineeringu tej
binarki (Ghidra, 2026). Nie zawiera assetów gry — żeby zagrać,
potrzebujesz własnej kopii płyty.

Co robi port, czego nie robi oryginał:

- Chodzi na **macOS, Linux, Windows** i **Miyoo Mini Plus** (i kompatybilne
  handheldy SigmaStar — Anbernic RG35XX, Powkiddy RGB30 itd.)
- Statycznie linkowany — jedna binarka, zero zależności w runtime
  (oprócz `WACKI.EXE` jako build-time dependency; szczegóły niżej)
- 47 suit testów (458 case'ów), CI matrix dla 4 platform
- Zachowuje binary-identyczny PKv2 depack i format save'a — możesz
  wymieniać save'y z oryginałem

---

## Jak uruchomić

### Co musisz mieć

1. **Pliki gry** — `Dane_*.dta` z oryginalnej płyty + `WACKI.EXE`
   (z tego embedujemy `.rdata`/`.data` w czasie buildu).
2. **SDL2** — tylko na desktopie do buildu. Wersja release linkowana statycznie.

| Platforma           | SDL2                                              |
|---------------------|---------------------------------------------------|
| macOS (Homebrew)    | `brew install sdl2`                               |
| Debian / Ubuntu     | `sudo apt install libsdl2-dev`                    |
| Fedora              | `sudo dnf install SDL2-devel`                     |
| Arch                | `sudo pacman -S sdl2`                             |
| Windows (MSYS2)     | `pacman -S mingw-w64-x86_64-{gcc,SDL2} make`      |

### Build

```bash
mkdir -p data
cp /Volumes/WACKI_1/*.DTA data/        # assety
cp /Volumes/WACKI_1/WACKI.EXE data/    # tylko do buildu

make all                               # → dist/wacki + tools
./dist/wacki
```

Albo gotowe artefakty z GitHub Releases — statycznie linkowane, nic
nie instalujesz.

### Miyoo Mini Plus (i pin-kompatybilne handheldy)

Build chodzi w kontenerze Docker z toolchainem buildroot.

```bash
make miyoo                             # → dist/wacki-miyoo (ARM ELF)
./tools/pack-miyoo.sh                  # → dist/wacki-miyoo.zip (OnionOS .pak)
```

Zawartość `.zip` (`Wacki.pak/`) wrzuć na kartę SD pod `/Apps/` (OnionOS)
lub `/App/` (stock firmware), do `data/` w środku dorzuć `.dta`-ki i
gotowe.

### Sterowanie

| Platforma  | Ruch kursora     | Klik lewy   | Klik prawy   | Quit |
|------------|------------------|-------------|--------------|------|
| Desktop    | mysz             | LMB         | RMB          | ESC  |
| Handheld   | D-pad (przyspiesza) | A (Space) | B (LCtrl)   | Menu |

| Klawisz   | Co robi                                           |
|-----------|---------------------------------------------------|
| `ESC`     | wyjście                                           |
| `SPACE`   | przełącz aktywną postać (Ebek ↔ Fjej)             |
| `F5`      | quick-save (slot 0)                               |
| `F9`      | quick-load (slot 0)                               |
| `F12`     | menu pauzy                                        |
| `Ctrl-C`  | graceful shutdown (jak ESC)                       |

### Flagi runtime

```bash
./dist/wacki --headless                # bez okna (do CI / smoke)
./dist/wacki --scale 2 --scaler linear # okno 1280×960, linear scaling
./dist/wacki --seed 42                 # deterministyczny RNG
```

| Flaga / env                          | Efekt                                |
|--------------------------------------|--------------------------------------|
| `--headless` / `WACKI_HEADLESS=1`    | Brak okna i renderera                |
| `--scale N` / `WACKI_SCALE=N`        | Okno N×640 × N×480, framebuffer zostaje 640×480 |
| `--scaler MODE` / `WACKI_SCALER=...` | `nearest` / `linear` / `best`        |
| `--seed N` / `WACKI_SEED=N`          | Seed dla `WackiRand`                 |
| `WACKI_PATH=...`                     | Override ścieżki do `Dane_*.dta`     |

Engine szuka assetów w kolejności: `$WACKI_PATH` → `./data/` →
`<binary_dir>/data/` → `<binary_dir>/` → cwd. Case-fold działa, więc
`DANE_02.DTA` z ISO9660 pod macOS znajdzie się tak samo jak
`Dane_02.dta`.

---

## Architektura

### Skąd ten kod

Cały silnik jest **ręcznie zrekonstruowany** z dekompilacji
`WACKI.EXE` w Ghidrze. Każdy plik `.c` ma adres oryginalnej funkcji w
header-comment, żeby dało się ścigać każdą linijkę z powrotem do PE.
Pełny raport RE: [`docs/architecture.md`](docs/architecture.md), opcody
VM: [`docs/script-vm.md`](docs/script-vm.md), format binarny:
[`docs/asset-format.md`](docs/asset-format.md).

### Jak silnik widzi WACKI.EXE

Cygertowy build trzymał tablice skryptów, palety, nazwy assetów i
sztywne stringi w `.rdata` / `.data` PE. Port robi sztuczkę: w czasie
buildu `tools/embed-pe-data` parsuje `WACKI.EXE`, wyciąga *tylko* te
dwie sekcje danych i emituje je jako `const` tablicę slice'ów w
`src/embedded_wacki_pe.c`. Loader PE w runtime rozwiązuje oryginalne
VA-y (`0x40xxxx`) przeciw tej tablicy.

Co to znaczy w praktyce:

- Binarka jest **w pełni samodzielna w runtime** — `WACKI.EXE` jest
  potrzebny tylko gdy się buduje
- `.text` (x86 code), `.idata`, `.rsrc` są **pomijane** — engine ich
  nie dotyka, więc port nie wykonuje x86 → port jest **w pełni
  cross-platform na ARM** (stąd Miyoo działa)
- Jednorazowa canary w `PeLoaderRead` zawyje w logu, jakby kiedykolwiek
  spróbował odwołać się do pominiętej sekcji

### Format danych

```
Dane_XX.dta (BASE container)
   └─ SPIS (directory entries — name + offset)
   └─ PKv2 streams (LZ77 + Huffman-prefix, custom format)
       └─ ANIM / MASK / FILD / PIC / PAL / .scr / .wav / FLIC
```

PKv2 depack jest **byte-perfect** na wszystkich 1782 wpisach w
`Dane_02.dta` (regresja: `tools/dta-validate.sh`).

### Per-frame tick

```
PlatformPumpEvents     drain SDL, virtual cursor, button latch
  → ScriptVM.tick      78 main opcodes (script.c) +
                       37 per-entity opcodes (actor/vm.c)
  → walker step        16.16 fixed-point line stepper + Dijkstra waypoints
  → render             Z-sorted entity list → 8-bpp shadow buffer
  → PlatformPresent    palette LUT → ARGB8888 → SDL_Texture → SDL_RenderPresent
  → audio mixer        callback on SDL_AudioDevice: 8 channels @22050 Hz
```

Wszystko jest jednowątkowe (poza callbackiem audio). Frame budget jest
łaskawy — silnik z '97 nie zna pojęcia VSync, ale `SDL_RENDERER_PRESENTVSYNC`
pinuje do odświeżania monitora.

### Drzewko źródeł

```
include/wacki.h              typy, magic numbers, API modułów
include/entity_offsets.h     byte offsets entity struct (EOFF_*)
src/
   main.c                    int main() + data-root + arg parsing + SIGINT
   game.c                    main loop, stage loader, frame tick
   graphics.c                portable 8-bpp blitter + alpha-plane
   audio.c                   SDL2 mixer (8 channels, 22050 Hz S16 stereo)
   archive.c                 BASE/SPIS container parser
   depack.c                  PKv2 LZ77 + Huffman-prefix decoder
   assets.c                  ANIM/MASK/FILD asset loaders
   pe_loader.c               passive PE image map + xlat_binary_ptr
   embedded_wacki_pe.c       (generated) .rdata + .data slice table
   save.c                    Wacki.sav r/w + slot restore + atomic write
   font.c                    Futura.30 parser + text rasteriser
   flic.c, flic/             FLIC/AVI cutscene decoder
   platform_sdl.c            SDL2 window/event/present + virtual cursor
   log.c                     LOG_TRACE/DEBUG/INFO/WARN/ERROR macros
   vm/                       main script VM (78 ops) + parser
   actor/                    entity allocator + per-entity VM + walker
   scene/                    stage lifecycle + per-frame tick + click queue
   hud/                      panel + inventory + cursor
   menu/                     main menu + chapter select + slot picker
   audio/                    sound queue + cutscene audio + SFX
   text/, anim/, script_bridge/   smaller utility layers
tools/
   embed-pe-data.c           PE → const slice table generator
   dta-extract.c             dump every file from a Dane_XX.dta
   pkv2-depack.c             decompress a standalone PKv2 blob
   build-miyoo.sh            Docker wrapper for ARM cross-build
   pack-miyoo.sh             produce OnionOS .pak
   dta-validate.sh           PKv2 regression (1782 SHA-256 checksums)
   smoke-runner.sh           deterministic CI smoke (--seed × multiple)
```

---

## Status podsystemów

| Podsystem                  | Status | Uwagi                                                              |
|----------------------------|--------|--------------------------------------------------------------------|
| Standalone runtime         | ✅     | PE data sekcje embedded; runtime nie potrzebuje WACKI.EXE          |
| Cross-platform (desktop)   | ✅     | macOS arm64, Linux x86_64, Windows x86_64 (MSYS2/mingw)            |
| Handheld (armv7l)          | ✅     | Miyoo Mini Plus + SigmaStar SSD20x (Anbernic, Powkiddy, …)         |
| Window / event pump        | ✅     | SDL2 — 640×480 8-bpp paletted backbuffer → SDL_Texture             |
| Virtual cursor             | ✅     | D-pad → akcelerowany kursor; addytywnie z myszą                    |
| Archive container          | ✅     | BASE/SPIS layout fully parsed                                      |
| PKv2 depack                | ✅     | byte-perfect na wszystkich 1782 entries                            |
| Asset registry             | ✅     | ANIM/MASK/FILD/PIC/PAL headers + pointer table fixup               |
| Software blitter           | ✅     | color-key, opaque, translucent, scaled, alpha-plane                |
| Palette install / present  | ✅     | shadow → SDL_Texture przez 8→32 LUT + RGB12 kwantyzator            |
| Save / load                | ✅     | `Wacki.sav` r/w + atomic write (POSIX rename / MoveFileEx)         |
| Font rasteriser            | ✅     | Futura.30 BE — 1-bpp i colour-plane glyphy                         |
| Script VM (main)           | ✅     | 78 opcodes 0x00..0x57, line-by-line audit vs Ghidra                |
| Script VM (per-entity)     | ✅     | opcodes 0x00..0x24                                                 |
| Walker / pathfinding       | ✅     | 16.16 fixed-point + waypoint Dijkstra                              |
| Audio mixer                | ✅     | 22050 Hz S16 stereo, 8 channels (music + dialog + 6 SFX)           |
| Positional SFX stereo pan  | ✅     | per-channel `gain_l`/`gain_r`                                      |
| Per-line dialog + lip-sync | ✅     | mixer ch 1 + `IsDialogLinePlaying` poll                            |
| FLIC/AVI cutscenes         | ✅     | custom FLIC decoder — intro + death + stage-end AVI                |
| Inventory                  | ✅     | page-swap, AddItem/RemoveItem, op 0x1A-0x1F                        |
| Stage 1 end-to-end         | ✅     | intro → menu → gameplay → dialogue → save → load                   |
| Dialog choice picker UI    | 🟡    | linear playback OK; interaktywny picker (op 0x1A) deferred         |
| Custom cursor animation    | 🟡    | OS cursor + held-item ghost; Krazek 10-state anim deferred         |
| Health bar depletion       | 🟡    | renderuje, ale mechanizm depletion nie został wyciągnięty z RE     |
| Scene snapshot (op 0x2C)   | 🟡    | exit-reachability graph deferred — nie ma consumera w porcie       |
| Stages 2-5 (gameplay loop) | 🟡    | entry chain działa, end-to-end nie zweryfikowane interactively     |

Legenda: ✅ port 1:1 / 🟡 partial — wszystko co poza 🟡 albo działa
identycznie z oryginałem, albo jest świadomym wyjściem poza scope.

---

## Asset tools

```bash
make tools
./dist/dta-extract /Volumes/WACKI_1/Dane_10.dta out/
./dist/pkv2-depack out/EBEK.WYC ebek.raw
```

Regresja depack (porównanie SHA-256 ze złotymi):

```bash
./tools/dta-validate.sh data/DANE_02.DTA
# [validate] PASS — all 1782 files match baseline
```

---

## Testy

```bash
make test          # 47 suit, 458 case'ów, zero SDL deps, <1s
make debug         # build z ASan + UBSan dla fuzz / crash debug
```

Co pokrywają testy:

- **Format**: PKv2 depack, DTA archive parse, ANIM/MASK/FILD loader,
  PE loader + `xlat_binary_ptr`
- **Save**: `Wacki.sav` roundtrip + atomic write na wszystkich
  platformach (Windows używa `MoveFileEx` zamiast `rename`, żeby
  zachować atomicity przy istniejącym pliku)
- **VM**: produkcyjne `RunScriptInterpreter` linkowane przez stub SDL,
  egzekwowane hand-craftowanym bytekodem — arytmetyka, this/that
  remap, IF/ELSE/GOTO/LABEL/LOOP, `ScriptCall*` dispatch
- **Math**: walker fixed-point determinizm + golden vector,
  RNG determinizm
- **Layout**: compile-time invariants na strukturach (entity offsets,
  WackiSaveFile size)
- **Engine**: inventory state machine, sound queue z pannami stereo,
  panel/click hit-test, click queue FIFO, update registration LIFO

Plus dwa zewnętrzne smoke-testy:

- `tools/dta-validate.sh` — depack regression na 1782 plikach
- `tools/smoke-runner.sh` — deterministyczne `--seed N` przejścia
  menu → stage 1

---

## CI / release

Push na `master` odpala matrix [4 legi]:

- `macos-latest` (arm64)
- `ubuntu-latest` (x86_64)
- `windows-latest` (MSYS2 / mingw-w64)
- Miyoo Mini Plus (armv7l, Docker / buildroot)

Każdy leg buduje statycznie linkowaną binarkę (poza Miyoo, gdzie SDL2
przychodzi z firmware OnionOS), odpala 458 testów i loguje zestripowany
rozmiar artefaktu. Push tagu `v*` dokleja artefakty do GitHub Release
automatycznie.

`WACKI.EXE` przychodzi do CI przez sekret `WACKI_EXE_URL` (prywatny
serwer z pobraniem na czas joba) — bytes nie trafiają nigdy do repo
ani do publishowanych artefaktów.

---

## Limity i co dalej

1. **Stages 2-5** — entry chain działa, brak interactive playthrough.
   Stage 1 to jedyny z pełnym end-to-end coverage.
2. **Dialog choice picker** — interaktywny UI deferred (skrypty stage 1-2
   nie używają multi-choice dialogów).
3. **Health bar depletion** — renderuje OK, ale mechanizm odejmowania
   życia nie wyszedł z reverse'u. [`docs/health-bar-depletion.md`](docs/health-bar-depletion.md)
   to open question.
4. **Krazek 10-state cursor anim** — w porcie OS cursor + held-item
   ghost. Krazek (`FUN_004067C0`) odłożony.

---

## Credits

- **Henryk Cygert** — oryginalny silnik i game design (1997)
- TopWare Interactive Polska — publisher oryginalny
- Port + reverse engineering — Ghidra MCP, 2026
