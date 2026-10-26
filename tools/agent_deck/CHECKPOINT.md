# Deck Agents — checkpoint 2026-10-26

Fase software chiusa, senza nuove funzionalità. Conservata l'implementazione
esistente: app on demand, parser limitato, dashboard, monitor e correzione del
timer appkit quando la pagina viene ricostruita.

Verifiche eseguite sul Surface:
- 10 test monitor: PASS (job 10210eac-04c5-42b3-9264-a55f01e0d4a9).
- Parser C reale con MSVC/cJSON: PASS (job f011e3f3-2947-40c7-a027-f7af03a3077e).
- Generazione dashboard, compileall Python e git diff --check: PASS.
- Build AMOLED 1.8 con toolchain già configurata: PASS, exit 0
  (job 71260598-ed34-4ae6-968d-d327630f8d14). Log privati in ~/.agent-deck.

Hardware: NON VERIFICATO. COM3, porta storicamente usata dall'AMOLED 1.8,
non disponibile; Windows non elenca porte COM. Nessun reset, flash o accesso
ad altri dispositivi. Nessun comportamento touch/rete/schermo dichiarato verificato.

L'attivazione standard ESP-IDF non trova correttamente tool/constraints; la build
è stata completata con ninja nel build esistente, PATH verso Python v6.1,
xtensa esp-15.2.0_20251204, cmake 4.0.3 e ccache. Nessuna reinstallazione.

Backlog non bloccante: prova visiva e associazione sul solo AMOLED 1.8 quando
accessibile, verifica runtime chiusura app e snapshot, rifiniture estetiche.
Non serve estendere Deck Agents per avviare Aetherion.
