# Wacki — port na Androida

Port silnika Wacki na Androida przez SDL2. Rdzeń silnika jest niezmieniony —
ten katalog dokłada tylko projekt Gradle + CMake, warstwę HAL Androida
(`src/platform/android/`) i launcher importujący dane gry.

To jest **projekt do zbudowania w Android Studio** (sideload APK). Możesz też
zbudować samodzielnie z linii poleceń (niżej).

**Gotowe APK z CI:** job `android` w `.github/workflows/build.yml` buduje
debugowe `wacki-android.apk` przy każdym pushu (master / port/android) — pobierz
z artefaktów danego runu (zakładka Actions). Wydania na tagach `v*` mają APK
dołączone do GitHub Release. APK jest podpisane kluczem debug (instaluje się
bezpośrednio); podpisany build release wymagałby keystore w sekretach.

---

## Wymagania

- **Android Studio** (najnowsze) z wbudowanym JDK 17+. Przy pierwszym
  „Sync" zaproponuje doinstalowanie **Android SDK 34**, **NDK** i **CMake** —
  zgódź się.
- **`WACKI.EXE`** z oryginalnej płyty w `../data/` (sekcje `.rdata` + `.data`
  są wszywane w binarkę — patrz `../BUILDING.md`).
- **Źródła SDL2** jako submoduł (poniżej).
- Pliki **`DANE_*.DTA`** z płyty — wgrywane na telefon przy pierwszym
  uruchomieniu (nie do APK).

## Krok po kroku

```bash
# 1. Pobierz źródła SDL2 (submoduł, ~jednorazowo). Przypięte do release-2.32.x.
git -C .. submodule update --init --recursive

# 2. Wygeneruj wszyty blob z WACKI.EXE. CMake zrobi to sam przy buildzie,
#    o ile masz na hoście kompilator C. Najprościej raz zbudować desktop:
( cd .. && make )            # tworzy ../src/embedded_wacki_pe.c

# 3. Otwórz katalog android/ w Android Studio → Sync → Run.
#    Albo z linii poleceń (po wygenerowaniu wrappera przez AS):
./gradlew assembleDebug
#    APK: app/build/outputs/apk/debug/app-debug.apk
```

Budowane ABI: `arm64-v8a`, `armeabi-v7a`, `x86_64` (emulator). Zawęź w
`app/build.gradle` (`abiFilters`), żeby szybciej budować.

## Pierwsze uruchomienie (na telefonie)

APK jest malutkie — danych gry w nim nie ma. Po instalacji:

