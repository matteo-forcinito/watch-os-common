# 7. Test

## A cosa servono

Un test è un piccolo programma che esegue un pezzo di codice reale con dati noti e
controlla che il risultato sia quello atteso. Serve a:

- accorgersi subito se una modifica rompe qualcosa che funzionava;
- provare casi difficili da riprodurre sul dispositivo (dati corrotti, errori di rete,
  limiti) senza flashare nulla;
- documentare come una funzione deve comportarsi.

Non sostituiscono la prova sul dispositivo: la completano.

## Quando scriverne uno

| Situazione | Test? |
|---|---|
| Logica "pura": parsing di testi/JSON/protocolli, calcoli, decoder, formati | **Sì, sempre** |
| Correzione di un bug in quella logica | **Sì**: prima un test che fallisce, poi la correzione |
| Codice che dipende dall'hardware (driver) | Solo se si può simulare (es. `tests/pmu` simula l'I²C) |
| Layout grafici | No: screenshot dalla console di sviluppo su ogni forma di schermo |
| Companion (Dart) | Sì, per logica e widget: `flutter test` |

Il trucco per rendere testabile il C: **separare la logica dalle chiamate a ESP-IDF**.
Per esempio `pieno/main/fuel.c` contiene solo parsing e calcoli, senza Wi-Fi né LVGL, e
quindi si compila anche sul PC.

## I test che esistono

| Dove | Cosa prova | Come si lancia |
|---|---|---|
| `watch-os-common/<app>/tests/run_tests.py` (pieno, giornata, news, copertura, ombra, radar, deck, bilancio) | Logica delle app hosted | vedi sotto |
| `watch-os-common/components/services/ecos/tests` | Formato radio dell'Ecosistema | `run_tests.py` |
| `watch-os-common/remote/hub/tests`, `sentinel/main/tests`, `vault/main/tests` | Stato hub Remote, pacchetti Sentinel, formato Vault | GitHub Actions (Linux) |
| `watch-os-common/tools/test_remote_frame.cjs` | Decoder ORF2 della dashboard web | `node` |
| `ESP32-S3-Touch-AMOLED-1.75/tests/pmu` | Driver PMU AXP2101 con I²C simulato | `run_tests.py` |
| `SenseCap-Watcher-W1-A/tools/test_market_catalog.py` | Generatore del catalogo Market | `python` |
| `Orbit-Companion/test/*.dart` | Companion: protocollo, decoder frame, BLE, UI | `flutter test` |

## Lanciare i test C su Windows

Su Windows non c'è un compilatore C per il PC, quindi i test C girano **dentro un
emulatore RISC-V** (Unicorn) con il compilatore ESP-IDF. Le librerie Python
necessarie sono già in `watch-os-common/build/ecos-checks/test-deps`.

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd C:\Users\matte\Documents\ESP-IDF\watch-os-common
python pieno\tests\run_tests.py --deps build\ecos-checks\test-deps --output $env:TEMP\pieno-test
```

Risultato atteso:

```
PASS: pieno parsing and maths, real C in RISC-V emulator
```

In caso di errore stampa la riga del test fallito, es. `FAIL: test_fuel.c:42`.

Tutti i test delle app in un colpo:

```powershell
foreach ($t in Get-ChildItem -Recurse -Filter run_tests.py -Path pieno,giornata,news,copertura,ombra,radar,deck,bilancio) {
    "== $($t.Directory.Parent.Name)"
    python $t.FullName --deps build\ecos-checks\test-deps --output "$env:TEMP\t-$($t.Directory.Parent.Name)"
}
# Ecosistema: vuole il compilatore esplicito
$gcc = (Get-Command riscv32-esp-elf-gcc).Source
python components\services\ecos\tests\run_tests.py --gcc $gcc --deps build\ecos-checks\test-deps --output $env:TEMP\t-ecos
```

Ogni riga deve iniziare con `PASS:` (verificato il 16/10/2026: tutti superati).

Su Linux (GitHub) lo stesso script usa `--host cc` e compila con i sanitizer
(trovano accessi fuori memoria).

## Test JavaScript della dashboard

```powershell
node C:\Users\matte\Documents\ESP-IDF\watch-os-common\tools\test_remote_frame.cjs
```

Dopo ogni modifica a `index.html`, `kit.js` o a una `*_dashboard.html`, controlla anche
la sintassi (un solo apice sbagliato svuota tutte le pagine):

```powershell
node --check components\services\web\kit.js
python -I -c "import re,sys;s=open(sys.argv[1],encoding='utf-8').read();open(sys.argv[2],'w',encoding='utf-8').write('\n'.join(re.findall(r'<script>(.*?)</script>',s,re.S)))" components\services\web\index.html $env:TEMP\dash.js
node --check $env:TEMP\dash.js
```

## Scrivere un nuovo test C

Esempio: hai una funzione `int clamp_percent(int v)` in `miaapp/main/logic.c`
(senza include di ESP-IDF).

`miaapp/tests/test_logic.c`:

```c
/* Returns the failing line, 0 if all pass. */
#include "logic.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

int miaapp_tests(void)
{
    CHECK(clamp_percent(50) == 50);
    CHECK(clamp_percent(-3) == 0);
    CHECK(clamp_percent(140) == 100);
    return 0;
}
```

Poi copia `pieno/tests/run_tests.py` in `miaapp/tests/` e cambia: l'elenco `sources`,
il nome della funzione di test e i nomi dei file di output. Lo script fornisce già
l'avvio dell'emulatore e il controllo del risultato.

## Test della companion

```powershell
$env:PATH = "C:\Users\matte\Documents\flutter\bin;$env:PATH"
cd C:\Users\matte\Documents\ESP-IDF\Orbit-Companion
flutter analyze      # controllo statico: deve dire "No issues found!"
flutter test         # tutti i test: deve finire con "All tests passed!"
flutter test test\remote_patch_test.dart   # un solo file
```

Un test Dart (esempio reale, `test/remote_patch_test.dart`):

```dart
test('empty patch means unchanged', () {
  final idle = RemotePatch.decode(patch(x: 0, y: 0, w: 0, h: 0, flags: 0, words: []));
  expect(idle.rgba, isNull);
  expect(RemoteScreen(4, 4).apply(idle), isTrue);
});
```

## Prova su dispositivo (collaudo)

Per ogni modifica visibile o di sistema, oltre ai test:

1. build di **tutti** i firmware;
2. flash su almeno un dispositivo tondo e uno rettangolare (o i due collegati al PC);
3. log di avvio fino a `main: Ready`, senza `E (` nuovi;
4. screenshot delle schermate toccate ([pagina 8](08-console-e-diagnostica.md));
5. per funzioni di rete: prova vera con dashboard o companion (es. `tools/dev/remote_mirror_test.py`
   per lo schermo remoto).

Annota cosa hai provato nelle note di rilascio.
