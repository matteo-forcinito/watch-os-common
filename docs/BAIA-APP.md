# Baia app (AMOLED 1.75C, 32 MB)

Più app esterne installate insieme, ciascuna nel suo slot, avviate senza copie.
Introdotta il 04/11/2026, provata sulla 1.75C (flash GigaDevice GD25Q256).

## Disposizione

| Partizione | Offset | Dimensione | Uso |
|---|---|---|---|
| ota_0 / ota_1 | 0x20000 / 0x620000 | 6 + 6 MB | OS e suo aggiornamento |
| storage | 0xc60000 | 3,6 MB | invariato |
| ota_2 | 0x1000000 | 6 MB | app grande |
| ota_3, ota_4 | 0x1600000, 0x1b00000 | 5 + 5 MB | app medie |

I primi 16 MB sono identici a prima: NVS, slot dell'OS e storage non si spostano.

## Come funziona

- `services/extapp`: `extapp_bay_size()` conta gli slot oltre ota_1. Con 0 slot tutto
  resta come prima (un solo slot app, chiave NVS `slotapp`).
- L'OS si aggiorna solo in ota_0/ota_1 (`extapp_os_update_slot`).
- Il Market sceglie lo slot (`extapp_slot_for`): quello dove l'app è già, altrimenti il
  più piccolo libero che la contiene. Ogni slot ha la sua nota NVS `n_<label>`.
  Se un aggiornamento sposta l'app in uno slot più grande, la vecchia copia viene cancellata.
- Un'app appena installata finisce anche tra le preferite della home, se c'è posto.
- La scheda dell'app (pressione lunga) mostra partizione e slot occupati; la dashboard
  web espone `partition` e `app_bay`.
- Ritorno all'OS: l'OS scrive il proprio slot in NVS (`extapp`/`launcher`) a ogni avvio e
  prima di lanciare un'app; `extapp_sdk` (versione 2) torna lì.

## Requisiti per le app

- Compilare con `components/extapp_sdk/sdkconfig.bay` (flash 32 MB e
  `BOOTLOADER_CACHE_32BIT_ADDR_QUAD_FLASH`, opzione **sperimentale** di ESP-IDF).
- Pubblicare nel catalogo con `"sdk": 2`. Il Market rifiuta nella baia le app senza:
  tornerebbero allo slot sbagliato.
- Le app attuali (Solco, Remote, Pocket Tank) vanno ricompilate e ripubblicate per la 1.75C.

## Migrazione di una 1.75C esistente (solo USB)

Un OTA non cambia bootloader né tabella delle partizioni. Serve, via USB:

1. backup di 0x0-0x21000;
2. scrittura di bootloader (con l'opzione a 32 bit), tabella delle partizioni e OS;
3. riscrittura di `otadata` per lo slot dell'OS: il bootloader sceglie lo slot come
   `(seq - 1) % numero_slot`, e passando da 2 a 5 slot una sequenza esistente può
   puntare a uno slot vuoto.

NVS e storage non vanno toccati.

## Da verificare prima di una release

- L'OS compilato con l'opzione a 32 bit deve girare anche sulle 1.75C con il vecchio
  bootloader e la vecchia tabella (aggiornate via OTA, senza baia).
- Gli script di installazione USB (`muse/install_usb.py`, `solco/tools/select_slot_usb.py`)
  conoscono solo due slot.
