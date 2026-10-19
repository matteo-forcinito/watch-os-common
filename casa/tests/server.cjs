const http=require('http'),fs=require('fs'),path=require('path');
const {WebSocketServer}=require('ws');
const fixture=require('./fixtures.cjs');
const initialStates=JSON.stringify(fixture.states);
const root=path.resolve(__dirname,'../..');
let favorites=[],connected=false,url='',actions=[],lastSave=null,eventCount=0;
const category=e=>e.id.startsWith('light.')?'Luci':e.id.startsWith('climate.')?'Termostati':e.device_class==='outlet'?'Prese':e.device_class==='temperature'?'Termometri':e.device_class==='humidity'?'Umidità':'Sensori';
function entity(e){const source=fixture.states.find(item=>item.entity_id===e.id),a=source?.attributes||{},value=source?.state||'unknown';return {...e,category:category(e),state:value,known:!!source,available:!['unknown','unavailable'].includes(value),on:['on','heat','auto'].includes(value),display:value+(a.unit_of_measurement?' '+a.unit_of_measurement:''),brightness:Math.round((a.brightness||0)*100/255),position:-1,current_temp:a.current_temperature??null,target_temp:a.temperature??null,min_temp:a.min_temp??null,max_temp:a.max_temp??null,temp_step:a.target_temp_step??.5,hvac_modes:a.hvac_modes||[],supports_brightness:!!a.brightness};}
function json(res,value){res.writeHead(200,{'Content-Type':'application/json'});res.end(JSON.stringify(value));}
const server=http.createServer(async(req,res)=>{
  const pathname=new URL(req.url,'http://localhost').pathname;
  if(req.method==='GET'&&['/casa/','/kit.js','/kit.css'].includes(pathname)){
    const name=pathname==='/casa/'?'casa/main/casa_dashboard.html':'components/services/web/'+pathname.slice(1);
    res.writeHead(200,{'Content-Type':pathname.endsWith('.js')?'application/javascript':pathname.endsWith('.css')?'text/css':'text/html'});return res.end(fs.readFileSync(path.join(root,name)));
  }
  let data='';for await(const chunk of req)data+=chunk;
  if(pathname==='/casa/api/kit')return json(res,{name:'Casa',board:'GUITION QA',battery:-1});
  if(pathname==='/casa/api/state')return json(res,{connected,configured:!!favorites.length,url,limit:256,revision:eventCount+1,event_count:eventCount,message:connected?'Collegato a Home Assistant':'Configura Casa dal PC',entities:favorites.map(entity)});
  if(pathname==='/casa/api/config'&&req.method==='GET')return json(res,{url,has_token:!!favorites.length,favorites,limit:256});
  if(pathname==='/casa/api/config'&&req.method==='POST'){lastSave=JSON.parse(data);favorites=lastSave.favorites;url=lastSave.url;connected=true;return json(res,{ok:true});}
  if((pathname==='/casa/api/config'&&req.method==='DELETE')||(pathname==='/casa/api/disconnect'&&req.method==='POST')){favorites=[];url='';connected=false;if(req.method==='DELETE'){fixture.states.splice(0,fixture.states.length,...JSON.parse(initialStates));actions=[];eventCount=0;}return json(res,{ok:true});}
  if(pathname==='/casa/api/action'){actions.push(JSON.parse(data));return json(res,{ok:true,message:'Comando inviato; attendo lo stato da Home Assistant'});}
  if(pathname==='/qa'){if(req.method==='POST'){const changes=JSON.parse(data);if('connected'in changes)connected=changes.connected;if(changes.state){const item=fixture.states.find(entity=>entity.entity_id===changes.state.id);if(item){item.state=changes.state.value;eventCount++;}}if(changes.drop)for(const socket of wss.clients)socket.close();}return json(res,{lastSave,actions});}
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
