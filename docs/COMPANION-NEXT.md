# Richieste successive - 16 ottobre 2026

Priorita corrente: stabilita del collegamento e chiusura della companion, gestione rete senza indirizzi manuali, comandi diretti affidabili. Produzione: firmware 1.8.2 e companion 0.4.1. Candidati su fix/connection-stability; nessuna nuova release stabile prima del collaudo LAN domestico e della chiusura nativa Windows con dispositivi reali.

Da completare senza perdere il perimetro concordato:

- Family: scambio esplicito di reti Wi-Fi, cifrato per membro, con permesso dedicato e salvataggio sul destinatario. Nessuna password nei log, nel gossip o nelle pagine elenco; non cambiare canale durante una sessione di recovery attiva.
- Risorse e configurazioni: trasferimenti versionati per Giornata e Sentinel, controllo integrita, quote, job asincroni e area pending per conflitti. Non sovrascrivere il database attivo da un pacchetto remoto.
- Sentinel/Copertura: adottare il linguaggio visivo di Copertura su dispositivo e Web; valutare una base dati/servizi condivisa, mantenendo scansioni e storicizzazione separati. Sentinel Lite deve aprire lo stesso database della versione esterna tramite un contratto versionato, non due copie che divergono. Caldo/freddo e cambio rete sono low priority.
- Avviso allontanamento companion: opt-in per dispositivo, perdita collegamento con periodo di tolleranza e cancellazione al rientro, esclusione degli scollegamenti manuali, notifica locale. RSSI non equivale a distanza: nessuna promessa di metri o protezione garantita se OS/app viene terminato. Variante autonoma con ricerca BLE dopo misure di costo radio.
- Tendina: verificare le app che catturano gesture o occupano layer_top; accesso quasi globale con eccezioni esplicite per trasferimenti, dialoghi critici e app esterne con firmware indipendente.
- Remote esterna: selezione esplicita del destinatario prima della connessione, priorita bassa.
- Affidabilita app esterne: recovery e controllo devono essere un contratto comune del runtime, non soltanto feature del firmware base. Il codice odierno esegue un'altra partizione. Prima di una app risparmio senza LVGL occorre separare input/servizi/radio/Web dalla UI e prevedere recovery bootloader e persistenza del comando recovery.

Il background Family non e gratuito: ESP-NOW e Wi-Fi/BLE condividono risorse. Misurare RAM, duty cycle e autonomia prima di dichiarazioni quantitative. La prima implementazione mantiene il servizio Web mentre il collegamento BLE autorizzato possiede il riferimento; chiudere o disconnettere rilascia il riferimento. Questa scelta evita di interrompere operazioni mentre si passa ad altre pagine; serve una politica di lease piu granulare per il futuro risparmio.

## Collaudo diagnostico 1.8.3-dev.4

- Cattura del rendering normale (include overlay), allocazione RGB565 temporanea; formato BMP preservato e ORF1 compatto opzionale. RLE limitato, fallback raw per immagini poco comprimibili.
- Risvegli UI su semaforo dedicato: LVGL usa le notifiche FreeRTOS del task per sincronizzare il draw worker. Condividere quel canale poteva perdere il risveglio del comando e lasciarlo in coda fino a un evento successivo; riprodotto come timeout da 3 s.
- Dopo la correzione: AMOLED 1.75 recovery 10 frame + Impostazioni/Home, media 0,390 s e massimo 0,510 s; AMOLED 1.75C normale stesso ciclo, media 0,401 s e massimo 0,511 s. Nessun frame trattenuto dopo le richieste, preferenza mirroring ripristinata; nessuna operazione SD di scrittura/formattazione nel collaudo.
- Un frame hardware decodificato da Web e Dart produce lo stesso hash RGBA; test del decoder web ripetibile con `node tools/test_remote_frame.cjs [frame.orf]`.
- Queste sono misure su hotspot PC con diagnostica, non prove di autonomia, di video fluido o di raggiungibilita sulla rete domestica. Il collaudo Windows ha trovato un processo che resta attivo dopo la chiusura, non riprodotto dalla CI pulita: resta bloccante.
