"""Muted hardware acceptance for Solco's landscape NEON NOIR layout (AMOLED 1.8, 448x368).

Coordinates are logical pixels of the landscape screen, from ui_decks.c geometry() and
the strip weights (28/10/2026):
  DECKS  strips y 90-140 (A) / 228-278 (B): PLAY|CUE|LOOP|1/2|X2|SPEED (26/17/21/11/11/14)
         waveform A y 38-86 (tap = FOCUS A)
  FOCUS  loop strip y 210-258 (20/20/16/28/16): IN|1|2|4|OUT with a known tempo (demo decks),
         IN|OUT|1/2|LOOP|X2 otherwise; while a loop runs the outer cells shift it << / >>
         transport y 264-312: PLAY|CUE|HOT1..HOT4 (24/16/15/15/15/15)
         nav y 318-362, x 56-392: DECKS|TONO|REV|SPEED|SCRATCH|OTHER DECK (20/15/14/14/21/16)
The master volume must stay 0: the test never opens the MASTER sheet.

usage: test_landscape_usb.py PORT REPORT.json
"""
import contextlib, importlib.util, io, json, re, sys, time
from pathlib import Path
import serial

root = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('devcon', root / 'ESP32-S3-Knob-Touch-LCD-1.8/tools/devcon.py')
d = importlib.util.module_from_spec(spec)
spec.loader.exec_module(d)
port_name, output = sys.argv[1:]
history = []
report = {'status': 'running', 'history': history}
p = serial.Serial()
p.port = port_name
p.baudrate = 115200
p.timeout = .25
p.write_timeout = 3
p.dtr = p.rts = False

STRIP_A, STRIP_B = 115, 253
DECK_CELLS = {'play': 64, 'cue': 157, 'loop': 239, 'half': 308, 'double': 356, 'speed': 410}
WAVE_A = 62
LOOP_Y, TRANS_Y, NAV_Y = 234, 288, 340
LOOP_CELLS = {'first': 51, 'second': 130, 'third': 207, 'fourth': 302, 'last': 401}
TRANS_CELLS = {'play': 60, 'cue': 146, 'hot1': 197, 'hot2': 262, 'hot3': 327, 'hot4': 407}
NAV_CELLS = {'decks': 90, 'tono': 148, 'rev': 197, 'speed': 244, 'scratch': 303, 'other': 365}


def save():
    Path(output).write_text(json.dumps(report, indent=2), encoding='utf-8')


def run(command):
    text = io.StringIO()
    with contextlib.redirect_stdout(text):
        lines = d.run(p, command, timeout=20)
    entry = {'command': command, 'lines': lines, 'log': text.getvalue()}
    history.append(entry)
    save()
    assert not any(x.startswith('@ERR') for x in lines), entry
    assert not re.search('Guru Meditation|task stuck|CORRUPT HEAP', entry['log']), entry
    assert any(x.startswith('@OK') for x in lines), 'Missing acknowledgement: ' + command
    return '\n'.join(lines)


def fields(command, marker):
    text = run(command)
    line = next(x for x in text.splitlines() if x.startswith(marker + ' '))
    return dict(re.findall(r'(\w+)=([^ ]+)', line))


def layout(**expected):
    result = fields('solco-layout', '@LAYOUT')
    for key, value in expected.items():
        assert result[key] == str(value), (key, result)
    return result


def press(x, y, pause=.3):
    run(f'press {x} {y} 110')
    time.sleep(pause)


def solco():
    v = fields('solco', '@SOLCO')
    assert v['volume'] == '0', 'User volume changed; stop test'
    return v


def loops():
    return fields('solco', '@LOOPS')


def first(value):
    return int(value.split('/')[0])


save()
try:
    p.open()
    time.sleep(.5)
    solco()
    layout(width=448, height=368, landscape=1, studio=-1, detail=0, master=0)
    # DECKS: play both decks from their strips.
    for deck, y in ((0, STRIP_A), (1, STRIP_B)):
        press(DECK_CELLS['play'], y)
        assert solco()['playing'].split('/')[deck] == '1', deck
    # FOCUS A from its waveform.
    press(240, WAVE_A, .5)
    layout(detail=1, deck=0)
    # Deck A is the 0.5 s demo loop (one beat at 120 BPM), shorter than any beat loop:
    # mark a short manual loop early in the cycle, then shift it with the UI's >> cell.
    for _ in range(40):
        if first(fields('solco', '@VINYL')['pos']) % 22050 < 3000:
            break
        time.sleep(.05)
    run('deck-loop 0 0')
    time.sleep(.06)
    run('deck-loop 0 1')
    time.sleep(.3)
    before = loops()
    assert first(before['active']) == 1, before
    lin, lout = first(before['in']), first(before['out'])
    assert lout + (lout - lin) <= 22050, ('loop too long for a forward shift', before)
    press(LOOP_CELLS['last'], LOOP_Y)  # >> while looping
    deadline = time.time() + 4
    while time.time() < deadline:
        now = loops()
        if first(now['in']) == lout:
            break
        time.sleep(.2)
    assert first(now['in']) == lout and first(now['out']) == 2 * lout - lin, now
    run('deck-loop 0 2')  # exit the loop
    assert first(loops()['active']) == 0
    # Hot cue 1: set, then jump (a set cue lights the cell; jump keeps playing).
    press(TRANS_CELLS['hot1'], TRANS_Y)
    hot = fields('solco', '@SOLCO')
    # Nav: the other deck, then back to DECKS.
    press(NAV_CELLS['other'], NAV_Y, .5)
    layout(detail=1, deck=1)
    press(NAV_CELLS['decks'], NAV_Y, .5)
    layout(detail=0)
    for deck, y in ((0, STRIP_A), (1, STRIP_B)):
        press(DECK_CELLS['play'], y)
        assert solco()['playing'].split('/')[deck] == '0', deck
    memory = fields('qa-memory', '@QA')
    assert memory['integrity'] == '1'
    report.update(status='passed', loop_before=[lin, lout], loop_after=[lout, 2 * lout - lin],
                  memory=memory, audio=solco())
except Exception as exc:
    report.update(status='failed', error=str(exc))
    raise
finally:
    report['finished'] = time.time()
    save()
    p.close()
print(json.dumps({k: v for k, v in report.items() if k != 'history'}, indent=2))
