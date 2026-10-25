# Prestazioni e attività su richiesta

Prima iterazione, 25 ottobre 2026, base stabile 1.15.0.

## Cambiamenti

- Bluetooth spento non prenota più 44 KiB interni. La guardia prima
  dell'avvio verifica memoria totale e blocco contiguo e rinvia l'avvio
  quando insufficienti: non entra nell'allocatore del controller che può abortire.
- Family automatico usa richieste UI/web; la sola alimentazione USB non
  autorizza più un servizio radio permanente. Il background resta opt-in.
- Notizie alloca/carica l'archivio al primo utilizzo in un worker e non
  esegue download periodici. Se l'allocazione fallisce rilascia i parziali.
- Il quadrante usa il meteo disponibile; il risveglio non provoca download.
- Il launcher non viene ricostruito durante lo swipe; indice esterne in cache,
  pagine vicine create su richiesta (focus/knob mantiene tutti i controlli).
- Nessuna trasformazione di scala sulle schede premute; progresso compatto
  invece di invalidazione di un intero perimetro; meno ridisegni ripetuti.
- Market direttamente al catalogo e compagnia pet ferma durante input/transizioni.
- Sistema apre Attività: task reali dello scheduler, stato, motivo classificato,
  stack noto/margine, riferimenti radio, servizi e misure UI azzerabili.
  La diagnostica non introduce un task di polling permanente.

Orologio, sveglie, timer e promemoria Giornata restano disponibili ad app chiusa.

## Misure fisiche

Stessa strumentazione USB nella baseline e nella nuova build locale 1.16.0-dev.1.
Sequenze scriptate su AMOLED 1.8 e AMOLED 1.75C; valori in byte/ms, non FPS.
Le condizioni di rete e le cache possono variare: i picchi sono campioni,
non una garanzia di latenza costante.

| Caso | Baseline | Prima ottimizzazione |
|---|---:|---:|
| 1.75C home: RAM interna libera | 39.263 | 148.747 |
| 1.75C home: blocco massimo | 23.552 | 63.488 |
| 1.75C home: PSRAM libera | 7.283.200 | 8.042.084 |
| 1.75C dopo sleep: RAM interna libera | 39.259 | 147.643 |
| 1.8 dopo sleep: RAM interna libera | 122.719 | 156.435 |
| 1.75C swipe shell: picco passaggio UI | 246 ms | 194 ms |
| 1.8 swipe shell: picco passaggio UI | 1.174 ms | 153 ms |
| 1.8 lista app: picco passaggio UI | 1.630 ms | 360 ms |
| 1.75C lista app: picco passaggio UI | 514 ms | 524 ms |
| 1.75C griglia: picco passaggio UI | 85 ms | 93 ms |
| 1.8 griglia: picco passaggio UI | 136 ms | 240 ms |

Il recupero RAM è confermato. La griglia non migliora ancora uniformemente:
serve ulteriore profilazione di creazione differita e rendering. Il primo
catalogo Market sul tondo ha ancora passaggi di alcuni secondi; non è risolto
da questa iterazione. Nessun IRQ display perso nei report raccolti.

Dopo l'uso della rete il blocco contiguo può scendere sotto i 40 KiB necessari
al Bluetooth: il totale libero non equivale a memoria contigua. La guardia
mantiene il rinvio anziché provocare un crash; occorre verificare il ciclo
abilitazione BLE dopo servizi pesanti prima di un rilascio stabile generale.

## Diagnostica

`perf reset`, eseguire la sequenza, `perf` per passaggi oltre16/33 ms e massimo.
`tasks` elenca nome, stato, stack noto e margine minimo. Questi numeri non
attribuiscono tutto l'heap ai task e non sono una percentuale di CPU.
Gli stack delle attività SDK non registrate compaiono con dimensione zero,
esplicitamente indicata come sconosciuta. I task creati dal wrapper sono loggati.

Log, screenshot e backup USB restano fuori dal repository in
`progress-files/performance`. Le righe blu di alcuni PNG sono righe perse
nel trasporto della console, non pixel del dispositivo.

Build dei sei modelli e test dei lettori C News/Giornata passati. Gli ultimi
ritocchi diagnostici richiedono la successiva build completa con le nuove app.
La successiva 1.16.0 integra Raccolta, Zampe, risparmio energetico e l'app esterna
Watch Lite. Nel catalogo Market le prime otto schede vengono create subito,
le altre su richiesta; le immagini dei pacchetti si decodificano nei dettagli.
Il pet decorativo resta sulla home, senza coprire i contenuti delle app.

Il ciclo fisico BLE dopo la rete è stato verificato: con poca memoria contigua
l'avvio viene rinviato; chiusa la rete, l'abilitazione riesce senza riavvio.
La Lite sul tondo lascia circa 265 KiB interni prima della rete e circa 198 KiB
con Wi-Fi/dashboard accesi; non collega LVGL né Bluetooth. Sono misure RAM,
non misure di durata della batteria. I limiti delle transizioni sopra restano
espliciti finché non vengono misurati nuovamente.
