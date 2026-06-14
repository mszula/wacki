# Wacki: Kosmiczna rozgrywka

[![Latest release](https://img.shields.io/github/v/release/mszula/wacki?sort=semver&style=flat-square&label=release&color=6f42c1)](../../releases/latest)
[![Downloads](https://img.shields.io/github/downloads/mszula/wacki/total?style=flat-square&label=downloads&color=44cc11&logo=github&logoColor=white)](../../releases)
[![Platforms](https://img.shields.io/badge/platforms-macOS%20%C2%B7%20Linux%20%C2%B7%20Windows%20%C2%B7%20Miyoo%20%C2%B7%20PortMaster%20%C2%B7%20PS2-1f6feb?style=flat-square)](../../releases)
[![Language](https://img.shields.io/badge/C99-SDL2-555555?style=flat-square&logo=c&logoColor=white)](#)
[![License](https://img.shields.io/github/license/mszula/wacki?style=flat-square&label=license&color=blue)](LICENSE)
[![Stars](https://img.shields.io/github/stars/mszula/wacki?style=flat-square&label=stars&logo=github&logoColor=white)](../../stargazers)
[![Website](https://img.shields.io/badge/website-mszula.github.io%2Fwacki-0a66c2?style=flat-square&logo=githubpages&logoColor=white)](https://mszula.github.io/wacki/)

A faithful, cross-platform port of **Wacki: Kosmiczna rozgrywka**
(1998) — a Polish point-and-click adventure — reconstructed from the
decompiled `WACKI.EXE`. It runs on desktop (macOS / Linux / Windows),
on handhelds (Miyoo, plus Anbernic and dozens more via PortMaster),
and on the PlayStation 2.
The game is in Polish, set in Poland, and was made in Poland; for that
reason the rest of this README is in Polish too.

![Wacki: Kosmiczna rozgrywka](docs/img/wacki.png)

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

## 🖥️ Wersja PC (macOS / Linux / Windows)

### Wymagania

- gotowa binarka portu z zakładki [Releases](../../releases) — dla
  docelowej platformy:
  - macOS (Apple Silicon)
  - Linux x86_64
  - Windows 10/11 x86_64
- pliki danych z oryginalnej płyty: `Dane_*.dta`

### Uruchomienie

1. Pobierz archiwum dla swojego systemu z zakładki
   [Releases](../../releases) i rozpakuj w dowolnym katalogu.
2. Skopiuj pliki `Dane_*.dta` z oryginalnej płyty w jedno z miejsc
   które gra przeszukuje (patrz niżej). Najprościej:

   - **macOS**: folder `data/` **obok** `Wacki.app` (po prostu wrzuć
     tam pliki), albo prawy klik na `Wacki.app` → **Show Package
     Contents** → `Contents/Resources/data/`
   - **Linux / Windows**: podkatalog `data/` obok binarki

3. Uruchom grę:

   - **macOS**: dwukrotne kliknięcie `Wacki.app` (przy pierwszym
     uruchomieniu zobacz „Pierwsze uruchomienie na macOS" niżej)
   - **Linux**: `./wacki`
   - **Windows**: dwukrotne kliknięcie `wacki.exe`

Gra przeszukuje (pierwszy trafiony wygrywa): `WACKI_PATH` →
katalog uruchomienia → `./data/` → katalog binarki (lub folder
obok `Wacki.app`) → napędy CD / podłączone dyski. Jeśli nic nie
znajdzie, pokaże okno wyboru folderu. Wielkość liter w nazwach
plików nie ma znaczenia.

#### Pierwsze uruchomienie na macOS

Binarki nie są podpisane certyfikatem Apple (port jest darmowy, a
Apple Developer Program kosztuje 99 $/rok), więc Gatekeeper przy
pierwszym uruchomieniu zablokuje `Wacki.app` komunikatem typu
*„Apple nie może sprawdzić, czy nie zawiera złośliwego
oprogramowania"*. To jednorazowe — wybierz **jeden** sposób:

- **Przez Ustawienia systemowe** *(zalecane — sam GUI, bez Terminala)*:
  1. Spróbuj uruchomić `Wacki.app` dwuklikiem — pojawi się blokada,
     kliknij **Done / Gotowe**.
  2. Otwórz **System Settings → Privacy & Security** (Ustawienia →
     Prywatność i bezpieczeństwo), przewiń na sam dół do sekcji
     *Security*.
  3. Będzie tam wpis *„Wacki.app was blocked…"* z przyciskiem
     **Open Anyway** (*Otwórz mimo to*) — kliknij i potwierdź.
  4. `Wacki.app` rusza i od teraz odpala się normalnie dwuklikiem.
- **Z Terminala** *(jedna komenda, każda wersja macOS)*: w katalogu
  z grą wpisz
  ```bash
  xattr -dr com.apple.quarantine Wacki.app
  ```
  i już — dwuklik działa.
- **Prawy klik** *(tylko macOS ≤ 14 Sonoma)*: prawy klik / Ctrl-klik
  na `Wacki.app` → **Open** → w dialogu jeszcze raz **Open**. Na
  macOS 15 Sequoia Apple usunęło tę ścieżkę — użyj Ustawień powyżej.

Blokada dotyczy też innych niepodpisanych plików (dlatego nie
dołączamy skryptu „odblokuj" — sam też zostałby zablokowany).
Najpewniejsza jest droga przez Ustawienia.

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

Przy **pierwszym uruchomieniu** gra zapyta jak chcesz grać —
*Pełny ekran*, *Okno 2×* albo *Okno 1×*. Wybór jest zapamiętany
(plik `wacki.cfg` obok gry), więc pytanie pojawia się tylko raz.

W dowolnej chwili możesz zmienić:

- **F11** — przełącza pełny ekran ↔ okno
- **rozciągnij okno za róg** — płynnie zmienia powiększenie; gra
  renderuje wewnętrznie 640×480 i skaluje z zachowaniem proporcji
  (czarne pasy przy nietypowych proporcjach okna)

Żeby znów zobaczyć pytanie o tryb — skasuj `wacki.cfg`. Zaawansowani
mogą wymusić tryb z linii poleceń (sekcja niżej) — flagi mają
pierwszeństwo nad zapamiętanym wyborem.

### Opcje uruchomienia

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

| Flaga                  | Zmienna środowiskowa     | Działanie                                        |
|------------------------|--------------------------|--------------------------------------------------|
| `-q` / `--quiet`       | —                        | tylko ostrzeżenia i błędy (default to INFO)      |
| `-v` / `--verbose`     | —                        | maksimum logów (wymaga buildu z `-DWACKI_VERBOSE`) |

Przykład: uruchomienie w oknie 1280×960 ze skalowaniem liniowym —

```bash
./wacki --scale 2 --scaler linear
```

Pełny ekran (zachowuje rozdzielczość pulpitu, letterbox 640×480) —

```bash
./wacki --fullscreen
```

Skok prosto do etapu 3 (kiosk) —

```bash
./wacki --start-stage 3
```

Pozostałe niżej-poziomowe / dev opcje (`--headless`, `--play-avi`,
`--test-cutscenes`, `--no-pacing`, `WACKI_INPUT_DEBUG`) udokumentowane
są w [`docs/architecture.md`](docs/architecture.md#11-flagi-runtime).

---

## 🎮 Wersja na handheld (Miyoo Mini Plus i pokrewne)

### Wymagania

- urządzenie z firmware'em **OnionOS 4.2** lub nowszym; stock
  firmware nie jest wspierane (różni się układ katalogów
  i mechanizm uruchamiania portów)
- gotowe archiwum `wacki-miyoo.zip` z zakładki [Releases](../../releases)
- pliki danych z oryginalnej płyty: `Dane_*.dta`

Wspierane modele:

- **Miyoo Mini Plus** — referencyjna platforma, najlepsze wsparcie
- **Miyoo Mini** — pin-kompatybilny, prawdopodobnie działa bez zmian
- inne handheldy z firmware'em zgodnym z OnionOS Ports — wymagana
  ręczna integracja z launcher'em danego firmware'u

Dla Anbernica i innych urządzeń z **PortMasterem** jest osobna paczka —
patrz sekcja niżej.

### Instalacja

Archiwum `wacki-miyoo.zip` jest zgodne ze standardem OnionOS Ports.

1. Rozpakuj zawartość archiwum bezpośrednio w katalogu głównym karty
   pamięci urządzenia. Folder `Roms/` z archiwum scali się
   z istniejącym `Roms/` na karcie.
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

Krzyżak przyspiesza w miarę przytrzymania — krótkie naciśnięcia
służą do precyzyjnego pozycjonowania kursora, dłuższe trzymanie
do szybkiego przemieszczania go po ekranie.

---

## 🕹️ Wersja na handheld (Anbernic i PortMaster)

Osobna paczka **PortMaster** działa na większości nowoczesnych
handheldów Anbernic — a przy okazji na całym ekosystemie PortMastera
(Powkiddy, TrimUI, RGB30, Miyoo Flip i dziesiątki innych urządzeń).

### Wymagania

- firmware z **PortMasterem**: muOS, ROCKNIX, Knulli, ArkOS, JELOS lub
  Batocera (stock firmware Anbernica zwykle nie ma PortMastera)
- gotowe archiwum `wacki-portmaster.zip` z zakładki [Releases](../../releases)
- pliki danych z oryginalnej płyty: `Dane_*.dta`

Architektury (jedna paczka, dwie binarki):

- **aarch64** — Allwinner H700 (RG35XX Plus / H / SP / 2024, RG34XX,
  RG40XX, RG28XX, RG CubeXX), Rockchip RK3566 (RG353x, RG503) i RK3399
  (RG552)
- **armhf** — seria Anbernic RG351 (RK3326) oraz oryginalny RG35XX
  (Actions, Cortex-A9) przez Koriki/Batocera

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

Przyciski w pozycjach SDL: **A** = dolny, **B** = prawy. Na padach
w układzie Nintendo (większość Anbernica) są one fizycznie oznaczone
jako **B** i **A**. Gałka steruje kursorem proporcjonalnie (delikatny
wychył = precyzyjne celowanie), krzyżak — z przyspieszeniem.

---

## 📀 Wersja na konsolę — PlayStation 2

Wacki działa też na **PlayStation 2** — renderowanie sprzętowe gsKit,
dźwięk przez audsrv, sterowanie DualShockiem (lewa gałka = kursor) lub
myszą USB. Paczka `wacki-ps2.zip` zawiera bootowalny plik ELF.

### Wymagania

- PS2 z możliwością uruchamiania homebrew — **FreeMcBoot** /
  **FreeDVDBoot** + `uLaunchELF` — albo emulator **PCSX2**.
- Pendrive (FAT32) na pliki gry (na sprzęcie) lub HostFS (w PCSX2).
- Oryginalne pliki `Dane_*.dta` (port ich nie zawiera).

### Instalacja

1. Rozpakuj `wacki-ps2.zip` — w środku jest folder `wacki/` z plikiem
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

---

## 🔨 Budowanie ze źródeł

Wszystkie instrukcje — wymagania, instalacja SDL2, polecenia `make`
i buildy w kontenerach Docker dla wszystkich platform (PC, Miyoo,
Anbernic / PortMaster, PlayStation 2) — są w osobnym pliku:

➡️ [**BUILDING.md**](BUILDING.md)

---

## 💾 Zapis stanu

Gra obsługuje:

- **10 nazwanych slotów** dostępnych z menu Sejw / Load — każdemu
  można nadać własną nazwę, slot trzyma rozdział i pozycję
  w rozgrywce
- **Quick-save / quick-load** — natychmiastowy zapis i wczytanie ze
  slotu 0 z poziomu gry (na PC: `F5` zapis, `F9` odczyt; na
  handheldzie: `R1` zapis, `L1` odczyt)

🔁 **Format `Wacki.sav` jest byte-identyczny z oryginałem z 1998 r.**
Save zrobiony w porcie da się załadować w oryginalnej binarce
`WACKI.EXE` (i odwrotnie) — można dograć w porcie etap zaczęty
w oryginalnej grze pod Windows i kontynuować na handheldzie albo
wręcz przeciwnie.

Wszystkie sloty trzymane są w jednym pliku `Wacki.sav` w katalogu
roboczym gry — obok binarki na PC, w `Roms/PORTS/Games/Wacki/` na
handheldzie. Zapis jest atomowy (tymczasowy plik + rename), więc
zanik zasilania lub crash w trakcie save'a nie psuje istniejących
slotów.

---

## Zgłaszanie błędów

Coś się sypie? Otwórz issue:

[**github.com/mszula/wacki/issues**](https://github.com/mszula/wacki/issues/new)

W zgłoszeniu pomogą:

- **wersja portu** — pierwsza linia logu uruchomieniowego (`[info/wacki] Wacki port vX.Y.Z…`)
- **platforma** — macOS / Linux / Windows / Miyoo / Anbernic (PortMaster)
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
