'use strict';
const dogs = document.body.dataset.app === 'zampe';
const $ = s => document.querySelector(s);
const E = (tag, cls, text) => { const e=document.createElement(tag); if(cls)e.className=cls;if(text!=null)e.textContent=text;return e; };
const button = (text, fn, cls='') => {const b=E('button',cls,text);b.type='button';b.onclick=fn;return b;};
const append = (p,...children) => {children.filter(Boolean).forEach(c=>p.append(c));return p;};
let state, selected=0, busy=false, calendarMonth=0, calendarDate='';
const dayNames=['Lun','Mar','Mer','Gio','Ven','Sab','Dom'];
const arr = k => state?.[k] || [];
const profile = id => arr('pets').find(p=>p.id===id);
const shortDate = d => d ? new Date(d+'T12:00:00').toLocaleDateString('it-IT',{day:'numeric',month:'short'}) : '—';
function cityWeek(city,startDate=state.runtime.today,count=7){
  const start=Date.parse(startDate+'T00:00:00Z')/86400000;
  return Array.from({length:count},(_,i)=>{
    const day=start+i,date=new Date(day*86400000).toISOString().slice(0,10),weekday=((day+3)%7+7)%7;
    const override=city.exceptions.find(e=>e.date===date);
    const bins=city.bins.filter(b=>{if(override)return override.bins.includes(b.id);
      const anchor=Date.parse((b.anchor||'1970-01-05')+'T00:00:00Z')/86400000;
      return day>=anchor&&(b.days&(1<<weekday))&&Math.floor((day-anchor)/7)%b.every===0;});
    return {date,weekday,bins};
  });
}
function notice(text,error=false){const n=E('div','notice'+(error?' error':''),text);return n;}
async function refresh(){state=await kit.api('/api/state');const list=arr(dogs?'pets':'cities');if(!list.some(p=>p.id===selected))selected=dogs?(list[0]?.id||0):(state.preferred||list[0]?.id||0);render();}
async function act(command){
  if(busy)return false;busy=true;$('#status').textContent='Salvataggio in corso…';
  try{await kit.api('/api/action',{body:command});await refresh();return true;}
  catch(e){kit.toast(e.message||'Operazione non riuscita','error');return false;}
  finally{busy=false;}
}
const field=(key,label,type='text',options)=>({key,label,type,options});
const schemas={
 city:[field('name','Nome della città'),field('zone','Zona / tipo di utenza'),field('source','Fonte del calendario','url')],
 bin:[field('name','Materiali raccolti insieme'),field('days','Giorni di passaggio','days'),field('every','Ogni quante settimane','number'),field('anchor','Data nella prima settimana attiva','date'),field('color','Colore','color')],
 exception:[field('date','Data dell’eccezione','date'),field('bins','Raccolte previste in quella data','bins')],
 pet:[field('name','Nome'),field('birth','Data di nascita','date'),field('sex','Sesso','select',[['unknown','Non specificato'],['female','Femmina'],['male','Maschio']]),field('breed','Razza'),field('relation','Relazione familiare'),field('neutered','Sterilizzato/a','checkbox'),field('food','Alimentazione'),field('allergies','Allergie e sensibilità'),field('vet','Veterinario'),field('phone','Telefono veterinario','tel'),field('microchip','Microchip'),field('notes','Note private','textarea')],
 task:[field('pet','Cane','pets'),field('label','Attività'),field('kind','Categoria','select',[['routine','Routine'],['food','Alimentazione'],['walk','Passeggiata'],['groom','Igiene e denti'],['visit','Visita veterinaria'],['vaccine','Vaccino'],['parasite','Antiparassitario'],['medicine','Terapia prescritta'],['heat','Ciclo / riproduzione'],['other','Altro']]),field('due','Data prevista','date'),field('minute','Ora','time'),field('repeat_days','Ripeti ogni N giorni (0 = una volta)','number'),field('enabled','Attività attiva','checkbox'),field('notify','Notifica sul dispositivo anche ad app chiusa','checkbox'),field('notes','Indicazioni del veterinario / note','textarea')],
 weight:[field('pet','Cane','pets'),field('date','Data della misura','date'),field('grams','Peso in grammi','number')],
 log:[field('pet','Cane','pets'),field('date','Data','date'),field('label','Titolo'),field('kind','Categoria'),field('note','Osservazione privata','textarea')]
};
const titles={city:'Città',bin:'Raccolta',exception:'Eccezione',pet:'Profilo',task:'Attività',weight:'Peso',log:'Diario'};
async function edit(entity,record,defaults={}){
  const d=$('#editor');if(d.open)return;d.replaceChildren();const f=E('form');f.method='dialog';
  const existing=!!record;
  const data={name:'',date:state.runtime.today,due:state.runtime.today,pet:selected,minute:540,repeat_days:0,every:1,anchor:state.runtime.today,color:0x70d5b0,enabled:true,notify:false,days:0,bins:[],sex:'unknown',...defaults,...record};
  append(f,E('h2','',(existing?'Modifica ':'Nuovo ')+titles[entity].toLowerCase()));
  const readers=[];
  for(const s of schemas[entity]){
    const label=E('label','field',s.label);let input;
    if(s.type==='days'||s.type==='bins'){
      const box=E('div','checks');const options=s.type==='days'?dayNames.map((n,i)=>[i,n]):(arr('cities').find(c=>c.id===selected)?.bins||[]).map(b=>[b.id,b.name]);
      const choices=options.map(([value,name])=>{const l=E('label');const i=E('input');i.type='checkbox';i.checked=s.type==='days'?!!(data.days&(1<<value)):data.bins.includes(value);append(l,i,document.createTextNode(name));box.append(l);return [value,i];});
      append(f,E('div','small',s.label),box);
      if(s.type==='bins')f.append(E('p','small','Nessuna selezione: nessun ritiro. Questa eccezione sostituisce il calendario settimanale.'));
      readers.push(()=>{data[s.key]=s.type==='days'?choices.reduce((mask,[v,i])=>mask|(i.checked?1<<v:0),0):choices.filter(([,i])=>i.checked).map(([v])=>v);});continue;
    }
    if(s.type==='select'||s.type==='pets'){
      input=E('select');const options=s.type==='pets'?arr('pets').map(p=>[p.id,p.name]):s.options;
      options.forEach(([v,n])=>{const o=E('option','',n);o.value=v;input.append(o);});input.value=data[s.key]??options[0]?.[0]??'';
    }else if(s.type==='textarea'){input=E('textarea');input.value=data[s.key]||'';input.maxLength=500;}
    else {input=E('input');input.type=s.type;
      if(s.type==='checkbox')input.checked=!!data[s.key];
      else if(s.type==='time')input.value=String(Math.floor(data.minute/60)).padStart(2,'0')+':'+String(data.minute%60).padStart(2,'0');
      else if(s.type==='color')input.value='#'+Number(data.color).toString(16).padStart(6,'0');
      else input.value=data[s.key]??'';
      if(s.type==='number'){input.min=s.key==='every'?1:s.key==='grams'?100:0;input.max=s.key==='every'?8:s.key==='grams'?150000:3650;input.step=1;}
      if(s.type==='text')input.maxLength=s.key==='name'?(entity==='pet'?40:48):s.key==='label'?80:s.key==='kind'?24:120;
      if(s.type==='date'&&(s.key==='birth'||entity==='weight'))input.max=state.runtime.today;
    }
    if(['name','label','due','date','grams','anchor'].includes(s.key))input.required=true;
    input.name=s.key;append(label,input);f.append(label);
    readers.push(()=>{data[s.key]=s.type==='checkbox'?input.checked:s.type==='number'||s.type==='pets'?Number(input.value):s.type==='color'?parseInt(input.value.slice(1),16):s.type==='time'?Number(input.value.slice(0,2))*60+Number(input.value.slice(3)):input.value;});
  }
  const actions=E('div','dialog-actions');const submit=E('button','primary','Salva');submit.type='submit';
  append(actions,submit,button('Annulla',()=>d.close()));
  if(existing)actions.append(button('Elimina',async()=>{
    if(!confirm(entity==='pet'?'Eliminare il profilo e tutte le sue attività, misure e registrazioni?':'Eliminare questa voce?'))return;
    const c={op:entity+'.delete',id:record.id,city:entity==='city'?record.id:selected};if(entity==='exception')c.date=record.date;
    if(await act(c))d.close();
  },'danger'));
  f.append(actions);
  f.onsubmit=async e=>{e.preventDefault();if(!f.reportValidity())return;readers.forEach(r=>r());
    const c={op:entity+'.'+(entity==='exception'?'put':existing?'edit':'add'),city:selected};
    if(existing&&record.id)c.id=record.id;
    for(const s of schemas[entity])c[s.key]=data[s.key];
    if(entity==='city'&&existing)c.city=record.id;
    if(await act(c))d.close();
  };
  d.append(f);d.showModal();
}
function section(title,action){const p=E('section','panel');const head=E('div','section-head');append(head,E('h2','',title),action);p.append(head);return p;}
function entry(title,subtitle,actions){const r=E('div','entry');append(r,append(E('div','body'),E('h3','',title),subtitle?E('p','',subtitle):null),actions?append(E('div','actions'),...actions):null);return r;}
function render(){
  $('#status').textContent=state.save_error?'Salvataggio non riuscito':state.saving?'Salvataggio in corso…':'Dati sul dispositivo';
  const side=$('#side');side.replaceChildren(E('div','eyebrow',dogs?'I tuoi compagni':'Le tue città'));
  arr(dogs?'pets':'cities').forEach(p=>side.append(button(p.name,()=>{selected=p.id;render();},'choose'+(selected===p.id?' selected':''))));
  side.append(button(dogs?'+ Aggiungi cane':'+ Aggiungi città',()=>edit(dogs?'pet':'city'),'add'));
  if(!dogs)side.append(button('Civita Castellana · SATE',()=>act({op:'city.add',name:'Civita Castellana',preset:'civita-domestica'}),'add'));
  const main=$('#main');main.replaceChildren();
  if(state.save_error)main.append(notice(state.save_error,true),button('Riprova salvataggio',()=>act({op:'save.retry'})));
  if(dogs)renderDogs(main);else renderWaste(main);
}
function renderWaste(main){
  const city=arr('cities').find(c=>c.id===selected);$('#hero-title').textContent='Ogni cosa,\nal giorno giusto.';
  $('#hero-sub').textContent=city?city.name+' · '+(city.zone||'Il tuo calendario, le tue regole'):'Calendari di raccolta, materiali ed eccezioni. Organizza tutte le tue città in un solo posto.';
  const tomorrow=state.runtime.week[1];$('#stat').textContent=tomorrow.bins.length;$('#stat-caption').textContent='raccolte domani nella città preferita';
  if(!city){main.append(notice('Aggiungi una città oppure parti dal calendario domestico SATE di Civita Castellana. Ogni materiale e raggruppamento è modificabile.'));return;}
  if(city.id!==state.preferred)main.append(notice('Il riepilogo e l’indicazione nel menu usano la città preferita.'),button('Usa '+city.name+' come preferita',()=>act({op:'settings',preferred:city.id})));
  const calendar=section('I prossimi giorni',button('Modifica città',()=>edit('city',city)));
  {const week=E('div','week');cityWeek(city).forEach((d,i)=>{
    const p=E('div','day'+(i===1?' tomorrow':''));append(p,E('div','eyebrow',i===0?'Oggi':i===1?'Domani':dayNames[d.weekday]),E('div','date',shortDate(d.date)));
    if(!d.bins.length)p.append(E('div','small','Nessun ritiro'));
    d.bins.forEach(b=>{const line=E('div','material');const dot=E('span','dot');dot.style.background='#'+(b.color&0xffffff).toString(16).padStart(6,'0');append(line,dot,document.createTextNode(b.name));p.append(line);});week.append(p);
  });calendar.append(week);}
  main.append(calendar);const split=E('div','split');
  const today=new Date(state.runtime.today+'T12:00:00Z');
  const first=new Date(Date.UTC(today.getUTCFullYear(),today.getUTCMonth()+calendarMonth,1));
  const firstISO=first.toISOString().slice(0,10),length=new Date(Date.UTC(first.getUTCFullYear(),first.getUTCMonth()+1,0)).getUTCDate();
  const month=section(first.toLocaleDateString('it-IT',{month:'long',year:'numeric',timeZone:'UTC'}));
  const navigation=E('div','calendar-controls');
  append(navigation,button('← Mese precedente',()=>{calendarMonth--;calendarDate='';render();}),button('Oggi',()=>{calendarMonth=0;calendarDate=state.runtime.today;render();}),button('Mese successivo →',()=>{calendarMonth++;calendarDate='';render();}));
  month.append(navigation);const grid=E('div','month-grid');dayNames.forEach(n=>grid.append(E('div','calendar-weekday',n)));
  const offset=(first.getUTCDay()+6)%7;for(let i=0;i<offset;i++)grid.append(E('div','calendar-spacer'));
  const monthDays=cityWeek(city,firstISO,length);
  if(!monthDays.some(d=>d.date===calendarDate))calendarDate=monthDays.some(d=>d.date===state.runtime.today)?state.runtime.today:firstISO;
  monthDays.forEach(d=>{
    const cell=button('',()=>{calendarDate=d.date;render();},'calendar-cell'+(d.date===state.runtime.today?' is-today':'')+(d.date===calendarDate?' is-selected':''));
    cell.setAttribute('aria-label',shortDate(d.date)+': '+(d.bins.map(b=>b.name).join(', ')||'Nessun ritiro'));
    cell.append(E('strong','',Number(d.date.slice(8))));const dots=E('div','calendar-dots');
    d.bins.forEach(b=>{const dot=E('span','dot');dot.style.background='#'+(b.color&0xffffff).toString(16).padStart(6,'0');dot.title=b.name;dots.append(dot);});cell.append(dots);grid.append(cell);
  });month.append(grid);
  const chosen=monthDays.find(d=>d.date===calendarDate);
  month.append(entry(shortDate(chosen.date),chosen.bins.map(b=>b.name).join(' · ')||'Nessun ritiro previsto'));
  month.append(E('p','small','Tocca una data: i punti colorati indicano i materiali. Le eccezioni sostituiscono i passaggi ordinari.'));
  main.append(month);
  const bins=section('Materiali e passaggi',button('+ Raccolta',()=>edit('bin')));
  city.bins.forEach(b=>bins.append(entry(b.name,dayNames.filter((_,i)=>b.days&(1<<i)).join(' · ')+' · ogni '+b.every+' sett.',[button('Modifica',()=>edit('bin',b))])));
  if(!city.bins.length)bins.append(E('p','empty','Raggruppa liberamente i materiali raccolti insieme.'));
  const ex=section('Eccezioni',button('+ Data',()=>edit('exception')));
  city.exceptions.forEach(x=>ex.append(entry(shortDate(x.date),x.bins.map(id=>city.bins.find(b=>b.id===id)?.name).filter(Boolean).join(', ')||'Nessun ritiro',[button('Modifica',()=>edit('exception',x))])));
  if(!city.exceptions.length)ex.append(E('p','empty','Festività o variazioni: aggiungi soltanto le date comunicate dal gestore.'));
  append(split,bins,ex);main.append(split);
  const options=section('Un piccolo aiuto, ogni giorno');const l=E('label','toggle');const toggle=E('input');toggle.type='checkbox';toggle.checked=state.menu_hint;toggle.onchange=()=>act({op:'settings',menu_hint:toggle.checked});append(l,toggle,document.createTextNode('Mostra nel menu cosa passa domani nella città preferita'));options.append(l);
  if(city.source){const link=E('a','','Consulta la fonte del calendario');try{const url=new URL(city.source);if(['https:','http:'].includes(url.protocol)){link.href=url.href;link.target='_blank';link.rel='noopener noreferrer';options.append(link);}}catch(e){options.append(E('p','small',city.source));}}
  options.append(E('p','small','Il preset SATE è il calendario domestico in vigore dal 13 gennaio 2025. Verifica eventuali variazioni del gestore.'));main.append(options);
}
function renderDogs(main){
  const pet=profile(selected);const tasks=arr('tasks').filter(t=>t.pet===selected);const due=tasks.filter(t=>t.enabled&&t.due<=state.runtime.today);
  $('#hero-title').textContent='La cura vive\nnei piccoli gesti.';$('#hero-sub').textContent=pet?pet.name+' · '+(pet.breed||'Il suo spazio, ogni giorno'):'Routine, salute e ricordi. Uno spazio privato per prenderti cura dei tuoi cani.';
  $('#stat').textContent=due.length;$('#stat-caption').textContent='attività da completare per il profilo selezionato';
  if(!pet){main.append(notice('Aggiungi il primo profilo. Le informazioni rimangono sul dispositivo; le notifiche sono facoltative.'));return;}
  const agenda=section('Oggi, per '+pet.name,button('+ Attività',()=>edit('task')));
  if(due.length)due.sort((a,b)=>a.due.localeCompare(b.due)||a.minute-b.minute).forEach(t=>agenda.append(entry(t.label,(t.due<state.runtime.today?'In arretrato · ':'Oggi · ')+String(Math.floor(t.minute/60)).padStart(2,'0')+':'+String(t.minute%60).padStart(2,'0'),[button('Fatto',()=>act({op:'task.done',id:t.id}),'primary'),button('Apri',()=>edit('task',t))])));
  else agenda.append(E('p','empty',tasks.length?'Tutto in ordine per oggi. Qui compariranno le attività da completare.':'Il suo programma è ancora vuoto. Parti dalle azioni qui sotto: nulla viene attivato senza il tuo salvataggio.'));
  const next=tasks.filter(t=>t.enabled&&t.due>state.runtime.today).sort((a,b)=>a.due.localeCompare(b.due)||a.minute-b.minute).slice(0,3);
  next.forEach(t=>agenda.append(entry('Prossima · '+t.label,shortDate(t.due),[button('Apri',()=>edit('task',t))])));main.append(agenda);
  const quick=section('Costruisci il suo programma');const choices=E('div','quick-care');
  [['Pasto','food',1],['Passeggiata','walk',1],['Igiene e denti','groom',0],['Visita veterinaria','visit',0],['Vaccino','vaccine',0],['Antiparassitario','parasite',0],['Terapia prescritta','medicine',0]].forEach(([label,kind,repeat_days])=>choices.append(button('+ '+label,()=>edit('task',null,{label,kind,repeat_days}))));
  quick.append(choices,E('p','small','Scegli data, ora, frequenza e notifiche. Per gli interventi sanitari registra le indicazioni del veterinario.'));main.append(quick);
  const overview=section(pet.name,button('Profilo e contatti',()=>edit('pet',pet)));
  append(overview,E('span','pill',pet.sex==='female'?'Femmina':pet.sex==='male'?'Maschio':'Sesso non specificato'),pet.birth?E('span','pill','Nascita · '+shortDate(pet.birth)):null,pet.relation?E('span','pill',pet.relation):null);
  [pet.food&&'Alimentazione: '+pet.food,pet.allergies&&'Sensibilità: '+pet.allergies,pet.vet&&'Veterinario: '+pet.vet+(pet.phone?' · '+pet.phone:''),pet.notes].filter(Boolean).forEach(t=>overview.append(E('p','small',t)));main.append(overview);
  const plan=section('Il suo programma',button('+ Attività',()=>edit('task')));
  [...tasks].sort((a,b)=>a.due.localeCompare(b.due)||a.minute-b.minute).forEach(t=>{
    const time=String(Math.floor(t.minute/60)).padStart(2,'0')+':'+String(t.minute%60).padStart(2,'0');
    const controls=[button('Dettagli',()=>edit('task',t))];
    if(t.enabled){controls.unshift(button('Fatto',()=>act({op:'task.done',id:t.id}),'primary'));controls.push(button('+1 giorno',()=>act({op:'task.postpone',id:t.id})));}
    plan.append(entry(t.label,shortDate(t.due)+' · '+time+' · '+(t.enabled?'Attiva':'Completata / sospesa')+(t.repeat_days?' · ogni '+t.repeat_days+' giorni':'')+(t.notify?' · Notifica attiva':'')+(t.notes?'\n'+t.notes:''),controls));
  });
  if(!tasks.length)plan.append(E('p','empty','Passeggiate, pasti, igiene, visite o terapie prescritte: crea il programma adatto a lui.'));main.append(plan);
  const split=E('div','split');const weights=section('Crescita e peso',button('+ Misura',()=>edit('weight')));
  const points=arr('weights').filter(w=>w.pet===selected).sort((a,b)=>a.date.localeCompare(b.date)||a.id-b.id);
  if(points.length){weights.append(E('div','stat',(points.at(-1).grams/1000).toLocaleString('it-IT')+' kg'));
    if(points.length>1){const ns='http://www.w3.org/2000/svg',svg=document.createElementNS(ns,'svg');svg.setAttribute('viewBox','0 0 400 140');svg.classList.add('trend');svg.setAttribute('role','img');svg.setAttribute('aria-label','Andamento del peso');
      const recent=points.slice(-20),min=Math.min(...recent.map(p=>p.grams)),max=Math.max(...recent.map(p=>p.grams));
      const line=document.createElementNS(ns,'polyline');line.setAttribute('points',recent.map((p,i)=>(15+i*370/(recent.length-1))+','+(115-(p.grams-min)*95/Math.max(100,max-min))).join(' '));line.setAttribute('stroke','var(--accent)');line.setAttribute('stroke-width','3');line.setAttribute('fill','none');svg.append(line);weights.append(svg);}
    points.slice(-8).reverse().forEach(w=>weights.append(entry(shortDate(w.date),(w.grams/1000).toLocaleString('it-IT')+' kg',[button('Modifica',()=>edit('weight',w))])));
  }else weights.append(E('p','empty','Registra il peso per seguirne l’andamento nel tempo.'));
  const diary=section('Il diario',button('+ Nota',()=>edit('log')));const logs=arr('logs').filter(l=>l.pet===selected).sort((a,b)=>b.date.localeCompare(a.date)||b.id-a.id);
  logs.slice(0,30).forEach(l=>diary.append(entry(l.label,shortDate(l.date)+(l.note?' · '+l.note:''),[button('Apri',()=>edit('log',l))])));
  if(!logs.length)diary.append(E('p','empty','Qui ritroverai le attività completate e le tue osservazioni.'));append(split,weights,diary);main.append(split);
  main.append(notice('Vaccini, antiparassitari e terapie seguono il piano del tuo veterinario. Zampe registra le indicazioni che inserisci, senza suggerire dosaggi.'));
}
$('#export').onclick=async()=>{try{const data=await kit.api('/api/export');const url=URL.createObjectURL(new Blob([JSON.stringify(data,null,2)],{type:'application/json'}));const a=E('a');a.href=url;a.download=dogs?'zampe-private.json':'raccolta.json';a.click();setTimeout(()=>URL.revokeObjectURL(url),10000);}catch(e){kit.toast(e.message,'error');}};
$('#reload').onclick=()=>refresh().catch(e=>kit.toast(e.message,'error'));
kit.start({onReady:refresh});
setInterval(()=>{if(!document.hidden&&!$('#editor').open&&!busy)refresh().catch(()=>{$('#status').textContent='Dispositivo non raggiungibile';});},5000);
