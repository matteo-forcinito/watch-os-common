# ECOS: l'ecosistema

> Cinque dispositivi diversi, alla pari, e un'unica intelligenza. Ognuno fa quello che sa
> fare meglio, e insieme diventano una casa che ti conosce.

Questo documento raccoglie la visione, l'architettura e le idee di ECOS.
È il riferimento per le prossime sessioni: le decisioni prese qui valgono per
tutti i firmware (watch-os-common e e-paper). Va aggiornato quando cambia
qualcosa.

---

## 1. I membri della famiglia

| Dispositivo | Carattere | Sa fare | Energia |
|---|---|---|---|
| **AMOLED 1.8** (orologio) | Il compagno al polso | touch, IMU QMI8658, microfono + altoparlante ES8311, RTC, PMU, microSD | batteria |
| **AMOLED 1.75** (orologio tondo) | Il gemello elegante | touch, IMU, doppio microfono ES7210, altoparlante esterno opzionale, flash `/data` | batteria |
| **SenseCAP Watcher** | L'occhio e il cervello | fotocamera, schermo tondo 412, ghiera, microfono, LED RGB, microSD | **sempre acceso** (USB) |
| **Knob 1.8** | La mano | ghiera, aptica DRV2605, altoparlante Bluetooth (coprocessore A2DP), microfono PDM | **sempre acceso** (USB) |
| **ePaper 1.54** | La voce silenziosa | display che non consuma, SHTC3 temperatura/umidità, RTC, audio, microSD | dorme, si sveglia ogni minuto |

La regola d'oro: **tutti uguali; chi ha energia lavora, chi è a batteria chiede**.

---

## 2. Architettura

### 2.1 Tutti alla pari (nessun hub)

Decisione del 14 ottobre 2026: **nessun nodo è speciale**. Ogni dispositivo
esegue lo stesso codice ECOS e tiene la sua copia dello stato di casa.

- **Gossip**: quando due nodi si vedono in rete, si scambiano un riassunto
  (per ogni autore, l'ultima versione che conoscono) e si mandano solo le
  differenze. Le modifiche fanno il giro della casa di nodo in nodo.
- **Lavori a turno, senza capo**: per un lavoro "per tutti" (scaricare News,
  il catalogo del Market, un aggiornamento), ogni nodo calcola la stessa regola
  sugli altri nodi svegli: prima chi è alimentato, poi chi ha più batteria,
  poi il nome. Chi risulta primo lo fa e pubblica il risultato nello stato
  condiviso. Se sparisce, al giro dopo tocca al successivo. Nessuna elezione.
- **Comportamento secondo l'energia, non il ruolo**: chi è alimentato (oggi
  Watcher e Knob) resta in ascolto e quindi fa da "memoria" della casa;
  chi è a batteria partecipa quando ha già il Wi-Fi acceso; l'e-paper,
  alla sua finestra Wi-Fi, chiede le novità a chiunque sia sveglio.
- Se tutti dormono, i messaggi aspettano il primo incontro tra due nodi:
  il sistema rallenta ma non si rompe. ECOS aggiunge, non toglie.

### 2.2 Canali

| Canale | Per cosa | Note |
|---|---|---|
| **Wi-Fi di casa** (UDP + HTTP) | stato, sincronizzazione, contenuti | mDNS `_ecos._udp`: nome, ruolo, capacità |
| **ESP-NOW** | comandi istantanei tra vicini (suona, ferma, interfono) | anche senza router; già usato da Remote |
| **Bluetooth (annunci)** | presenza: "l'orologio è qui" | l'hub ascolta, l'orologio annuncia (già lo fa) |

### 2.3 Lo stato di casa

Un piccolo archivio chiave-valore replicato:

```
ecos/alarm/<id>         sveglie di casa
ecos/sensor/<nome>/<x>  ultime letture (temperatura, umidità, batteria...)
ecos/notify/<id>        notifiche attive (con "letta da")
ecos/presence/<nome>    dove si trova chi (scrivania, casa, fuori)
ecos/device/<nome>      chi c'è, versione, capacità, batteria
```

