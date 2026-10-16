# Collaudo recovery e controllo remoto - 16 ottobre 2026

Firmware 1.8.0 di collaudo installati via USB, hash verificati e NVS/file preservati. Nessuna release pubblicata.

| Controllo su hardware reale | AMOLED 1.75 COM14 | AMOLED 1.75C COM10 |
| --- | --- | --- |
| Identita e dimensione flash | amoled175_watch, 16 MB | amoled175c_watch, 32 MB |
| Ingresso/uscita recovery e persistenza | Passato | Passato |
| Chiave senza scadenza, valida dopo reboot | Passato | Passato |
| Screenshot e apertura app interne | Passato | Passato |
| Touch modifica il controllo reale e ripristina la preferenza | Passato | Passato |
| Perdita Wi-Fi -> hotspot -> rete nota, stessa chiave | Passato | Passato |
| Family in ascolto dopo recupero rete | Passato | Passato |
| Stato finale | Recovery, pannello/touch esclusi | Normale, web aperto |

La 1.75C rifiuta endpoint e chiave recovery fuori dalla recovery: verificato. Su COM14 il gesto BOOT e stato eseguito dall’utente e la recovery persistente confermata dal PC. Per il gesto: dopo l’avvio tenere BOOT cinque secondi e rilasciare; stesso gesto per uscire. La chiave rimane distinta dal normale abbinamento.

Build finali riuscite: 1.75, 1.75C, AMOLED 1.8, Knob 1.8 e SenseCAP Watcher. Test C del protocollo pairing/Family passati (formato, limiti, transcript binding, epoch e anti-replay). Le build installate includono la console USB diagnostica, esclusa nelle normali configurazioni di rilascio.

La companion include lo schermo remoto Windows/Android. 25 test e analisi statica passati; build native GitHub riuscite e avvio del pacchetto Windows verificato. Run privata: https://github.com/matteo-forcinito/Orbit-Companion/actions/runs/38832169601. Nessuna release companion pubblicata in questa fase.

Non ancora verificati: autonomia e portata radio. Speaker C confermato funzionante dall’utente (MCLK GPIO16). Family ora dispone di concessioni per membro, richiesta cifrata del codice web e letture passive delle risorse; inoltro del controllo senza LAN e continuita delle app esterne richiedono sviluppi ulteriori: vedi REMOTE-CONTROL.md.

Chiusura finale: il blocco delle app esterne e applicato anche nel servizio extapp_launch, prima di qualsiasi accesso ai file o cambio della partizione. Copre quindi launcher, touch remoto e API. Ricompilati con successo tutti e cinque i target dopo questa protezione.


Aggiornamento Family del 16 ottobre 2026: test fisici passati su COM14/COM10 per diniego iniziale, risposta cifrata recovery senza codice, risorse in entrambe le direzioni, revoca della concessione e conservazione dei permessi dopo riavvio. In modalita normale il codice a sei cifre richiesto via Family e stato riscattato con successo tramite /api/pair. I due dispositivi dell’utente hanno ora concessioni reciproche per codice web e risorse; gli altri membri mantengono i permessi disattivati. Sul firmware finale verificati di nuovo touch remoto su entrambi e perdita Wi-Fi/hotspot/rientro rete sulla 1.75. Le letture IMU non hanno forzato l’avvio del sensore e non hanno dichiarato validi campioni assenti.

Companion aggiornata con pagina Family: 26 test locali passati e analisi statica senza segnalazioni. Pipeline nativa di sola validazione: https://github.com/matteo-forcinito/Orbit-Companion/actions/runs/38863269497. Nessuna release pubblicata.
