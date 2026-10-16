# Progetto: app "Bilancio" (gestione del denaro)

Stato: **progetto pronto, codice da scrivere**. Scritto il 17/10/2026.

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

1. **Home**: grande "**18 €** oggi", sotto "Stipendio tra 10 giorni" e un anello
   (`ui_border_progress`) che mostra quanto del mese è passato rispetto a quanto budget resta.
   Colori: verde in linea, arancione sopra il ritmo, rosso se la previsione va sotto zero.
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

## Ordine di lavoro (ogni passo si può chiudere e rilasciare)

1. `calc.c` + `test_calc.c` (logica e test). Nessun hardware.
2. `store.c` + `web.c` + dashboard con Movimenti, Fissi, Entrate, Panoramica.
3. `ui.c` Home e Questo mese; screenshot su tondo 466/412/360 e rettangolare 368x448.
4. Grafici e analisi nella dashboard.
5. Import CSV.
6. Registrazione: `apps/CMakeLists.txt` (blocco `CONFIG_APPKIT_HOSTED`, come Pieno),
   `apps.c` `s_apps[]`, `apps_internal.h`. Build di tutti i firmware, release `1.9.0`
   ([guida pagina 9](../guida/09-release-firmware.md)).
7. Versione esterna (`external_apps/bilancio` nei firmware) e pacchetti Market
   ([guida pagina 5](../guida/05-app-esterne.md)).

Stima: passi 1–3 una sessione di lavoro piena; 4–7 un'altra.

## Limiti da dire chiaramente nell'app

- I numeri sono buoni quanto i dati inseriti: il saldo va aggiornato (a mano o con l'import).
- I consigli sono regole semplici, non consulenza finanziaria.
- I dati sono in chiaro sulla memoria del dispositivo: chi ha l'orologio e la pagina web
  abbinata li vede. Possibile in futuro un PIN per la dashboard Bilancio.
