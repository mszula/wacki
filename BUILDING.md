# Budowanie ze źródeł

Każdy build wymaga `WACKI.EXE` z oryginalnej płyty w katalogu `data/`.
Narzędzie `tools/embed-pe-data` wycina z niej dwa segmenty (`.rdata` +
`.data`) i zaszywa je w binarce portu; bez tego pliku build nie ruszy.

---

## PC (macOS / Linux / Windows)

Projekt buduje się standardowym `make`.

Wymagania:

- kompilator C (gcc lub clang)
- biblioteka SDL2 z plikami nagłówkowymi
- `WACKI.EXE` w `data/` (patrz wyżej)

Instalacja SDL2:

| System              | Polecenie                                          |
|---------------------|----------------------------------------------------|
| macOS (Homebrew)    | `brew install sdl2`                                |
| Debian / Ubuntu     | `sudo apt install libsdl2-dev`                     |
| Fedora              | `sudo dnf install SDL2-devel`                      |
| Arch                | `sudo pacman -S sdl2`                              |
| Windows (MSYS2)     | `pacman -S mingw-w64-x86_64-{gcc,SDL2} make`       |

Budowanie:

```bash
cp /sciezka/do/plyty/WACKI.EXE data/
cp /sciezka/do/plyty/Dane_*.dta data/

make all
./dist/wacki
```

Wynikowa binarka trafia do `dist/wacki` (lub `dist\wacki.exe` na
Windowsie).

Dla artefaktu releasowego bez zależności od `libSDL2` / `SDL2.dll`
linkuj SDL2 statycznie:

```bash
make all STATIC_SDL2=1
```

---

## Handheld — Miyoo Mini Plus (OnionOS)

Build cross-kompilowany dla Miyoo Mini Plus odbywa się w kontenerze
Docker (wymagany Docker Desktop lub `docker.io`):

```bash
cp /sciezka/do/plyty/WACKI.EXE data/

make miyoo
./tools/pack-miyoo.sh
```

Wynik: `dist/wacki-miyoo.zip` zawierający gotową strukturę OnionOS
Ports do przeniesienia na kartę pamięci urządzenia.

Toolchain pobiera się automatycznie jako obraz Docker
(`bqcuongas/sdl2-miyoo`); na hoście wystarczy `make`, `docker`
i `WACKI.EXE` w `data/`.

---

## Handheld — Anbernic i PortMaster

Cross-build dla aarch64 + armhf odbywa się w kontenerach Docker
(Debian bullseye — szeroka zgodność glibc; wymagana obsługa
`linux/arm64` i `linux/arm/v7`, którą Docker Desktop na Apple Silicon
ma od ręki):

```bash
cp /sciezka/do/plyty/WACKI.EXE data/

./tools/build-portmaster.sh        # buduje obie architektury
./tools/pack-portmaster.sh         # składa paczkę
```

Pojedynczą architekturę zbudujesz przez `./tools/build-portmaster.sh
aarch64` lub `armhf`.

Wynik: `dist/Wacki.zip` — gotowa paczka PortMaster (skrypt startowy,
`port.json`, obie binarki, miejsce na dane). SDL2 jest linkowane
dynamicznie: binarka korzysta z biblioteki dostarczanej przez
PortMaster w czasie uruchomienia (właściwy sterownik wyświetlania
per-urządzenie).

## Konsola — PlayStation 2

Cross-build przez obraz Docker **ps2dev** (toolchain
`mips64r5900el-ps2-elf` + SDL2-PS2). Na hoście wystarczy `docker`:

```bash
cp /sciezka/do/plyty/WACKI.EXE data/

./tools/build-ps2.sh        # buduje dist/wacki-ps2.elf
./tools/pack-ps2.sh         # składa dist/wacki-ps2.zip (ELF + README)
```

Wynik: `dist/wacki-ps2.zip` — bootowalny ELF z instrukcją uruchomienia
(`uLaunchELF` na sprzęcie z FreeMcBoot/FreeDVDBoot, albo „Boot ELF"
w PCSX2). `WACKI_STRIP=1 ./tools/build-ps2.sh` usuwa symbole do wydania;
domyślnie zostają (przydatne do debugowania przez PINE/PCSX2).

Tryb wideo (PAL / NTSC / 480p) wybiera się **w runtime** — ekranem wyboru
przy starcie ELF-a (domyślnie NTSC). Bootowalny obraz ISO z danymi w środku —
do PCSX2 bez konfiguracji HostFS — składa `./tools/build-ps2-iso.sh`.
