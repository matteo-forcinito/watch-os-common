# Stato del progetto — 23 ottobre 2026

Questa pagina descrive la release **1.13.0** per AMOLED 1.8, AMOLED 1.75,
AMOLED 1.75C, Knob 1.8, Watcher e Guition 4848. La versione effettivamente
disponibile si controlla dal catalogo OTA in **Impostazioni → Aggiornamento**.

## Pet e giochi

- **Ecosistema e viaggi**: correzioni della 1.11.0 incluse. Se Ecosistema è
  spento, la ricerca lo segnala e indica come riattivarlo.
- **Torna a casa**: dal menu di un pet ospite puoi avviare il ritorno al suo
  dispositivo d'origine. I dispositivi devono essere abbinati e raggiungibili.
  Se l'origine non è vicina, il pet resta sul dispositivo attuale: il ritorno
  automatico differito non è ancora implementato.
- **Fuga sulla neve**, **Pioggia di stelle** e **Tocca la stella** dal menu
  Pet → Minigiochi. Premi in XP e gioia, salvataggio tramite worker di sistema.
- **Base condivisa per i minigiochi**: tempo, pausa, comandi, memoria,
  disegno, Riprova, Esci e risultato sono gestiti dal motore comune.
- **Salto quantico**: quarto gioco, con il proprio pet astronauta. Tieni
  premuto per caricare e rallentare l'orbita, lascia per lanciarti; la scia
  puntinata anticipa la traiettoria. In volo tocca per correggere la rotta
  usando energia. Otto pianeti, tre scudi, buchi neri dopo il terzo salto.
  Sul Knob gira per mirare e clicca per lanciare o dare un impulso.

Per creare il tuo gioco, parti da
[Il tuo primo minigioco](14-minigiochi.md). Il comando
`python tools/new_minigame.py nome --title "Titolo"` crea una copia funzionante
dell'esempio e la registra nel firmware.

## Verifiche da usare

```powershell
python tools/test_minigames.py
python tools/test_pet_game.py
python tools/test_pet_store.py
```

Compilare tutti e sei i firmware prima del rilascio. Provare su hardware
apertura, comandi, pausa, Riprova, Esci, salvataggio premi e log delle
prestazioni. Per build e flash vedi [pagina 2](02-ambiente-build-flash.md);
per pubblicare vedi [pagina 9](09-release-firmware.md).

Le istruzioni e le attività precedenti sono conservate nello
[stato storico del 17 ottobre](STATO-2026-10-17.md). Le vecchie indicazioni
di restare sulla linea 1.8.x appartengono a quella fase del progetto.
