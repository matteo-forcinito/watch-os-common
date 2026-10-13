/* ICAO aircraft type designators -> names people recognise (the common ones over Europe). */
#include "cielo.h"

#include <string.h>

typedef struct
{
    const char *code, *name;
} type_t;

static const type_t s_types[] = {
    {"A19N", "Airbus A319neo"},     {"A20N", "Airbus A320neo"},     {"A21N", "Airbus A321neo"},
    {"A318", "Airbus A318"},        {"A319", "Airbus A319"},        {"A320", "Airbus A320"},
    {"A321", "Airbus A321"},        {"A332", "Airbus A330-200"},    {"A333", "Airbus A330-300"},
    {"A338", "Airbus A330-800neo"}, {"A339", "Airbus A330-900neo"}, {"A343", "Airbus A340-300"},
    {"A346", "Airbus A340-600"},    {"A359", "Airbus A350-900"},    {"A35K", "Airbus A350-1000"},
    {"A388", "Airbus A380"},        {"BCS1", "Airbus A220-100"},    {"BCS3", "Airbus A220-300"},
    {"A400", "Airbus A400M"},       {"B737", "Boeing 737-700"},     {"B738", "Boeing 737-800"},
    {"B739", "Boeing 737-900"},     {"B37M", "Boeing 737 MAX 7"},   {"B38M", "Boeing 737 MAX 8"},
    {"B39M", "Boeing 737 MAX 9"},   {"B3XM", "Boeing 737 MAX 10"},  {"B744", "Boeing 747-400"},
    {"B748", "Boeing 747-8"},       {"B752", "Boeing 757-200"},     {"B763", "Boeing 767-300"},
    {"B764", "Boeing 767-400"},     {"B772", "Boeing 777-200"},     {"B77L", "Boeing 777-200LR"},
    {"B77W", "Boeing 777-300ER"},   {"B778", "Boeing 777-8"},       {"B779", "Boeing 777-9"},
    {"B788", "Boeing 787-8"},       {"B789", "Boeing 787-9"},       {"B78X", "Boeing 787-10"},
    {"E170", "Embraer 170"},        {"E175", "Embraer 175"},        {"E190", "Embraer 190"},
    {"E195", "Embraer 195"},        {"E290", "Embraer E190-E2"},    {"E295", "Embraer E195-E2"},
    {"AT72", "ATR 72"},             {"AT75", "ATR 72-500"},         {"AT76", "ATR 72-600"},
    {"AT45", "ATR 42-500"},         {"AT46", "ATR 42-600"},         {"DH8D", "Dash 8 Q400"},
    {"CRJ9", "Bombardier CRJ900"},  {"CRJ7", "Bombardier CRJ700"},  {"CRJX", "Bombardier CRJ1000"},
    {"SB20", "Saab 2000"},          {"F70", "Fokker 70"},           {"F100", "Fokker 100"},
    {"C172", "Cessna 172"},         {"C152", "Cessna 152"},         {"C182", "Cessna 182"},
    {"C208", "Cessna Caravan"},     {"C25A", "Cessna Citation CJ2"}, {"C56X", "Cessna Citation Excel"},
    {"C68A", "Cessna Citation Latitude"}, {"C700", "Cessna Citation Longitude"},
    {"PC12", "Pilatus PC-12"},      {"PC24", "Pilatus PC-24"},      {"P28A", "Piper Cherokee"},
    {"SR22", "Cirrus SR22"},        {"SF50", "Cirrus Vision Jet"},  {"TBM9", "Daher TBM 900"},
    {"GLF6", "Gulfstream G650"},    {"GLEX", "Bombardier Global Express"}, {"GL7T", "Bombardier Global 7500"},
    {"CL35", "Bombardier Challenger 350"}, {"FA7X", "Dassault Falcon 7X"}, {"F2TH", "Dassault Falcon 2000"},
    {"E55P", "Embraer Phenom 300"}, {"E50P", "Embraer Phenom 100"}, {"LJ45", "Learjet 45"},
    {"EC35", "Elicottero H135"},    {"EC45", "Elicottero H145"},    {"A139", "Elicottero AW139"},
    {"A169", "Elicottero AW169"},   {"A109", "Elicottero AW109"},   {"R44", "Elicottero Robinson R44"},
    {"C130", "Lockheed C-130 Hercules"}, {"C30J", "Lockheed C-130J"}, {"K35R", "Boeing KC-135"},
    {"A3ST", "Airbus Beluga"},      {"A337", "Airbus BelugaXL"},    {"EUFI", "Eurofighter Typhoon"},
    {"F35", "Lockheed F-35"},       {"P8", "Boeing P-8 Poseidon"},  {"GLID", "Aliante"},
};

const char *cielo_type_name(const char *code)
{
    if (code == NULL || !code[0])
        return NULL;
    for (size_t i = 0; i < sizeof(s_types) / sizeof(s_types[0]); i++)
        if (strcmp(s_types[i].code, code) == 0)
            return s_types[i].name;
    return NULL;
}
