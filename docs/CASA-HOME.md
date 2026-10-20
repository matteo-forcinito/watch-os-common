# Casa come homepage

In **Impostazioni → Display → Homepage** si può scegliere Classica oppure Casa. La stessa scelta è nella dashboard `/casa/`, sezione Configura. La preferenza rimane dopo il riavvio.

Casa mostra orologio, illustrazione meteo giorno/notte e prossima sveglia locale. I quattro pulsanti sono Alarmo, temperatura/umidità, termostato e consumi. La sveglia apre l'app Sveglie e continua a usare il servizio allarmi del sistema.

- Casa sostituisce la schermata principale senza invertire musica e app: musica a sinistra, app a destra e centro di controllo sopra (scorrimento verso il basso).
- Sotto Casa (scorrimento verso l'alto) c'è Tutte, con sei dispositivi per pagina e navigazione per stanze, tipologie e dashboard. Le notifiche sono accessibili dal centro di controllo; nella home Classica rimangono accessibili anche sotto il quadrante.
- Dentro le app rimane il gesto originale dal bordo sinistro per tornare/chiudere. Slider, quadranti, tastiera e schermata della sveglia mantengono la propria interazione.
- In tutte le pagine native Casa sono presenti i pulsanti inferiori Indietro, Home, Tutte e App; sui display rotondi vengono mostrate le rispettive icone.

Alarmo alterna disinserito e fuori casa con un tocco; la freccia apre le altre modalità supportate. Se serve un codice, viene richiesto prima di inviare il comando. Gli stati indisponibili non vengono presentati come spenti.

La temperatura è la media dei termometri interni disponibili; l'umidità usa i rispettivi dispositivi fisici. In automatico sono esclusi nomi esterni/balconi/giardini e temperature diagnostiche senza sensore di umidità associato. **Configura → Media dei sensori interni** permette di includere/escludere esplicitamente ciascun termometro selezionato. Un valore assente resta assente, senza diventare zero. I consumi sommano le potenze disponibili e indicano quanti misuratori sono online; la pagina dedicata conserva anche quelli offline e mostra corrente, tensione e potenza separatamente.

**Reimporta catalogo dal pannello** usa URL e token già salvati. Dopo una lettura completa sostituisce il catalogo in `config/casa-catalog.json` sulla SD; sui dispositivi senza SD usa il volume FAT interno, se disponibile. Una scrittura temporanea e una copia di recupero proteggono il catalogo precedente. La pagina riaperta carica questa copia senza rileggere il catalogo da Home Assistant. Le preferenze selezionate e le esclusioni dalla media sono separate; gli stati aggiornati arrivano sempre dalla sottoscrizione Home Assistant. Il catalogo completo non viene tenuto residente nella RAM del firmware. Cambiando server, la copia del server precedente non viene riutilizzata.

Le prove browser usano dati simulati, incluse le azioni Alarmo e termostato. La prova sul GUITION con Home Assistant reale verifica solo letture, catalogo e navigazione; non comanda allarme, riscaldamento o prese dell'abitazione.