Ogni chiave porta un orologio logico (contatore + nome di chi scrive):
vince l'ultima scrittura, senza conflitti; due nodi rimasti separati, quando
si ritrovano, convergono allo stesso stato. Ogni nodo tiene la copia in PSRAM
(poche decine di KB, sulla microSD dove c'è); l'e-paper riceve solo il "diff".

### 2.4 Fiducia

- **Unirsi alla famiglia, una volta sola**: il nuovo dispositivo mostra un
  codice di 6 cifre, lo confermi dalla dashboard di un membro. La chiave
  dell'ecosistema (32 byte) viaggia cifrata con una chiave derivata dal codice.
- **Ogni messaggio firmato** (HMAC-SHA256, con data e numero progressivo
  contro le ripetizioni). Niente in chiaro che possa comandare un dispositivo.
- Un dispositivo perso si **espelle** da qualsiasi membro: la chiave ruota.
- La dashboard resta abbinata **una volta per tutta la casa** (stesso token).

### 2.5 Budget (non negoziabili)

- Orologi a riposo: **0 KB** di RAM interna in più (niente task fissi; solo
  PSRAM per lo stato replicato, e il listener vive solo con il Wi-Fi acceso).
- Nessun Wi-Fi acceso apposta sugli orologi; nessun impatto sul deep sleep
  dell'e-paper (al massimo qualche centinaio di ms nella sua finestra).
- Nodi alimentati: un task di rete in ascolto (6 KB) + archivio su SD.

---

## 3. Le idee

Ordinate per "quanto cambiano la vita" rispetto a "quanto costano".

### ★ Sveglia di casa
Imposti la sveglia sull'orologio: suona anche l'e-paper sul comodino e il Knob
in cucina. La spegni da uno → tacciono tutti. Posticipi da uno → vale per tutti.
La sveglia è replicata su tutti: ogni dispositivo suona per conto suo, anche
senza rete e con l'orologio scarico. Lo stop corre via ESP-NOW ai vicini;
chi era lontano lo riceve al primo incontro.

### ★ Risveglio dolce intelligente
L'IMU dell'orologio capisce il sonno leggero nella mezz'ora prima della
sveglia: l'e-paper mostra l'alba (grafica che si schiarisce), il Knob vibra
piano, poi la radio parte dall'altoparlante del Knob. Al mattino l'e-paper
mostra meteo, primo impegno di Giornata e la notizia del giorno.

### ★ Sensori di casa al polso
Temperatura e umidità dell'e-paper sul quadrante dell'orologio; la batteria di
tutti i dispositivi in una riga; avviso "aria umida, apri la finestra" quando
l'umidità supera la soglia. Lo storico va nella dashboard.

### ★ Trova il dispositivo
Dall'orologio: "suona il Knob". Dal Knob: "dov'è l'orologio?" → l'orologio
vibra e suona; l'hub dice anche *dove l'ha sentito l'ultima volta* (Bluetooth).

### ★ Notifiche ovunque, lette una volta
La notifica del telefono arriva all'orologio e compare anche sul Knob o sul
Watcher se sei alla scrivania. La leggi su uno → sparisce da tutti.

### Interfono (walkie-talkie)
Tieni premuto sull'orologio e parli: la voce esce dal Knob o dall'altro
orologio, via ESP-NOW (audio compresso, 16 kHz). "La cena è pronta" senza
telefono. Funziona anche senza router.

### Occhio sulla porta
Il Watcher vede un movimento davanti all'ingresso (o qualcuno suona): una foto
piccola arriva sull'orologio entro due secondi. Tutto resta in casa.

### Manopola universale
La ghiera del Knob regola il volume della radio dell'orologio, scorre le
notizie, cambia stazione all'e-paper. Il Knob diventa il telecomando fisico
della casa, con l'aptica che "scatta" a ogni passo.

### Focus di casa
Avvii una sessione di focus in Giornata: il Knob mostra l'anello del tempo,
l'e-paper scrive "non disturbare", le notifiche non urgenti aspettano la fine.

### Passaggio (handoff)
Leggi una notizia sull'orologio, giri la ghiera del Knob: la notizia si apre
sullo schermo più grande. Un'app interna aperta sull'orologio si "passa" a un
altro dispositivo con lo stesso stato.

### Lavori a turno che risparmiano batteria
Il nodo di turno (di solito uno alimentato) scarica News, meteo, prezzi di Pieno, calendari di Giornata,
catalogo del Market e aggiornamenti firmware. Gli orologi li prendono da lui in
rete locale in pochi millisecondi invece di andare su internet: meno Wi-Fi,
meno RAM, più batteria. Gli aggiornamenti si scaricano una volta per tutti.

### Presenza
L'hub sente l'orologio: sei a casa, alla scrivania o fuori. Quando entri,
l'e-paper ti saluta con la giornata; quando esci, Sentinel diventa più
sospettoso ("dispositivo nuovo in rete mentre sei fuori").

### Aura
Un colore per lo stato di casa: il LED del Watcher, il bordo dell'orologio e
l'anello del Knob raccontano la stessa cosa (blu = focus, verde = tutto
tranquillo, arancio = qualcosa da guardare, rosso = sveglia o allarme).

