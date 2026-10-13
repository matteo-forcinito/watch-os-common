'use strict';
// No local/session storage, external resources, HTML injection or service worker.
let session = '', entries = [], current = null, groupValue = null, selected = new Set(), preview = null, busy = false;
const $ = id => document.getElementById(id);
const fields = ['name', 'email', 'username', 'website', 'password', 'note'];
function notice(text) { $('notice').textContent = text; }
function clearSecrets() {
  session = ''; entries = []; current = null; preview = null; selected.clear();
  for (const id of fields.concat(['otp-secret','import-password','code'])) $(id).value = '';
  $('entries').replaceChildren(); $('groups').replaceChildren(); $('preview-entries').replaceChildren(); $('otp').textContent = '';
  $('workspace').hidden = true; $('lock').hidden = true; $('pairing').hidden = false; $('entry-form').hidden = true;
  $('import-dialog').close();
}
async function api(path, data, options = {}) {
  const headers = {'X-Vault-Key': session, ...(options.headers || {})};
  let body;
  if (data !== undefined) { headers['Content-Type'] = 'application/json'; body = JSON.stringify(data); }
  if (options.body !== undefined) body = options.body;
  const response = await fetch(path, {method: options.method || (body === undefined ? 'GET' : 'POST'), headers, body, cache: 'no-store', credentials: 'omit'});
  if (response.status === 401 || response.status === 423) { clearSecrets(); throw new Error('Sessione terminata. Riapri la dashboard sul dispositivo.'); }
  if (!response.ok) throw new Error(response.status === 409 ? 'Archivio occupato o dati cambiati: aggiorna e riprova.' : 'Operazione non completata ('+response.status+').');
  return options.binary ? response.blob() : response.json();
}
async function waitIdle() {
  for (let n = 0; n < 240; n++) {
    const status = await api('/api/status'); busy = status.busy; notice(status.message);
    if (!busy) { if(status.error)throw new Error(status.message);return status; }
    await new Promise(resolve => setTimeout(resolve,1000));
  }
  throw new Error('Operazione ancora in corso: verifica il dispositivo.');
}
function filtered() {
  const by = $('group-by').value, search = $('search').value.toLocaleLowerCase();
  return entries.filter(e => (!by || groupValue === null || e[by] === groupValue) &&
    (!search || ['name','email','username','website'].some(key => e[key].toLocaleLowerCase().includes(search))));
}
function button(text, callback) { const b = document.createElement('button'); b.type='button'; b.textContent=text; b.onclick=callback; return b; }
function render() {
  const by = $('group-by').value;
  $('groups').replaceChildren();
  if (by) {
    $('groups').append(button('Tutti i gruppi',() => { groupValue=null; render(); }));
    const values = [...new Set(entries.map(e=>e[by]))].sort((a,b)=>a.localeCompare(b,'it'));
    for (const value of values) {
      const count = entries.filter(e=>e[by]===value).length;
      const b = button((value || 'Non specificato')+' · '+count,() => {groupValue=value; render();});
      if (value === groupValue) b.classList.add('active'); $('groups').append(b);
    }
  }
  $('entries').replaceChildren();
  const visible = filtered();
  $('counts').textContent = visible.length+' voci · '+selected.size+' selezionate';
  for (const entry of visible) {
    const row = document.createElement('div'); row.className='entry';
    const check = document.createElement('input'); check.type='checkbox'; check.checked=selected.has(entry.id); check.setAttribute('aria-label','Seleziona '+entry.name);
    check.onchange=()=>{ check.checked ? selected.add(entry.id) : selected.delete(entry.id); $('counts').textContent=visible.length+' voci · '+selected.size+' selezionate'; };
    const open = button('',()=>openEntry(entry.id).catch(e=>notice(e.message)));
    const name = document.createElement('strong'); name.textContent=entry.name;
    const detail = document.createElement('span'); detail.textContent=entry.email || entry.username || entry.website || 'Nessun identificativo';
    open.append(name,detail); row.append(check,open); $('entries').append(row);
  }
}
async function refresh() {
  entries=(await api('/api/entries')).entries;
  const ids=new Set(entries.map(e=>e.id)); selected=new Set([...selected].filter(id=>ids.has(id))); render();
}
function fill(entry) {
  current=entry; $('editor-title').textContent=entry.name || 'Nuova credenziale'; $('entry-form').hidden=false;
  for (const field of fields) $(field).value=entry[field] || '';
  $('otp-secret').value=entry.otp || ''; $('otp-digits').value=String(entry.otp_digits); $('otp-period').value=String(entry.otp_period); $('otp-hash').value=String(entry.otp_hash);
  $('password').type='password'; $('reveal').textContent='Mostra'; $('otp').textContent='';
  $('delete').disabled=!entries.some(e=>e.id===entry.id); $('export-one').disabled=$('delete').disabled;
}
async function openEntry(id) { fill(await api('/api/entry',{id})); }
function newId() { return [...crypto.getRandomValues(new Uint8Array(16))].map(n=>n.toString(16).padStart(2,'0')).join(''); }
function readForm() {
  const entry={id:current.id}; for (const field of fields) entry[field]=$(field).value;
  entry.otp=$('otp-secret').value.trim(); entry.otp_digits=Number($('otp-digits').value); entry.otp_period=Number($('otp-period').value); entry.otp_hash=Number($('otp-hash').value);
  if (entry.otp.startsWith('otpauth://')) {
    const uri=new URL(entry.otp); if (uri.protocol!=='otpauth:' || uri.hostname!=='totp') throw new Error('Sono supportati gli URI TOTP.');
    entry.otp=uri.searchParams.get('secret') || ''; entry.otp_digits=Number(uri.searchParams.get('digits') || 6); entry.otp_period=Number(uri.searchParams.get('period') || 30);
    const algorithm=(uri.searchParams.get('algorithm') || 'SHA1').toUpperCase(); entry.otp_hash=algorithm==='SHA1'?1:algorithm==='SHA256'?256:algorithm==='SHA512'?512:0;
  }
  const maximum={name:64,email:128,username:128,website:256,password:256,note:256,otp:104};
  for (const [key,length] of Object.entries(maximum)) if (new TextEncoder().encode(entry[key]).length>length) throw new Error(key+': testo troppo lungo.');
  return entry;
}
async function download(ids) {
  if (ids && ids.length===0) throw new Error('Nessuna voce selezionata.');
  const blob=await api('/api/export',ids?{ids}:{},{binary:true});
  const url=URL.createObjectURL(blob), link=document.createElement('a'); link.href=url; link.download='Vault-'+new Date().toISOString().replace(/[:.]/g,'-')+'.wvault';
  document.body.append(link); link.click(); link.remove(); setTimeout(()=>URL.revokeObjectURL(url),1000);
  notice('Backup cifrato scaricato. Conservalo insieme a una copia sicura della password principale.');
}
$('pair-form').onsubmit=async event=>{
  event.preventDefault();
  try {
    const response=await fetch('/api/pair',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({code:$('code').value}),cache:'no-store',credentials:'omit'});
    if (!response.ok) throw new Error('Codice non valido o troppi tentativi.');
    session=(await response.json()).token; $('code').value=''; $('pairing').hidden=true; $('workspace').hidden=false; $('lock').hidden=false; await refresh(); notice('PC abbinato. Archivio locale cifrato.');
  } catch(error) { $('code').value=''; alert(error.message); }
};
$('lock').onclick=async()=>{ try {await api('/api/lock',{});} finally {clearSecrets();} };
$('search').oninput=render; $('group-by').onchange=()=>{groupValue=null;render();};
$('new').onclick=async()=>{try{const status=await api('/api/status');fill({id:newId(),name:'',otp_digits:6,otp_period:30,otp_hash:1,generation:status.generation});}catch(e){notice(e.message);}};
$('select-visible').onclick=()=>{for(const e of filtered())selected.add(e.id);render();};
$('entry-form').onsubmit=async event=>{event.preventDefault();try{await api('/api/save',{entry:readForm(),generation:current.generation});await waitIdle();await refresh();await openEntry(current.id);}catch(e){notice(e.message);}};
$('reveal').onclick=()=>{const shown=$('password').type==='text';$('password').type=shown?'password':'text';$('reveal').textContent=shown?'Mostra':'Nascondi';if(!shown)setTimeout(()=>{$('password').type='password';$('reveal').textContent='Mostra';},10000);};
$('generate').onclick=async()=>{try{$('password').value=(await api('/api/generate',{length:24})).password;notice('Password generata: premi Salva per conservarla.');}catch(e){notice(e.message);}};
$('delete').onclick=async()=>{if(!current || !confirm('Eliminare questa credenziale?'))return;try{await api('/api/delete',{id:current.id,generation:current.generation});await waitIdle();for(const f of fields)$(f).value='';$('otp-secret').value='';current=null;$('entry-form').hidden=true;await refresh();}catch(e){notice(e.message);}};
$('export-one').onclick=()=>download([current.id]).catch(e=>notice(e.message));
$('export-all').onclick=()=>download().catch(e=>notice(e.message));
$('export-filter').onclick=()=>download(filtered().map(e=>e.id)).catch(e=>notice(e.message));
$('export-selected').onclick=()=>download([...selected]).catch(e=>notice(e.message));
$('refresh-otp').onclick=async()=>{try{const result=await api('/api/otp',{id:current.id});$('otp').textContent=result.code+' · '+result.remaining+'s';setTimeout(()=>{$('otp').textContent='';},result.remaining*1000);}catch(e){notice('Controlla il segreto TOTP e l’orologio del dispositivo.');}};
$('open-import').onclick=()=>{$('preview').hidden=true;$('import-form').hidden=false;$('import-notice').textContent='';$('import-dialog').showModal();};
$('cancel-import').onclick=()=>{$('import-password').value='';$('import-file').value='';$('import-dialog').close();};
$('import-form').onsubmit=async event=>{
  event.preventDefault(); const file=$('import-file').files[0]; let password=$('import-password').value; $('import-password').value='';
  try {
    if(!file || file.size>1048656)throw new Error('Backup non valido o troppo grande.');
    const bytes=new TextEncoder().encode(password); const encoded=btoa(String.fromCharCode(...bytes));password='';bytes.fill(0);
    $('import-notice').textContent='Derivazione chiave e verifica: attendi…';
    await api('/api/import',undefined,{method:'POST',body:file,headers:{'X-Vault-Import-Password':encoded,'Content-Type':'application/octet-stream'}});
    await waitIdle(); preview=await api('/api/preview');renderPreview();
  } catch(e) {$('import-notice').textContent=e.message;} finally {password='';}
};
function renderPreview() {
  $('preview-entries').replaceChildren(); $('preview').hidden=false;$('import-form').hidden=true;$('confirm-import').checked=false;
  preview.entries.forEach((entry,index)=>{
    const row=document.createElement('div');row.className='preview-entry';
    const title=document.createElement('strong');title.textContent=entry.name;
    const detail=document.createElement('p');detail.textContent=(entry.email || entry.username || entry.website || 'Senza identificativo')+(entry.conflict>=0?' · Conflitto con una voce esistente':' · Nuova voce');
    const choices=document.createElement('select');choices.id='choice-'+index;choices.setAttribute('aria-label','Scelta per '+entry.name);
    for(const [value,label]of [[0,'Salta'],[1,'Mantieni esistente / aggiungi se nuova'],[2,'Sostituisci esistente / aggiungi'],[3,'Conserva entrambe']]){const option=document.createElement('option');option.value=String(value);option.textContent=label;choices.append(option);}
    choices.value=entry.conflict>=0?'0':'2';
    const edit=button('Modifica prima di importare',()=>{const draft={...entry};delete draft.conflict;fill(draft);current.previewIndex=index;$('import-dialog').close();notice('Modifica la voce importata, poi usa “Salva” per aggiornare l’anteprima.');});
    row.append(title,detail,choices,edit);$('preview-entries').append(row);
  });
  $('import-notice').textContent=preview.entries.length+' voci in anteprima. Nessuna modifica ancora salvata.';
}
// Preview edits use their own transaction; regular save cannot silently import.
const normalSave=$('entry-form').onsubmit;
$('entry-form').onsubmit=async event=>{
  if(current && current.previewIndex!==undefined){event.preventDefault();try{const index=current.previewIndex;await api('/api/preview/edit',{index,token:preview.token,entry:readForm()});await waitIdle();preview=await api('/api/preview');renderPreview();$('import-dialog').showModal();}catch(e){notice(e.message);}return;}
  return normalSave(event);
};
$('resolve').onclick=async()=>{
  try{if(!$('confirm-import').checked)throw new Error('Conferma le scelte prima di applicarle.');const choices=preview.entries.map((_,i)=>Number($('choice-'+i).value));await api('/api/resolve',{token:preview.token,choices,confirmed:true});await waitIdle();await refresh();preview=null;$('preview-entries').replaceChildren();$('import-dialog').close();notice('Importazione elaborata. Controlla il messaggio del dispositivo.');}catch(e){$('import-notice').textContent=e.message;}
};
let lastActivity=0;
for(const event of ['pointerdown','keydown'])document.addEventListener(event,()=>{if(session && Date.now()-lastActivity>15000){lastActivity=Date.now();api('/api/activity',{}).catch(()=>{});}});
setInterval(async()=>{if(!session)return;try{const status=await api('/api/status');busy=status.busy;notice(status.message);}catch(e){notice(e.message);}},5000);
window.addEventListener('pagehide',clearSecrets);
