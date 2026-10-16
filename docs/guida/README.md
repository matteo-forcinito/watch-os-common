# Guida al progetto Watch OS

Questa guida spiega come è fatto il progetto e come lavorarci, anche senza
conoscere bene il C, ESP-IDF o Flutter. Ogni pagina è autonoma: parti da quella
che ti serve. Se è la prima volta, leggi la 1 e poi la 13.

**Da dove ripartire oggi:** [STATO.md](STATO.md) (cosa è rilasciato, cosa resta aperto).

| # | Pagina | Quando leggerla |
|---|---|---|
| 1 | [Mappa del progetto](01-mappa-del-progetto.md) | Prima di tutto: quali cartelle esistono e come si collegano |
| 2 | [Ambiente, build e flash](02-ambiente-build-flash.md) | Per compilare e caricare un firmware sul dispositivo |
| 3 | [C, ESP-IDF e LVGL in breve](03-c-esp-idf-lvgl-in-breve.md) | Il minimo di teoria per leggere e modificare il codice |
| 4 | [App interne: crearle e modificarle](04-app-interne.md) | Aggiungere o cambiare un'app che sta dentro il firmware |
| 5 | [App esterne e Market](05-app-esterne.md) | App installate da microSD o dal Market |
| 6 | [Aggiungere un nuovo dispositivo](06-nuovo-dispositivo.md) | Supportare una nuova scheda |
| 7 | [Test](07-test.md) | Cosa sono, quando scriverli, come lanciarli |
| 8 | [Console di sviluppo e diagnostica](08-console-e-diagnostica.md) | Comandare il dispositivo dal PC, screenshot, crash, RAM, prestazioni |
| 9 | [Release dei firmware](09-release-firmware.md) | Pubblicare una versione che i dispositivi scaricano dal cloud |
| 10 | [Automazioni GitHub](10-automazioni-github.md) | Cosa fanno le GitHub Actions e cosa fare se falliscono |
| 11 | [La companion (Flutter)](11-companion-flutter.md) | Capire, modificare, provare e rilasciare l'app Orbit Companion |
| 12 | [Flusso di lavoro dalla A alla Z](12-flusso-di-lavoro.md) | La sequenza completa usata per ogni modifica |
| 13 | [Leggere il codice](13-leggere-il-codice.md) | VS Code "vai alla definizione", da dove viene una funzione, cicli di vita di app e servizi, memoria |

## Le tre regole da ricordare sempre

1. **Il codice condiviso sta in `watch-os-common`.** Ogni modifica lì va compilata
   in **tutti** i firmware (AMOLED 1.8, AMOLED 1.75, AMOLED 1.75C, Knob, Watcher).
2. **LVGL (la grafica) si usa solo dal task `lvgl`.** Dagli altri task si passa da
   `ui_async()`. È la regola che evita la maggior parte dei crash.
3. **Non si rinomina mai il nome del progetto** (`amoled_watch`, `knob_os`...):
   è l'identità con cui l'aggiornatore riconosce il firmware giusto.

Le regole complete, scritte per chi programma, sono in
`ESP32-S3-Amoled-Touch-1.8/CLAUDE.md` (valgono per tutti i firmware).
