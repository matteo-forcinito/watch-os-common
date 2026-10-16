# Gestione del dispositivo senza mirroring

Le pagine ottimizzate e la companion usano le API autenticate del webserver. GET /api/capabilities (versione 1) elenca servizi, disponibilita hardware, API e pagina. GET /api/apps distingue interaction=page da interaction=screen: non presenta il mirroring come interfaccia principale.

## Copertura corrente

Pagine delle app gia disponibili: Sveglie, Ecosistema, Market, Impostazioni, News, Giornata, Pieno, Copertura e Sentinel. Sezioni sistema: Wi-Fi, file, aggiornamenti e microSD. La pagina microSD e disponibile soltanto sui modelli con slot. La companion offre controlli nativi per impostazioni di base e microSD, catalogo app e accesso alle stesse pagine ottimizzate nel browser. Il browser conserva il proprio abbinamento; nessun token persistente viene inserito nell’URL.

Restano da convertire in pagine dedicate: Meteo, Timer, Sensori, Musica, Torcia, Batteria, Ombra e l’interfaccia di avvio del webserver. La gestione completa al 100% non e ancora dichiarata raggiunta. Le app esterne mantengono il proprio runtime; la recovery non permette il cambio di partizione che interromperebbe il controllo.

## Mirroring opzionale

GET /api/remote legge supported/enabled; POST con {enabled: true|false} cambia la preferenza e la salva subito in NVS. Default disattivato, anche migrando impostazioni precedenti. GET /api/remote/frame e POST /api/remote/input rispondono 403 quando disattivato. Le catture vengono allocate solo durante la richiesta e distrutte al termine; retained_frame_bytes=0 indica i buffer di cattura mantenuti tra richieste, non tutta la memoria LVGL. Il polling parte solo su scelta esplicita. LVGL resta per le app non ancora separate dai propri servizi, anche sul display virtuale di recovery.

## microSD

GET /api/storage: supporto slot, stato scheda, spazio e job (0 inattivo, 1 preparazione, 2 esecuzione, 3 completato, 4 errore), generation ed errore. POST {action: mount} rileva e monta. POST {action: format, confirmation: "FORMAT SD"} cancella la sola microSD, crea FAT32 e le cartelle apps/config. Impostazioni e abbinamenti NVS non vengono cancellati. Senza conferma esatta, senza slot/scheda o con USB/archiviazione occupata la richiesta viene rifiutata.

Il job usa un worker, non blocca HTTP o LVGL, acquisisce il lease file e chiude le altre app. Durante manutenzione le API diverse dal suo stato rispondono 503 per evitare file e aggiornamenti concorrenti. La registrazione/riproduzione delle note vocali condivide il lease e impedisce di formattare un file ancora aperto. Non e implementata la formattazione della memoria flash interna.

## Ecosistema

Il dettaglio del membro sul dispositivo, in dashboard e nella companion permette richiesta del codice web, risorse e concessioni. Le risorse annunciano slot SD e webserver disponibile. La raggiungibilita non resta valida oltre 90 secondi senza contatti. La dashboard suggerisce azioni in base a recovery, connettivita e batteria; la companion puo aprire il controllo di un membro con il codice ricevuto. Nessun token web persistente attraversa Family.

ESP-NOW mantiene un riferimento STA distinto da quello della connessione Wi-Fi: l’hotspot di fallback non deve spegnere l’interfaccia usata da Family. Tutti i membri devono comunque condividere un canale radio per parlarsi; non e un mesh multicanale. I comandi applicativi inoltrati senza LAN, streaming dei sensori, automazioni e revoca individuale dei client web sono sviluppi separati.

## Validazione e limiti

La formattazione reale di una scheda con dati non viene eseguita durante i test automatici. I test della companion verificano conferma esatta, assenza slot, job asincrono e nessuna richiesta di screenshot durante l’uso dei servizi. Le prove hardware verificano i rifiuti sicuri, le impostazioni e l’attivazione/disattivazione delle catture. Autonomia e portata radio non sono misurate.

Le richieste alla UI hanno un limite di tre secondi e contesti allocati con proprieta condivisa: una callback tardiva non accede allo stack della richiesta HTTP. Si mantiene al massimo un lavoro UI pendente; le API dirette restano disponibili se una cattura non termina. ui_pending segnala il caso e retained_frame_bytes=-1 indica memoria di cattura non ancora quantificabile mentre il lavoro e pendente. La manutenzione SD usa lo stesso ponte e rilascia il lease se la UI non risponde, senza iniziare la formattazione.

## Sviluppo successivo alla 1.8.0

Aggiunte API autenticate GET /api/battery, /api/sensors e /api/weather con pagine dedicate e annuncio capabilities. Batteria espone stime e valori sconosciuti, senza avviare test sotto carico o azzerare la storia. Sensori legge solo snapshot esistenti: fresh richiede acquisizione attiva e un campione non oltre un secondo; valori obsoleti non sono inclusi. Non accende l'IMU. Meteo legge dati memorizzati, origine e timestamp; nessun download o geocoding implicito. Le viste native della companion seguono le stesse regole e non effettuano polling. Non ancora incluse nella release 1.8.0.
