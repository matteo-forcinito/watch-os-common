# Collaudo della dashboard

Da questa cartella, con Node.js:

```sh
npm ci
npx playwright install chromium
npm run server
```

In un secondo terminale eseguire `npm test`. `CASA_QA_BROWSER` permette di usare un eseguibile Chrome già installato; `CASA_QA_OUTPUT` sceglie la cartella delle catture (predefinita: cartella temporanea di sistema).

Il server di test usa solo dati inventati e ascolta su localhost:8769. Il browser intercetta l'URL WSS fittizio `casa-qa.example.test`: nessun token reale e nessun accesso a un'istanza HA sono necessari. Le prove verificano importazione, area ereditata/sovrascritta, dashboard annidate e schede area, categorie, payload dei comandi, entità offline, escaping dei nomi, mancata persistenza delle credenziali e layout mobile.

Il server espone anche `/api/websocket` per un collaudo del client firmware con lo stesso protocollo. Per questa sola prova si può impostare `CASA_QA_BIND=0.0.0.0` e usare il token fittizio `casa-qa-token`; a fine prova cancellare dal dispositivo esclusivamente la configurazione di test. Non sovrascrivere configurazioni HA già esistenti. Queste simulazioni non sostituiscono il collaudo con Home Assistant reale.

Dopo `npm test`, con un dispositivo di collaudo già associato al server OS, `node device-test.cjs` esegue anche le prove firmware. Richiede `CASA_QA_PAIRING_FILE`, percorso esterno al repository di un JSON privato `{"url":"http://IP-PANNELLO/","token":"TOKEN-ASSOCIAZIONE-OS"}`, e `CASA_QA_HA_URL`, indirizzo del server simulato raggiungibile dal pannello (`http://IP-PC:8769`). Non inserire token HA reali. Il test rifiuta di sovrascrivere credenziali HA esistenti e rimuove la propria configurazione nel blocco `finally`.

Le prove sul dispositivo verificano autorizzazione, redazione del token, sottoscrizione e delta degli stati, servizi luce/termostato, errori del servizio, token rifiutato, riconnessione, salvataggi ripetuti su FAT, richieste non valide e catalogo di 256 entità. Questa suite modifica soltanto la configurazione Casa del dispositivo di collaudo e invia comandi al server simulato.

Una luce fittizia ha un identificativo lungo esattamente 255 caratteri: viene importato e salvato senza troncamento e riceve un comando con relativo delta confermato. Un identificativo da 256 caratteri viene rifiutato con un messaggio esplicito. Il test interrompe le scritture e conserva la configurazione se nel frattempo compare un URL HA diverso da quello del simulatore.
