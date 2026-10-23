# Pokémon: combattimenti e pet personale senza appesantire il sistema

Valutazione del 23 ottobre 2026, svolta **dopo** la pubblicazione della
stabile 1.14.0 sui sei modelli e di Petland 1.0.0 nei sei pacchetti.
Questa pagina descrive una proposta: le funzioni nuove qui sotto non sono
ancora implementate.

## Esito

Sì: possiamo scegliere un Pokémon come compagno personale mantenendo cure,
identità e viaggi dei pet. Conviene separare tre cose:

- **Sistema:** identità del pet, fame, energia, umore, compagnia e viaggi.
- **Aspetto opzionale:** specie Pokémon, forma e variante shiny, con un
  piccolo lettore di sprite condiviso e ritorno al disegno originale se manca
  il pacchetto. Nessuna logica di combattimento nel sistema.
- **Petland:** collezione, mosse, natura, statistiche, lotte, effetti ed
  evoluzioni. Il firmware esterno continua a occupare il proprio slot.

La prima integrazione consigliata è «usa questo Pokémon come aspetto del
mio pet». Non crea un secondo pet e non trasforma una cattura in un ospite
che può duplicarsi fra dispositivi. Importare davvero una creatura, con la
sua identità e tutta la progressione di combattimento, è una fase successiva
con più regole da definire.

## Cosa c'è oggi, e quali vincoli impone

| Parte | Stato verificato nel codice |
| --- | --- |
| Pet del sistema | `pet_record_t` conserva seed, origine, transazione, cure, XP e tempi. Non ha specie Pokémon, natura o mosse. |
| Identità e aspetto | Il seed determina il genoma e una delle otto specie originali. Il livello dipende dagli XP del pet. Non va reinterpretato per assegnare una specie Pokémon. |
| Salvataggio del pet | `pet_init()` accetta il blob NVS soltanto se dimensione e magic coincidono. Aggiungere campi alla struttura senza migrazione renderebbe il vecchio salvataggio non caricabile. |
| Viaggi | `pet_travel.c` usa passi durevoli e idempotenti. Il messaggio di ricezione contiene cure, XP, tempi e origine; nessun dato Pokémon. Il massimo è 120 byte, oggi ne usa fino a 74. |
| Disegno del sistema | Griglia indicizzata 32×32, palette di 12 colori e sei pose; scena fino a tre attori. Companion, miniature e minigiochi usano lo stesso sistema originale. |
| Disegno Pokémon | Il pacchetto esterno contiene 31 specie, normali/shiny, fino a quattro frame Idle/Walk orientati verso il basso, 40×40, palette RGB565 e trasparenza. Non offre già tutte le sei pose del pet. |
| Salvataggio RPG | Modello v3 di 1.384 byte su un limite condiviso di 1.536: restano **152 byte**, non spazio illimitato per nuove statistiche. |
| SDK esterno | Avvio e ritorno sicuro al sistema. Non esiste ancora un'API per proporre un nuovo aspetto al pet personale. |

Riferimenti locali: [pet.h](../components/apps/pet/pet.h),
[pet_store.c](../components/apps/pet/pet_store.c),
[pet_travel.c](../components/apps/pet/pet_travel.c),
[pet_app.c](../components/apps/pet/pet_app.c),
[pet_sprite.h](../components/orbi/include/orbi/pet_sprite.h),
[world.c](../petland/main/world.c),
[pokemon.c](../petland/main/pokemon.c),
[SDK](../components/extapp_sdk/include/extapp_sdk.h).

## Mosse, tipi, nature e statistiche

Il costo di calcolo è piccolo: il combattimento è a turni. La complessità
vera è rendere le regole coerenti e leggibili sul display.

Proposta per il motore di Petland:

1. Una tabella per specie con uno o due tipi, sei statistiche base e
   condizioni evolutive esplicite. Tipi e statistiche della forma evoluta
   dipendono dalla specie attuale, non soltanto dalla famiglia iniziale.
