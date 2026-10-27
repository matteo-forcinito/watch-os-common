# Agent Deck per Watch OS

Mission control locale per Codex, Claude Code, build e rilasci. Include un'app interna Watch OS e un monitor Windows con dashboard sul PC. L'app pesa soprattutto in flash: snapshot e schermata vengono allocati solo quando la apri, nessun task o collegamento di rete dedicato resta attivo dopo la chiusura. Sul device sono disponibili fino a 16 tessere, quattro per pagina; tutti i dettagli restano sul PC.

## Avvio

Richiede Python 3.10+ e Git, senza pacchetti Python aggiuntivi. Dalla directory del repository:

```powershell
python tools/agent_deck/monitor.py serve --workspace <cartella-progetti>
```

Apri `http://127.0.0.1:9784/`. Il server ascolta solo sul loopback, non apre porte LAN e non viene installato automaticamente all'avvio. Ctrl+C lo ferma. La raccolta procede ogni cinque secondi, i repository ogni minuto. Il browser sospende il polling quando nascosto. Senza `--live-quota` il monitor non effettua richieste ai servizi degli agenti. La lettura opzionale delle quote usa esclusivamente il client Codex ufficiale.

Le sessioni Codex delle ultime 24 ore vengono lette da `CODEX_HOME` (default `~/.codex`) usando SQLite in sola lettura e la coda dei rollout JSONL. Le sessioni Claude Code vengono lette da `CLAUDE_CONFIG_DIR` (default `~/.claude`) sotto `projects`. Credenziali, auth.json, configurazioni di login e variabili d'ambiente dei processi non vengono letti. Non occorre avviare agenti dal monitor. Una sessione presente solo nel cloud e non sincronizzata localmente non e rilevabile.

## Tracciare build, test e rilasci

Avvia i comandi attraverso il monitor per avere un esito certo:

```powershell
python tools/agent_deck/monitor.py run --title "Firmware: build" --agent codex --kind build --cwd <repository> -- python <script-build>
```

Il processo mantiene la propria identita: PID, branch, commit iniziale, orari, codice di uscita e log separato. Un'uscita diversa da zero e un errore, mai un completamento. Se il wrapper si interrompe senza registrare un esito, il monitor segnala interruzione quando il PID non esiste piu. I processi di compilazione esterni vengono mostrati separatamente per nome/PID, senza attribuire loro un repository o un risultato ipotetico.

Per decisioni, attivita non rappresentate da un processo e passaggi di consegne:

```powershell
python tools/agent_deck/monitor.py event --id release-example --title "Release firmware" --agent codex --status needs_input --activity "Serve scelta del canale" --notes "Build superata. Scegli stable o dev nella sessione originale." --resume "codex resume <session-id>" --cwd <repository>
```

Aggiorna lo stesso ID con `--status working`, `done`, `error` o `interrupted`. Gli eventi sono dichiarazioni esplicite dell'agente, distinguibili dagli esiti dei processi. Non mettere password o token nei titoli e nelle note. I log rimangono nella cartella privata `~/.agent-deck`; non vengono serviti dal browser ne trasmessi al dispositivo. Le schede forniscono il comando per riprendere la sessione nel terminale e le note di consegna.

## Collegare il dispositivo

1. Apri **Agent Deck** sul device e **Collegamento / dashboard**. La rete si attiva solo per la durata dell'app.
2. Nella dashboard PC scegli **Collega dispositivo** e inserisci l'indirizzo LAN e il codice a sei cifre visualizzati sul device.
3. Il token viene conservato nel file privato `~/.agent-deck/device.json`; non viene restituito al browser. Il monitor ricorda il dispositivo al prossimo avvio. Se cambia indirizzo IP, ripeti l'associazione.

In alternativa usa `python tools/agent_deck/monitor.py pair --url http://192.0.2.10 --code 123456`, sostituendo IP e codice con quelli reali. Per un file di abbinamento privato gia esistente e disponibile `serve --device-config <file-privato.json>` (campi `url` e `token`). Non salvare questo file nel repository.

Il monitor invia solo snapshot limitati a 24 KiB verso `/agentdeck/api/snapshot`, autenticati con X-Key secondo il protocollo locale esistente di Watch OS. Non invia trascrizioni, credenziali degli agenti o log. La chiusura dell'app rifiuta gli aggiornamenti, senza riaprirla automaticamente. Dopo venti secondi senza heartbeat la schermata indica offline: i vecchi dati non confermano attivita in corso. Swipe orizzontale cambia pagina; tap apre la scheda; Back torna al deck. Il monitor PC ha piu spazio per branch, commit, repository, processi e note.

## Quote e sessioni

Il contesto mostra l'ultimo utilizzo del turno rispetto alla finestra dichiarata, non i token cumulativi. Le quote Codex vengono mostrate solo se presenti negli eventi locali, con percentuale residua e data di osservazione/reset. Con `python tools/agent_deck/monitor.py quota` vengono lette anche le quote reali dell?account CLI corrente tramite `account/rateLimits/read` dell?App Server ufficiale: il processo stdio temporaneo viene chiuso subito dopo la lettura. Il monitor non legge i token di login e non salva identificativi account. `serve --live-quota` aggiorna queste quote ogni due minuti. Sono separate dalle sessioni, che potrebbero appartenere ad account differenti. Rimangono visibili data di rilevazione e reset. Claude non dichiara qui quote account e non vengono stimate. **Non esiste un numero affidabile di sessioni rimanenti:** durata e consumo delle sessioni variano. Un dato assente e indicato come non disponibile.

