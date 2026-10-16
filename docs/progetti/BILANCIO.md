# Progetto: app "Bilancio" (gestione del denaro)

Stato (17/10/2026): **passi 1, 2 fatti; 3 essenziale** (`ui.c`: Home con arco, Questo mese, Spesa
veloce, Menu/Dashboard) e **6 fatto in `watch-os-common`** (registrazione). Compilato e installato su
AMOLED 1.75C, avvio pulito. Mancano: screenshot su tutte le forme, versione esterna (`main.c`, passo 7),
icona, release 1.9.0. **Passo 2b da fare**: matematica dei debiti (capitale residuo, interessi, TAN
ricavato) e bollette a consumo (stima stagionale): tipi già in `calc.h` (`bl_loan_t`, `bl_usage_t`,
`bl_period_t`), funzioni, salvataggio e dashboard ancora da scrivere.

Obiettivo: non un registro di spese, ma un'app che risponda ogni giorno a tre domande:

1. **Quanto posso spendere oggi** senza arrivare in rosso allo stipendio?
2. **Cosa mi aspetta** questo mese (rate, abbonamenti, spese fisse)?
3. **Cosa sto sbagliando** (dove vanno i soldi, cosa cresce, quanto pesa il fisso)?

## Scelta architetturale

App **hosted**, come Pieno e Giornata ([guida pagina 5](../guida/05-app-esterne.md)):

- **interna** in tutti i firmware (`CONFIG_APPKIT_HOSTED`), quindi arriva a tutti con la
  prossima release OTA dei firmware;
- **esterna** per il Market, con lo stesso codice (pacchetto per ogni scheda);
- **dashboard web** sull'orologio (`/bilancio/`) per inserire dati e vedere i grafici: i
  dati si inseriscono comodamente dal telefono o dal PC, l'orologio mostra il riassunto.

I dati restano **sul dispositivo** (nessun cloud), la dashboard richiede l'abbinamento
(token `X-Key`) come tutte le altre.

```
watch-os-common/bilancio/
├─ main/
│  ├─ bilancio.h          tipi e API
│  ├─ calc.c / calc.h     LOGICA PURA (niente ESP-IDF): date, ricorrenze, stipendio, budget, analisi
│  ├─ store.c             caricamento e salvataggio JSON (storage_config_path("bilancio.json"))
│  ├─ ui.c                schermate sull'orologio
│  ├─ web.c               API della dashboard
│  ├─ app.c               app_t (versione interna)
│  ├─ main.c              app_main (versione esterna)
│  └─ bilancio_dashboard.html
├─ tests/
│  ├─ test_calc.c         test della logica
│  └─ run_tests.py        copia di pieno/tests/run_tests.py
└─ tools/create_icon.py
```

## Dati (`/data/config/bilancio.json` o `/sdcard/config/bilancio.json`)

```json
{
  "version": 1,
  "currency": "EUR",
  "balance": {"amount": 1240.50, "date": "2026-10-17"},
  "income": [
    {"id": 1, "name": "Stipendio", "amount": 1850.00, "day": 27, "weekend": "before"}
  ],
  "recurring": [
    {"id": 10, "name": "Rata auto",  "amount": 289.00, "every": "month", "day": 5,  "category": "auto",   "end": "2028-06"},
    {"id": 11, "name": "Affitto",    "amount": 650.00, "every": "month", "day": 1,  "category": "casa"},
    {"id": 12, "name": "Assicurazione", "amount": 540.00, "every": "year", "month": 3, "day": 15, "category": "auto"},
    {"id": 13, "name": "Netflix",    "amount": 13.99,  "every": "month", "day": 20, "category": "abbonamenti"}
  ],
  "transactions": [
    {"id": 100, "date": "2026-10-15", "amount": -42.30, "category": "spesa", "note": "Supermercato"}
  ],
  "categories": ["casa", "auto", "spesa", "abbonamenti", "salute", "svago", "altro"]
}
```

- Importi in **centesimi interi** nel C (`int64_t`), mai `float`: niente errori di arrotondamento.
- Le ricorrenze generano le scadenze al volo: non si salvano mille righe future.
- `"weekend": "before"` = se il giorno cade sabato/domenica, lo stipendio arriva il **venerdì
  prima** (caso più comune in Italia); `"after"` = lunedì dopo; `"exact"` = nessuna correzione.
  Opzionale in futuro: festività nazionali (1/1, 6/1, Pasqua e Pasquetta, 25/4, 1/5, 2/6,
  15/8, 1/11, 8/12, 25-26/12).
