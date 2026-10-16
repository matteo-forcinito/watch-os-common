# Richieste successive - 16 ottobre 2026

Priorita corrente: correzione HTTP, bootstrap BLE cifrato, controllo nella sagoma, foto 1.75/1.75C, display remoto, accordion, modalita Ecosystem e pagine dei servizi. Verifiche e rilascio 1.8.1 / companion 0.4.1 prima di estendere protocolli e database.

Da completare senza perdere il perimetro concordato:

- Family: scambio esplicito di reti Wi-Fi, cifrato per membro, con permesso dedicato e salvataggio sul destinatario. Nessuna password nei log, nel gossip o nelle pagine elenco; non cambiare canale durante una sessione di recovery attiva.
- Risorse e configurazioni: trasferimenti versionati per Giornata e Sentinel, controllo integrita, quote, job asincroni e area pending per conflitti. Non sovrascrivere il database attivo da un pacchetto remoto.
- Sentinel/Copertura: adottare il linguaggio visivo di Copertura su dispositivo e Web; valutare una base dati/servizi condivisa, mantenendo scansioni e storicizzazione separati. Sentinel Lite deve aprire lo stesso database della versione esterna tramite un contratto versionato, non due copie che divergono. Caldo/freddo e cambio rete sono low priority.
- Avviso allontanamento companion: opt-in per dispositivo, perdita collegamento con periodo di tolleranza e cancellazione al rientro, esclusione degli scollegamenti manuali, notifica locale. RSSI non equivale a distanza: nessuna promessa di metri o protezione garantita se OS/app viene terminato. Variante autonoma con ricerca BLE dopo misure di costo radio.
- Tendina: verificare le app che catturano gesture o occupano layer_top; accesso quasi globale con eccezioni esplicite per trasferimenti, dialoghi critici e app esterne con firmware indipendente.
- Remote esterna: selezione esplicita del destinatario prima della connessione, priorita bassa.
- Affidabilita app esterne: recovery e controllo devono essere un contratto comune del runtime, non soltanto feature del firmware base. Il codice odierno esegue un'altra partizione. Prima di una app risparmio senza LVGL occorre separare input/servizi/radio/Web dalla UI e prevedere recovery bootloader e persistenza del comando recovery.

Il background Family non e gratuito: ESP-NOW e Wi-Fi/BLE condividono risorse. Misurare RAM, duty cycle e autonomia prima di dichiarazioni quantitative. La prima implementazione mantiene il servizio Web mentre il collegamento BLE autorizzato possiede il riferimento; chiudere o disconnettere rilascia il riferimento. Questa scelta evita di interrompere operazioni mentre si passa ad altre pagine; serve una politica di lease piu granulare per il futuro risparmio.
