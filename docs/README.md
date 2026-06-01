# Wacki — dokumentacja techniczna

Skarbnica wiedzy o tym jak Wacki działa pod maską. Port jest
zrekonstruowany ze zdekompilowanej oryginalnej binarki, więc każdy
dokument tu odzwierciedla zarówno **jak engine był zaprojektowany w
1997** jak i **jak nasz port to odtwarza**.

## Mapa dokumentów

| Dokument | O czym |
|---|---|
| [architecture.md](architecture.md) | Mapa modułów, główna pętla, scene lifecycle, audio, save, build |
| [script-vm.md](script-vm.md) | Główna maszyna wirtualna `.scr` — pełna tablica 78 opcode'ów |
| [per-entity-vm.md](per-entity-vm.md) | Per-entity VM (33 opcode'y) — to co tickają NPC i animowane sprite'y |
| [audio-pipeline.md](audio-pipeline.md) | Pełna ścieżka SFX: `[sampl]` parser → frame triggers → mixer |
| [entity-system.md](entity-system.md) | Struct `Entity`, kategorie (`kind`), lista renderera/clicków, lifecycle |
| [pe-loader.md](pe-loader.md) | Trick z embedowaniem `WACKI.EXE` jako passive image |
| [asset-format.md](asset-format.md) | Binarna specyfikacja: DTA / PKv2 / ANIM / MASK / FILD / PIC / PAL |
| [flic-decoder.md](flic-decoder.md) | Cutscene'y — AAFLC w AVI: dekoder, audio path, pacing |
| [save-file.md](save-file.md) | `Wacki.sav` — format na dysku, atomic write, slot lifecycle, F5/F9 |
| [glossary.md](glossary.md) | Polskie terminy z silnika i scenariuszy — szybkie tłumaczenie |
| [health-bar-depletion.md](health-bar-depletion.md) | Open research note — mechanizm odejmowania życia |

## Konwencje

- **Adresy `0x0040xxxx`** to oryginalne VA z `WACKI.EXE` (PE base 0x00400000) —
  używane tylko w opisie PE loadera (gdzie pokazujemy konkretne tablice danych
  do których engine sięga).
- **Termin „komnata"** to oryginalne (polskie) słowo na scenę/pomieszczenie.
  „Etap" to stage/chapter. Pełen słownik: [glossary.md](glossary.md).
- **Per-frame tick** = jedna iteracja głównej pętli gry, target 30 FPS.
- **Bytecode** w opisach VM zapisany w formie `[op:u8][len:u8][operands…]`,
  gdzie `len` to długość instrukcji w **dword'ach** (4 bajty każdy).

## Skąd zaczynać

Czytaj w tej kolejności jeśli pierwszy raz patrzysz na kod:

1. **[architecture.md](architecture.md)** — wysoki poziom, zrozum przepływ
2. **[entity-system.md](entity-system.md)** — co to entity i jak wygląda struct
3. **[script-vm.md](script-vm.md)** lub **[per-entity-vm.md](per-entity-vm.md)**
   — który VM Cię interesuje
4. **[asset-format.md](asset-format.md)** — gdy dotykasz formatu danych
5. **[pe-loader.md](pe-loader.md)** — gdy zastanawiasz się czemu kod
   pyta o adresy `0x0042xxxx`