- Ultime 24 mensilità di movimenti, il resto riassunto per mese (spazio limitato).

### Com'è stato realizzato (passi 1–2)

Il file reale ha in più: `"next_id"`, `"balance": {..., "after": 812}` (i movimenti con id più alto sono
stati registrati dopo il saldo), `"rule": 13` nei movimenti collegati a una voce fissa, `"archive"`
(mesi oltre i 24, solo entrate/uscite) e `"every"` che può valere `"month"`, `"bimonth"`, `"quarter"`,
`"four"`, `"semester"`, `"year"` oppure un numero di mesi oltre l'anno (`24` revisione, `120` patente).

Regole decise scrivendo il codice (commentate in `calc.h`):

- **Saldo stimato oggi** = saldo inserito + movimenti registrati dopo (id più alto, data non precedente)
  + entrate dopo quel giorno fino a oggi incluso − spese fisse dopo quel giorno e prima di oggi.
  Il saldo inserito comprende già le voci del suo giorno (la dashboard lo ricorda quando lo inserisci).
- **Pagamento collegato**: un movimento con `rule` paga la data di quella voce entro ±12 giorni, così
  la bolletta vera sostituisce la stima e non conta due volte. Lo storico dei pagamenti collegati
  mostra per ogni bolletta ultimo importo, media e scarto dalla stima.
- **Movimenti previsti**: un movimento con data futura (rimborso 730, bollo, bonus) entra subito nel
  budget del periodo e nel saldo quando arriva il giorno.
- **Periodo**: da oggi al prossimo stipendio escluso (stipendio = la più grande entrata mensile; senza
  entrate, fine mese). Budget giornaliero fissato all'inizio della giornata: "Oggi puoi spendere
  ancora X" scende con le spese di oggi, il budget del giorno no.
- **Ritmo**: spese variabili (non collegate a voci fisse) degli ultimi 30 giorni, con almeno 7 giorni
  di divisore. **Tendenza per categoria**: dal 1° a oggi contro gli stessi giorni dei 3 mesi prima
  (solo mesi già registrati).
- **Oltre l'anno** (`every` > 12) il mese di `start` è la prossima scadenza e ancora il ciclo; la quota
  da mettere da parte conta già da ora.
- **Accantonamento**: somma delle quote mensili delle spese non mensili (condominio, assicurazione...).
- Massimi: 64 voci fisse, 3000 movimenti, 16 categorie. Categorie predefinite: casa, bollette, rate,
  auto, spesa, abbonamenti, salute, svago, tasse, altro.

