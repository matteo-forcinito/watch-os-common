"""Create and register a minigame from the working beginner example.

Run from watch-os-common: python tools/new_minigame.py palloncini --title "Palloncini"
No build, flash, commit or publication is performed.
"""
import argparse
import json
from pathlib import Path
import re


def create(root: Path, name: str, title: str) -> Path:
    if not re.fullmatch(r'[a-z][a-z0-9_]{1,23}', name):
        raise ValueError('Usa 2-24 caratteri: lettere minuscole, numeri e underscore; inizia con una lettera.')
    if not title.strip() or len(title) > 48 or any(ord(c) < 32 for c in title):
        raise ValueError('Il titolo deve avere 1-48 caratteri e stare su una sola riga.')
    apps = root / 'components/apps'
    target = apps / 'minigames/games' / (name + '.c')
    catalog = apps / 'pet/pet_game_catalog.c'
    cmake = apps / 'CMakeLists.txt'
    template = (apps / 'minigames/games/tap_star.c').read_text(encoding='utf-8')
    old_catalog = catalog.read_text(encoding='utf-8')
    old_cmake = cmake.read_text(encoding='utf-8')
    if target.exists() or f'mg_{name}' in old_catalog:
        raise ValueError('Questo nome esiste gia: nessun file e stato modificato.')
    declaration = '/* Generated game declarations. */'
    entry = '/* Generated game entries. */'
    source = '        "minigames/games/tap_star.c"'
    if old_catalog.count(declaration) != 1 or old_catalog.count(entry) != 1 or old_cmake.count(source) != 1:
        raise ValueError('I punti di registrazione sono cambiati: nessun file e stato modificato.')
    new_source = template.replace('mg_tap_star', 'mg_' + name).replace('.id = "tap_star"', '.id = "' + name + '"')
    new_source = new_source.replace('.title = "Tocca la stella"', '.title = ' + json.dumps(title, ensure_ascii=False))
    new_catalog = old_catalog.replace(declaration, f'extern const mg_game_t mg_{name};\n' + declaration)
    new_catalog = new_catalog.replace(entry, f'&mg_{name},\n    ' + entry)
    new_cmake = old_cmake.replace(source, source + f'\n        "minigames/games/{name}.c"')
    # Exclusive create protects an existing game. Roll back a partial edit on I/O failure.
    created = False
    try:
        with target.open('x', encoding='utf-8', newline='\n') as f:
            created = True
            f.write(new_source)
        catalog.write_text(new_catalog, encoding='utf-8')
        cmake.write_text(new_cmake, encoding='utf-8')
    except OSError:
        catalog.write_text(old_catalog, encoding='utf-8')
        cmake.write_text(old_cmake, encoding='utf-8')
        if created:
            target.unlink()
        raise
    return target


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('name')
    parser.add_argument('--title', required=True)
    args = parser.parse_args()
    try:
        path = create(Path(__file__).resolve().parents[1], args.name, args.title)
    except (ValueError, OSError) as e:
        parser.exit(1, str(e) + '\n')
    print('Creato e registrato:', path)
    print('Modifica questo file; poi: python tools/test_minigames.py')
    print('Guida: docs/guida/14-minigiochi.md')
