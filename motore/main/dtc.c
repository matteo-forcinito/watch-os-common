/*
 * Italian descriptions of the generic (SAE J2012, x0xx) trouble codes most
 * often seen. Manufacturer codes (P1xxx, P3xxx, ...) and unknown ones get a
 * description from their family, so every code reads as something useful.
 */
#include "motore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    const char *code;
    const char *text;
} dtc_entry_t;

static const dtc_entry_t s_codes[] = {
    {"P0010", "Attuatore fasatura variabile aspirazione, circuito (banco 1)"},
    {"P0011", "Fasatura albero a camme aspirazione troppo anticipata (banco 1)"},
    {"P0012", "Fasatura albero a camme aspirazione troppo ritardata (banco 1)"},
    {"P0016", "Correlazione albero motore / albero a camme (banco 1, sensore A)"},
    {"P0030", "Riscaldatore sonda lambda, circuito (banco 1 sensore 1)"},
    {"P0031", "Riscaldatore sonda lambda, circuito basso (banco 1 sensore 1)"},
    {"P0032", "Riscaldatore sonda lambda, circuito alto (banco 1 sensore 1)"},
    {"P0036", "Riscaldatore sonda lambda, circuito (banco 1 sensore 2)"},
    {"P0037", "Riscaldatore sonda lambda, circuito basso (banco 1 sensore 2)"},
    {"P0038", "Riscaldatore sonda lambda, circuito alto (banco 1 sensore 2)"},
    {"P0068", "Correlazione pressione collettore / flusso d'aria / farfalla"},
    {"P0087", "Pressione rail carburante troppo bassa"},
    {"P0088", "Pressione rail carburante troppo alta"},
    {"P0100", "Sensore flusso d'aria (MAF), circuito"},
    {"P0101", "Sensore flusso d'aria (MAF), valori fuori campo"},
    {"P0102", "Sensore flusso d'aria (MAF), segnale basso"},
    {"P0103", "Sensore flusso d'aria (MAF), segnale alto"},
    {"P0105", "Sensore pressione collettore (MAP), circuito"},
    {"P0106", "Sensore pressione collettore (MAP), valori fuori campo"},
    {"P0107", "Sensore pressione collettore (MAP), segnale basso"},
    {"P0108", "Sensore pressione collettore (MAP), segnale alto"},
    {"P0110", "Sensore temperatura aria aspirata, circuito"},
    {"P0112", "Sensore temperatura aria aspirata, segnale basso"},
    {"P0113", "Sensore temperatura aria aspirata, segnale alto"},
    {"P0115", "Sensore temperatura liquido refrigerante, circuito"},
    {"P0116", "Sensore temperatura refrigerante, valori fuori campo"},
    {"P0117", "Sensore temperatura refrigerante, segnale basso"},
    {"P0118", "Sensore temperatura refrigerante, segnale alto"},
    {"P0120", "Sensore posizione farfalla / pedale A, circuito"},
    {"P0121", "Sensore posizione farfalla A, valori fuori campo"},
    {"P0122", "Sensore posizione farfalla A, segnale basso"},
    {"P0123", "Sensore posizione farfalla A, segnale alto"},
    {"P0125", "Temperatura refrigerante insufficiente per il controllo in anello chiuso"},
    {"P0128", "Termostato: temperatura refrigerante sotto la soglia"},
    {"P0130", "Sonda lambda, circuito (banco 1 sensore 1)"},
    {"P0131", "Sonda lambda, tensione bassa (banco 1 sensore 1)"},
    {"P0132", "Sonda lambda, tensione alta (banco 1 sensore 1)"},
    {"P0133", "Sonda lambda, risposta lenta (banco 1 sensore 1)"},
    {"P0134", "Sonda lambda, nessuna attività (banco 1 sensore 1)"},
    {"P0135", "Riscaldatore sonda lambda, circuito (banco 1 sensore 1)"},
    {"P0136", "Sonda lambda, circuito (banco 1 sensore 2)"},
    {"P0137", "Sonda lambda, tensione bassa (banco 1 sensore 2)"},
    {"P0138", "Sonda lambda, tensione alta (banco 1 sensore 2)"},
    {"P0140", "Sonda lambda, nessuna attività (banco 1 sensore 2)"},
    {"P0141", "Riscaldatore sonda lambda, circuito (banco 1 sensore 2)"},
    {"P0170", "Correzione carburante (banco 1)"},
    {"P0171", "Miscela troppo magra (banco 1)"},
    {"P0172", "Miscela troppo ricca (banco 1)"},
    {"P0174", "Miscela troppo magra (banco 2)"},
    {"P0175", "Miscela troppo ricca (banco 2)"},
    {"P0190", "Sensore pressione rail carburante, circuito"},
    {"P0201", "Iniettore cilindro 1, circuito"},
    {"P0202", "Iniettore cilindro 2, circuito"},
    {"P0203", "Iniettore cilindro 3, circuito"},
    {"P0204", "Iniettore cilindro 4, circuito"},
    {"P0217", "Motore surriscaldato"},
    {"P0219", "Fuorigiri motore"},
    {"P0221", "Sensore posizione farfalla / pedale B, valori fuori campo"},
    {"P0230", "Pompa carburante, circuito primario"},
    {"P0234", "Sovralimentazione turbo eccessiva"},
    {"P0235", "Sensore pressione turbo A, circuito"},
    {"P0299", "Sovralimentazione turbo insufficiente"},
    {"P0300", "Mancate accensioni casuali o multiple"},
    {"P0301", "Mancata accensione cilindro 1"},
    {"P0302", "Mancata accensione cilindro 2"},
    {"P0303", "Mancata accensione cilindro 3"},
    {"P0304", "Mancata accensione cilindro 4"},
    {"P0305", "Mancata accensione cilindro 5"},
    {"P0306", "Mancata accensione cilindro 6"},
    {"P0315", "Variazione albero motore non appresa"},
    {"P0325", "Sensore battito in testa 1, circuito"},
    {"P0327", "Sensore battito in testa 1, segnale basso"},
    {"P0328", "Sensore battito in testa 1, segnale alto"},
    {"P0335", "Sensore posizione albero motore, circuito"},
    {"P0336", "Sensore posizione albero motore, valori fuori campo"},
    {"P0340", "Sensore posizione albero a camme, circuito"},
    {"P0341", "Sensore posizione albero a camme, valori fuori campo"},
    {"P0351", "Bobina accensione A, circuito"},
    {"P0352", "Bobina accensione B, circuito"},
    {"P0353", "Bobina accensione C, circuito"},
    {"P0354", "Bobina accensione D, circuito"},
    {"P0380", "Candelette, circuito A"},
    {"P0400", "Ricircolo gas di scarico (EGR), flusso"},
    {"P0401", "Ricircolo gas di scarico (EGR), flusso insufficiente"},
    {"P0402", "Ricircolo gas di scarico (EGR), flusso eccessivo"},
    {"P0403", "Valvola EGR, circuito"},
    {"P0420", "Efficienza catalizzatore sotto la soglia (banco 1)"},
    {"P0430", "Efficienza catalizzatore sotto la soglia (banco 2)"},
    {"P0440", "Sistema evaporativo, malfunzionamento"},
    {"P0441", "Sistema evaporativo, flusso di spurgo non corretto"},
    {"P0442", "Sistema evaporativo, piccola perdita"},
    {"P0443", "Valvola spurgo canister, circuito"},
    {"P0455", "Sistema evaporativo, grande perdita"},
    {"P0456", "Sistema evaporativo, perdita molto piccola"},
    {"P0480", "Ventola raffreddamento 1, circuito"},
    {"P0500", "Sensore velocità veicolo"},
    {"P0501", "Sensore velocità veicolo, valori fuori campo"},
    {"P0505", "Controllo del minimo"},
    {"P0506", "Regime del minimo più basso del previsto"},
    {"P0507", "Regime del minimo più alto del previsto"},
    {"P0520", "Sensore pressione olio, circuito"},
    {"P0560", "Tensione di sistema"},
    {"P0562", "Tensione di sistema bassa"},
    {"P0563", "Tensione di sistema alta"},
    {"P0571", "Interruttore freno A, circuito"},
    {"P0600", "Collegamento seriale centralina"},
    {"P0601", "Centralina: errore checksum memoria"},
    {"P0602", "Centralina: errore di programmazione"},
    {"P0603", "Centralina: memoria permanente"},
    {"P0604", "Centralina: errore memoria RAM"},
    {"P0605", "Centralina: errore memoria ROM"},
    {"P0606", "Processore centralina"},
    {"P0620", "Alternatore, circuito di controllo"},
    {"P0627", "Pompa carburante, circuito di controllo"},
    {"P0638", "Attuatore farfalla, campo di controllo (banco 1)"},
    {"P0641", "Tensione di riferimento sensori A"},
    {"P0650", "Spia avaria motore (MIL), circuito"},
    {"P0700", "Cambio automatico: guasto segnalato dalla sua centralina"},
    {"P0705", "Sensore posizione leva cambio, circuito"},
    {"P0715", "Sensore velocità turbina cambio, circuito"},
    {"P2096", "Correzione a valle del catalizzatore: miscela troppo magra"},
    {"P2097", "Correzione a valle del catalizzatore: miscela troppo ricca"},
    {"P2100", "Motore farfalla, circuito"},
    {"P2101", "Motore farfalla, valori fuori campo"},
    {"P2135", "Correlazione sensori posizione farfalla / pedale A-B"},
    {"P2138", "Correlazione sensori pedale acceleratore D-E"},
    {"P2187", "Miscela troppo magra al minimo (banco 1)"},
    {"P2188", "Miscela troppo ricca al minimo (banco 1)"},
    {"P2195", "Sonda lambda: segnale bloccato su magro (banco 1 sensore 1)"},
    {"P2196", "Sonda lambda: segnale bloccato su ricco (banco 1 sensore 1)"},
    {"P2270", "Sonda lambda post-catalizzatore bloccata su magro (banco 1)"},
    {"P2271", "Sonda lambda post-catalizzatore bloccata su ricco (banco 1)"},
    {"P2463", "Filtro antiparticolato: accumulo di fuliggine"},
    {"U0001", "Rete CAN ad alta velocità"},
    {"U0073", "Bus di comunicazione A spento"},
    {"U0100", "Comunicazione persa con la centralina motore"},
    {"U0101", "Comunicazione persa con la centralina del cambio"},
    {"U0121", "Comunicazione persa con la centralina ABS"},
    {"U0140", "Comunicazione persa con la centralina carrozzeria"},
    {"U0155", "Comunicazione persa con il quadro strumenti"},
};

