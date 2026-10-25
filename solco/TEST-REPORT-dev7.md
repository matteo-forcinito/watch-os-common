# Solco 0.1.0-dev.7 - Studio locale e Orbit Bluetooth

24 ottobre 2026. Build sperimentale installata via USB su AMOLED 1.8 COM3
e Knob COM9. Nessuna distribuzione OTA generale. Il difetto audio descritto
sotto resta aperto: questa build non e' una certificazione di qualita' professionale.

## Funzioni disponibili

SOLCO+ apre Studio: 8 drumpad, sequencer a 16 step con overdub, 60-200 BPM,
salvataggio del pattern su microSD, tre strumenti percussivi (EPiano, Bass,
Bell) con otto note ciascuno. Sedici voci simultanee, coda comandi limitata,
limiter interno e transizioni con margine tra Studio e i due deck.
Il selettore KIT A/B confronta kick e tom originali con la revisione B,
che parte come predefinita. Gli altri suoni restano uguali.

AMOLED: movimento MARACAS/BATTERIA con QMI8658, soglia, isteresi e cooldown.
Profilo sensore piu' rapido soltanto in Solco; Knob non ha IMU.
Interfaccia ancora verticale su AMOLED: modalita' orizzontale non inclusa.
Restano disponibili mixer, MP3 preparati in PCM e scratch touch/ghiera.

## Verifiche sul dispositivo

Entrambi i dispositivi hanno passato i test USB: selezione dei due kit,
32 banchi musicali, burst di 32 note con limite di 16 voci, registrazione,
loop, cancellazione e corrispondenza del pattern salvato sulla microSD.
I test hanno conservato il volume scelto dall'utente: il Knob risultava al 59%.

| Misura | AMOLED 1.8 | Knob |
| --- | ---: | ---: |
| Generazione iniziale banchi | 1,021 s | 1,170 s |
| Massimo mix osservato | 2.952 us | 2.897 us |
| Massimo intervallo ciclo audio | 8.277 us | 8.320 us |
| Cicli oltre 17.415 us | 0 | 0 |
| Buffer vuoti / errori / clip digitali | 0 / 0 / 0 | 0 / 0 / 0 |
| Letture IMU nella prova | 105 | non supportato |
| Errori IMU | 0 | non applicabile |

Non sono misure di latenza acustica o THD. Non e' stato aggiunto un nuovo
stress combinato SD + Studio; i risultati SD/MP3 precedenti sono nel report dev.6.
Test host: mixer/WAV, headroom Studio e mute, scratch/EOF, banchi, limite
voci, coda, sequencer, clock e movimento passati. Catture native delle tre
pagine Studio sul Knob ricostruite da righe identiche di piu' screenshot USB;
nessun pixel inventato. Le altre anteprime nel pacchetto sono storiche.

Correzione Knob: disabilitati gli interrupt GPIO ereditati prima di installare
il servizio ISR all'avvio. I successivi avvii USB sono riusciti. Il firmware
principale del launcher non e' stato riflashato con questa modifica.

## Difetto audio aperto

L'utente sente ancora uno scrocchio importante su kick e tom, senza differenza
evidente tra KIT A/B, e anche sulle demo Pulse e Chord. Non attribuito con
certezza alla cassa. I PCM delle demo non mostrano clipping o salti al punto
di loop: picchi 10.521 / 6.581 su 32.767, primo e ultimo campione zero.
I test non rilevano starvation digitale, ma questo non esclude altri difetti.

La revisione B riduce energia bassa e picco, senza risolvere all'ascolto.
Il controllo del driver ha inoltre trovato che volume codec API 100 non e'
unity sull'AMOLED: la compensazione pa_voltage=5 / dac_voltage=3,3 produce
circa +3,6 dB nel DAC. Non modificato alla cieca, e non dimostra la causa:
i segnali demo restano sotto il fondo scala digitale anche con quel guadagno.
Restano da separare codec/I2S, amplificatore e speaker con una prova mirata.
Come richiesto dall'utente, il problema non blocca le altre funzionalita'.

## Radio, hub e telefono

Solco locale non avvia BLE, Wi-Fi o ESP-NOW. Hub non implementato in dev.7.
La direzione prevista e' ESP-NOW tra dispositivi e BLE opzionale col telefono,
con audio locale distribuito oppure una sola uscita selezionabile. Il ruolo
di coordinatore non impone un unico speaker. Sincronismo e coesistenza radio
richiedono ancora test reali; nessuna promessa di sincronismo perfetto.

Orbit Solco Test 0.5.4 apre le app preferendo BLE senza accendere il Wi-Fi.
Rimosso il bootstrap Wi-Fi implicito nei percorsi di upload interessati.
Upload canzoni via BLE e controllo remoto di Solco in esecuzione non inclusi:
per ora usare /music sulla microSD. Il trasferimento HTTP richiede una
connessione gia' disponibile, non abilita automaticamente la radio.
APK separato dalla companion normale: dev.watchos.orbit.companion.solco_test.
Flutter analyze senza problemi e 14 test mirati passati; nessun telefono
Android collegato per verifica fisica dell'APK.

## Identita' dei binari USB verificati

- AMOLED: 1.479.744 byte; SHA256 9972b5702e654802f46422b5cd0026f7378c7cd378d90a8a3274294e9578e95d
- Knob: 1.464.512 byte; SHA256 559f39a5709a66e0c7d4d819d9db338e3a3262faffa8d1da34baf95b50acb215
- APK 0.5.4: SHA256 464714cfbd713bbaec0e4ee80759e9cce534980463d1106027fa691731967c99

I pacchetti richiedono corrispondenza SHA con la ricevuta dell'installazione USB.
MIDI, hub, altri modelli e OTA generale restano da implementare/verificare.

## Correzione accesso Studio

Il pannello trasparente del mixer intercettava i tocchi sopra SOLCO +.
Disabilitato il hit-test del solo contenitore mixer; i suoi controlli figli
restano interattivi. Aggiunto stato vista nella diagnostica USB per verificare
l'apertura tramite eventi puntatore, non soltanto con studio-view.
Le misure audio precedenti si riferiscono alla build KIT A/B precedente;
questa correzione riguarda soltanto hit-testing e diagnostica della UI.

Verifica correttiva passata su COM3 e COM9: tocco SOLCO + apre Studio,
tocco DRUMPAD apre vista 1, ritorno al mixer e tocco STRUMENTI apre vista 2.
Dispositivi lasciati nel menu Studio. Nessun comando audio o volume nel test.
Log: progress-files/solco-{amoled,knob}-header-touch-test.json.
