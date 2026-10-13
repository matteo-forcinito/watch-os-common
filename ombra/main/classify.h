/*
 * Ombra advertising classifier: pure C, no ESP-IDF (host-tested in tests/).
 *
 * A Bluetooth LE advertisement is a list of AD structures:
 *
 *   [length][type][data ...][length][type][data ...] ...
 *
 * classify_adv() walks them once and fills an ombra_adv_t: local name,
 * manufacturer (company id), 16-bit service UUIDs, TX power, GAP appearance and
 * the "kind" of device this is (tracker, phone, headphones, beacon ...).
 *
 * Tracker rules follow the public formats used by anti-stalking tools:
 *   Apple Find My  manufacturer 0x004C, type 0x12; length 25 = separated from
 *                  its owner (the state that matters), status byte bits 4-5:
 *                  0 Apple device, 1 AirTag, 2 Find My accessory, 3 AirPods.
 *   Samsung        service data 0xFD5A (SmartTag)
 *   Tile           service 0xFEED / 0xFEEC
 *   Chipolo        service 0xFE33 / 0xFE65
 *   Google FMDN    service data 0xFEAA, frame 0x40 / 0x41 (0x41 = unwanted
 *                  tracking protection: separated from the owner)
 */
#ifndef OMBRA_CLASSIFY_H
#define OMBRA_CLASSIFY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
    OMBRA_KIND_UNKNOWN,
    OMBRA_KIND_TRACKER,    /* AirTag, SmartTag, Tile, Chipolo, Find My accessory */
    OMBRA_KIND_PHONE,
    OMBRA_KIND_COMPUTER,
    OMBRA_KIND_WATCH,      /* watches and fitness bands */
    OMBRA_KIND_AUDIO,      /* headphones, earbuds, speakers */
    OMBRA_KIND_TV,         /* TVs, streaming boxes */
    OMBRA_KIND_INPUT,      /* keyboards, mice, remotes, gamepads */
    OMBRA_KIND_BEACON,     /* iBeacon, Eddystone */
    OMBRA_KIND_HOME,       /* smart home: lights, plugs, sensors, appliances */
    OMBRA_KIND_HEALTH,     /* scales, thermometers, blood pressure, glucose */
    OMBRA_KIND_VEHICLE,    /* cars, e-bikes, scooters */
    OMBRA_KIND_COUNT,
} ombra_kind_t;

typedef enum
{
    OMBRA_TRACKER_NONE,
    OMBRA_TRACKER_AIRTAG,
    OMBRA_TRACKER_FINDMY,   /* third-party Find My accessory */
    OMBRA_TRACKER_APPLE,    /* iPhone/iPad/Mac in Find My offline mode */
    OMBRA_TRACKER_AIRPODS,  /* AirPods in Find My offline mode */
    OMBRA_TRACKER_SMARTTAG,
    OMBRA_TRACKER_TILE,
    OMBRA_TRACKER_CHIPOLO,
    OMBRA_TRACKER_GOOGLE,   /* Find My Device network tag */
} ombra_tracker_t;

typedef struct
{
    char name[30];
    uint16_t company;       /* 0xFFFF = none */
    uint16_t appearance;    /* 0 = none */
    int8_t tx_power;        /* dBm, valid if has_tx_power */
    bool has_tx_power;
    bool connectable_hint;  /* flags say "LE general discoverable" */
    uint8_t kind;           /* ombra_kind_t */
    uint8_t tracker;        /* ombra_tracker_t */
    bool separated;         /* tracker away from its owner (the alert state) */
    int8_t battery;         /* tracker battery 0 full .. 3 critical, -1 unknown */
    uint8_t apple_type;     /* Apple continuity type (0 = none) */
    uint16_t services[6];   /* 16-bit UUIDs (list or service data) */
    uint8_t service_count;
    char detail[40];        /* what the classifier recognised, for the user */
} ombra_adv_t;

/* Parse one advertisement (or scan response). Never reads past length. */
void classify_adv(const uint8_t *data, size_t length, ombra_adv_t *out);

/* Merge a scan response into an advertisement of the same device. */
void classify_merge(ombra_adv_t *into, const ombra_adv_t *more);

/* Bluetooth SIG company name, or NULL. */
const char *classify_company(uint16_t company);

/* Italian label for a kind / tracker model ("Tracker", "AirTag"...). */
const char *classify_kind_name(ombra_kind_t kind);
const char *classify_tracker_name(ombra_tracker_t tracker);

/* Distance estimate in centimetres from RSSI and the 1 m reference power
 * (tx_power_1m): log-distance path loss with exponent 2.2 indoors. */
uint32_t classify_distance_cm(int rssi, int tx_power_1m);

#endif