1. Skopiuj `DANE_*.DTA` z płyty gdziekolwiek na telefon (np. osobny folder
   w „Pliki" / Downloads).
2. Uruchom Wacki → ekran powitalny → **„Wskaż folder z plikami gry"** →
   wybierz ten folder (Storage Access Framework, bez uprawnień do pamięci).
3. Gra **czyta pliki wprost z tego folderu** (przez `content://` fd) — nic nie
   jest kopiowane, więc to natychmiastowe. Dostęp jest utrwalony, więc przy
   kolejnych startach gra wchodzi od razu. Folder musi pozostać na miejscu.

> **Bez SAF (adb/menedżer):** zamiast wskazywać folder możesz wrzucić
> `DANE_*.DTA` wprost do prywatnego katalogu aplikacji
> `Android/data/pl.mszula.wacki.debug/files/data/` (release: `pl.mszula.wacki`) —
> silnik też go tam znajdzie
> (`adb push DANE_01.DTA /sdcard/Android/data/pl.mszula.wacki.debug/files/data/`).

Zapisy i `wacki.cfg` lądują w pamięci wewnętrznej aplikacji.

## Sterowanie (dotyk)

| Gest / klawisz      | Akcja                                            |
|---------------------|--------------------------------------------------|
| Dotknięcie          | klik / chodzenie / interakcja (LPM)              |
| Dotknięcie 2 palcami| przełączenie aktywnej postaci (Ebek ↔ Fjej, PPM) |
| Przycisk Wstecz     | menu pauzy (zapis / wczytanie / wyjście)         |
| **Tab** (klawiatura)| przełączenie postaci — wygodne na emulatorze     |

Klik w obszarze gry idzie przez wbudowaną syntezę dotyk→mysz SDL-a (mapowaną
przez realną transformatę renderera), więc trafia dokładnie pod palec na każdym
urządzeniu.

**Nakładka dotykowa w pasach (telefon).** Na szerokim ekranie 640×480
letterboxuje się do czarnych pasów po bokach. Wypełnia je półprzezroczysta
gałka (lewo — rusza kursorem) + duży LPM / mniejszy PPM (prawo). Działa na
**prawdziwym telefonie** (cały ekran to jedna powierzchnia dotyku). **Na
BlueStacks nie zadziała** — emulator ściska dotyk z pasów do obszaru gry, więc
kontrolki tam są nieosiągalne (ograniczenie emulatora, nie do obejścia w kodzie).
Na emulatorze steruj bezpośrednim klikiem + **Tab** (zmiana postaci).

Pad Bluetooth/USB też działa (przez `SDL_GameController`), tak jak na
handheldach.

## Wydanie produkcyjne (podpisywanie)

To, co budowałeś dotąd (i CI na każdym pushu), to **debug APK** — podpisany
automatycznym kluczem debugowym Androida (`...-debug`, `applicationId`
`pl.mszula.wacki.debug`). Nadaje się tylko do testów/sideloadu; **na produkcję
się nie nadaje** (Sklep Play go odrzuci, a klucz debug jest publiczny).

Na produkcję potrzebujesz **własnego klucza** (keystore). Generujesz go **raz**
i trzymasz na zawsze — każda przyszła aktualizacja musi być podpisana tym samym
kluczem; zgubienie klucza = brak możliwości aktualizacji apki.

**1. Wygeneruj keystore (raz):**
```bash
keytool -genkeypair -v -keystore wacki-release.jks \
        -alias wacki -keyalg RSA -keysize 2048 -validity 10000
```
Plik `.jks` i hasła trzymaj bezpiecznie, **poza repo** (`.gitignore` blokuje
`*.jks`/`*.keystore`).

**2. Lokalny podpisany build.** W `~/.gradle/gradle.properties` (poza repo):
```properties
wacki.keystore=/pełna/ścieżka/wacki-release.jks
wacki.keystore.password=…
wacki.key.alias=wacki
wacki.key.password=…
```
potem:
```bash
cd android
./gradlew assembleRelease   # podpisany APK → app/build/outputs/apk/release/app-release.apk
./gradlew bundleRelease     # AAB pod Sklep Play → app/build/outputs/bundle/release/app-release.aab
```
Bez tych właściwości `assembleRelease` zbuduje APK **niepodpisany**.

**3. Podpisany build w CI (automatycznie przy tagu `v*`).** Dodaj 4 sekrety
repo (Settings → Secrets and variables → Actions):

| Sekret | Wartość |
|--------|---------|
| `WACKI_KEYSTORE_BASE64`   | `base64 -i wacki-release.jks` (cała zawartość) |
| `WACKI_KEYSTORE_PASSWORD` | hasło do keystore |
| `WACKI_KEY_ALIAS`         | `wacki` |
| `WACKI_KEY_PASSWORD`      | hasło do klucza |

Wtedy `git tag v1.0 && git push --tags` zbuduje **podpisany release APK** i
dołączy `wacki-android.apk` do GitHub Release. Bez tych sekretów (albo na
zwykłym pushu) CI buduje debug APK do testów — nic się nie wywala.

> **Sklep Play vs sideload.** Powyższe daje podpisany APK do samodzielnego
> rozprowadzania (GitHub Releases / strona). Sklep Play wymaga **AAB**
> (`bundleRelease`), konta deweloperskiego (jednorazowa opłata), oceny treści
> i tzw. Play App Signing (Google trzyma wtedy klucz dystrybucyjny). Pamiętaj
> też, że to port komercyjnej gry z 1997 (wszyte sekcje `WACKI.EXE`) — Play
> egzekwuje prawa autorskie ostrzej niż GitHub; ta decyzja jest po Twojej stronie.

## Jak to się składa

- **Build**: `app/jni/CMakeLists.txt` buduje `libmain.so` z tej samej listy
  źródeł co Makefile (`ENGINE_SRCS` + `SDL_PLATFORM_SRCS`) oraz `libSDL2.so`
  z submodułu. To androidowy odpowiednik `mk/<target>.mk`.
- **HAL Androida**: `src/platform/android/{hooks_android,data_root_android,saf}.c`
  + gałąź `__ANDROID__` w `src/platform/sdl/system_sdl.c` (writable cwd +
  trap przycisku Wstecz). Patrz `docs/platform-hal.md`.
- **Czytanie danych w miejscu (SAF)**: `saf.c` woła przez JNI
  `WackiActivity.nativeOpenDataFd(name)` → `ContentResolver.openFileDescriptor`
  → `fdopen` (pliki lokalne są seekowalne, więc `fseek/ftell` archiwum działa).
  `data_root_android.c` włącza ten tryb i ustawia korzeń `saf:`; współdzielone
  `file_host.c` + `flic_host.c` routują tam pod `#ifdef __ANDROID__`. Bez kopii.
- **Launcher**: `SetupActivity` (wybór folderu przez SAF + utrwalenie dostępu,
  bez kopiowania) → `WackiActivity` (`SDLActivity`). Glue Javy SDL-a
  (`org.libsdl.app`) kompiluje się prosto z submodułu, więc wersje Javy
  i natywnego SDL nie mogą się rozjechać.

## Aktualizacja SDL

```bash
git -C app/jni/SDL fetch --depth 1 origin release-2.32.x
git -C app/jni/SDL checkout FETCH_HEAD
git add app/jni/SDL && git commit -m "chore(android): bump SDL"
```
