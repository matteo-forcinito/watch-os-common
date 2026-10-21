const http=require('http'),fs=require('fs'),path=require('path');
const {WebSocketServer}=require('ws');
const fixture=require('./fixtures.cjs');
const initialStates=JSON.stringify(fixture.states);
const root=path.resolve(__dirname,'../..');
let favorites=[],connected=false,url='',actions=[],lastSave=null,eventCount=0;
let catalogJob=0,catalogResponse=null;
let homepage=0,cache=null;
function catalogPage(page=0){return {total:fixture.states.length,entities:fixture.states.slice(page*64,(page+1)*64).map(s=>{const entry=fixture.registry.find(e=>e.entity_id===s.entity_id),device=fixture.devices.find(d=>d.id===entry?.device_id),area=fixture.areas.find(a=>a.area_id===(entry?.area_id||device?.area_id));return {id:s.entity_id,name:s.attributes.friendly_name||s.entity_id,state:s.state,unit:s.attributes.unit_of_measurement||'',device_class:s.attributes.device_class||'',device_id:device?.id||'',device_name:device?.name_by_user||device?.name||'',area_id:area?.area_id||'',area:area?.name||'Senza stanza',dashboards:[]};})};}
const category=e=>e.id.startsWith('light.')?'Luci':e.id.startsWith('climate.')?'Termostati':e.device_class==='outlet'?'Prese':e.device_class==='temperature'?'Termometri':e.device_class==='humidity'?'Umidità':'Sensori';
function entity(e){const source=fixture.states.find(item=>item.entity_id===e.id),a=source?.attributes||{},value=source?.state||'unknown';return {...e,category:category(e),state:value,known:!!source,available:!['unknown','unavailable'].includes(value),on:['on','heat','auto'].includes(value),display:value+(a.unit_of_measurement?' '+a.unit_of_measurement:''),brightness:Math.round((a.brightness||0)*100/255),position:-1,current_temp:a.current_temperature??null,target_temp:a.temperature??null,min_temp:a.min_temp??null,max_temp:a.max_temp??null,temp_step:a.target_temp_step??.5,hvac_modes:a.hvac_modes||[],supports_brightness:!!a.brightness,code_required:!!a.code_format,code_arm_required:!!a.code_arm_required,alarm_features:a.supported_features||0};}
function json(res,value){res.writeHead(200,{'Content-Type':'application/json'});res.end(JSON.stringify(value));}
const server=http.createServer(async(req,res)=>{
  const pathname=new URL(req.url,'http://localhost').pathname;
  if(req.method==='GET'&&['/casa/','/kit.js','/kit.css'].includes(pathname)){
    const name=pathname==='/casa/'?'casa/main/casa_dashboard.html':'components/services/web/'+pathname.slice(1);
    res.writeHead(200,{'Content-Type':pathname.endsWith('.js')?'application/javascript':pathname.endsWith('.css')?'text/css':'text/html'});return res.end(fs.readFileSync(path.join(root,name)));
  }
  let data='';for await(const chunk of req)data+=chunk;
  if(pathname==='/casa/api/kit')return json(res,{name:'Casa',board:'GUITION QA',battery:-1});
  if(pathname==='/casa/api/state')return json(res,{homepage,weather:{code:0,temperature:23.5,city:'Citta di prova',is_day:true},connected,configured:!!favorites.length,url,limit:256,revision:eventCount+1,event_count:eventCount,message:connected?'Collegato a Home Assistant':'Configura Casa dal PC',entities:favorites.map(entity)});
  if(pathname==='/casa/api/config'&&req.method==='GET')return json(res,{homepage,url,has_token:!!favorites.length,favorites,limit:256});
  if(pathname==='/casa/api/home'&&req.method==='POST'){const choice=JSON.parse(data).homepage;if(![0,1].includes(choice))return json(res,{ok:false});homepage=choice;return json(res,{ok:true});}
  if(pathname==='/casa/api/catalog/cache'){
    if(req.method==='POST'){cache={...JSON.parse(data),saved_at:Math.floor(Date.now()/1000)};return json(res,{ok:true});}
    if(cache)return json(res,cache);
    res.writeHead(404);return res.end('No cached catalog');
  }
  if(pathname==='/casa/api/catalog'&&req.method==='POST'){
    const request=JSON.parse(data);catalogJob++;
    try{catalogResponse=request.type==='catalog'?{type:'event',event:{result:JSON.stringify(catalogPage(request.page))}}:{success:true,result:fixture.reply(request)};}catch(error){catalogResponse={success:false,error:{message:error.message}};}
    return json(res,{ok:true,job:catalogJob});
  }
  if(pathname==='/casa/api/catalog'&&req.method==='GET')return json(res,{ready:true,response:catalogResponse});
  if(pathname==='/casa/api/config'&&req.method==='POST'){lastSave=JSON.parse(data);favorites=lastSave.favorites;url=lastSave.url;connected=true;return json(res,{ok:true});}
  if((pathname==='/casa/api/config'&&req.method==='DELETE')||(pathname==='/casa/api/disconnect'&&req.method==='POST')){favorites=[];url='';connected=false;cache=null;homepage=0;if(req.method==='DELETE'){fixture.states.splice(0,fixture.states.length,...JSON.parse(initialStates));actions=[];eventCount=0;}return json(res,{ok:true});}
  if(pathname==='/casa/api/action'){actions.push(JSON.parse(data));return json(res,{ok:true,message:'Comando inviato; attendo lo stato da Home Assistant'});}
  if(pathname==='/qa'){if(req.method==='POST'){const changes=JSON.parse(data);if('connected'in changes)connected=changes.connected;if(changes.state){const item=fixture.states.find(entity=>entity.entity_id===changes.state.id);if(item){item.state=changes.state.value;eventCount++;}}if(changes.drop)for(const socket of wss.clients)socket.close();}return json(res,{lastSave,actions});}
  if(pathname==='/qa/extras'&&req.method==='POST'){
    const extras=[{entity_id:'alarm_control_panel.alarmo',state:'disarmed',attributes:{friendly_name:'Alarmo',code_format:'number',code_arm_required:true,supported_features:7}},...['power','current','voltage'].map((dc,i)=>({entity_id:'sensor.meter_'+dc,state:['142','0.62','230'][i],attributes:{friendly_name:dc,device_class:dc,unit_of_measurement:['W','A','V'][i]}})),{entity_id:'binary_sensor.appliance_power',state:'on',attributes:{friendly_name:'Appliance power',device_class:'power'}},...Array.from({length:65},(_,i)=>({entity_id:'sensor.extra_'+i,state:'on',attributes:{friendly_name:'Extra '+i}}))];
    for(const e of extras)if(!fixture.states.some(s=>s.entity_id===e.entity_id))fixture.states.push(e);
    if(!fixture.devices.some(d=>d.id==='meter-device'))fixture.devices.push({id:'meter-device',name:'Consumo casa',area_id:'living'});
    for(const e of extras)if(!fixture.registry.some(r=>r.entity_id===e.entity_id))fixture.registry.push({entity_id:e.entity_id,area_id:'living',device_id:e.entity_id.startsWith('sensor.meter_')?'meter-device':null});
    fixture.states.find(s=>s.entity_id==='binary_sensor.motion').attributes.device_class='motion';
    return json(res,{ok:true});
  }
  res.writeHead(404);res.end('Not found');
});
const wss=new WebSocketServer({server,path:'/api/websocket'});
wss.on('connection',socket=>{
  let authenticated=false,subscription=0;
  socket.send(JSON.stringify({type:'auth_required',ha_version:'QA-2026'}));
  socket.on('message',data=>{
    const message=JSON.parse(data);
    if(message.type==='auth'){authenticated=message.access_token==='casa-qa-token';socket.send(JSON.stringify({type:authenticated?'auth_ok':'auth_invalid',ha_version:'QA-2026'}));return;}
    if(!authenticated)return socket.close();
    if(message.type==='ping'){socket.send(JSON.stringify({id:message.id,type:'pong'}));return;}
    if(message.type==='subscribe_entities'){
      subscription=message.id;socket.send(JSON.stringify({id:message.id,type:'result',success:true,result:null}));
      const added={};for(const id of message.entity_ids){const state=fixture.states.find(entity=>entity.entity_id===id);if(state)added[id]={s:state.state,a:state.attributes,lc:Math.floor(Date.now()/1000)};}
      socket.send(JSON.stringify({id:subscription,type:'event',event:{a:added}}));return;
    }
    if(message.type==='call_service'){
      if(message.service_data?.temperature===999){socket.send(JSON.stringify({id:message.id,type:'result',success:false,error:{message:'Temperatura di collaudo non valida'}}));return;}
      actions.push(message);const id=message.target.entity_id,state=fixture.states.find(entity=>entity.entity_id===id);
      socket.send(JSON.stringify({id:message.id,type:'result',success:true,result:null}));
      if(state){if(message.service==='toggle')state.state=state.state==='on'?'off':'on';if(message.service_data?.temperature)state.attributes.temperature=message.service_data.temperature;if(message.service_data?.brightness_pct){state.attributes.brightness=Math.round(message.service_data.brightness_pct*255/100);state.state='on';}socket.send(JSON.stringify({id:subscription,type:'event',event:{c:{[id]:{'+':{s:state.state,a:state.attributes}}}}}));}return;
    }
    try{socket.send(JSON.stringify({id:message.id,type:'result',success:true,result:fixture.reply(message)}));}
    catch(error){socket.send(JSON.stringify({id:message.id,type:'result',success:false,error:{message:error.message}}));}
  });
});
server.listen(8769,process.env.CASA_QA_BIND||'127.0.0.1',()=>console.log('CASA_QA_SERVER_READY 8769'));