## Simulatore (senza token)

`python tools/agent_deck/monitor.py sim` scrive tre sessioni Claude sintetiche (working, needs_input, done a rotazione ogni 4 s) in `~/.agent-deck-sim`, mai in `~/.agent-deck/claude-hooks`.
Per vederle in dashboard e sul device avvia il monitor con `serve --sim-dir ~/.agent-deck-sim` (sorgente `simulator`). `sim --dir <cartella> --interval 2 --steps 6` limita giri e cartella.
Per ogni sessione Claude reale il monitor riporta anche `context_percent` (ultimo messaggio rispetto a 200k, o 1M se il modello contiene `[1m]`), `tokens` e `last_event_age`; nessun costo viene stimato.

## Azioni e limiti

Il monitor osserva e fornisce il passaggio al terminale. Non inietta risposte nelle sessioni Codex/Claude, non approva permessi, non ferma processi esterni e non esegue shell dal browser. Le richieste di input vanno risolte nella sessione originale; l'assenza di eventi recenti viene segnalata come stato da verificare, non come una build ancora attiva. I task interni a un agente non esposti dai suoi file richiedono un evento esplicito o il wrapper `run`.

Questa integrazione riusa, su autorizzazione esplicita del proprietario, renderer
C++, font, sprite SVG, cappelli e animazioni di
[brushknight/agents-deck](https://github.com/brushknight/agents-deck).
Origine, revisione e adattamenti sono descritti in
`components/apps/agentdeck/UPSTREAM.md` e `upstream/PROVENANCE.json`.
Il trasporto resta lo snapshot Watch OS autenticato: non e il client ufficiale
Deck Link. Le azioni Answer/Focus/Interrupt/Dismiss upstream non sono abilitate
per sessioni soltanto osservate.

Sul nuovo renderer, tocca il titolo "agents" o tieni premuto per aprire filtri,
ordinamento e comandi Aetherion. Back torna dal dettaglio. Il filtro iniziale
mostra working/needs_input. Test e anteprime dei tre layout:
`python tools/agent_deck/test_panel.py` (Windows/MSVC).


Riferimento ufficiale Codex: https://learn.chatgpt.com/docs/app-server

## Verifiche

```powershell
python tools/agent_deck/test_monitor.py
python tools/agent_deck/generate.py
```

Il generatore mantiene la UI della dashboard device allineata a quella del PC. Il firmware valida schema, numero di tessere, dimensioni delle stringhe, stati, contesto, quote e ID duplicati prima di sostituire lo snapshot precedente.

## Assistente operativo Aetherion

Il nuovo firmware espone anche una coda autenticata di input assistente.
Dalla dashboard Aetherion seleziona una conversazione e premi Usa sul dispositivo.
Sul device: menu Agent Deck, Assistente / input. Puoi inviare una richiesta
o rispondere alla domanda mostrata con la tastiera touch. La risposta conserva
l'ID della domanda visualizzata: una domanda scaduta o appartenente a un'altra
conversazione non viene accettata. L'ACK significa input acquisito, non lavoro
completato. Il testo/risultato resta limitato; nessuna trascrizione intera al device.

Il Surface continua a interrogare solo l'AMOLED gia associata. Non apre porte
PC sulla LAN e non assegna al dispositivo permessi shell, flash o pubblicazione.
Il supporto richiede il nuovo firmware; l'assenza del flag assistant.supported
conserva la compatibilita con la precedente console a due comandi.

## Descrizione del lavoro osservato

Le sessioni Codex espongono obiettivo, passo osservato e ultimo aggiornamento testuale.
Il monitor legge una porzione iniziale e la coda del rollout: output molto grandi non
fanno sparire la richiesta iniziale. Il contesto JSON Aetherion viene escluso dalla
descrizione. Le tessere device ricevono lo stesso obiettivo nel campo activity,
compatibile con il firmware esistente. Una richiesta descrive il lavoro, non ne prova
il completamento; senza eventi disponibili non viene inventata una descrizione.

Le conversazioni di uno stesso provider sono raggruppate sotto un solo agente
Codex/Claude nel Deck e sul device. Il numero di sessioni resta visibile; la
dashboard Aetherion permette di aprire la singola conversazione. Metriche e token
appartengono alla sessione selezionata: non vengono sommati come uso corrente.

Con `serve --aetherion-data <cartella-dati>` il monitor legge in sola lettura
anche lo stato terminale delle conversazioni gestite da Aetherion. La corrispondenza
usa l'ID remoto esatto: un processo arrestato senza evento finale nel rollout non
rimane erroneamente attivo. Il servizio Aetherion configura questa opzione;
non vengono lette credenziali e non vengono avviati lavori dal monitor.
