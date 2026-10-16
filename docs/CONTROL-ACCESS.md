# Accesso dalla companion

BLE porta stato e negozia il controllo con GB({t:control, request:nonce}). Il firmware avvia Web in un worker transitorio. Solo un collegamento cifrato e bonded riceve URL e codice abbinamento; la persona non deve copiarli. Risposta correlata a dispositivo, nonce e generazione della connessione. Nessun token persistente nelle notifiche BLE o negli URL.

Un riferimento Web per connessione, riusato e rilasciato alla disconnessione. Consegna dalla coda NimBLE, sincronizzata con lo spegnimento Bluetooth. Richieste pendenti limitate: una nel firmware, una per dispositivo nella companion.

HTTP richiede LAN/hotspot raggiungibile. BLE non e un tunnel Wi-Fi; il sistema puo richiedere la connessione all'hotspot. Nessun aggiramento dei dialoghi di abbinamento. Il display puo essere spento/riacceso, il dispositivo completamente spento non riceve comandi.

POST JSON con Content-Length dei byte UTF-8: ESP-IDF rifiuta chunked. Non seguire redirect con credenziali. Connessioni malformate non devono compromettere richieste successive.

Ecosystem: 0 automatico, 1 background, 2 spento fuori recovery. Background sospeso durante audio, OTA e trasferimenti. Operazioni Family esplicite prioritarie. Recovery mantiene Family; ESP-NOW richiede lo stesso canale, non e una rete IP mesh. Autonomia da misurare.
