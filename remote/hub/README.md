# Remote Hub: nucleo di sviluppo

Questo codice non è ancora collegato alla Remote e non è una release Hub.
Remote 1.0.1 resta il pacchetto stabile. Nessun firmware cambia includendo
questa cartella: non compare nei CMakeLists delle app o dell'OS.

## Comportamento verificato

- Cinque stati indipendenti: controllo locale e fino a quattro satelliti.
- Unione di modificatori, tasti distinti e pulsanti mouse. Oltre sei tasti
  distinti: ErrorRollOver HID, senza perdere silenziosamente un tasto.
- Disconnessione o timeout di 500 ms: rilascio del solo nodo interessato;
  anche i suoi movimenti ancora pendenti vengono eliminati.
- Sessione obbligatoria, sequenza monotona con wrap a 32 bit, duplicati e
  pacchetti vecchi respinti senza prolungare la connessione.
- Movimento e scroll verticale/orizzontale consumati soltanto dopo invio
  USB accettato. Backlog per nodo limitato a ±32767 per asse; saturazioni
  contate. Il worker dovrà imporre un limite più breve di età/latency.
- Media e gamepad: un solo report ciascuno nel descrittore esistente.
  Precedenza deterministica del primo slot attivo, senza fondere gli assi
  di giocatori diversi. Non sono gamepad indipendenti: richiedono altri
  descrittori/report e verifiche sul computer.
- Nessuna allocazione dinamica, I/O, callback UI o dipendenza ESP-IDF.

## Contratto per l'integrazione

Un solo worker possiede `hub_state_t`. La callback radio accoda senza attese;
il worker verifica MAC nella allowlist, cifratura/sessione e host connesso.
`hub_state_authorize` può essere chiamata solo dopo handshake autenticato
con una nuova sessione casuale non nulla; il pacchetto non autorizza un nodo.
La sessione non è una chiave né un codice di abbinamento: non sostituisce
ESP-NOW cifrato, protezione delle chiavi e autenticazione dell'handshake.

Pacchetto esatto di 24 byte, little endian:

| Offset | Contenuto |
| --- | --- |
| 0..1 | `RH` |
| 2 | Versione 1 |
| 3 | Report HID 1=tastiera, 2=media, 3=mouse, 4=gamepad |
| 4..7 | Sequenza globale del nodo |
| 8..15 | Sessione negoziata |
| 16..23 | Payload HID, byte non usati obbligatoriamente zero |

Tastiera: 8 byte; media: 2; mouse: 5 (pulsanti/X/Y/wheel/pan);
gamepad: 4 (X/Y/pulsanti a 16 bit). Tutti corrispondono alla mappa HID
Remote attuale. Chiavi, indirizzi MAC, discovery e ACK non sono nel payload.

Il worker prepara un report (`snapshot`), lo invia, quindi esegue `commit`
solo dopo accettazione. Nessun nuovo input fra snapshot e commit. In caso
di invio fallito, lascia il report pendente; applica timeout e revoche prima
del successivo tentativo. Commit di snapshot obsoleti o modificati rifiutato.
**Non accorpare una pressione e il suo rilascio prima che il PC abbia ricevuto
la pressione:** i pacchetti di transizione richiedono accodamento ordinato,
ACK applicativo dopo consegna e retry. Accorpare solo movimento/scroll.

I heartbeat trasmettono lo stato corrente con sequenza nuova, con delta
mouse zero; non ritrasmettono movimenti come se fossero nuovi. La scadenza
revoca la sessione: occorre un nuovo handshake per riprendere. Su USB scollegata
azzerare tutti i nodi, eliminare la coda radio e richiedere nuove sessioni.
Non considerare il solo successo MAC una conferma di consegna HID.

## Test

`tests/test_hub_state.c` esegue il codice C reale con assert su sicurezza degli
stati, rollover, wrap temporale/sequenza, pacchetti malformati, saturazione,
disconnessione, revoca, ownership e commit. Include 256 combinazioni ripetibili
di movimento su cinque nodi/quattro assi e duplicati; confronto con somma attesa.

Può essere compilato su host con un compilatore C11 e `hub_state.c`; restituisce
0 al successo o la riga fallita. Non richiede il dispositivo.

Verifica eseguita il 12 ottobre 2026 con GCC Espressif 15.2.0, RV32IM,
`-Wall -Wextra -Werror`, emulatore Unicorn 2.1.4 isolato in build:

```powershell
python tests/run_tests.py --gcc <riscv32-esp-elf-gcc> --output <cartella-build>
```

Dipendenze Python: `pyelftools`, `unicorn==2.1.4`. `--deps` indica una cartella
di pacchetti di test isolata. Nessun pacchetto o binario viene vendorizzato.
Compilazione separata anche con toolchain ESP32-S3: nessun warning.

Questi test non misurano radio, USB, latenza, RAM sul dispositivo, cifratura
o comportamento del PC. Abbinamento, worker di trasporto, UI e prove reali
sono ancora da implementare; nessuna release stabile Hub a questo punto.