const char *mt_dtc_system(const char *code)
{
    switch (code[0])
    {
    case 'P': return "Motore e cambio";
    case 'C': return "Telaio";
    case 'B': return "Carrozzeria";
    case 'U': return "Rete di bordo";
    default: return "Sconosciuto";
    }
}

const char *mt_dtc_text(const char *code, char *buf, size_t size)
{
    for (size_t i = 0; i < sizeof(s_codes) / sizeof(s_codes[0]); i++)
    {
        if (strcmp(s_codes[i].code, code) == 0)
        {
            return s_codes[i].text;
        }
    }
    /* Family of the code: the second character says generic (0, 2) or
     * manufacturer (1, 3), the third the subsystem. */
    const char *maker = code[1] == '1' || code[1] == '3' ? "specifico del costruttore" : "generico";
    const char *area = "";
    if (code[0] == 'P')
    {
        switch (code[2])
        {
        case '0': case '1': case '2': area = "miscela e misura aria/carburante"; break;
        case '3': area = "accensione e mancate accensioni"; break;
        case '4': area = "controllo emissioni"; break;
        case '5': area = "velocità e regime del minimo"; break;
        case '6': area = "centralina e uscite"; break;
        case '7': case '8': case '9': area = "cambio"; break;
        default: area = "motore"; break;
        }
    }
    else
    {
        area = mt_dtc_system(code);
    }
    snprintf(buf, size, "Codice %s: %s", maker, area);
    return buf;
}
