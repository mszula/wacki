# Wacki — formaty binarnych assetów

Specyfikacja plików danych z płyty Wackich: kontener archiwum
(`Dane_NN.dta`), kompresja (`PKv2`) oraz poszczególne typy assetów
(`ANIM`, `MASK`, `FILD`, `PIC`, `PAL`).

Wszystkie wartości są little-endian, wszystkie współrzędne 16-bitowe,
wszystkie piksele 8-bit paletted (paleta w `paleta.pal` albo
per-stage wariancie).

---

## 1. Kontener — `Dane_NN.dta`

Archiwum DTA trzyma wiele skompresowanych assetów w jednym pliku z
katalogiem na końcu. Dwa markery magic delimitują katalog:

```
+-----------+-------------------------+
| BASE      | payload (PKv2 blobs)    |
| magic     |                         |
| dword 0   |                         |
| dword 0   |                         |
+-----------+-------------------------+
| SPIS      | directory               |
| magic     | entry[count]            |
| count     |                         |
+-----------+-------------------------+
```

### 1.1 Nagłówek (12 bajtów od offsetu 0)
| Off | Size | Field | Notes |
|----:|-----:|---|---|
|   0 | 4 | `magic` | `'BASE'` = 0x45534142 |
|   4 | 4 | `compressed_size` | rozmiar payloadu BASE |
|   8 | 4 | `unpacked_size` | (informacyjnie; 0 w shipped samples) |

Za nagłówkiem payload region to ciąg PKv2-skompresowanych blobów
konkatenowanych back-to-back. Offset każdego wpisu trzymany w katalogu.

### 1.2 Katalog (`SPIS`)
Na końcu pliku:

| Off | Size | Field | Notes |
|----:|-----:|---|---|
|   0 | 4    | `magic` | `'SPIS'` = 0x53495053 |
|   4 | 4    | `count` | liczba wpisów (1782 w `Dane_02`) |
|   8 | 16×N | `entry[N]` | nazwa + offset per wpis |

Każdy wpis katalogu = 16 bajtów:

| Off | Size | Field | Notes |
|----:|-----:|---|---|
|   0 | 12 | `name` | nul-padded ASCII (DOS 8.3-ish, np. `EBEK.WYC`) |
|  12 | 4  | `file_offset` | absolutny byte offset wpisu w archiwum |

Port traktuje nazwy case-insensitively (case-fold w lookup time),
ponieważ mounty ISO9660 na macOS mogą zwracać mixed case.

### 1.3 Lookup flow (port: `src/archive.c`)
```c
OpenDtaArchiveFile(path)  → mmap lub fread całego archiwum
LoadFileFromDta(name, &raw, &sz):
    walk directory, case-fold compare name
    seek file_offset, czytaj PKv2 header, DepackPkv2Buffer → raw
```

---

## 2. Kompresja — `PKv2`

LZ77 + prefixy Huffman-style. Każdy wpis pliku w DTA zaczyna się
12-bajtowym headerem PKv2:

| Off | Size | Field |
|----:|-----:|---|
|   0 | 4 | `magic` = 'PKv2' |
|   4 | 4 | `compressed_size` |
|   8 | 4 | `unpacked_size` |

Stream skompresowany następuje. Port'owy dekoder żyje w `src/depack.c`
(`DepackPkv2Buffer`); standalone tool `tools/pkv2-depack.c`
dekompresuje pojedynczy blob.

Dekoder czyta dwa state bytes blisko końca compressed buffer (bit-stream
count + buf pod offsetami `eot-0x19` / `eot-0x1A`), potem u32
initial-literal length pod `eot-0x1E`. Chodzi po strumieniu wstecz w
bit terms, zapisując output do przodu. Literal/match codes używają
prefix'ów variable-length, których tablice zostały zrekonstruowane
z oryginalnego dekodera.

**Port status:** byte-perfect. `tools/dta-validate.sh` weryfikuje
wszystkie 1782 wpisy z `DANE_02.DTA` przeciwko
`tools/dta-baseline.sha256` i przechodzi.

---

## 3. Atlas ANIM — `.wyc` (`ASSET_MAGIC_ANIM = 'ANIM'`)

Atlasy sprite'ów (aktorzy, propy, maski). Header 16 bajtów, potem
kilka parallel arrays, na końcu pixel bloby:

| Off | Size | Field |
|----:|-----:|---|
|   0 | 4 | `magic` = 'ANIM' (0x4D494E41) |
|   4 | 4 | (reserved) |
|   8 | 2 | `frame_count` |
|  10 | 2 | (reserved) |
|  12 | 2 | `flag_22` (alpha-plane / 8bpp click select bits) |
|  14 | 2 | offset do pixel-offset table (relative to file start) |

Za headerem trzy uint16 arrays długości `frame_count`:
- `off_widths[i]`  — szerokość klatki w pixelach
- `off_heights[i]` — wysokość klatki w pixelach
- `off_drawX[i]`   — draw-offset X (kompensacja foot-anchor)
- `off_drawY[i]`   — draw-offset Y

Potem `uint32_t pix_off_arr[frame_count]` z byte offsetami (relative
to file start) na pixel data każdej klatki. Sam pixel data to albo
raw 8bpp (`kind=2`) albo RLE-compressed (`kind=3`).

### 3.1 Kodowanie RLE (`kind=3`)
Header to 3 bajty:

| Off | Size | Field | Notes |
|----:|-----:|---|---|
|   0 | 1 | `fill_value` | zwykle 0 = transparent |
|   1 | 1 | `marker_A` | wprowadza run-of-fill |
|   2 | 1 | `marker_B` | wprowadza run-of-arbitrary-value |

