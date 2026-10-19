# Casa: Home Assistant sul dispositivo e sul PC

Casa è disponibile come app interna con `CONFIG_APPKIT_HOSTED` e mantiene il progetto esterno nella propria cartella. Dalla homepage dell'OS, tenere premuta l'ora per aprirla. Le gesture direzionali esistenti continuano a navigare nell'OS.

## Configurazione

Aprire Casa e toccare il titolo CASA per vedere indirizzo e codice di associazione. Dal PC sulla stessa rete aprire `/casa/` sul server del dispositivo, associarsi con il codice del pannello e scegliere **Configura**. Inserire l'URL dell'istanza Home Assistant e un token di accesso a lunga durata creato nel proprio profilo Home Assistant. Per Nabu Casa usare l'indirizzo remoto dell'istanza `https://….ui.nabu.casa`, non il portale dell'account. Non occorre che Home Assistant sia sulla stessa LAN del pannello.

**Leggi entità, stanze e dashboard** usa dal browser le API WebSocket di Home Assistant per caricare entità, stanze, dispositivi e configurazioni Lovelace accessibili all'utente. Scegliere fino a 256 entità e salvare. Il pannello si collega direttamente a Home Assistant tramite WebSocket, con verifica dei certificati per HTTPS/WSS: il PC serve per la configurazione, non come ponte durante l'uso.

Il token viene conservato nella NVS del dispositivo, non nel catalogo sulla SD né nel localStorage del browser; le risposte delle API espongono solo `has_token`. Le rotte della dashboard richiedono l'associazione dell'OS. **Scollega Home Assistant** cancella credenziali e catalogo di Casa, senza toccare altre app o file. Cambiare URL richiede l'inserimento esplicito del token per la nuova istanza.

## Navigazione e controlli

- **Stanze**: area assegnata all'entità, oppure area ereditata dal dispositivo.
- **Tipi**: termometri, termostati, prese, luci e altre categorie, usando dominio e `device_class`.
- **Dashboard**: gruppi dashboard/vista ricavati dalle configurazioni Lovelace, incluse schede annidate e schede area. Ogni entità può appartenere a quattro gruppi. Le viste dinamiche e le schede personalizzate che generano entità a runtime non sono ricostruite; la pagina segnala le limitazioni e offre **Apri Home Assistant** per la dashboard originale.

La UI nativa impagina gruppi ed entità a 12 elementi; la pagina PC aggiunge ricerca, filtro disponibilità e accesso a tutte le entità selezionate. Un tocco esegue l'azione principale; una pressione prolungata apre i dettagli. Luci, prese, termostati, tapparelle, ventilatori, media, scene e serrature usano i relativi servizi HA. Le entità indisponibili e i sensori di sola lettura non ricevono comandi; lo sblocco di una serratura richiede conferma nell'interfaccia.

Gli stati arrivano con `subscribe_entities`, limitato alle entità selezionate. I comandi restano in attesa dello stato confermato dal server; gli errori dei servizi sono mostrati nella UI. Dopo una disconnessione la coda dei vecchi comandi viene scartata. Chiudere Casa rilascia client WebSocket, task e relativo riferimento Wi-Fi; il modello e il buffer dei messaggi restano in PSRAM per la riapertura.

## Verifica

Il collaudo del BSP registra separatamente compilazione, flash, simulazione del protocollo e prove sull'istanza reale. Un test con server simulato non certifica l'accesso alla propria istanza Nabu Casa: per quello occorrono URL e token inseriti nella pagina di configurazione.