File: `calc.c/.h` (logica pura), `store.c` (libro in PSRAM, salvataggio 1,5 s dopo l'ultima modifica,
scrittura atomica `.new` + rename, JSON scritto a mano per non creare 20.000 nodi cJSON), `web.c`
(API in centesimi, date `YYYY-MM-DD`, elenco nell'intestazione del file), `bilancio_dashboard.html`.

Verifiche fatte: `tests/run_tests.py` (passa nell'emulatore RISC-V; una prova con un valore sbagliato
fallisce come deve); `calc.c`, `store.c`, `web.c` compilati con `-Werror` e i flag esatti della build
AMOLED 1.75C (solo sintassi: il linking arriva con il passo 6); JavaScript della pagina con
`node --check`; dashboard vista in Edge headless (1440 px e 375 px) con un server finto che imita
l'API. **Non provato su hardware.**

## I calcoli (tutti in `calc.c`, tutti testati)

| Funzione | Cosa calcola |
|---|---|
| `next_payday(today, income)` | data del prossimo stipendio con la regola del weekend |
| `days_to_payday` | giorni rimanenti (mostrato come "Stipendio tra 10 giorni, venerdì 27") |
| `due_until(today, payday)` | rate e spese fisse che scadono **prima** dello stipendio |
| `free_to_spend` | saldo − scadenze fino allo stipendio |
| `daily_budget` | `free_to_spend / giorni allo stipendio` → **"Oggi puoi spendere 18 €"** |
| `month_fixed` | totale fisso del mese (mensili + quota del mese delle annuali) |
| `fixed_ratio` | fisso / entrate. Sopra il 50% è un campanello d'allarme |
| `category_trend` | spesa per categoria: mese corrente vs media degli ultimi 3 mesi |
| `subscriptions_total` | totale annuo degli abbonamenti ("Netflix + Spotify = 287 € l'anno") |
| `end_of_month_forecast` | saldo previsto il giorno prima dello stipendio, al ritmo di spesa attuale |

Test minimi: stipendio il 27 di sabato → 26 venerdì; il 1° di domenica con "before" → venerdì
del mese precedente; rata annuale nel mese giusto; rata con `end` scaduta che non conta più;
budget giornaliero con saldo negativo (deve dire 0 e segnalare il problema).

## Sull'orologio (schermi tondi e rettangolari)

Linguaggio visivo preso da **Copertura** (`copertura/main/ui.c`, schermata LIVE), che sui tondi rende
benissimo: fondo quasi nero (`0x060B12`), **arco a 270°** (`lv_arc`, rotazione 135°) grande quanto lo
schermo meno 20 px con spessore `size/16`, numero enorme al centro con l'unità piccola accanto, una
parola colorata sotto, un mini-grafico **dentro** l'arco in basso e una fila di **bottoni rotondi**
da 56 px in fondo. Sul rettangolare l'arco va in alto e il grafico sotto.

1. **Home** (come LIVE di Copertura):
   - arco esterno = quanto resta del budget di oggi (`today_left / daily`), colore di stato:
     verde in linea, arancione sopra il ritmo, rosso se la previsione va sotto zero;
   - al centro "**18**,40 €" (`ui_font_big`), sotto "oggi" e "Stipendio tra 10 giorni";
   - dentro l'arco in basso la linea del saldo previsto fino allo stipendio (`lv_chart`, come il
     segnale dell'ultimo minuto in Copertura), una tacca rossa se scende sotto zero;
   - bottoni rotondi: **+** spesa veloce (verde), **lista** questo mese, **lampadina** consigli,
     **ingranaggio** menu/dashboard. La rotella (Knob) scorre tra i bottoni.
   - Stessi numeri della dashboard: `bl_summary()`.
2. **Questo mese**: elenco delle scadenze (rata auto 5/10 ✓ pagata, Netflix 20/10...), totale fisso.
3. **Aggiungi spesa veloce**: importi preimpostati (5/10/20/50 €) + categoria, con la rotella
   sulle schede che ce l'hanno. Il resto dalla dashboard.
4. **Consigli**: una frase al giorno dall'analisi ("La categoria svago è +40% rispetto alla tua
   media", "Il fisso pesa il 62% dello stipendio").
5. Facoltativo: complicazione sul quadrante con il budget del giorno.

## Dashboard web (`bilancio_dashboard.html`, stile kit.js come le altre)

- **Panoramica**: oggi puoi spendere, giorni allo stipendio, previsione di fine mese, fisso/entrate.
- **Movimenti**: inserimento rapido, modifica, filtri per mese e categoria.
- **Fissi e rate**: ricorrenze mensili/annuali con fine opzionale ("ultima rata giugno 2028").
- **Entrate**: stipendio (giorno, regola weekend), entrate extra.
- **Grafici** (SVG disegnato a mano, niente librerie esterne: la pagina è servita dall'orologio
  anche senza internet): torta per categoria, barre mese per mese (12 mesi) entrate vs uscite,
  linea del saldo previsto fino allo stipendio, confronto mese corrente vs medio per categoria.
- **Analisi "cosa sto sbagliando"**: le stesse regole dei consigli, spiegate per esteso.
- **Backup**: esporta/importa JSON e CSV.

## Estratto conto mensile

- **Fattibile subito: import CSV.** Quasi tutte le banche italiane esportano CSV o Excel (da
  salvare come CSV). Il parsing si fa **nel browser** (JavaScript nella dashboard): l'utente
  sceglie quali colonne sono data, descrizione e importo, l'app ricorda lo schema per quella
  banca. Categorizzazione automatica per parole chiave ("ESSELUNGA" → spesa,
  "TELEPASS" → auto), modificabili; riconoscimento delle ricorrenze (stesso importo e
  descrizione ogni mese → "Vuoi aggiungerla ai fissi?"). Duplicati evitati con
  data+importo+descrizione.
- **Coming soon: PDF.** Leggere un PDF di estratto conto nel browser richiede una libreria
  (pdf.js, circa 1 MB) che l'orologio non può servire comodamente, e ogni banca ha un
  impaginato diverso. Da valutare come funzione della companion (che ha più risorse) o con
  pdf.js caricato da CDN quando c'è internet.

## Bollette, auto e confronto delle tariffe

**Già possibile (passo 2).** Voci predefinite da un tocco nella dashboard: mutuo, prestito,
finanziamento auto, affitto, condominio, luce, gas, acqua, internet, telefono, TARI, assicurazione
auto e casa, bollo, revisione (ogni 2 anni), tagliando, cambio gomme, rinnovo patente (ogni 10 anni),
abbonamenti; entrate: stipendio, tredicesima, quattordicesima, pensione, affitto percepito; previsti:
rimborso 730, 730 a debito/F24, bonus. Ogni bolletta pagata va collegata alla sua voce ("In arrivo" →
tocca la scadenza → importo vero): la scheda mostra mini-grafico degli importi, media e scarto dalla
stima.

**Da fare (passo 8): confronto con il mercato, in background.**

- Dati pubblici candidati, **da verificare** prima di scrivere codice (formato, licenza, frequenza):
  - offerte luce e gas di tutti i venditori: Portale Offerte di ARERA / Acquirente Unico
    (`ilportaleofferte.it`), nato per legge (124/2017) per pubblicarle **in open data**;
  - prezzo all'ingrosso dell'elettricità (PUN Index): GME, `mercatoelettrico.org`, "Esiti dei mercati";
  - gas (PSV): non c'è un valore ufficiale unico, servirà una fonte con metodo dichiarato.
- Architettura proposta: **una GitHub Action** nel repo delle release (come il catalogo del Market)
  scarica ogni giorno i dati grezzi (pesanti), li riduce a un `tariffe.json` di pochi KB (indici del
  mese, le 10 offerte più convenienti per profilo di consumo, con nome, prezzo €/kWh o €/Smc, quota
  fissa annua, scadenza) e lo pubblica. L'orologio e la dashboard lo scaricano quando c'è Wi-Fi,
  senza bloccare nulla ("sezione asincrona"), e mostrano la data dei dati.
- Cosa serve dall'utente: dalla bolletta consumo annuo (kWh, Smc), prezzo della materia prima e quota
  fissa del contratto attuale. Nuovi campi facoltativi sulle voci "bollette".
- Risultato: "Con la tua offerta paghi circa 1.140 €/anno di luce; la più conveniente oggi per il tuo
  consumo ne costa 960 (−180 €)". Avvisi: offerta in scadenza, prezzo variabile che sale col PUN.
- Limiti da dire: stima sui consumi dichiarati, oneri e imposte uguali per tutti, nessun cambio di
  fornitore fatto dall'app.
- Altre idee nella stessa direzione: stima stagionale del gas (inverno più caro, dallo storico delle
  bollette), promemoria scadenza revisione/patente con 30 giorni di anticipo come notifica.

## Ordine di lavoro (ogni passo si può chiudere e rilasciare)

1. ✓ `calc.c` + `test_calc.c` (logica e test). Nessun hardware.
2. ✓ `store.c` + `web.c` + dashboard con Movimenti, Fissi, Entrate, Panoramica (più: previsione fino
   allo stipendio, scadenze, fisso/entrate, accantonamento, mutuo e prestiti, categorie, consigli,
   backup JSON/CSV, storico bollette). Restano per il passo 4: torta categorie, barre 12 mesi,
   analisi estesa. `tools/create_icon.py` va fatto al passo 6.
3. `ui.c` Home e Questo mese; screenshot su tondo 466/412/360 e rettangolare 368x448.
4. Grafici e analisi nella dashboard.
5. Import CSV.
6. Registrazione: `apps/CMakeLists.txt` (blocco `CONFIG_APPKIT_HOSTED`, come Pieno),
   `apps.c` `s_apps[]`, `apps_internal.h`. Build di tutti i firmware, release `1.9.0`
   ([guida pagina 9](../guida/09-release-firmware.md)).
7. Versione esterna (`external_apps/bilancio` nei firmware) e pacchetti Market
   ([guida pagina 5](../guida/05-app-esterne.md)). `bl_start()` mette da sé i nodi cJSON in PSRAM
   (come appkit), quindi l'ordine di avvio nell'app esterna non conta.
8. Confronto delle tariffe luce/gas (sezione sopra).

Stima: passi 1–3 una sessione di lavoro piena; 4–7 un'altra.

## Limiti da dire chiaramente nell'app

- I numeri sono buoni quanto i dati inseriti: il saldo va aggiornato (a mano o con l'import).
- I consigli sono regole semplici, non consulenza finanziaria.
- I dati sono in chiaro sulla memoria del dispositivo: chi ha l'orologio e la pagina web
  abbinata li vede. Possibile in futuro un PIN per la dashboard Bilancio.
