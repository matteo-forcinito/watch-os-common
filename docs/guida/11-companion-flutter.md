# 11. La companion: Orbit Companion (Flutter)

Repository: `Orbit-Companion` (privato). Una sola app per **Windows, Android** (le
piattaforme rilasciate) e, da collaudare, iOS, macOS, Linux e web.

## Flutter in 5 minuti

- **Flutter** è un kit di Google per fare app con un solo codice per più sistemi.
- Il linguaggio è **Dart**: somiglia a Java/JavaScript, con tipi. File `.dart`.
- Tutto lo schermo è fatto di **widget** annidati (testo, bottone, colonna, pagina...).
- Un widget **Stateless** disegna sempre la stessa cosa dai suoi parametri; uno
  **Stateful** ha uno *stato* e si ridisegna quando chiami `setState(...)`.

```dart
class Contatore extends StatefulWidget {
  const Contatore({super.key});
  @override
  State<Contatore> createState() => _ContatoreState();
}

class _ContatoreState extends State<Contatore> {
  int _n = 0;                                    // stato

  @override
  Widget build(BuildContext context) => Column(  // come disegnarlo
        children: [
          Text('Tocchi: $_n'),
          FilledButton(
            onPressed: () => setState(() => _n++),   // cambia lo stato → ridisegna
            child: const Text('Tocca'),
          ),
        ],
      );
}
```

- `async`/`await`: per operazioni che richiedono tempo (Bluetooth, rete) senza
  bloccare l'interfaccia. `Future<T>` = "un valore che arriverà".
- `pubspec.yaml`: nome, **versione** e dipendenze (pacchetti) dell'app.

## Avviare, provare, analizzare

```powershell
$env:PATH = "C:\Users\matte\Documents\flutter\bin;$env:PATH"
cd C:\Users\matte\Documents\ESP-IDF\Orbit-Companion
flutter pub get                       # scarica le dipendenze (dopo modifiche a pubspec.yaml)
flutter run -d windows                # avvia l'app su Windows (r = ricarica a caldo, q = esci)
flutter run -d chrome --web-port 8765 # versione web (Bluetooth via Chrome/Edge)
flutter analyze                       # errori e stile: deve dire "No issues found!"
flutter test                          # test automatici: "All tests passed!"
dart format lib test                  # formatta il codice nello stile standard
```

Per Android: telefono con debug USB attivo, poi `flutter devices` e `flutter run -d <id>`.

## Com'è fatto il progetto

```
Orbit-Companion/
├─ pubspec.yaml            versione (es. 0.4.2-dev.3+8) e dipendenze
├─ lib/                    TUTTO il codice dell'app
├─ test/                   test automatici (flutter test)
├─ assets/devices/         foto dei dispositivi
├─ android/ windows/ ios/ macos/ linux/ web/   "contenitori" per ogni sistema (raramente da toccare)
├─ release/notes/X.Y.Z.md  note di rilascio
├─ tools/                  script di pacchettizzazione e prove (Windows, Android)
└─ docs/                   STATUS, RELEASE, PERMISSIONS, STABILITY
```

I file in `lib/`:

| File | Cosa fa |
|---|---|
| `main.dart` | Avvio, tema, pagina principale: elenco dispositivi, scheda del dispositivo selezionato |
| `ble.dart` | **Bluetooth**: ricerca, connessione, sessioni per dispositivo, invio comandi (`send`), richiesta di controllo (`requestControl`) |
| `protocol.dart` | Decodifica delle righe JSON che arrivano dal dispositivo (Nordic UART) |
| `devices.dart` | I **modelli** supportati (nome, foto, risoluzione, posizione dello schermo nella foto) |
| `device_photo.dart` | Disegna la foto del dispositivo |
| `device_search.dart` | Finestra "Aggiungi dispositivo" |
| `remote_control.dart` | Gestione via Wi-Fi: collegamento web, **schermo remoto**, scelta rete Wi-Fi |
| `remote_frame.dart` | Decoder dei frame dello schermo remoto (ORF1/BMP vecchi, **ORF2** nuovo) |
| `remote_touch_surface.dart` | Converte tocchi/swipe sull'immagine in coordinate del dispositivo |
| `device_http.dart` | Richieste HTTP alla pagina web del dispositivo (con token `X-Key`) |
| `device_services.dart`, `device_telemetry.dart` | Pagine native: impostazioni, microSD, batteria, sensori, meteo |
| `family.dart` | Pagina Famiglia (permessi tra dispositivi) |
| `companion_services.dart`, `companion_platform.dart` | Servizi Android: notifiche inoltrate, servizio in primo piano |

## Come parla con i dispositivi

Due canali:

