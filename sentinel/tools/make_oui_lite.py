"""Compact OUI table for Sentinel Lite (the app built into the OS).

The full table (oui_table.c, ~680 KB of flash) stays with the external
Sentinel. The OS keeps only the makers found in homes: phones, computers,
routers, smart home, TVs, printers, consoles, chips of IoT modules.

    python make_oui_lite.py      # rewrites ../main/oui_lite.c from ../main/oui_table.c
"""
import re
from pathlib import Path

here = Path(__file__).resolve().parent
source = (here.parent / 'main' / 'oui_table.c').read_text(encoding='utf-8')

# Vendor name prefixes (case-insensitive) kept, with the short name shown.
KEEP = [
    ('Apple', 'Apple'), ('Samsung', 'Samsung'), ('Xiaomi', 'Xiaomi'), ('Beijing Xiaomi', 'Xiaomi'),
    ('Huawei', 'Huawei'), ('Honor', 'Honor'), ('Google', 'Google'), ('Amazon', 'Amazon'), ('OnePlus', 'OnePlus'),
    ('Guangdong Oppo', 'Oppo'), ('vivo', 'vivo'), ('Motorola', 'Motorola'), ('Sony', 'Sony'), ('LG Electronics', 'LG'),
    ('Nokia', 'Nokia'), ('Realme', 'Realme'), ('Fairphone', 'Fairphone'), ('Microsoft', 'Microsoft'),
    ('Intel', 'Intel'), ('Dell', 'Dell'), ('Hewlett', 'HP'), ('HP Inc', 'HP'), ('Lenovo', 'Lenovo'), ('ASUSTek', 'ASUS'),
    ('Acer', 'Acer'), ('Micro-Star', 'MSI'), ('Gigabyte', 'Gigabyte'), ('Realtek', 'Realtek'), ('Raspberry', 'Raspberry Pi'),
    ('TP-Link', 'TP-Link'), ('TP-LINK', 'TP-Link'), ('AVM', 'AVM (FRITZ!)'), ('Netgear', 'Netgear'), ('Ubiquiti', 'Ubiquiti'),
    ('Zyxel', 'Zyxel'), ('D-Link', 'D-Link'), ('Cisco', 'Cisco'), ('Technicolor', 'Technicolor'), ('Sagemcom', 'Sagemcom'),
    ('Arcadyan', 'Arcadyan'), ('Mikrotik', 'MikroTik'), ('Routerboard', 'MikroTik'), ('Tenda', 'Tenda'), ('Linksys', 'Linksys'),
    ('Shenzhen Tenda', 'Tenda'), ('Hon Hai', 'Foxconn'), ('Murata', 'Murata'), ('Espressif', 'Espressif'),
    ('Tuya', 'Tuya'), ('Shelly', 'Shelly'), ('Allterco', 'Shelly'), ('Signify', 'Philips Hue'), ('Philips', 'Philips'),
    ('Sonos', 'Sonos'), ('Bose', 'Bose'), ('Roku', 'Roku'), ('Nintendo', 'Nintendo'), ('Sony Interactive', 'PlayStation'),
    ('Valve', 'Valve'), ('Canon', 'Canon'), ('Seiko Epson', 'Epson'), ('Brother', 'Brother'), ('Kyocera', 'Kyocera'),
    ('Xerox', 'Xerox'), ('Synology', 'Synology'), ('QNAP', 'QNAP'), ('Western Digital', 'WD'), ('Hikvision', 'Hikvision'),
    ('Hangzhou Hikvision', 'Hikvision'), ('Zhejiang Dahua', 'Dahua'), ('Ring', 'Ring'), ('Arlo', 'Arlo'), ('Netatmo', 'Netatmo'),
    ('Withings', 'Withings'), ('Garmin', 'Garmin'), ('Fitbit', 'Fitbit'), ('Nest', 'Google Nest'), ('ecobee', 'ecobee'),
    ('iRobot', 'iRobot'), ('Roborock', 'Roborock'), ('Dyson', 'Dyson'), ('Vorwerk', 'Vorwerk'), ('Panasonic', 'Panasonic'),
    ('Sharp', 'Sharp'), ('Hisense', 'Hisense'), ('TCL', 'TCL'), ('Vestel', 'Vestel'), ('Skyworth', 'Skyworth'),
    ('Amlogic', 'Amlogic'), ('Rockchip', 'Rockchip'), ('MediaTek', 'MediaTek'), ('Qualcomm', 'Qualcomm'),
    ('Broadcom', 'Broadcom'), ('Texas Instruments', 'Texas Instruments'), ('Silicon Laboratories', 'Silicon Labs'),
    ('Nordic', 'Nordic'), ('AzureWave', 'AzureWave'), ('Liteon', 'Lite-On'), ('Chicony', 'Chicony'), ('Compal', 'Compal'),
    ('Quanta', 'Quanta'), ('Wistron', 'Wistron'), ('Pegatron', 'Pegatron'), ('Universal Global', 'USI'),
    ('Vodafone', 'Vodafone'), ('Telecom Italia', 'TIM'), ('Wind Tre', 'WindTre'), ('Fastweb', 'Fastweb'), ('Iliad', 'Iliad'),
    ('Freebox', 'Freebox'), ('Zte', 'ZTE'), ('ZTE', 'ZTE'), ('Vantiva', 'Vantiva'), ('Askey', 'Askey'),
    ('Meross', 'Meross'), ('Sonoff', 'Sonoff'), ('ITEAD', 'Sonoff'), ('Govee', 'Govee'), ('Yeelight', 'Yeelight'),
    ('Wiz', 'WiZ'), ('Lifx', 'LIFX'), ('IKEA', 'IKEA'), ('Bosch', 'Bosch'), ('Siemens', 'Siemens'), ('Miele', 'Miele'),
    ('BSH', 'Bosch-Siemens'), ('Electrolux', 'Electrolux'), ('Whirlpool', 'Whirlpool'), ('Haier', 'Haier'),
    ('Daikin', 'Daikin'), ('Mitsubishi', 'Mitsubishi'), ('Gree', 'Gree'), ('Tesla', 'Tesla'), ('Ezviz', 'EZVIZ'),
    ('Reolink', 'Reolink'), ('Imou', 'Imou'), ('Eufy', 'eufy'), ('Anker', 'Anker'), ('Logitech', 'Logitech'),
    ('Kindle', 'Amazon'), ('Peloton', 'Peloton'), ('Polar', 'Polar'), ('Huami', 'Amazfit'), ('Zepp', 'Amazfit'),
]


