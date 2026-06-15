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
3. Pliki `.DTA` zostaną skopiowane do prywatnej pamięci aplikacji
   (`Android/data/pl.wacki.debug/files/data/`). Robisz to raz; przy kolejnych
   startach gra wchodzi od razu.

Zapisy i `wacki.cfg` lądują w pamięci wewnętrznej aplikacji.

## Sterowanie (dotyk)

| Gest / klawisz      | Akcja                                            |
|---------------------|--------------------------------------------------|
| Dotknięcie          | klik / chodzenie / interakcja (LPM)              |
| Dotknięcie 2 palcami| przełączenie aktywnej postaci (Ebek ↔ Fjej, PPM) |
| Przycisk Wstecz     | menu pauzy (zapis / wczytanie / wyjście)         |
| **Tab** (klawiatura)| przełączenie postaci — wygodne na emulatorze     |

Pad Bluetooth/USB też działa (przez `SDL_GameController`), tak jak na
handheldach.

**Emulator (BlueStacks itp.):** dwupalcowy tap myszką jest niewygodny, a prawy
klik zwykle nie jest przekazywany do apki — użyj **Tab** do zmiany postaci.
Prawy przycisk myszy też zadziała, jeśli emulator go forwarduje (silnik mapuje
PPM natywnie).

## Jak to się składa

- **Build**: `app/jni/CMakeLists.txt` buduje `libmain.so` z tej samej listy
  źródeł co Makefile (`ENGINE_SRCS` + `SDL_PLATFORM_SRCS`) oraz `libSDL2.so`
  z submodułu. To androidowy odpowiednik `mk/<target>.mk`.
- **HAL Androida**: `src/platform/android/{hooks_android,data_root_android}.c`
  + gałąź `__ANDROID__` w `src/platform/sdl/system_sdl.c` (writable cwd +
  trap przycisku Wstecz). Patrz `docs/platform-hal.md`.
- **Launcher**: `SetupActivity` (import danych przez SAF) → `WackiActivity`
  (`SDLActivity`). Glue Javy SDL-a (`org.libsdl.app`) kompiluje się prosto
  z submodułu, więc wersje Javy i natywnego SDL nie mogą się rozjechać.

## Aktualizacja SDL

```bash
git -C app/jni/SDL fetch --depth 1 origin release-2.32.x
git -C app/jni/SDL checkout FETCH_HEAD
git add app/jni/SDL && git commit -m "chore(android): bump SDL"
```