1. **Bluetooth LE** (servizio Nordic UART, come Gadgetbridge). La companion invia righe di
   testo, il firmware risponde con righe JSON. Lato firmware:
   `watch-os-common/components/companion/ble_companion.c`.
   ```
   companion → dispositivo:  GB({"t":"find","n":true});      fai suonare
                             GB({"t":"notify","title":"...","body":"..."});
                             setTime(1760000000);
                             GB({"t":"control","request":"<nonce>","network":"Casa","password":"..."});
   dispositivo → companion:  {"t":"status","bat":87,...}
                             {"t":"control","request":"<nonce>","url":"http://192.168.1.11","code":"002571",...}
   ```
2. **Wi-Fi / HTTP**: con `control` il dispositivo accende la sua pagina web e restituisce
   indirizzo e codice; da lì la companion usa le stesse API della dashboard
   (`/api/system`, `/api/remote/frame?format=orf2`, `/api/remote/input`...).
   Documentate in `watch-os-common/docs/REMOTE-CONTROL.md` e `DEVICE-SERVICES.md`.

## Esempio: aggiungere un comando nuovo, da entrambe le parti

Obiettivo: un bottone "Saluta" nella companion che mostra un messaggio sull'orologio.

**Firmware** — in `ble_companion.c`, dentro `handle_gb()`, accanto a `find`:

```c
    else if (strcmp(type, "hello") == 0)
    {
        /* BLE task: never touch LVGL here, hand it to the UI. */
        ui_async(show_hello, NULL);
    }
```

con, sopra nel file:

```c
static void show_hello(void *arg)   /* runs in the lvgl task */
{
    (void)arg;
    ui_toast("Ciao dalla companion!");
}
```

**Companion** — in `lib/ble.dart`, vicino a `find`:

```dart
Future<void> hello(String id) =>
    send(id, 'GB(${jsonEncode({'t': 'hello'})});');
```

e una voce accanto a "Trova dispositivo" in `main.dart` (stesso schema: `command()`
invia e mostra il messaggio di conferma o l'errore):

```dart
ListTile(
  contentPadding: EdgeInsets.zero,
  leading: Icon(Icons.waving_hand_outlined, color: blue),
  title: Text('Saluta'),
  enabled: session?.ready == true,
  onTap: () => command(
    () => ble.hello(session!.id),
    'Saluto inviato.',
  ),
),
```

I firmware vecchi ignorano i comandi che non conoscono: è sicuro. Se un comando
richiede una funzione nuova, il firmware lo annuncia (es. `"network_provisioning": true`
nella risposta di `control`) e la companion mostra il comando solo se c'è.

## Aggiungere un modello di dispositivo

In `lib/devices.dart`:

```dart
DeviceModel(
  'round2',                          // id
  'Round 1.9',                       // nome mostrato
  'Produttore',
  'assets/devices/round2-front.jpg', // foto quadrata, in assets/devices/
  '410 × 502',
  Rect.fromLTWH(.30, .28, .40, .48), // area dello schermo nella foto (0..1)
  false,                             // tondo?
  Color(0xFF3868EE),                 // colore
),
```

## Rilasciare la companion

Dettagli in `Orbit-Companion/docs/RELEASE.md`. In breve:

1. In `pubspec.yaml` aggiorna `version: X.Y.Z+BUILD` (BUILD sempre crescente, es. `0.5.0+9`).
2. Scrivi `release/notes/X.Y.Z.md`.
3. `flutter analyze` e `flutter test` puliti.
4. Commit su `main` (o sul branch di lavoro unito a `main`), poi:
   ```powershell
   git checkout -b release/0.5.0
   git push -u origin release/0.5.0
   ```
5. GitHub Actions compila Windows e Android, prova l'APK su un emulatore, e pubblica la
   release `v0.5.0` con zip Windows, APK, `SHA256SUMS.txt`. Segui con:
   ```powershell
   $gh = "C:\Users\matte\Documents\ESP-IDF\ESP32-S3-Knob-Touch-LCD-1.8\.companion-tools\gh\bin\gh.exe"
   & $gh run list -R matteo-forcinito/Orbit-Companion --limit 3
   ```
6. Versioni dev (`0.5.0-dev.1`): stesso procedimento con branch `release/0.5.0-dev.1`.

Il branch deve chiamarsi esattamente come la versione di `pubspec.yaml`, altrimenti la
Action si ferma. Le release pubblicate non si modificano.

## Stato al 16/10/2026

- Branch `fix/connection-stability` (commit `a9cf8be`): schermo remoto ORF2 e "Altra rete..."
  per aggiungere un Wi-Fi via Bluetooth. Analisi e 46 test superati, **non ancora rilasciato**.
- Ultima versione nel `pubspec.yaml`: `0.4.2-dev.3+8`.