def array_body(name):
    m = re.search(r'static const [^\n]*\b' + name + r'\[[^\]]*\](?:\[3\])? = \{(.*?)\n\};', source, re.S)
    return m.group(1)


ouis = [tuple(int(x, 16) for x in t) for t in re.findall(r'\{0x(..),0x(..),0x(..)\}', array_body('s_oui'))]
vendor_index = [int(x) for x in re.findall(r'\d+', array_body('s_vendor'))]
offsets = [int(x) for x in re.findall(r'\d+', array_body('s_offset'))]
names_src = re.search(r'static const char s_names\[\] =\n(.*?);\n', source, re.S).group(1)
raw = ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', names_src))
blob = raw.encode('utf-8').decode('unicode_escape').encode('latin-1')


def vendor_name(i):
    start = offsets[i]
    end = blob.index(b'\0', start)
    return blob[start:end].decode('utf-8', 'replace')


short_names = []
entries = []
for oui, v in zip(ouis, vendor_index):
    name = vendor_name(v)
    for prefix, short in KEEP:
        if name.lower().startswith(prefix.lower()):
            if short not in short_names:
                short_names.append(short)
            entries.append((oui, short_names.index(short)))
            break

entries.sort()
out = [
    '/* GENERATED by sentinel/tools/make_oui_lite.py from oui_table.c. Do not edit.',
    f' * Sentinel Lite: {len(entries)} OUIs of {len(short_names)} common home makers. */',
    '#include "oui.h"',
    '#include <stddef.h>',
    '',
    f'static const uint8_t s_oui[{len(entries)}][4] = {{',
]
line = []
for (a, b, c), v in entries:
    line.append('{0x%02x,0x%02x,0x%02x,%d}' % (a, b, c, v))
    if len(line) == 8:
        out.append('    ' + ','.join(line) + ',')
        line = []
if line:
    out.append('    ' + ','.join(line) + ',')
out.append('};')
out.append('')
out.append(f'static const char *const s_names[{len(short_names)}] = {{')
for i in range(0, len(short_names), 6):
    out.append('    ' + ', '.join('"%s"' % n for n in short_names[i:i + 6]) + ',')
out.append('};')
out.append('''
const char *sn_oui_vendor(const uint8_t mac[3])
{
    size_t low = 0, high = sizeof(s_oui) / sizeof(s_oui[0]);
    uint32_t key = (uint32_t)mac[0] << 16 | (uint32_t)mac[1] << 8 | mac[2];
    while (low < high)
    {
        size_t middle = (low + high) / 2;
        uint32_t value = (uint32_t)s_oui[middle][0] << 16 | (uint32_t)s_oui[middle][1] << 8 | s_oui[middle][2];
        if (value == key) return s_names[s_oui[middle][3]];
        if (value < key) low = middle + 1;
        else high = middle;
    }
    return NULL;
}''')
(here.parent / 'main' / 'oui_lite.c').write_text('\n'.join(out) + '\n', encoding='utf-8')
print(len(entries), 'OUIs,', len(short_names), 'makers')
