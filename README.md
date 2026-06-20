# Wacki: Kosmiczna rozgrywka

[![Latest release](https://img.shields.io/github/v/release/mszula/wacki?sort=semver&style=flat-square&label=release&color=6f42c1)](../../releases/latest)
[![Downloads](https://img.shields.io/github/downloads/mszula/wacki/total?style=flat-square&label=downloads&color=44cc11&logo=github&logoColor=white)](../../releases)
[![Platforms](https://img.shields.io/badge/platforms-macOS%20%C2%B7%20Linux%20%C2%B7%20Windows%20%C2%B7%20Android%20%C2%B7%20Miyoo%20%C2%B7%20PortMaster%20%C2%B7%20PS2-1f6feb?style=flat-square)](../../releases)
[![Language](https://img.shields.io/badge/C99-SDL2-555555?style=flat-square&logo=c&logoColor=white)](#)
[![License](https://img.shields.io/github/license/mszula/wacki?style=flat-square&label=license&color=blue)](LICENSE)
[![Stars](https://img.shields.io/github/stars/mszula/wacki?style=flat-square&label=stars&logo=github&logoColor=white)](../../stargazers)
[![Website](https://img.shields.io/badge/website-mszula.github.io%2Fwacki-0a66c2?style=flat-square&logo=githubpages&logoColor=white)](https://mszula.github.io/wacki/)

A faithful, cross-platform port of **Wacki: Kosmiczna rozgrywka**
(1998) — a Polish point-and-click adventure — reconstructed from the
decompiled `WACKI.EXE`. It runs on desktop (macOS / Linux / Windows),
on Android phones and tablets, on handhelds (Miyoo, plus Anbernic and
dozens more via PortMaster), and on the PlayStation 2.
The game is in Polish, set in Poland, and was made in Poland; for that
reason the rest of this README is in Polish too.

![Wacki: Kosmiczna rozgrywka](docs/img/wacki.png)

> **Najszybszy start:** wejdź na **[mszula.github.io/wacki](https://mszula.github.io/wacki/)**
> — wybierz swój sprzęt i przejdź ilustrowaną instrukcję krok po kroku.
> Ten README to skrócona wersja tego samego.

## 👽 O grze

**Wacki: Kosmiczna rozgrywka** to polska gra przygodowa typu
point-and-click, wydana 1 lipca 1998 roku przez studio
Seven Stars Multimedia na komputery z systemem Microsoft Windows.

Akcja rozgrywa się na polskim osiedlu, gdzie dwaj nastolatkowie —
Franc i Edek — spotykają kosmitę Aargha. Obcy prosi ich o pomoc
w odnalezieniu części zagubionego urządzenia **ACME** (Atomowego
Czasoprzestrzennego Modyfikatora Energii), które uległo rozpadowi.
Gracz steruje dwoma bohaterami, eksplorując kolejne lokacje na
osiedlu i rozwiązując zagadki — klika myszą na obiekty i postacie,
zbiera przedmioty, prowadzi dialogi. Gra zawiera liczne elementy
humorystyczne, często opierające się na polskiej kulturze i
absurdalnych sytuacjach.

Niniejszy projekt jest portem silnika gry — odtworzonym ze
zdekompilowanej oryginalnej binarki `WACKI.EXE`. Repozytorium
**nie zawiera materiałów z gry**; aby zagrać, potrzebna jest własna
kopia oryginalnej płyty.

## ✅ Status portu

Port jest funkcjonalnie kompletny — gra przechodzi się od intro
do napisów końcowych z większością mechanik i detali oryginału:
sterowanie dwoma postaciami, system dialogów i dymków, ekwipunek,
zapis i wczytanie stanu (sloty + quick save / quick load), pełna
animacja postaci i obiektów, dźwięk (muzyka, SFX, kwestie mówione),
intra i cutscenki AVI, panel z paskiem życia, menu opcji.

Szczegóły techniczne — jak silnik działa pod maską, format danych,
maszyna wirtualna skryptów, dekoder animacji FLIC, pipeline audio
itd. — znajdują się w katalogu [`docs/`](docs/). To dokumentacja
**budowy gry**, nie roadmapa portu.

---

## ▶️ Wybierz platformę

Gotowe paczki są w zakładce [**Releases**](../../releases). Pełne,
ilustrowane przewodniki (pobieranie → dane → sterowanie →
rozwiązywanie problemów) żyją na stronie — linki w tabeli. Skrócone
instrukcje masz tu niżej, rozwijane per platforma.

Do gry zawsze potrzebne są jeszcze pliki `Dane_*.dta` z **oryginalnej
płyty** — port ich nie zawiera.

| Platforma | Paczka z Releases | Przewodnik krok po kroku |
|---|---|---|
| 🪟 **Windows** 10/11 (64-bit) | `wacki-windows-x86_64.zip` | [strona — Windows](https://mszula.github.io/wacki/platformy/windows.html) |
| 🍎 **macOS** (Apple Silicon) | `wacki-macos-arm64.tar.gz` | [strona — macOS](https://mszula.github.io/wacki/platformy/macos.html) |
| 🐧 **Linux** x86_64 | `wacki-linux-x86_64.tar.gz` | [strona — Linux](https://mszula.github.io/wacki/platformy/linux.html) |
| 🤖 **Android** 7.0+ | `wacki-android.apk` | [strona — Android](https://mszula.github.io/wacki/platformy/android.html) |
| 🕹️ **Miyoo** (OnionOS) | `wacki-miyoo.zip` | [strona — Miyoo](https://mszula.github.io/wacki/platformy/miyoo.html) |
| 🎮 **Anbernic / PortMaster** | `wacki-portmaster.zip` | [strona — PortMaster](https://mszula.github.io/wacki/platformy/portmaster.html) |
| 📀 **PlayStation 2** | `wacki-ps2.zip` | [strona — PS2](https://mszula.github.io/wacki/platformy/ps2.html) |

<details>
<summary><b>🖥️ PC — macOS / Linux / Windows</b></summary>

### Wymagania

- gotowa binarka portu z zakładki [Releases](../../releases) dla
  Twojego systemu: macOS (Apple Silicon), Linux x86_64 lub
  Windows 10/11 x86_64
- pliki danych z oryginalnej płyty: `Dane_*.dta`

### Uruchomienie

1. Pobierz archiwum dla swojego systemu z [Releases](../../releases)
   i rozpakuj w dowolnym katalogu.
2. Skopiuj pliki `Dane_*.dta` z oryginalnej płyty w jedno z miejsc,
   które gra przeszukuje (patrz niżej). Najprościej:
   - **macOS**: folder `data/` **obok** `Wacki.app`, albo prawy klik
     na `Wacki.app` → **Show Package Contents** →
     `Contents/Resources/data/`
   - **Linux / Windows**: podkatalog `data/` obok binarki
3. Uruchom grę:
   - **macOS**: dwuklik `Wacki.app` (przy pierwszym razie zobacz
     „Pierwsze uruchomienie na macOS" niżej)
   - **Linux**: `./wacki`
   - **Windows**: dwuklik `wacki.exe`

Gra przeszukuje (pierwszy trafiony wygrywa): `WACKI_PATH` → katalog
uruchomienia → `./data/` → katalog binarki (lub folder obok
`Wacki.app`) → napędy CD / podłączone dyski. Jeśli nic nie znajdzie,
pokaże okno wyboru folderu. Wielkość liter w nazwach nie ma znaczenia.

#### Pierwsze uruchomienie na macOS

Binarki nie są podpisane certyfikatem Apple (port jest darmowy, a
Apple Developer Program kosztuje 99 $/rok), więc Gatekeeper zablokuje
`Wacki.app` przy pierwszym uruchomieniu komunikatem typu *„Apple nie
może sprawdzić…"*. To jednorazowe — najprościej:

- **Ustawienia systemowe → Prywatność i bezpieczeństwo** → przewiń na
  dół do sekcji *Security* → przy wpisie *„Wacki.app was blocked…"*
  kliknij **Open Anyway** i potwierdź.
- albo z Terminala (każda wersja macOS), w katalogu z grą:
  ```bash
  xattr -dr com.apple.quarantine Wacki.app
  ```

Pełny przewodnik (3 sposoby, w tym uwaga o macOS 15 Sequoia):
[strona — macOS](https://mszula.github.io/wacki/platformy/macos.html).

### Sterowanie

| Czynność              | Wejście              |
|-----------------------|----------------------|
| Ruch kursora          | mysz                 |
| Kliknięcie lewe       | LPM                  |
| Kliknięcie prawe      | PPM                  |
| Wyjście z gry         | `ESC`                |
| Quick-save (slot 0)   | `F5`                 |
| Quick-load (slot 0)   | `F9`                 |
| Menu pauzy            | `F12`                |
| Przełącz postać       | `SPACE`              |
| Pełny ekran           | `F11`                |

### Tryb wyświetlania

Przy **pierwszym uruchomieniu** gra zapyta jak chcesz grać — *Pełny
ekran*, *Okno 2×* albo *Okno 1×*. Wybór jest zapamiętany (plik
`wacki.cfg` obok gry), więc pytanie pojawia się tylko raz.

- **F11** — przełącza pełny ekran ↔ okno
- **rozciągnij okno za róg** — płynnie zmienia powiększenie; gra
  renderuje wewnętrznie 640×480 i skaluje z zachowaniem proporcji

Żeby znów zobaczyć pytanie o tryb — skasuj `wacki.cfg`. Zaawansowani
mogą wymusić tryb z linii poleceń (sekcja „⚙️ Opcje uruchomienia"
niżej) — flagi mają pierwszeństwo nad zapamiętanym wyborem.

</details>

<details>
<summary><b>📱 Android (telefony i tablety, 7.0+)</b></summary>

Gra renderuje się w wyśrodkowanym kadrze 4:3; wolne miejsce po bokach
(na szerokim ekranie trzymanym poziomo) wypełnia półprzezroczyste
sterowanie dotykowe: wirtualna gałka po lewej, a po prawej dwa
przyciski — duży (lewy klik) i mniejszy nad nim (prawy klik). Można
też grać samym dotykiem ekranu albo padem Bluetooth/USB.

### Wymagania

- telefon lub tablet z **Androidem 7.0** (API 24) lub nowszym
- plik **`wacki-android.apk`** z [Releases](../../releases) (podpisany
  kluczem release — instaluje się bezpośrednio)
- pliki danych z oryginalnej płyty: `Dane_*.dta`

### Instalacja

1. Pobierz `wacki-android.apk` z [Releases](../../releases) i
   zainstaluj. To sideload, więc Android poprosi o zgodę na instalację
   z tego źródła.
2. Skopiuj pliki `Dane_*.dta` z oryginalnej płyty gdziekolwiek na
   telefon (np. osobny folder w „Pliki" / Downloads). **Nie pakuj ich
   do APK.**
3. Uruchom Wacki → ekran powitalny → **„Wskaż folder z plikami gry"** →
   wskaż ten folder. Gra **czyta pliki wprost z tego miejsca** (przez
   Storage Access Framework) — nic nie jest kopiowane, więc start jest
   natychmiastowy. Dostęp jest utrwalony; folder z danymi musi
   pozostać na miejscu.

Gra zawsze startuje w orientacji poziomej. Zapisy i `wacki.cfg` lądują
w pamięci wewnętrznej aplikacji.

### Sterowanie

| Czynność                          | Wejście                                          |
|-----------------------------------|--------------------------------------------------|
| Ruch kursora                      | dotyk ekranu · gałka (lewy panel)                |
| Kliknięcie lewe                   | dotyk w grze · duży przycisk (prawy panel)       |
| Kliknięcie prawe / przełącz postać| mały przycisk (prawy panel) · dotyk 2 palcami · **Tab** |
| Menu pauzy                        | przycisk **Wstecz**                              |

Szczegóły portu (SAF, nakładka dotykowa, ograniczenia emulatorów) i
budowanie: [`android/README.md`](android/README.md).

</details>

<details>
<summary><b>🎮 Handheld — Miyoo Mini Plus i pokrewne (OnionOS)</b></summary>

### Wymagania

- urządzenie z firmware'em **OnionOS 4.2** lub nowszym; stock firmware
  nie jest wspierane (różni się układ katalogów i mechanizm
  uruchamiania portów)
- archiwum `wacki-miyoo.zip` z [Releases](../../releases)
- pliki danych z oryginalnej płyty: `Dane_*.dta`

Wspierane: **Miyoo Mini Plus** (platforma referencyjna), **Miyoo
Mini** (pin-kompatybilny, działa bez zmian) oraz inne handheldy z
firmware'em zgodnym z OnionOS Ports. Dla Anbernica i pokrewnych jest
osobna paczka PortMaster (niżej).

### Instalacja

Archiwum jest zgodne ze standardem OnionOS Ports.

1. Rozpakuj zawartość archiwum bezpośrednio w katalogu głównym karty
   pamięci. Folder `Roms/` z archiwum scali się z istniejącym `Roms/`.
2. Skopiuj pliki `Dane_*.dta` z oryginalnej płyty do katalogu:
   ```
   Roms/PORTS/Games/Wacki/data/
   ```
3. Włóż kartę, włącz urządzenie. W menu wybierz **Ports → Adventure
   → Wacki**.

### Sterowanie

| Czynność              | Przycisk             |
|-----------------------|----------------------|
| Ruch kursora          | krzyżak              |
| Kliknięcie lewe       | **A**                |
| Kliknięcie prawe      | **B**                |
| Menu pauzy            | **START**            |
| Quick-load            | **L1** / **L2**      |
| Quick-save            | **R1** / **R2**      |
| Wyjście z gry         | **MENU**             |

Krzyżak przyspiesza w miarę przytrzymania — krótkie naciśnięcia służą
do precyzyjnego pozycjonowania kursora, dłuższe trzymanie do szybkiego
przemieszczania go po ekranie.

</details>

<details>
<summary><b>🕹️ Handheld — Anbernic i PortMaster</b></summary>

Osobna paczka **PortMaster** działa na większości nowoczesnych
handheldów Anbernic — a przy okazji na całym ekosystemie PortMastera
(Powkiddy, TrimUI, RGB30, Miyoo Flip i dziesiątki innych).

### Wymagania

- firmware z **PortMasterem**: muOS, ROCKNIX, Knulli, ArkOS, JELOS lub
  Batocera (stock firmware Anbernica zwykle go nie ma)
- archiwum `wacki-portmaster.zip` z [Releases](../../releases)
- pliki danych z oryginalnej płyty: `Dane_*.dta`

Jedna paczka, dwie binarki: **aarch64** (Allwinner H700, Rockchip
RK3566/RK3399) i **armhf** (seria RG351 / RK3326 oraz oryginalny RG35XX
przez Koriki/Batocera). Pełna lista urządzeń:
[strona — PortMaster](https://mszula.github.io/wacki/platformy/portmaster.html).

### Instalacja

1. Wgraj `wacki-portmaster.zip` przez PortMaster („Install from zip"),
   albo wrzuć archiwum do folderu `autoinstall` PortMastera.
2. Skopiuj pliki `Dane_*.dta` z oryginalnej płyty do katalogu portu:
   ```
   /roms/ports/Wacki/data/
   ```
3. Uruchom z menu **Ports → Wacki**.

### Sterowanie

| Czynność              | Przycisk             |
|-----------------------|----------------------|
| Ruch kursora          | gałka / krzyżak      |
| Kliknięcie lewe       | **A** (dolny)        |
| Kliknięcie prawe      | **B** (prawy)        |
| Menu pauzy            | **START**            |
| Quick-load            | **L1**               |
| Quick-save            | **R1**               |
| Wyjście z gry         | **START + SELECT**   |

Przyciski w pozycjach SDL: **A** = dolny, **B** = prawy. Na padach w
układzie Nintendo (większość Anbernica) są one fizycznie oznaczone
jako **B** i **A** — kieruj się pozycją, nie napisem.

</details>

<details>
<summary><b>📀 Konsola — PlayStation 2</b></summary>

Renderowanie sprzętowe gsKit, dźwięk przez audsrv, sterowanie
DualShockiem (lewa gałka = kursor) lub myszą USB. Paczka
`wacki-ps2.zip` zawiera bootowalny plik ELF.

### Wymagania

- PS2 z homebrew — **FreeMcBoot** / **FreeDVDBoot** + `uLaunchELF` —
  albo emulator **PCSX2**
- pendrive (FAT32) na pliki gry (na sprzęcie) lub HostFS (w PCSX2)
- oryginalne pliki `Dane_*.dta` (port ich nie zawiera)

### Instalacja

1. Rozpakuj `wacki-ps2.zip` — w środku folder `wacki/` z plikiem
   `wacki-ps2.elf` i podkatalogiem `data/`.
2. Skopiuj do `wacki/data/` pliki `Dane_*.dta` z oryginalnej płyty.
3. Wrzuć cały folder `wacki/` na pendrive, tak by powstało
   `mass:/wacki/wacki-ps2.elf` oraz `mass:/wacki/data/Dane_*.dta`.
4. Uruchom `mass:/wacki/wacki-ps2.elf` przez `uLaunchELF`.

W PCSX2: **System → Boot ELF** i wskaż `wacki-ps2.elf` (dane przez
HostFS), albo zbuduj bootowalny obraz ISO — `./tools/build-ps2-iso.sh`.

### Sterowanie

| Czynność              | Przycisk              |
|-----------------------|-----------------------|
| Ruch kursora          | lewa gałka / mysz USB |
| Kliknięcie lewe       | **✕**                 |
| Kliknięcie prawe      | **○** (kółko)         |
| Menu pauzy            | **START**             |

</details>

<details>
<summary><b>⚙️ Opcje uruchomienia (zaawansowane, PC)</b></summary>

Wybrane opcje można podać z linii poleceń lub przez zmienne
środowiskowe. CLI ma pierwszeństwo nad ENV.

**Display:**

| Flaga                  | Zmienna środowiskowa     | Działanie                                        |
|------------------------|--------------------------|--------------------------------------------------|
| `--scale N`            | `WACKI_SCALE=N`          | okno powiększone N-krotnie (gra wewnętrznie 640×480) |
| `--scaler MODE`        | `WACKI_SCALER=MODE`      | jakość skalowania: `nearest`, `linear`, `best`   |
| `--fullscreen` / `-f`  | `WACKI_FULLSCREEN=1`     | start w trybie pełnoekranowym (F11 przełącza w grze) |

**Dane gry:**

| Flaga                  | Zmienna środowiskowa     | Działanie                                        |
|------------------------|--------------------------|--------------------------------------------------|
| —                      | `WACKI_PATH=...`         | ścieżka do katalogu z `Dane_*.dta` (default: `./data/`) |

**Rozgrywka:**

| Flaga                  | Zmienna środowiskowa     | Działanie                                        |
|------------------------|--------------------------|--------------------------------------------------|
| `--start-stage N`      | `WACKI_START_STAGE=N`    | start od razu od etapu **N** (1..5), pomija menu i intro |

**Logowanie:**

Build release jest **cichy** — domyślnie tylko ostrzeżenia i błędy (`WARN`),
żeby wydany port nie zalewał użytkownika logami. Build dev (`make debug`,
`-DWACKI_VERBOSE`) domyślnie pokazuje `INFO`. Poziom można zmienić w locie:

| Flaga                  | Zmienna środowiskowa            | Działanie                                          |
|------------------------|---------------------------------|----------------------------------------------------|
| `-v` / `--verbose`     | `WACKI_LOG_LEVEL=info`/`trace`  | więcej logów (`TRACE` wymaga buildu z `-DWACKI_VERBOSE`) |
| `-q` / `--quiet`       | `WACKI_LOG_LEVEL=error`         | tylko błędy                                         |

`WACKI_LOG_LEVEL` przyjmuje `trace\|debug\|info\|warn\|error` i pozwala włączyć
logi na wydanym buildzie (np. bundle macOS) bez rekompilacji; flaga `-v`/`-q`
ma pierwszeństwo nad zmienną.

Przykłady:

```bash
./wacki --scale 2 --scaler linear   # okno 1280×960, skalowanie liniowe
./wacki --fullscreen                # pełny ekran, letterbox 640×480
./wacki --start-stage 3             # skok prosto do etapu 3 (kiosk)
```

Pozostałe niżej-poziomowe / dev opcje (`--headless`, `--play-avi`,
`--test-cutscenes`, `--no-pacing`, `WACKI_INPUT_DEBUG`) udokumentowane
są w [`docs/architecture.md`](docs/architecture.md#11-flagi-runtime).

</details>

---

## 💾 Zapis stanu

Gra obsługuje:

- **10 nazwanych slotów** dostępnych z menu Sejw / Load — każdemu
  można nadać własną nazwę, slot trzyma rozdział i pozycję
  w rozgrywce
- **Quick-save / quick-load** — natychmiastowy zapis i wczytanie ze
  slotu 0 z poziomu gry (na PC: `F5` zapis, `F9` odczyt; na
  handheldzie: `R1` zapis, `L1` odczyt)

🔁 **Format zapisu jest byte-identyczny z oryginałem z 1998 r.**
Save zrobiony w porcie da się załadować w oryginalnej binarce
`WACKI.EXE` (i odwrotnie) — można dograć w porcie etap zaczęty
w oryginalnej grze pod Windows i kontynuować na handheldzie albo
wręcz przeciwnie.

Stan zapisu to jeden blok danych (format `Wacki.sav`), trzymany tam,
gdzie danej platformie najbliżej:

- **PC** — plik `Wacki.sav` obok binarki
- **Miyoo** — `Roms/PORTS/Games/Wacki/`; **PortMaster** — `ports/Wacki/`
- **Android** — pamięć wewnętrzna aplikacji
- **PS2** — karta pamięci (z własną ikoną i nazwą zapisu)

Na platformach plikowych zapis jest atomowy (tymczasowy plik +
rename), więc zanik zasilania lub crash w trakcie save'a nie psuje
istniejących slotów.

---

## 🔨 Budowanie ze źródeł

Wszystkie instrukcje — wymagania, instalacja SDL2, polecenia `make`
i buildy w kontenerach Docker dla wszystkich platform (PC, Miyoo,
Anbernic / PortMaster, PlayStation 2, Android) — są w osobnym pliku:

➡️ [**BUILDING.md**](BUILDING.md)

Dokumentacja wnętrza silnika (VM skryptów, format danych, dekoder
FLIC, pipeline audio itd.) jest w [`docs/`](docs/) — zacznij od
[`docs/README.md`](docs/README.md).

---

## 🔍 Assets Explorer

Chcesz zajrzeć do środka plików gry? **Wacki Assets Explorer** to osobne,
otwarte narzędzie (Nuklear + SDL2), które otwiera archiwa `Dane_*.dta`
i pokazuje każdy zasób — sprite'y, animacje, palety, dźwięki, cutscenki
i skrypty — z eksportem do PNG / GIF / WAV. Działa na Linuksie, Windowsie
i macOS.

**Pobierz** (od wydania `assets-explorer-v1.0.0`):

| Platforma | Paczka |
|---|---|
| 🐧 **Linux** x86_64 | [`wacki-assets-explorer-linux-x86_64.tar.gz`](https://github.com/mszula/wacki/releases/download/assets-explorer-v1.0.0/wacki-assets-explorer-linux-x86_64.tar.gz) |
| 🪟 **Windows** x86_64 | [`wacki-assets-explorer-windows-x86_64.zip`](https://github.com/mszula/wacki/releases/download/assets-explorer-v1.0.0/wacki-assets-explorer-windows-x86_64.zip) |
| 🍎 **macOS** (Apple Silicon) | [`wacki-assets-explorer-macos-arm64.zip`](https://github.com/mszula/wacki/releases/download/assets-explorer-v1.0.0/wacki-assets-explorer-macos-arm64.zip) |

- 🌐 Strona: **[mszula.github.io/wacki/eksplorer.html](https://mszula.github.io/wacki/eksplorer.html)**
- 📂 Kod, instrukcja budowania i szczegóły: **[`assets-explorer/`](assets-explorer/README.md)**

---

## Zgłaszanie błędów

Coś się sypie? Otwórz issue:

[**github.com/mszula/wacki/issues**](https://github.com/mszula/wacki/issues/new)

W zgłoszeniu pomogą:

- **wersja portu** — pierwsza linia logu uruchomieniowego (`[info/wacki] Wacki port vX.Y.Z…`)
- **platforma** — macOS / Linux / Windows / Android / Miyoo / Anbernic (PortMaster) / PS2
  + wersja systemu/firmware'u
- **kroki reprodukcji** — co robiłeś bezpośrednio przed crashem
- **`wacki.log`** — silnik loguje na stderr; gdzie szukać:
  - **Windows**: plik `wacki.log` automatycznie obok `wacki.exe`
    (binarka jest GUI-subsystem więc stderr leci do pliku)
  - **macOS / Linux**: stderr w terminalu albo `Console.app`
    (uruchom z terminala `Wacki.app/Contents/MacOS/Wacki` żeby zobaczyć logi)
  - **Miyoo**: `Roms/PORTS/Games/Wacki/wacki.log`
    (zapisywany przez wrapper `wacki.sh`)
  - **Anbernic / PortMaster**: `ports/Wacki/log.txt`
    (zapisywany przez `Wacki.sh`)
  - **Android**: `adb logcat -s wacki` (silnik loguje do systemowego logcata)
  - **PS2**: konsola IOP (na sprzęcie) lub logi PCSX2 przy uruchomieniu z HostFS
- **screenshot** — jeśli problem jest wizualny (popsuta paleta,
  brakujący sprite, glitch animacji)

Jeśli to feature request lub pytanie o przyszłe platformy / mody —
też smiało, ale w issue zaznacz `[feature]` lub `[question]` w tytule.

---

## ☕ Wsparcie

Wacki portuję po godzinach, za darmo i z czystej miłości do tej
zwariowanej gry. Jeśli port przywołał uśmiech (albo łezkę nostalgii)
i chcesz dorzucić się na kawę — to paliwo do kolejnych poprawek
i portów polskiej klasyki:

➡️ [**suppi.pl/mszula**](https://suppi.pl/mszula)

Dzięki! 🙌

---

## Licencja i prawa

Kod portu silnika udostępniony jest na licencji **GNU General
Public License v3.0** — pełny tekst w pliku [`LICENSE`](LICENSE).
W skrócie: możesz kod kopiować, modyfikować i redystrybuować, ale
każda pochodna praca musi pozostać pod tą samą licencją, ze źródłami
udostępnionymi odbiorcom.

Copyright © 2026 Mateusz Szuła.

Port silnika jest dziełem niezależnym i nie zawiera materiałów
chronionych prawem autorskim z oryginalnej gry. Pliki danych
(`Dane_*.dta`, `WACKI.EXE`) pozostają własnością ich twórców
(Seven Stars Multimedia, TopWare Interactive Polska) i nie są
dystrybuowane wraz z tym repozytorium — licencja GPL v3 obejmuje
wyłącznie kod portu, nie zawartość oryginalnej gry.

## Podziękowania

- **Seven Stars Multimedia** — twórcy oryginalnej gry (1998)
- TopWare Interactive Polska — wydawca oryginalny
