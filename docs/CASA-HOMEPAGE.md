# Casa come schermata principale

La scelta **Impostazioni → Schermo → Homepage → Casa** cambia il contenuto del riquadro centrale della shell. La scelta è persistente e si applica prima di caricare la schermata iniziale. `app_home()` torna sempre alla shell: non apre Casa nello stack delle app e non ricrea il collegamento Home Assistant.

La composizione conserva la navigazione della shell:

- a sinistra: musica;
- a destra: menu delle app;
- sopra, con swipe verso il basso: controlli e accesso alle notifiche;
- sotto, con swipe verso l'alto: Tutte, con navigazione per stanze, tipi e dashboard.

La home e Tutte sono due istanze del componente Casa, ciascuna con widget, timer, filtri e callback propri. I pulsanti inferiori offrono Indietro, Home, Tutte e App. I dettagli aperti intenzionalmente dalle card usano l'app Casa; chiuderli riporta alla home che è rimasta montata. La home classica resta disponibile scegliendola nelle impostazioni.

La shell e le pagine dell'app condividono il servizio Home Assistant con acquisizioni bilanciate. Il servizio termina solo quando non rimane alcun proprietario. Il catalogo completo resta su SD o sul FAT interno; le pagine usano le entità selezionate e mostrano al massimo sei unità per pagina. Le medie considerano soltanto sensori interni disponibili e consentono esclusioni esplicite dalla configurazione web.

AppKit ospita una dashboard per volta. Un'altra app può usarla temporaneamente; quando Casa torna visibile nella shell, recupera la propria dashboard senza aprire un'app o riavviare il servizio Home Assistant. Lo stop di Casa verifica di possedere ancora la dashboard.

Per il collaudo verificare sia l'avvio con Casa salvata sia il ritorno da dettagli, app e tile adiacenti. Provare il cambio ripetuto Classica/Casa, la navigazione con pulsanti e gesture, le notifiche nei controlli e il mantenimento degli stati live. Le prove di navigazione non richiedono comandi ad allarmi, serrature o termostati reali. I test browser in `casa/tests` usano dati simulati per verificare anche queste azioni.
