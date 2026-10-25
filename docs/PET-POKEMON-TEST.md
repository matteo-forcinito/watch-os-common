# Verifica Pet Pokémon — 25 ottobre 2026

Versione locale: `1.15.0-dev.1`, asset Petland inclusi in flash.

## Test host

- Lettore PKP1 reale compilato con MSVC `/W4 /WX`: file valido, CRC,
  trasparenza, offset, 13 casi malformati e tutte le 62 varianti reali.
- Renderer e archivio aspetti reali: 31 specie, normale/shiny, 496 frame,
  commit fallito, riavvio, identità invariata, ripristino e archivio pieno.
- Archivio child: nascita, cure, riavvio, rollback, trasferimenti e premi.
- Minigiochi: dimensioni dei canvas, checkpoint, storage e colori oltre
  l'indice 12, necessario per le palette Pokémon.

## Dispositivi

AMOLED 1.8 e AMOLED 1.75C: installazione del solo slot di sistema a `0x20000`,
con verifica del modello e backup precedente. Non sono stati scritti tabella
partizioni o NVS dal programma di installazione.

Sulla 1.75C la selezione dall'interfaccia è stata provata passando da
Charmander shiny a Charmeleon normale: seed `1d6a7653fc85e481` e XP 515
invariati. Provato anche il ripristino originale, poi rimesso Charmander
shiny scelto dall'utente. L'aumento precedente da XP 475 a 515 deriva dalle
cure manuali dell'utente durante le prove.

Sulla 1.8 selezione e ripristino conservano seed `1ee82c36f32ef0fb` e XP 1135.
Il pulsante Bagno porta felicità da 0 a 8 e XP da 1135 a 1139; viene mantenuto
Charmander normale. Controllati scena, etichette dei comandi e selettore sui
display rettangolare e tondo. Nessun IRQ di flush perso nei report raccolti.
Confermata la scelta persistente dopo l'installazione della build finale
su entrambi. In Arcipet sulla 1.8 il personaggio usa la sprite Charmander;
uscire dalla prova conserva XP 1139. Nella 1.75C il report finale misura
38 KB interni liberi, blocco maggiore 24 KB e circa 6,8 MB di PSRAM libera.
Le misure comprendono Wi-Fi e console; gli snapshot rallentano un passaggio
UI e non costituiscono una misura del frame rate dell'animazione.

Gli snapshot USB possono ancora perdere qualche riga durante il trasferimento:
le righe blu nei PNG diagnostici non sono parte della pagina disegnata.

Il Knob ha ricevuto e avviato la prima build con gli asset e risposto alle
prove della console. La build finale è disponibile, ma non è stata
reinstallata perché il dispositivo è stato scollegato per problemi USB/cavo
segnalati dall'utente.

## Limiti di questa versione

L'aspetto Pokémon è locale. Radio e companion telefono mantengono il
disegno originale; i dati di identità, cure e viaggio restano compatibili.
I frame sono quelli Idle/Walk già presenti in Petland; gli effetti della
scena rappresentano cibo, bagno e sonno. Non ci sono nuove animazioni dedicate
o evoluzioni automatiche.

I log USB, le schermate e i backup sono in `progress-files/pet-pokemon`
nel workspace, fuori dal repository. I backup NVS contengono dati privati
e non fanno parte degli artefatti distribuibili.