2. Mosse con ID stabile, tipo, categoria fisica/speciale/stato, potenza,
   precisione, PP, priorità ed effetto. Quattro mosse per creatura e scelta
   esplicita quando se ne apprende una nuova.
3. Tabella dei 18 tipi, anche se inizialmente le specie disponibili ne
   usano meno. Una cella per interazione: 18×18 = **324 byte** usando
   un byte per cella. I due tipi difensivi combinano i moltiplicatori:
   immunità, ¼, ½, 1, 2 e 4. Applicare il bonus del tipo della mossa,
   distinguendolo dall'efficacia sul bersaglio. Messaggi chiari dopo il colpo.
4. Natura immutabile per individuo: coppia di statistiche favorita e
   penalizzata, comprese nature neutre. Le statistiche effettive si
   calcolano con interi da specie, livello e natura; non serve salvarle
   tutte come copie. IV/EV sono una decisione separata per una fase successiva.
5. Separare stato persistente della creatura e condizioni del turno:
   scottatura, veleno, sonno, paralisi ecc. da una parte; modificatori,
   protezione e cariche dall'altra. Definire ordine, durata, immunità,
   cura, cambio compagno e KO per ogni effetto. Nessun timer continuo.
6. Sul Knob una selezione breve delle quattro mosse; sul touch pulsanti
   grandi. Mostrare tipo, PP, effetto e indizio sulla prossima azione
   nemica. Mantenere spazio per cambio, oggetti e fuga.

**Budget di progetto, non misure di una funzione già sviluppata:** fino a
16 KB di tabelle e testi di combattimento in flash per questo primo roster,
e poche centinaia di byte di stato aggiuntivo in PSRAM. Nessuna nuova task
per creatura e nessuna allocazione durante la risoluzione del turno.
Il budget deve essere verificato sul binario e sui device prima del rilascio.

Il limite di 152 byte richiede prima un formato RPG v4: campi piccoli con
limiti controllati, statistiche derivate, dati transitori fuori dal blocco
persistente. Non basta aggiungere array alla struttura attuale. Mantenere
il limite di 1.536 byte se possibile; se la collezione lo supera, progettare
un archivio dedicato all'RPG invece di aumentare automaticamente tutte le
otto cache di salvataggio del motore comune.

La migrazione deve accettare davvero v3 nel backend, che oggi controlla
versione e dimensione prima di chiamare il gioco: cambiare soltanto la
versione nel descrittore non migra i progressi. Vecchi dati originali di
Cronache del Faro e dati di Legami selvatici restano distinti.

## Come collegare Petland al pet personale

### Identità e persistenza

Conservare seed, origine, cure e stato dei viaggi. Salvare un piccolo
record di aspetto separato e versionato, indicizzato dal seed: provider,
ID nazionale stabile della specie, forma, shiny e revisione. Non usare
l'indice dell'attuale array di dodici famiglie come identificatore pubblico.
I vecchi pet senza questo record conservano il loro aspetto originale.

Petland deposita una richiesta di scelta nella memoria condivisa tramite
una piccola API SDK. Il sistema, al ritorno, controlla versione, specie
supportata e pet di destinazione, poi applica la scelta. Richiesta con ID
idempotente, scrittura durevole e conferma: niente modifiche dirette al
blob privato `orbi_children/state` da parte del gioco. Non cambiare aspetto
a un pet in viaggio; applicare soltanto sul proprietario quando è a casa.

Gli XP del pet e il livello di combattimento non vanno sommati o copiati
liberamente. Nella prima fase il collegamento riguarda l'aspetto; un futuro
trasferimento di progressione richiede proprietà, ricompense limitate e
regole contro la duplicazione.

### Disegno e pacchetto opzionale

Estrarre un'interfaccia di disegno condivisa, con implementazione originale
sempre disponibile. Adattare sia la scena sia miniature, companion e attore
dei minigiochi: cambiare solo la pagina del pet darebbe aspetti incoerenti.