Reguły streamu:
- `byte == marker_A` → następny byte = count-1; emit `count` × `fill_value`
- `byte == marker_B` → następny byte = count-1; następny = value; emit `count` × value
- w innym wypadku → emit byte raz

Dekoder: `DepackRleFrame` w `src/graphics.c`.

### 3.2 Bity `flag_22`
| Bit | Znaczenie |
|----:|---|
| 0 | alpha-plane source (użyj RGB12 quantization blit) |
| 1 | 8bpp click test (vs 1bpp packed) |

Routowane przez:
- entity flag 0x100 (`actor/render.c`) → BlitAlphaScaled mode 2
- click-mask flag pod entity[+0x14] (`stubs.c`)

---

## 4. Plik MASK — `.msk` (`ASSET_MAGIC_MASK = 'MASK'`)

Maski regionów klikalnych. Struktura podobna do ANIM (header +
per-frame metadata + pixel bloby) ale z 1-bpp packed pixel data —
`(w+7) & ~7 / 8` bajtów per row. Używane do hit-testingu
(`ClickHitTest` switch na flag_22 bit pod entity[+0x14]).

---

## 5. Walkability FILD — `.fld` (`ASSET_MAGIC_FILD = 'FILD'`)

Per-room bitmapa walkability. Pojedyncza klatka, 1-bpp packed,
rozmiar = walkable region tła. Header trzyma origin (`ox`, `oy`)
i stride dla partial-bitmap overlays.

| Off | Size | Field | Notes |
|----:|-----:|---|---|
|   0 | 4 | `magic` | 'FILD' (0x444C4946) |
|   4 | 2 | `w` | szerokość w pixelach |
|   6 | 2 | `h` | wysokość |
|   8 | 2 | `ox` | origin X na tle |
|  10 | 2 | `oy` | origin Y na tle |
|  12 | 2 | `stride` | bajtów per row (`= (w+7)/8`) |
|  14 | … | pixels | 1-bpp packed, LSB = leftmost |

Bit set → walkable. Czytane przez `is_walkable_at(x,y)` w `src/game.c`.

---

## 6. Tła PIC — `.pic`

Statyczne tła pokoi, 8-bpp paletted (typowo 640×400, czasem 640×480).
Format (consumer: `paint_rawb_pic`):

| Off | Size | Field | Notes |
|----:|-----:|---|---|
|     0 | 4 | `magic` | `'RAWB'` = 0x42574152 |
|     4 | 2 | `w` | szerokość (typowo 640) |
|     6 | 2 | `h` | wysokość (typowo 400; bywa 480) |
|     8 | 0x300 | header | misc flags + padding |
| 0x308 | w*h | pixels | flat 8bpp |

Prefix 0x308 jest traktowany jako opaque metadata przez port
(`paint_rawb_pic` czyta pixele tylko od offsetu 0x308). NOTE: wymiary
leżą PO 4-bajtowym magic `RAWB` (w@4, h@6) — wcześniejsza wersja tej
tabeli błędnie podawała w@0/h@2. Zweryfikowane renderowaniem assetów
w narzędziu viewer (PLAC.PIC = 640×400).

---

## 7. Paleta PAL — `.pal`

Flat 256×3 **RGB** triplety (768 bajtów total). Ładowana bezpośrednio do
`g_palette_rgb` i aplikowana przez `InstallPalette` która rebuilduje
też LUT-y RGB12 quantization dla alpha-plane rendering. NOTE: bajty to
R,G,B (nie BGR) — `plat_video_present` mapuje e[0]→R, e[1]→G, e[2]→B
(`src/platform/sdl/video_sdl.c`). Wcześniejsza wersja błędnie pisała BGR.

```
+--- byte 0 ---+--- byte 1 ---+--- byte 2 ---+ ...
| R[0]         | G[0]         | B[0]         | R[1] ...
```

Oryginał czasami fadem ładuje partial paletę (rzędy N..N+M tylko);
mapuje to się na `InstallPalette(rgb, first)` gdzie `first` wybiera
starting index.

---

## 8. Bitmap font Futura.30

Custom bitmap font. Ładowany raz przy starcie (`PreloadCommonAssets`),
parsowany przez `ParseFutFontFile` (`src/font.c`). Format to
Big-Endian header + per-glyph descriptor array + glyph bitmaps
(1-bpp packed dla system text, color-plane dla menu text).

Używany przez:
- `RenderTextLineToBuffer` dla op 0x09 PRINT i dialog speech
- `MeasureTextLine` dla layoutu

Glyph table to standardowy zakres Mazovia ASCII używany w polskich
grach z DOS-erki (kody 0x80+).

---

## Reference implementations

| Format | Reader | Writer (tool) |
|---|---|---|
| DTA | `src/archive.c` | `tools/dta-extract.c` |
| PKv2 | `src/depack.c` | `tools/pkv2-depack.c` |
| ANIM | `src/assets.c` (`LoadAssetFromDtaBase`) | — (decoder game-time) |
| MASK | `src/assets.c` + `ClickHitTest` | — |
| FILD | `src/assets.c` + `is_walkable_at` | — |
| PIC | `paint_rawb_pic` w `src/game.c` | — |
| PAL | `InstallPalette` w `src/graphics.c` | — |
| Font | `src/font.c` | — |

Dla binary-faithful asset round-trippingu (np. ekstrakcja klatek do
PNG, repakowanie), zbuduj standalone tools:

```bash
make tools
./dist/dta-extract /Volumes/WACKI_1/Dane_02.dta out/
./dist/pkv2-depack out/EBEK.WYC ebek.raw
```
