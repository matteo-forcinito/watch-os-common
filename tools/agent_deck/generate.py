"""Generate the paired-device dashboard from the same original UI as the PC."""
from pathlib import Path
root = Path(__file__).resolve().parents[2]
page = (Path(__file__).parent/'dashboard.html').read_text(encoding='utf-8')
page = page.replace('<style>', '<link rel="stylesheet" href="/kit.css"><style>', 1)
page = page.replace('<html lang="it">','<html lang="it" data-theme="dark">',1)
page = page.replace('<script>\n', '<script src="/kit.js"></script><script>\n', 1)
page = page.replace("const r=await fetch('/api/state',{headers:{'X-Deck-Key':key},signal:AbortSignal.timeout(5000)});if(!r.ok)throw Error('HTTP');state=await r.json();",
                    "const raw=await kit.api('/api/state');state={...raw,agents:raw.cards||[],jobs:[],repositories:[],processes:[],sources:{device:'Ultimo snapshot del monitor PC'},session_allowance:{reason:'Quote e dettagli completi sono disponibili nel monitor PC.'}};")
page = page.replace("refresh();setInterval(refresh,5000);", "kit.start({onReady:()=>{refresh();setInterval(refresh,5000)}});")
page = page.replace('MONITOR LOCALE', 'DISPOSITIVO')
page = page.replace('<button id="pair-device">Collega dispositivo</button>', '<button id="pair-device" class="hide">Collega dispositivo</button>')
page = page.replace('Build e rilasci</button>', 'Build e rilasci</button>')
page = page.replace('<button data-tab="jobs">Build e rilasci</button><button data-tab="repos">Repository</button><button data-tab="usage">Quote e sessioni</button><button data-tab="system">Background</button>', '')
target = root/'components/apps/agentdeck/agentdeck_dashboard.html'
target.write_text(page, encoding='utf-8')
print('Generated device dashboard', target.stat().st_size, 'bytes')