Il pacchetto attuale ha circa **350 KB di pixel**, più palette e indice.
Questi byte sono nel firmware esterno: il sistema non può semplicemente
usare i puntatori C dell'altra applicazione dopo un riavvio. Serve un
pacchetto dati indipendente nella memoria file interna o SD, con indice,
versione, dimensioni e checksum validati. Il solo flash USB del firmware
Petland non installa automaticamente questo futuro pacchetto per il sistema.

Caricare fuori dal thread grafico soltanto i frame degli attori visibili,
con cache limitata in PSRAM e coordinamento con l'accesso USB/storage.
Si possono normalizzare a 32×32 per riusare le dimensioni dei canvas
esistenti, con un adattatore di palette/trasparenza. I dati indicizzati di
un frame 40×40 occupano 1.600 byte; tre frame visibili occupano 4.800 byte,
escluse palette e canvas già presenti. Un obiettivo ragionevole è una
cache aggiuntiva complessiva di **8–12 KB in PSRAM**, da misurare.

Animare il Pokémon quando cambia frame, fino a 4 Hz iniziali; non leggere
file né decodificare PNG a ogni tick. Il normale movimento della scena
mantiene il suo timer. Il pacchetto attuale non contiene mangiare, dormire
e tutte le direzioni: servono una rappresentazione semplice con effetti
della scena o nuovi asset. Non promettere quelle animazioni prima di averle.

### Viaggi e compatibilità

I 46 byte nominalmente liberi nel messaggio di ricezione sono sufficienti
per un piccolo descrittore di aspetto, non giustificano cambiare il formato
senza negoziazione. Aggiungere capacità/versione e metadati opzionali
validati; renderli durevoli prima del passo di partenza e coerenti durante
retry, annullamento e ripresa dopo riavvio. Non creare una seconda
transazione indipendente per l'aspetto che possa restare disallineata.

Un destinatario vecchio o senza pacchetto mostra l'animale originale
derivato dal seed e conserva, dove supportato, il descrittore per il ritorno.
Non trasferire sprite via radio durante il viaggio. Anche l'annuncio
`OP_INFO`, le miniature degli amici e il companion telefono avranno bisogno
di un'estensione per mostrare il Pokémon, mantenendo il fallback precedente.

## Prestazioni e ordine consigliato

Misure di avvio delle build di produzione installate via USB:

| Dispositivo | Petland.bin | RAM interna libera | Blocco interno maggiore | PSRAM libera |
| --- | ---: | ---: | ---: | ---: |
| AMOLED 1.75C | 1.582.256 byte | 203 KB | 98 KB | 8.152 KB |
| Knob | 1.613.088 byte | 201 KB | 98 KB | 8.154 KB |

Sono misure dell'app appena avviata, **non** memoria disponibile del sistema
con Bluetooth attivo né un benchmark delle nuove regole. Nel sistema 1.14.0
entrambi hanno superato il controllo di 44 KB contigui all'avvio Bluetooth.
Quella riserva interna va preservata: niente grandi cache nella RAM interna.
Gli altri quattro modelli hanno build verificate; queste misure fisiche
riguardano soltanto i due dispositivi collegati.

Ordine proposto:

1. Regole di combattimento in Petland, dati compatti e migrazione v3.
2. Lettore di aspetto opzionale nel sistema, inizialmente un solo Pokémon,
   senza cambiare salvataggi né viaggio del pet esistente.
3. Scelta da Petland tramite SDK, poi estensione versionata dei viaggi.
4. Più specie, pose e progressione condivisa soltanto dopo le misure.

Prima del rilascio: test di interazioni fra tipi, nature, effetti, PP,
cambio/KO; migrazione dei salvataggi esistenti; pacchetto assente o corrotto;
prove di viaggio fra versioni diverse, riavvio in ogni fase e ritorno a casa;
misure di refresh, tick, PSRAM, RAM interna e Bluetooth su AMOLED e Knob.

**Conclusione:** non serve riscrivere cure o trasporto dei pet. Servono
estensioni mirate a disegno, metadati, SDK e protocollo. Il costo delle
regole è contenuto; grafica e compatibilità richiedono più attenzione.
Tenere l'RPG esterno e gli sprite opzionali permette di lasciare leggero
il progetto di base.