### Automazioni "quando… allora…"
Dalla dashboard, frasi semplici senza programmare:
"Quando Sentinel vede un dispositivo nuovo → avvisa l'orologio",
"Quando l'umidità supera 70% → notifica",
"Quando esco di casa → spegni la radio".

### Diario di casa
L'hub tiene una timeline settimanale: sonno (IMU), clima (SHTC3), rete
(Sentinel), notizie della settimana (News), focus (Giornata). Ogni domenica un
riassunto sull'e-paper e nella dashboard.

---

## 4. Le fasi

| Fase | Contenuto | Si usa da subito? |
|---|---|---|
| **1. Famiglia** | scoperta, fiducia, dashboard "Ecosistema" (tutti i dispositivi, stato, batteria, link alle dashboard), trova il dispositivo, sensori condivisi | sì |
| **2. Sveglia di casa** | sveglie replicate, stop/posticipo ovunque, e-paper dormiente sincronizzato | sì |
| **3. Lavori a turno** | contenuti e aggiornamenti scaricati una volta per tutti, notifiche ovunque, manopola universale | sì |
| **4. Presenza e voce** | presenza Bluetooth, interfono ESP-NOW, risveglio intelligente | sì |
| **5. Casa che pensa** | automazioni, aura, diario, occhio sulla porta | sì |

Ogni fase si chiude con: build delle 4 board + e-paper, prova su almeno due
dispositivi reali, misura della RAM (soglie dei margini invariate), release.

---

## 5. Dove vive il codice

- `watch-os-common/components/services/ecos/`: il servizio (scoperta, fiducia,
  stato replicato, messaggi). Nessuna dipendenza dalla UI.
- `watch-os-common/components/apps/ecos/`: l'app "Ecosistema" sull'orologio
  (chi c'è, trova, stato di casa) + la pagina nella dashboard di sistema.
- e-paper: un client "dormiente" minimo (`services/ecos/` nel suo firmware),
  che parla lo stesso protocollo in versione ridotta.
- Protocollo e formati descritti in un header comune (`ecos_proto.h`), puro C e
  testato sul PC come `news/` e `ombra/`.

---

## 6. Domande aperte

1. ~~L'hub~~: deciso, nessun hub, tutti alla pari (2.1).
2. Ordine delle fasi: la sveglia di casa può passare davanti alla famiglia, se è
   la funzione che si desidera di più.
3. Interfono: va bene il formato audio "voce" (16 kHz, compresso) per
   privilegiare la latenza rispetto alla qualità?
4. Occhio sulla porta: dove sarà fisicamente il Watcher?
